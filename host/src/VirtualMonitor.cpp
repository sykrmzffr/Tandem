#include "VirtualMonitor.h"
#include <dwmapi.h>
#include <algorithm>
#include <climits>
#include <cmath>
#include <mutex>
#include <vector>
#include "Util.h"

namespace {

struct MonitorRects {
    HMONITOR handle = nullptr;
    RECT work{}; // monitor area minus taskbars
};

bool FindMonitor(const std::wstring& gdiName, MonitorRects& out) {
    struct Ctx {
        const std::wstring* name;
        MonitorRects* out;
        bool found;
    } ctx{&gdiName, &out, false};
    EnumDisplayMonitors(
        nullptr, nullptr,
        [](HMONITOR m, HDC, LPRECT, LPARAM lp) -> BOOL {
            auto* c = reinterpret_cast<Ctx*>(lp);
            MONITORINFOEXW mi{};
            mi.cbSize = sizeof mi;
            if (GetMonitorInfoW(m, &mi) && *c->name == mi.szDevice) {
                c->out->handle = m;
                c->out->work = mi.rcWork;
                c->found = true;
                return FALSE;
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&ctx));
    return ctx.found;
}

// Windows a user would see in Alt+Tab, plus visible owned dialogs.
bool IsMovableWindow(HWND w) {
    if (!IsWindowVisible(w) || IsHungAppWindow(w)) return false;
    if (GetWindowLongW(w, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) return false;
    BOOL cloaked = FALSE; // hidden UWP frames, windows on other virtual desktops
    if (SUCCEEDED(DwmGetWindowAttribute(w, DWMWA_CLOAKED, &cloaked, sizeof cloaked)) && cloaked) return false;
    wchar_t cls[64] = {};
    GetClassNameW(w, cls, 64);
    for (const wchar_t* shell : {L"Shell_TrayWnd", L"Shell_SecondaryTrayWnd", L"Progman", L"WorkerW"})
        if (!wcscmp(cls, shell)) return false;
    RECT r;
    return GetWindowRect(w, &r) && r.right > r.left && r.bottom > r.top;
}

// Maps `r` from one work area to another, keeping its relative place and size.
RECT MapRect(const RECT& r, const RECT& from, const RECT& to) {
    double sx = double(to.right - to.left) / std::max<LONG>(1, from.right - from.left);
    double sy = double(to.bottom - to.top) / std::max<LONG>(1, from.bottom - from.top);
    RECT o;
    o.left = to.left + LONG((r.left - from.left) * sx);
    o.top = to.top + LONG((r.top - from.top) * sy);
    o.right = o.left + LONG((r.right - r.left) * sx);
    o.bottom = o.top + LONG((r.bottom - r.top) * sy);
    // Keep it fully inside the target work area.
    LONG dx = std::max(to.left - o.left, std::min<LONG>(0, to.right - o.right));
    LONG dy = std::max(to.top - o.top, std::min<LONG>(0, to.bottom - o.bottom));
    OffsetRect(&o, dx, dy);
    return o;
}

// Windows taken off the VDD monitor, to put back when the tablet returns.
struct MovedWindow {
    HWND hwnd;
    DWORD pid;
    RECT from;     // normal position on the VDD monitor (screen coordinates)
    RECT fromArea; // the VDD monitor's work area then
    POINT placedAt; // top-left after the move, to notice when the user moves it
    bool maximized, minimized;
};
std::mutex g_movedMu;
std::vector<MovedWindow> g_moved;

void Remember(HWND w, const RECT& from, const RECT& fromArea, bool maximized, bool minimized) {
    MovedWindow m{w, 0, from, fromArea, {}, maximized, minimized};
    GetWindowThreadProcessId(w, &m.pid);
    RECT now;
    if (GetWindowRect(w, &now)) m.placedAt = {now.left, now.top};
    std::lock_guard<std::mutex> lock(g_movedMu);
    g_moved.push_back(m);
}

// Window placements use workspace coordinates: origin at the primary work area.
POINT WorkspaceOffset() {
    MONITORINFO pmi{sizeof pmi};
    GetMonitorInfoW(MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY), &pmi);
    return {pmi.rcWork.left - pmi.rcMonitor.left, pmi.rcWork.top - pmi.rcMonitor.top};
}

// Windows placed on a monitor with other scaling rescale themselves once there
// (WM_DPICHANGED), e.g. 2.5x bigger going from a 100% screen to a 250% tablet.
// Placing the normal (not minimized or maximized) ones again keeps the size meant.
void Settle(const std::vector<std::pair<HWND, WINDOWPLACEMENT>>& placed) {
    if (placed.empty()) return;
    Sleep(200); // the apps handle the change on their own threads
    for (const auto& [w, wp] : placed) {
        WINDOWPLACEMENT now{sizeof now};
        if (wp.showCmd == SW_SHOWNOACTIVATE && !IsIconic(w) && !IsZoomed(w) && GetWindowPlacement(w, &now) &&
            !EqualRect(&now.rcNormalPosition, &wp.rcNormalPosition))
            SetWindowPlacement(w, &wp);
    }
}

// Where windows go when the tablet's screen goes away: the main display, or when the
// tablet itself is the main display, the largest other screen.
bool MainMonitor(HMONITOR exclude, MONITORINFO& mi) {
    HMONITOR primary = MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY);
    if (primary != exclude) return GetMonitorInfoW(primary, &mi) != FALSE;
    struct Ctx {
        HMONITOR exclude, best;
        LONGLONG area;
    } ctx{exclude, nullptr, 0};
    EnumDisplayMonitors(
        nullptr, nullptr,
        [](HMONITOR m, HDC, LPRECT r, LPARAM lp) -> BOOL {
            auto* c = reinterpret_cast<Ctx*>(lp);
            LONGLONG a = LONGLONG(r->right - r->left) * (r->bottom - r->top);
            if (m != c->exclude && a > c->area) {
                c->area = a;
                c->best = m;
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&ctx));
    return ctx.best && GetMonitorInfoW(ctx.best, &mi);
}

// Counts the active screens by kind. False if Windows could not say.
bool CountActive(int& builtIn, int& vdd, int& other) {
    builtIn = vdd = other = 0;
    UINT32 paths = 0, modes = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &paths, &modes) != ERROR_SUCCESS) return false;
    std::vector<DISPLAYCONFIG_PATH_INFO> p(paths);
    std::vector<DISPLAYCONFIG_MODE_INFO> m(modes);
    if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &paths, p.data(), &modes, m.data(), nullptr) != ERROR_SUCCESS)
        return false;
    for (UINT32 i = 0; i < paths; i++) {
        DISPLAYCONFIG_TARGET_DEVICE_NAME name{};
        name.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
        name.header.size = sizeof name;
        name.header.adapterId = p[i].targetInfo.adapterId;
        name.header.id = p[i].targetInfo.id;
        if (DisplayConfigGetDeviceInfo(&name.header) != ERROR_SUCCESS) return false;
        switch (name.outputTechnology) {
        case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_INTERNAL:
        case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_DISPLAYPORT_EMBEDDED:
        case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_UDI_EMBEDDED:
        case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_LVDS: builtIn++; break;
        default:
            if (wcsstr(name.monitorFriendlyDeviceName, L"VDD")) vdd++;
            else other++;
        }
    }
    return true;
}

bool IsVddTarget(const DISPLAYCONFIG_PATH_INFO& path) {
    DISPLAYCONFIG_TARGET_DEVICE_NAME name{};
    name.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
    name.header.size = sizeof name;
    name.header.adapterId = path.targetInfo.adapterId;
    name.header.id = path.targetInfo.id;
    return DisplayConfigGetDeviceInfo(&name.header) == ERROR_SUCCESS &&
           wcsstr(name.monitorFriendlyDeviceName, L"VDD") != nullptr;
}

// Every possible source-target path (the active ones included) and the modes they use.
bool QueryAllPaths(std::vector<DISPLAYCONFIG_PATH_INFO>& p, std::vector<DISPLAYCONFIG_MODE_INFO>& m) {
    UINT32 paths = 0, modes = 0;
    if (GetDisplayConfigBufferSizes(QDC_ALL_PATHS, &paths, &modes) != ERROR_SUCCESS) return false;
    p.resize(paths);
    m.resize(modes);
    if (QueryDisplayConfig(QDC_ALL_PATHS, &paths, p.data(), &modes, m.data(), nullptr) != ERROR_SUCCESS) return false;
    p.resize(paths);
    m.resize(modes);
    return true;
}

// The VDD monitor's last place and mode on the desktop (see SetScreen). Its place
// is also kept in the registry for the next start of Tandem.
std::mutex g_vddModeMu;
DISPLAYCONFIG_MODE_INFO g_vddSource{}, g_vddTarget{};
constexpr const wchar_t* kStateKey = L"Software\\Tandem";
constexpr const wchar_t* kScreenValue = L"TabletScreenPlace";

bool SameSource(const DISPLAYCONFIG_PATH_INFO& a, const DISPLAYCONFIG_PATH_INFO& b) {
    return a.sourceInfo.adapterId.LowPart == b.sourceInfo.adapterId.LowPart &&
           a.sourceInfo.adapterId.HighPart == b.sourceInfo.adapterId.HighPart && a.sourceInfo.id == b.sourceInfo.id;
}

bool FindDevice(DISPLAY_DEVICEW& out) {
    DISPLAY_DEVICEW dd{sizeof dd};
    for (DWORD i = 0; EnumDisplayDevicesW(nullptr, i, &dd, 0); i++, dd.cb = sizeof dd) {
        if (wcsstr(dd.DeviceID, L"MttVDD") || wcsstr(dd.DeviceString, L"Virtual Display Driver")) {
            out = dd;
            return true;
        }
    }
    return false;
}

} // namespace

namespace VirtualMonitor {

std::wstring Find() {
    DISPLAY_DEVICEW dd{};
    return FindDevice(dd) ? dd.DeviceName : L"";
}

bool IsAttached() {
    DISPLAY_DEVICEW dd{};
    return FindDevice(dd) && (dd.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP);
}

bool MatchTablet(const std::wstring& gdiName, UINT tabletW, UINT tabletH, UINT hz) {
    DEVMODEW cur{};
    cur.dmSize = sizeof cur;
    if (!EnumDisplaySettingsW(gdiName.c_str(), ENUM_CURRENT_SETTINGS, &cur) || !tabletW || !tabletH) return false;

    // Resolution: same orientation first, then the closest shape, then the closest size.
    const bool portrait = tabletH > tabletW;
    const double aspect = std::log(double(tabletW) / tabletH);
    DWORD bestW = cur.dmPelsWidth, bestH = cur.dmPelsHeight;
    double bestScore = 1e18;
    DEVMODEW dm{};
    dm.dmSize = sizeof dm;
    for (DWORD i = 0; EnumDisplaySettingsW(gdiName.c_str(), i, &dm); i++) {
        if (!dm.dmPelsWidth || !dm.dmPelsHeight) continue;
        double score = ((dm.dmPelsHeight > dm.dmPelsWidth) != portrait ? 1000.0 : 0.0) +
                       std::round(std::abs(std::log(double(dm.dmPelsWidth) / dm.dmPelsHeight) - aspect) * 100) +
                       std::abs(std::log(double(dm.dmPelsWidth) * dm.dmPelsHeight / (double(tabletW) * tabletH))) * 0.01;
        if (score < bestScore) {
            bestScore = score;
            bestW = dm.dmPelsWidth;
            bestH = dm.dmPelsHeight;
        }
    }
    // Refresh rate at that resolution: the requested one, else the closest.
    DWORD bestHz = cur.dmDisplayFrequency;
    int bestDiff = INT_MAX;
    for (DWORD i = 0; EnumDisplaySettingsW(gdiName.c_str(), i, &dm); i++) {
        if (dm.dmPelsWidth != bestW || dm.dmPelsHeight != bestH) continue;
        int diff = std::abs(int(dm.dmDisplayFrequency) - int(hz));
        if (diff < bestDiff) {
            bestDiff = diff;
            bestHz = dm.dmDisplayFrequency;
        }
    }
    if (cur.dmPelsWidth == bestW && cur.dmPelsHeight == bestH && cur.dmDisplayFrequency == bestHz) return false;

    DEVMODEW want{};
    want.dmSize = sizeof want;
    want.dmPelsWidth = bestW;
    want.dmPelsHeight = bestH;
    want.dmDisplayFrequency = bestHz;
    want.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_DISPLAYFREQUENCY;
    LONG rc = ChangeDisplaySettingsExW(gdiName.c_str(), &want, nullptr, 0, nullptr); // not saved to the registry
    Log("Virtual monitor %lux%lu @ %lu Hz -> %lux%lu @ %lu Hz for a %ux%u tablet: %s", cur.dmPelsWidth,
        cur.dmPelsHeight, cur.dmDisplayFrequency, bestW, bestH, bestHz, tabletW, tabletH,
        rc == DISP_CHANGE_SUCCESSFUL ? "ok" : "failed");
    return rc == DISP_CHANGE_SUCCESSFUL;
}

bool HasMode(const std::wstring& gdiName, UINT w, UINT h) {
    DEVMODEW dm{};
    dm.dmSize = sizeof dm;
    for (DWORD i = 0; EnumDisplaySettingsW(gdiName.c_str(), i, &dm); i++)
        if (dm.dmPelsWidth == w && dm.dmPelsHeight == h) return true;
    return false;
}

int Topology() {
    UINT32 paths = 0, modes = 0;
    if (GetDisplayConfigBufferSizes(QDC_DATABASE_CURRENT, &paths, &modes) != ERROR_SUCCESS) return -1;
    std::vector<DISPLAYCONFIG_PATH_INFO> p(paths);
    std::vector<DISPLAYCONFIG_MODE_INFO> m(modes);
    DISPLAYCONFIG_TOPOLOGY_ID topology{};
    if (QueryDisplayConfig(QDC_DATABASE_CURRENT, &paths, p.data(), &modes, m.data(), &topology) != ERROR_SUCCESS)
        return -1;
    switch (topology) {
    case DISPLAYCONFIG_TOPOLOGY_EXTEND: return 0;
    case DISPLAYCONFIG_TOPOLOGY_CLONE: return 1;
    case DISPLAYCONFIG_TOPOLOGY_INTERNAL: return 2;
    case DISPLAYCONFIG_TOPOLOGY_EXTERNAL: return 3;
    default: return -1;
    }
}

bool SetTopology(int mode) {
    static const UINT32 flags[] = {SDC_TOPOLOGY_EXTEND, SDC_TOPOLOGY_CLONE, SDC_TOPOLOGY_INTERNAL,
                                   SDC_TOPOLOGY_EXTERNAL};
    static const char* names[] = {"extend", "duplicate", "PC screen only", "second screen only"};
    if (mode < 0 || mode > 3) return false;
    LONG rc = SetDisplayConfig(0, nullptr, 0, nullptr, SDC_APPLY | flags[mode]);
    Log("Screen mode -> %s: %s", names[mode], rc == ERROR_SUCCESS ? "ok" : "failed");
    return rc == ERROR_SUCCESS;
}

bool OnlyVddBesidesBuiltIn() {
    int builtIn, vdd, other;
    return CountActive(builtIn, vdd, other) && builtIn > 0 && vdd > 0 && other == 0;
}

bool OnlyVddActive() {
    int builtIn, vdd, other;
    return CountActive(builtIn, vdd, other) && vdd > 0 && builtIn == 0 && other == 0;
}

int ScreenState() {
    std::vector<DISPLAYCONFIG_PATH_INFO> p;
    std::vector<DISPLAYCONFIG_MODE_INFO> m;
    if (!QueryAllPaths(p, m)) return 0;
    int state = 0;
    for (const auto& path : p) {
        if (!path.targetInfo.targetAvailable || !IsVddTarget(path)) continue;
        if (path.flags & DISPLAYCONFIG_PATH_ACTIVE) return 2;
        state = 1;
    }
    return state;
}

bool SetScreen(bool on) {
    std::vector<DISPLAYCONFIG_PATH_INFO> all;
    std::vector<DISPLAYCONFIG_MODE_INFO> modes;
    if (!QueryAllPaths(all, modes)) return false;
    // The other screens keep their current modes. (Letting Windows take this set of
    // screens' setup from its database brought back an old resolution of the
    // laptop's own screen in testing.)
    std::vector<DISPLAYCONFIG_PATH_INFO> paths;
    std::vector<DISPLAYCONFIG_MODE_INFO> used;
    std::vector<UINT32> newIndex(modes.size(), DISPLAYCONFIG_PATH_MODE_IDX_INVALID);
    auto keepMode = [&](UINT32& idx) {
        if (idx >= modes.size()) {
            idx = DISPLAYCONFIG_PATH_MODE_IDX_INVALID;
            return;
        }
        if (newIndex[idx] == DISPLAYCONFIG_PATH_MODE_IDX_INVALID) {
            newIndex[idx] = UINT32(used.size());
            used.push_back(modes[idx]);
        }
        idx = newIndex[idx];
    };
    bool vddActive = false;
    for (const auto& path : all) {
        if (!(path.flags & DISPLAYCONFIG_PATH_ACTIVE)) continue;
        if (IsVddTarget(path)) {
            vddActive = true;
            // Mirroring another screen (Duplicate, or some "Second screen only" setups):
            // it shows nothing of its own, so it is left as Windows set it up.
            if (std::any_of(all.begin(), all.end(), [&](const DISPLAYCONFIG_PATH_INFO& other) {
                    return &other != &path && (other.flags & DISPLAYCONFIG_PATH_ACTIVE) && SameSource(other, path);
                }))
                return on;
            // Remember its place and mode for turning it back on.
            std::lock_guard<std::mutex> lock(g_vddModeMu);
            g_vddSource = path.sourceInfo.modeInfoIdx < modes.size() ? modes[path.sourceInfo.modeInfoIdx] : DISPLAYCONFIG_MODE_INFO{};
            g_vddTarget = path.targetInfo.modeInfoIdx < modes.size() ? modes[path.targetInfo.modeInfoIdx] : DISPLAYCONFIG_MODE_INFO{};
            if (g_vddSource.infoType == DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE)
                RegSetKeyValueW(HKEY_CURRENT_USER, kStateKey, kScreenValue, REG_BINARY, &g_vddSource, sizeof g_vddSource);
            if (!on) continue;
        }
        paths.push_back(path);
        keepMode(paths.back().sourceInfo.modeInfoIdx);
        keepMode(paths.back().targetInfo.modeInfoIdx);
    }
    if (vddActive == on) return true;
    const size_t otherModes = used.size();
    if (on) { // a path to the VDD monitor from a source nothing else uses
        auto it = std::find_if(all.begin(), all.end(), [&](const DISPLAYCONFIG_PATH_INFO& path) {
            return !(path.flags & DISPLAYCONFIG_PATH_ACTIVE) && path.targetInfo.targetAvailable && IsVddTarget(path) &&
                   std::none_of(paths.begin(), paths.end(),
                                [&](const DISPLAYCONFIG_PATH_INFO& a) { return SameSource(a, path); });
        });
        if (it == all.end()) return false;
        DISPLAYCONFIG_PATH_INFO add = *it;
        add.flags |= DISPLAYCONFIG_PATH_ACTIVE;
        add.sourceInfo.modeInfoIdx = add.targetInfo.modeInfoIdx = DISPLAYCONFIG_PATH_MODE_IDX_INVALID;
        // Back where it was, when its mode is known (else Windows places it).
        std::lock_guard<std::mutex> lock(g_vddModeMu);
        DWORD size = sizeof g_vddSource;
        if (g_vddSource.infoType != DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE &&
            RegGetValueW(HKEY_CURRENT_USER, kStateKey, kScreenValue, RRF_RT_REG_BINARY, nullptr, &g_vddSource, &size) !=
                ERROR_SUCCESS)
            g_vddSource = {};
        if (g_vddSource.infoType == DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE) {
            DISPLAYCONFIG_MODE_INFO m = g_vddSource;
            m.id = add.sourceInfo.id;
            m.adapterId = add.sourceInfo.adapterId;
            add.sourceInfo.modeInfoIdx = UINT32(used.size());
            used.push_back(m);
            if (g_vddTarget.infoType == DISPLAYCONFIG_MODE_INFO_TYPE_TARGET && g_vddTarget.id == add.targetInfo.id) {
                add.targetInfo.modeInfoIdx = UINT32(used.size());
                used.push_back(g_vddTarget);
            }
        }
        paths.push_back(add);
    }
    if (paths.empty()) return false;
    LONG rc = SetDisplayConfig(UINT32(paths.size()), paths.data(), UINT32(used.size()), used.data(),
                               SDC_APPLY | SDC_USE_SUPPLIED_DISPLAY_CONFIG | SDC_ALLOW_CHANGES | SDC_SAVE_TO_DATABASE);
    if (rc != ERROR_SUCCESS && on && paths.back().sourceInfo.modeInfoIdx != DISPLAYCONFIG_PATH_MODE_IDX_INVALID) {
        // Its old place no longer fits (screens rearranged): let Windows place it.
        Log("Tablet screen on at its last place failed (%ld)", rc);
        paths.back().sourceInfo.modeInfoIdx = paths.back().targetInfo.modeInfoIdx = DISPLAYCONFIG_PATH_MODE_IDX_INVALID;
        used.resize(otherModes);
        rc = SetDisplayConfig(UINT32(paths.size()), paths.data(), UINT32(used.size()), used.data(),
                              SDC_APPLY | SDC_USE_SUPPLIED_DISPLAY_CONFIG | SDC_ALLOW_CHANGES | SDC_SAVE_TO_DATABASE);
    }
    Log("Tablet screen %s: %s (%ld)", on ? "on" : "off", rc == ERROR_SUCCESS ? "ok" : "failed", rc);
    return rc == ERROR_SUCCESS;
}

bool Bounds(RECT& out) {
    std::wstring name = Find();
    if (name.empty()) return false;
    DEVMODEW dm{};
    dm.dmSize = sizeof dm;
    if (!EnumDisplaySettingsW(name.c_str(), ENUM_CURRENT_SETTINGS, &dm) || !dm.dmPelsWidth) return false;
    MonitorRects m;
    if (!FindMonitor(name, m)) return false;
    MONITORINFO mi{sizeof mi};
    if (!GetMonitorInfoW(m.handle, &mi)) return false;
    out = mi.rcMonitor;
    return true;
}

int MoveStrandedWindows(const RECT& area) {
    HMONITOR primary = MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO pmi{sizeof pmi};
    if (!GetMonitorInfoW(primary, &pmi)) return 0;
    std::vector<HWND> windows;
    EnumWindows(
        [](HWND w, LPARAM lp) -> BOOL {
            reinterpret_cast<std::vector<HWND>*>(lp)->push_back(w);
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&windows));
    int moved = 0;
    for (HWND w : windows) {
        if (!IsMovableWindow(w) || IsIconic(w) || MonitorFromWindow(w, MONITOR_DEFAULTTONULL)) continue;
        RECT r, overlap;
        if (!GetWindowRect(w, &r) || r.right - r.left < 40 || r.bottom - r.top < 40 ||
            !IntersectRect(&overlap, &r, &area))
            continue;
        RECT target = MapRect(r, area, pmi.rcWork);
        if (SetWindowPos(w, nullptr, target.left, target.top, target.right - target.left,
                         target.bottom - target.top, SWP_NOZORDER | SWP_NOACTIVATE)) {
            Remember(w, r, area, false, false);
            moved++;
        }
    }
    return moved;
}

int MoveWindowsToPrimary() {
    MonitorRects vdd;
    std::wstring name = Find();
    if (name.empty() || !FindMonitor(name, vdd)) return 0;
    MONITORINFO pmi{sizeof pmi}; // the screen windows go to
    if (!MainMonitor(vdd.handle, pmi)) return 0;
    // Window placements use workspace coordinates (origin at the primary work area).
    POINT ws = WorkspaceOffset();

    std::vector<HWND> windows;
    EnumWindows(
        [](HWND w, LPARAM lp) -> BOOL {
            reinterpret_cast<std::vector<HWND>*>(lp)->push_back(w);
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&windows));

    {
        std::lock_guard<std::mutex> lock(g_movedMu);
        g_moved.clear(); // a new disconnect: remember this batch
    }
    struct Move {
        RECT normal;
        bool maximized, minimized;
    };
    std::vector<std::pair<HWND, WINDOWPLACEMENT>> placed;
    std::vector<Move> moves;
    for (HWND w : windows) {
        if (!IsMovableWindow(w)) continue;
        WINDOWPLACEMENT wp{sizeof wp};
        if (!GetWindowPlacement(w, &wp)) continue;
        RECT normal = wp.rcNormalPosition;
        OffsetRect(&normal, ws.x, ws.y);
        bool minimized = IsIconic(w) != FALSE;
        HMONITOR on = minimized ? MonitorFromRect(&normal, MONITOR_DEFAULTTONEAREST)
                                : MonitorFromWindow(w, MONITOR_DEFAULTTONEAREST);
        if (on != vdd.handle) continue;

        RECT target = MapRect(normal, vdd.work, pmi.rcWork);
        OffsetRect(&target, -ws.x, -ws.y);
        wp.rcNormalPosition = target;
        wp.flags &= WPF_RESTORETOMAXIMIZED;
        bool maximized = !minimized && IsZoomed(w);
        // A maximized window keeps its monitor when only its restore spot changes,
        // so restore it onto the main screen first and maximize it there.
        wp.showCmd = minimized ? SW_SHOWMINNOACTIVE : SW_SHOWNOACTIVATE;
        if (!SetWindowPlacement(w, &wp)) continue; // fails for elevated windows; those stay put
        placed.push_back({w, wp});
        if (maximized) {
            wp.showCmd = SW_SHOWMAXIMIZED;
            SetWindowPlacement(w, &wp);
        }
        moves.push_back({normal, maximized, minimized});
    }
    Settle(placed);
    for (size_t i = 0; i < placed.size(); i++) // after Settle: notes where each one ended up
        Remember(placed[i].first, moves[i].normal, vdd.work, moves[i].maximized, moves[i].minimized);
    int moved = int(placed.size());

    POINT cursor;
    if (GetCursorPos(&cursor) && MonitorFromPoint(cursor, MONITOR_DEFAULTTONULL) == vdd.handle)
        SetCursorPos((pmi.rcWork.left + pmi.rcWork.right) / 2, (pmi.rcWork.top + pmi.rcWork.bottom) / 2);
    return moved;
}

int RestoreWindows(bool apply) {
    std::vector<MovedWindow> moved;
    {
        std::lock_guard<std::mutex> lock(g_movedMu);
        moved.swap(g_moved);
    }
    MonitorRects vdd;
    std::wstring name = Find();
    if (!apply || moved.empty() || name.empty() || !FindMonitor(name, vdd)) return 0;
    POINT ws = WorkspaceOffset();

    std::vector<std::pair<HWND, WINDOWPLACEMENT>> placed;
    for (const MovedWindow& m : moved) {
        DWORD pid = 0;
        if (!IsWindow(m.hwnd) || !GetWindowThreadProcessId(m.hwnd, &pid) || pid != m.pid) continue; // closed
        // Leave windows the user dealt with since: moved, minimized, restored or maximized.
        bool minimized = IsIconic(m.hwnd) != FALSE, maximized = !minimized && IsZoomed(m.hwnd);
        if (minimized != m.minimized || maximized != m.maximized) continue;
        if (!minimized && MonitorFromWindow(m.hwnd, MONITOR_DEFAULTTONEAREST) == vdd.handle) continue; // already there
        RECT now;
        if (!minimized && !maximized &&
            (!GetWindowRect(m.hwnd, &now) || std::abs(now.left - m.placedAt.x) > 64 ||
             std::abs(now.top - m.placedAt.y) > 64))
            continue;

        WINDOWPLACEMENT wp{sizeof wp};
        if (!GetWindowPlacement(m.hwnd, &wp)) continue;
        RECT target = MapRect(m.from, m.fromArea, vdd.work);
        OffsetRect(&target, -ws.x, -ws.y);
        wp.rcNormalPosition = target;
        wp.flags &= WPF_RESTORETOMAXIMIZED;
        // As in MoveWindowsToPrimary: a maximized window moves monitors only when
        // restored onto the new one first.
        wp.showCmd = minimized ? SW_SHOWMINNOACTIVE : SW_SHOWNOACTIVATE;
        if (!SetWindowPlacement(m.hwnd, &wp)) continue;
        placed.push_back({m.hwnd, wp});
        if (maximized) {
            wp.showCmd = SW_SHOWMAXIMIZED;
            SetWindowPlacement(m.hwnd, &wp);
        }
    }
    Settle(placed);
    return int(placed.size());
}

} // namespace VirtualMonitor
