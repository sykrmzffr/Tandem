#include "Tray.h"
#include <shellapi.h>
#include <commctrl.h>
#include "Driver.h"
#include "Installer.h"
#include "Resources.h"
#include "Settings.h"
#include "Updater.h"
#include "Util.h"
#include "VirtualMonitor.h"
#include <shellscalingapi.h>
#include <share.h>
#include <cctype>
#include <ctime>
#include <memory>
#include <thread>
#include <vector>

namespace {
constexpr UINT kTrayMsg = WM_APP + 1;    // icon clicks
constexpr UINT kStatusMsg = WM_APP + 2;  // status changed on another thread
constexpr UINT kUpdateCheckedMsg = WM_APP + 6;    // lParam: UpdateResult*
constexpr UINT kUpdateDownloadedMsg = WM_APP + 7; // lParam: UpdateResult*
constexpr UINT kIconId = 1;
constexpr UINT_PTR kStatusTimer = 1;
constexpr UINT_PTR kUpdateTimer = 2; // on the tray window: look for a newer release
constexpr UINT kUpdateFirstMs = 30 * 1000, kUpdateEveryMs = 24 * 60 * 60 * 1000;
constexpr const wchar_t* kIssueUrl = L"https://github.com/sykrmzffr/Tandem/issues/new";
constexpr size_t kMaxReportUrl = 7000; // longer links are cut off by browsers or GitHub

enum MenuId : UINT {
    kIdTouch = 100,
    kIdPen,
    kIdSharp,
    kIdBalanced,
    kIdFast,
    kIdSaverOnBattery,
    kIdSaverAlways,
    kIdSaverOff,
    kIdAutostart,
    kIdMoveBack,
    kIdRestoreWindows,
    kIdSettings,
    kIdExtend,
    kIdDuplicate,
    kIdRestart,
    kIdOrientAuto,
    kIdOrientLandscape,
    kIdOrientPortrait,
    kIdLog,
    kIdUpdates,
    kIdQuit,
    kIdStream,
    kIdRestartTablet,
    kIdFeedback,
};

// Settings window controls.
enum ControlId : int {
    kCtlTitle = 200,
    kCtlVersion,
    kCtlStatus,
    kCtlInputLabel,
    kCtlTouch,
    kCtlPen,
    kCtlQualityLabel,
    kCtlQuality,
    kCtlSaverLabel,
    kCtlSaver,
    kCtlMoveBack,
    kCtlAutostart,
    kCtlLog,
    kCtlModeLabel,
    kCtlMode,
    kCtlRestart,
    kCtlOrientLabel,
    kCtlOrient,
    kCtlRestoreWindows,
    kCtlUpdates,
    kCtlDriver,
    kCtlDriverFix,
    kCtlStream,
    kCtlRestartTablet,
    kCtlFeedback,
};

constexpr int kQualityValues[] = {100, 75, 50};
constexpr const wchar_t* kQualityNames[] = {L"Sharp - full resolution", L"Balanced - 75% (recommended)",
                                            L"Fastest - 50%, lowest delay"};
constexpr const wchar_t* kSaverNames[] = {L"When on battery - 60 Hz", L"Always - 60 Hz",
                                          L"Off - up to 120 Hz"};

constexpr const wchar_t* kOrientNames[] = {L"Auto - follow the tablet's rotation", L"Landscape",
                                           L"Portrait"};
constexpr const wchar_t* kModeNames[] = {L"Extend - tablet is a second screen",
                                         L"Duplicate - tablet mirrors this screen"};

// Current projection mode as in Win+P: 0 extend, 1 duplicate, -1 other (e.g. one screen only).
int CurrentMode() {
    int mode = VirtualMonitor::Topology();
    return mode == 0 || mode == 1 ? mode : -1;
}

// Same as choosing Extend or Duplicate in Win+P; the stream follows on its own.
void SetMode(int mode) {
    AppSettings().restoreMode = -1; // the user chose; nothing to restore later
    VirtualMonitor::SetTopology(mode == 1 ? 1 : 0);
    // Extend can leave out a screen that was turned off on its own.
    if (mode == 0 && VirtualMonitor::ScreenState() == 1) VirtualMonitor::SetScreen(true);
}

struct UpdateResult {
    bool manual = true; // asked for by the user: also report errors and "up to date"
    bool ok = false;
    updater::Release release;
    std::wstring error, path;
};

void OpenLog() {
    ShellExecuteW(nullptr, L"open", (Settings::DataDir() + L"host.log").c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

// A message with its own button labels. Returns the index of the button chosen, or
// -1 when the box was closed. The text may hold <a href="...">links</a>.
int Choose(HWND owner, const std::wstring& heading, const std::wstring& text,
           std::initializer_list<const wchar_t*> labels) {
    std::vector<TASKDIALOG_BUTTON> buttons;
    for (const wchar_t* label : labels) buttons.push_back({100 + int(buttons.size()), label});
    TASKDIALOGCONFIG c{sizeof c};
    c.hwndParent = owner;
    c.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_ENABLE_HYPERLINKS | TDF_SIZE_TO_CONTENT |
                (owner ? TDF_POSITION_RELATIVE_TO_WINDOW : 0);
    c.pszWindowTitle = L"Tandem";
    c.pszMainIcon = TD_INFORMATION_ICON;
    c.pszMainInstruction = heading.c_str();
    c.pszContent = text.c_str();
    c.cButtons = UINT(buttons.size());
    c.pButtons = buttons.data();
    c.nDefaultButton = 100;
    c.pfCallback = [](HWND, UINT msg, WPARAM, LPARAM lp, LONG_PTR) -> HRESULT {
        if (msg == TDN_HYPERLINK_CLICKED)
            ShellExecuteW(nullptr, L"open", reinterpret_cast<const wchar_t*>(lp), nullptr, nullptr, SW_SHOWNORMAL);
        return S_OK;
    };
    int pressed = 0;
    if (FAILED(TaskDialogIndirect(&c, &pressed, nullptr, nullptr))) return -1;
    return pressed >= 100 ? pressed - 100 : -1;
}

// ---- problem report -------------------------------------------------------

std::wstring WindowsVersion() {
    auto read = [](const wchar_t* name) {
        wchar_t text[64] = {};
        DWORD size = sizeof text;
        RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", name, RRF_RT_REG_SZ, nullptr,
                     text, &size);
        return std::wstring(text);
    };
    DWORD ubr = 0, size = sizeof ubr;
    RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", L"UBR", RRF_RT_REG_DWORD, nullptr,
                 &ubr, &size);
    std::wstring build = read(L"CurrentBuild");
    return std::wstring(_wtoi(build.c_str()) >= 22000 ? L"Windows 11 " : L"Windows 10 ") + read(L"DisplayVersion") +
           L" (build " + build + L"." + std::to_wstring(ubr) + L")";
}

// "2: 1920x1200 at 125%, 3200x2136 at 250% (tablet)"
std::wstring ScreensText() {
    struct Ctx {
        std::wstring text, vdd;
        int count = 0;
    } ctx;
    ctx.vdd = VirtualMonitor::Find();
    EnumDisplayMonitors(
        nullptr, nullptr,
        [](HMONITOR m, HDC, LPRECT, LPARAM lp) -> BOOL {
            auto* c = reinterpret_cast<Ctx*>(lp);
            MONITORINFOEXW mi{};
            mi.cbSize = sizeof mi;
            UINT dpiX = 96, dpiY = 96;
            if (!GetMonitorInfoW(m, &mi)) return TRUE;
            GetDpiForMonitor(m, MDT_EFFECTIVE_DPI, &dpiX, &dpiY);
            c->text += (c->count++ ? L", " : L"") + std::to_wstring(mi.rcMonitor.right - mi.rcMonitor.left) + L"x" +
                       std::to_wstring(mi.rcMonitor.bottom - mi.rcMonitor.top) + L" at " +
                       std::to_wstring(MulDiv(int(dpiX), 100, 96)) + L"%" + (c->vdd == mi.szDevice ? L" (tablet)" : L"");
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&ctx));
    return std::to_wstring(ctx.count) + L": " + ctx.text;
}

// The last lines of the log, without what could identify the user: the Windows user
// name in paths and the tablet's serial number.
std::vector<std::string> LogTail(size_t maxLines, const std::string& serial) {
    std::vector<std::string> lines;
    FILE* f = _wfsopen((Settings::DataDir() + L"host.log").c_str(), L"rb", _SH_DENYNO);
    if (!f) return lines;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, size > 32768 ? size - 32768 : 0, SEEK_SET);
    std::string text(size_t(size > 32768 ? 32768 : size), '\0');
    text.resize(fread(text.data(), 1, text.size(), f));
    fclose(f);
    for (size_t at = 0; at < text.size();) {
        size_t end = text.find('\n', at);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(at, end - at);
        at = end + 1;
        while (!line.empty() && line.back() == '\r') line.pop_back();
        for (size_t u = line.find("\\Users\\"); u != std::string::npos; u = line.find("\\Users\\", u + 7)) {
            size_t nameEnd = line.find('\\', u + 7);
            line.replace(u + 7, (nameEnd == std::string::npos ? line.size() : nameEnd) - (u + 7), "...");
        }
        for (size_t s = serial.empty() ? std::string::npos : line.find(serial); s != std::string::npos;
             s = line.find(serial, s + 3))
            line.replace(s, serial.size(), "...");
        if (!line.empty()) lines.push_back(line);
    }
    if (lines.size() > maxLines) lines.erase(lines.begin(), lines.end() - maxLines);
    return lines;
}

std::string UrlEncode(const std::string& s) {
    static const char hex[] = "0123456789ABCDEF";
    std::string out;
    for (unsigned char ch : s) {
        if (isalnum(ch) || ch == '-' || ch == '_' || ch == '.' || ch == '~') out += char(ch);
        else (out += '%') += hex[ch >> 4], out += hex[ch & 15];
    }
    return out;
}

// The link that opens a filled-in GitHub issue. Log lines are dropped from the oldest
// until it fits.
std::wstring ReportUrl(const std::wstring& sessionInfo, const std::string& serial) {
    driver::Health health = driver::CheckHealth();
    std::string head = Narrow(L"Tandem " FD_VERSION_WSTR L" on " + WindowsVersion() + L"\n" + sessionInfo + L"Screens: " +
                              ScreensText() + L"\n" + health.Summary() + L"\n\n") +
                       "### What happened, and what did you expect?\n\n\n\n### Log (last lines)\n";
    std::vector<std::string> lines = LogTail(40, serial);
    const std::string prefix = Narrow(kIssueUrl) + "?title=" + UrlEncode("Tandem " FD_VERSION_STR ": ") + "&body=";
    for (;;) {
        std::string body = head + "```\n";
        for (const std::string& line : lines) body += line + "\n";
        std::string url = prefix + UrlEncode(body + "```\n");
        if (url.size() <= kMaxReportUrl || lines.empty()) return Widen(url);
        lines.erase(lines.begin());
    }
}
} // namespace

bool Tray::Create(HINSTANCE inst, Callbacks cb) {
    cb_ = std::move(cb);
    inst_ = inst;
    InitializeCriticalSection(&cs_);
    WNDCLASSW wc{};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.lpszClassName = L"TandemTray";
    RegisterClassW(&wc);
    // A message-only window would not receive the taskbar's re-create broadcast.
    hwnd_ = CreateWindowExW(0, wc.lpszClassName, L"Tandem", 0, 0, 0, 0, 0, nullptr, nullptr, inst, this);
    if (!hwnd_) return false;
    SetWindowLongPtrW(hwnd_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));

    WNDCLASSEXW sc{sizeof sc};
    sc.lpfnWndProc = SettingsProc;
    sc.hInstance = inst;
    sc.lpszClassName = L"TandemSettings";
    sc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    sc.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
    LoadIconMetric(inst, MAKEINTRESOURCEW(IDI_APP), LIM_LARGE, &sc.hIcon);
    LoadIconMetric(inst, MAKEINTRESOURCEW(IDI_APP), LIM_SMALL, &sc.hIconSm);
    RegisterClassExW(&sc);

    LoadIconMetric(inst, MAKEINTRESOURCEW(IDI_APP), LIM_SMALL, &icon_);
    NOTIFYICONDATAW nid{sizeof nid};
    nid.hWnd = hwnd_;
    nid.uID = kIconId;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP | NIF_SHOWTIP;
    nid.uCallbackMessage = kTrayMsg;
    nid.hIcon = icon_;
    wcscpy_s(nid.szTip, L"Tandem");
    if (!Shell_NotifyIconW(NIM_ADD, &nid)) return false;
    nid.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &nid);

    // Look for a newer release shortly after starting, then daily. Only an installed
    // copy does: "Install now" would install one that was started just to try it.
    UINT firstMs = kUpdateFirstMs;
    bool installed = _wcsicmp(ExeDir().c_str(), installer::InstallDir().c_str()) == 0;
#ifdef TANDEM_TEST // seconds until the first check and between checks, from any folder
    wchar_t fake[16];
    if (GetEnvironmentVariableW(L"TANDEM_TEST_UPDATE_SECONDS", fake, 16)) firstMs = UINT(_wtoi(fake)) * 1000, installed = true;
#endif
    if (installed) SetTimer(hwnd_, kUpdateTimer, firstMs, nullptr);
    return true;
}

void Tray::Destroy() {
    if (settings_) DestroyWindow(settings_);
    if (hwnd_) {
        NOTIFYICONDATAW nid{sizeof nid};
        nid.hWnd = hwnd_;
        nid.uID = kIconId;
        Shell_NotifyIconW(NIM_DELETE, &nid);
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
    if (icon_) DestroyIcon(icon_);
    icon_ = nullptr;
    if (font_) DeleteObject(font_);
    if (titleFont_) DeleteObject(titleFont_);
    font_ = titleFont_ = nullptr;
}

void Tray::SetStatus(const std::wstring& status, bool notify) {
    EnterCriticalSection(&cs_);
    status_ = status;
    notifyPending_ |= notify;
    LeaveCriticalSection(&cs_);
    if (hwnd_) PostMessageW(hwnd_, kStatusMsg, 0, 0);
}

std::wstring Tray::CurrentStatus() {
    EnterCriticalSection(&cs_);
    std::wstring status = status_;
    LeaveCriticalSection(&cs_);
    return status;
}

void Tray::ApplyStatus() {
    EnterCriticalSection(&cs_);
    std::wstring status = status_;
    bool notify = notifyPending_;
    notifyPending_ = false;
    LeaveCriticalSection(&cs_);

    NOTIFYICONDATAW nid{sizeof nid};
    nid.hWnd = hwnd_;
    nid.uID = kIconId;
    nid.uFlags = NIF_TIP | NIF_SHOWTIP;
    std::wstring tip = L"Tandem - " + status;
    wcsncpy_s(nid.szTip, tip.c_str(), _TRUNCATE);
    if (notify) {
        nid.uFlags |= NIF_INFO;
        wcscpy_s(nid.szInfoTitle, L"Tandem");
        wcsncpy_s(nid.szInfo, status.c_str(), _TRUNCATE);
        nid.dwInfoFlags = NIIF_INFO | NIIF_NOSOUND;
    }
    Shell_NotifyIconW(NIM_MODIFY, &nid);
    if (settings_) SetDlgItemTextW(settings_, kCtlStatus, status.c_str());
}

void Tray::SettingsChanged(int oldQuality, int oldOrientation) {
    Settings& s = AppSettings();
    s.Save();
    Log("Settings: streamToTablet=%d touch=%d pen=%d quality=%d%% batterySaver=%d orientation=%d "
        "startWithWindows=%d moveWindowsBack=%d restoreWindows=%d",
        int(s.streamOn), int(s.touch), int(s.pen), int(s.qualityPct), int(s.batterySaver), int(s.orientation),
        int(s.startWithWindows), int(s.moveWindowsBack), int(s.restoreWindows));
    if ((s.qualityPct != oldQuality || s.orientation != oldOrientation) && cb_.onQualityChanged)
        cb_.onQualityChanged();
    // Battery saver changes are picked up by the stream itself (it restarts at the new rate).
    if (settings_) RefreshSettings();
}

void Tray::ShowMenu() {
    Settings& s = AppSettings();
    std::wstring status = CurrentStatus();

    HMENU quality = CreatePopupMenu();
    AppendMenuW(quality, MF_STRING | (s.qualityPct == 100 ? MF_CHECKED : 0), kIdSharp, L"&Sharp (full resolution)");
    AppendMenuW(quality, MF_STRING | (s.qualityPct == 75 ? MF_CHECKED : 0), kIdBalanced, L"&Balanced (75%)");
    AppendMenuW(quality, MF_STRING | (s.qualityPct == 50 ? MF_CHECKED : 0), kIdFast, L"&Fastest (50%)");

    HMENU saver = CreatePopupMenu();
    AppendMenuW(saver, MF_STRING | (s.batterySaver == Settings::kSaverOnBattery ? MF_CHECKED : 0), kIdSaverOnBattery,
                L"When on &battery (60 Hz)");
    AppendMenuW(saver, MF_STRING | (s.batterySaver == Settings::kSaverAlways ? MF_CHECKED : 0), kIdSaverAlways,
                L"&Always (60 Hz)");
    AppendMenuW(saver, MF_STRING | (s.batterySaver == Settings::kSaverOff ? MF_CHECKED : 0), kIdSaverOff,
                L"&Off (up to 120 Hz)");

    int mode = CurrentMode();
    HMENU screen = CreatePopupMenu();
    // While streaming is off the tablet's screen stays off, so there is no mode to pick.
    const UINT modeGray = s.streamOn ? 0 : MF_GRAYED;
    AppendMenuW(screen, MF_STRING | (mode == 0 ? MF_CHECKED : 0), kIdExtend, L"&Extend (second screen)");
    AppendMenuW(screen, MF_STRING | (mode == 1 ? MF_CHECKED : 0), kIdDuplicate, L"&Duplicate (mirror)");

    HMENU orient = CreatePopupMenu();
    AppendMenuW(orient, MF_STRING | (s.orientation == 0 ? MF_CHECKED : 0), kIdOrientAuto, L"&Auto (follow the tablet)");
    AppendMenuW(orient, MF_STRING | (s.orientation == 1 ? MF_CHECKED : 0), kIdOrientLandscape, L"&Landscape");
    AppendMenuW(orient, MF_STRING | (s.orientation == 2 ? MF_CHECKED : 0), kIdOrientPortrait, L"&Portrait");

    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, status.c_str());
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kIdSettings, L"&Settings...");
    SetMenuDefaultItem(menu, kIdSettings, FALSE);
    AppendMenuW(menu, MF_STRING | (s.streamOn ? MF_CHECKED : 0), kIdStream, L"Strea&m to tablet");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING | (s.touch ? MF_CHECKED : 0), kIdTouch, L"&Touch input");
    AppendMenuW(menu, MF_STRING | (s.pen ? MF_CHECKED : 0), kIdPen, L"&Pen input");
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(quality), L"&Video quality");
    AppendMenuW(menu, MF_POPUP | modeGray, reinterpret_cast<UINT_PTR>(screen), L"Screen m&ode");
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(orient), L"Tablet orie&ntation");
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(saver), L"&Battery saver");
    AppendMenuW(menu, MF_STRING | (s.moveWindowsBack ? MF_CHECKED : 0), kIdMoveBack,
                L"Turn off the tablet screen when it dis&connects");
    AppendMenuW(menu, MF_STRING | (s.restoreWindows ? MF_CHECKED : 0), kIdRestoreWindows,
                L"Move &windows back to the tablet when it reconnects");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING | (s.startWithWindows ? MF_CHECKED : 0), kIdAutostart, L"St&art with Windows");
    AppendMenuW(menu, MF_STRING, kIdLog, L"Open &log");
    AppendMenuW(menu, MF_STRING | (updateBusy_ ? MF_GRAYED : 0), kIdUpdates, L"Check for &updates...");
    AppendMenuW(menu, MF_STRING, kIdFeedback, L"Send &feedback...");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kIdRestart, L"&Restart Tandem");
    AppendMenuW(menu, MF_STRING | (cb_.tabletConnected && cb_.tabletConnected() ? 0 : MF_GRAYED), kIdRestartTablet,
                L"R&estart tablet app");
    AppendMenuW(menu, MF_STRING, kIdQuit, L"&Quit Tandem");

    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(hwnd_); // so the menu closes when clicking elsewhere
    UINT cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, pt.x, pt.y, 0, hwnd_, nullptr);
    DestroyMenu(menu); // also destroys the submenus

    int oldQuality = s.qualityPct, oldOrientation = s.orientation;
    switch (cmd) {
    case kIdOrientAuto: s.orientation = 0; break;
    case kIdOrientLandscape: s.orientation = 1; break;
    case kIdOrientPortrait: s.orientation = 2; break;
    case kIdStream: s.streamOn = !s.streamOn; break;
    case kIdTouch: s.touch = !s.touch; break;
    case kIdPen: s.pen = !s.pen; break;
    case kIdSharp: s.qualityPct = 100; break;
    case kIdBalanced: s.qualityPct = 75; break;
    case kIdFast: s.qualityPct = 50; break;
    case kIdSaverOnBattery: s.batterySaver = Settings::kSaverOnBattery; break;
    case kIdSaverAlways: s.batterySaver = Settings::kSaverAlways; break;
    case kIdSaverOff: s.batterySaver = Settings::kSaverOff; break;
    case kIdMoveBack: s.moveWindowsBack = !s.moveWindowsBack; break;
    case kIdRestoreWindows: s.restoreWindows = !s.restoreWindows; break;
    case kIdAutostart:
        if (Settings::ApplyStartWithWindows(!s.startWithWindows)) s.startWithWindows = !s.startWithWindows;
        break;
    case kIdSettings: ShowSettings(); return;
    case kIdExtend: SetMode(0); return;
    case kIdDuplicate: SetMode(1); return;
    case kIdRestart:
        if (cb_.onRestart) cb_.onRestart();
        return;
    case kIdRestartTablet:
        if (cb_.onRestartTablet) cb_.onRestartTablet();
        return;
    case kIdLog: OpenLog(); return;
    case kIdUpdates: CheckForUpdates(); return;
    case kIdFeedback: SendFeedback(); return;
    case kIdQuit:
        if (cb_.onQuit) cb_.onQuit();
        PostQuitMessage(0);
        return;
    default:
        return;
    }
    SettingsChanged(oldQuality, oldOrientation);
}

// ---- settings window ------------------------------------------------------

void Tray::ShowSettings() {
    if (settings_) {
        ShowWindow(settings_, SW_RESTORE);
        SetForegroundWindow(settings_);
        return;
    }
    settings_ = CreateWindowExW(0, L"TandemSettings", L"Tandem settings",
                                WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, CW_USEDEFAULT, CW_USEDEFAULT,
                                400, 400, nullptr, nullptr, inst_, this);
    if (!settings_) return;
    auto add = [&](const wchar_t* cls, const wchar_t* text, DWORD style, int id) {
        CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10, settings_,
                        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), inst_, nullptr);
    };
    add(L"STATIC", L"Tandem", SS_LEFT, kCtlTitle);
    add(L"STATIC", L"Version " FD_VERSION_WSTR, SS_RIGHT, kCtlVersion);
    add(L"STATIC", L"", SS_LEFT | SS_NOPREFIX, kCtlStatus);
    add(L"STATIC", L"", SS_LEFT | SS_NOPREFIX, kCtlDriver);
    add(L"BUTTON", L"&Stream to tablet", BS_AUTOCHECKBOX | WS_TABSTOP, kCtlStream);
    add(L"STATIC", L"Tablet input", SS_LEFT, kCtlInputLabel);
    add(L"BUTTON", L"&Touch", BS_AUTOCHECKBOX | WS_TABSTOP, kCtlTouch);
    add(L"BUTTON", L"&Pen", BS_AUTOCHECKBOX | WS_TABSTOP, kCtlPen);
    add(L"STATIC", L"Video &quality", SS_LEFT, kCtlQualityLabel);
    add(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL, kCtlQuality);
    add(L"STATIC", L"&Battery saver", SS_LEFT, kCtlSaverLabel);
    add(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL, kCtlSaver);
    add(L"STATIC", L"Screen m&ode", SS_LEFT, kCtlModeLabel);
    add(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL, kCtlMode);
    add(L"STATIC", L"Tablet orie&ntation", SS_LEFT, kCtlOrientLabel);
    add(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL, kCtlOrient);
    add(L"BUTTON", L"Turn off the tablet screen when it dis&connects", BS_AUTOCHECKBOX | WS_TABSTOP,
        kCtlMoveBack);
    add(L"BUTTON", L"&Move windows back to the tablet when it reconnects", BS_AUTOCHECKBOX | WS_TABSTOP,
        kCtlRestoreWindows);
    add(L"BUTTON", L"Start with &Windows", BS_AUTOCHECKBOX | WS_TABSTOP, kCtlAutostart);
    add(L"BUTTON", L"Check for &updates", BS_PUSHBUTTON | WS_TABSTOP, kCtlUpdates);
    add(L"BUTTON", L"", BS_PUSHBUTTON | WS_TABSTOP, kCtlDriverFix);
    add(L"BUTTON", L"&Restart app", BS_PUSHBUTTON | WS_TABSTOP, kCtlRestart);
    add(L"BUTTON", L"Restart tablet &app", BS_PUSHBUTTON | WS_TABSTOP, kCtlRestartTablet);
    add(L"BUTTON", L"Open &log", BS_PUSHBUTTON | WS_TABSTOP, kCtlLog);
    add(L"BUTTON", L"Send &feedback", BS_PUSHBUTTON | WS_TABSTOP, kCtlFeedback);
    add(L"BUTTON", L"Close", BS_DEFPUSHBUTTON | WS_TABSTOP, IDCANCEL);
    for (const wchar_t* name : kQualityNames)
        SendDlgItemMessageW(settings_, kCtlQuality, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(name));
    for (const wchar_t* name : kSaverNames)
        SendDlgItemMessageW(settings_, kCtlSaver, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(name));
    for (const wchar_t* name : kOrientNames)
        SendDlgItemMessageW(settings_, kCtlOrient, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(name));
    for (const wchar_t* name : kModeNames)
        SendDlgItemMessageW(settings_, kCtlMode, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(name));

    LayoutSettings();
    RefreshSettings();
    SetDlgItemTextW(settings_, kCtlStatus, CurrentStatus().c_str());
    RefreshDriver();

    // Center on the monitor the pointer is on.
    RECT wr;
    GetWindowRect(settings_, &wr);
    POINT pt;
    GetCursorPos(&pt);
    MONITORINFO mi{sizeof mi};
    GetMonitorInfoW(MonitorFromPoint(pt, MONITOR_DEFAULTTOPRIMARY), &mi);
    int w = wr.right - wr.left, h = wr.bottom - wr.top;
    SetWindowPos(settings_, nullptr, (mi.rcWork.left + mi.rcWork.right - w) / 2,
                 (mi.rcWork.top + mi.rcWork.bottom - h) / 2, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
    ShowWindow(settings_, SW_SHOW);
    SetForegroundWindow(settings_);
    SetTimer(settings_, kStatusTimer, 1000, nullptr);
}

// Positions the controls for the window's current DPI (all sizes in 96-DPI units).
void Tray::LayoutSettings() {
    UINT dpi = GetDpiForWindow(settings_);
#ifdef TANDEM_TEST // lay the window out as at another scaling: TANDEM_TEST_DPI=96..240
    wchar_t fake[8];
    if (GetEnvironmentVariableW(L"TANDEM_TEST_DPI", fake, 8)) dpi = UINT(_wtoi(fake));
#endif
    auto px = [dpi](int v) { return MulDiv(v, int(dpi), 96); };

    NONCLIENTMETRICSW ncm{sizeof ncm};
    SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0, dpi);
    if (font_) DeleteObject(font_);
    if (titleFont_) DeleteObject(titleFont_);
    font_ = CreateFontIndirectW(&ncm.lfMessageFont);
    LOGFONTW title = ncm.lfMessageFont;
    title.lfHeight = title.lfHeight * 3 / 2;
    title.lfWeight = FW_SEMIBOLD;
    titleFont_ = CreateFontIndirectW(&title);

    // Labels on the left, their fields on the right; buttons in two columns.
    const int margin = 20, width = 400, inner = width - 2 * margin;
    const int labelW = 120, fieldX = margin + labelW, fieldW = inner - labelW;
    const int half = (inner - 12) / 2, col2 = margin + half + 12;
    struct Place {
        int id, x, y, w, h;
    };
    const Place places[] = {
        {kCtlTitle, margin, 16, 200, 28},
        {kCtlVersion, width - margin - 120, 24, 120, 20},
        {kCtlStatus, margin, 50, inner, 36},
        {kCtlDriver, margin, 88, inner, 36},
        {kCtlStream, margin, 130, inner, 22},
        {kCtlInputLabel, margin, 162, labelW, 20},
        {kCtlTouch, fieldX, 160, 110, 22},
        {kCtlPen, fieldX + 120, 160, 110, 22},
        {kCtlQualityLabel, margin, 196, labelW, 20},
        {kCtlQuality, fieldX, 192, fieldW, 200},
        {kCtlSaverLabel, margin, 230, labelW, 20},
        {kCtlSaver, fieldX, 226, fieldW, 200},
        {kCtlModeLabel, margin, 264, labelW, 20},
        {kCtlMode, fieldX, 260, fieldW, 200},
        {kCtlOrientLabel, margin, 298, labelW, 20},
        {kCtlOrient, fieldX, 294, fieldW, 200},
        {kCtlMoveBack, margin, 332, inner, 22},
        {kCtlRestoreWindows, margin, 358, inner, 22},
        {kCtlAutostart, margin, 384, inner, 22},
        {kCtlUpdates, margin, 418, half, 30},
        {kCtlDriverFix, col2, 418, half, 30},
        {kCtlRestart, margin, 456, half, 30},
        {kCtlRestartTablet, col2, 456, half, 30},
        {kCtlLog, margin, 494, 105, 30},
        {kCtlFeedback, margin + 117, 494, 126, 30},
        {IDCANCEL, width - margin - 105, 494, 105, 30},
    };
    for (const Place& p : places) {
        HWND c = GetDlgItem(settings_, p.id);
        SetWindowPos(c, nullptr, px(p.x), px(p.y), px(p.w), px(p.h), SWP_NOZORDER | SWP_NOACTIVATE);
        SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(p.id == kCtlTitle ? titleFont_ : font_), TRUE);
    }

    RECT rc{0, 0, px(width), px(542)};
    AdjustWindowRectExForDpi(&rc, GetWindowLongW(settings_, GWL_STYLE), FALSE, 0, dpi);
    SetWindowPos(settings_, nullptr, 0, 0, rc.right - rc.left, rc.bottom - rc.top,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

void Tray::RefreshSettings() {
    Settings& s = AppSettings();
    CheckDlgButton(settings_, kCtlStream, s.streamOn ? BST_CHECKED : BST_UNCHECKED);
    // While streaming is off the tablet's screen stays off, so there is no mode to pick.
    EnableWindow(GetDlgItem(settings_, kCtlMode), s.streamOn);
    EnableWindow(GetDlgItem(settings_, kCtlRestartTablet), cb_.tabletConnected && cb_.tabletConnected());
    CheckDlgButton(settings_, kCtlTouch, s.touch ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(settings_, kCtlPen, s.pen ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(settings_, kCtlMoveBack, s.moveWindowsBack ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(settings_, kCtlRestoreWindows, s.restoreWindows ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(settings_, kCtlAutostart, s.startWithWindows ? BST_CHECKED : BST_UNCHECKED);
    int q = 1;
    for (int i = 0; i < 3; i++)
        if (kQualityValues[i] == s.qualityPct) q = i;
    SendDlgItemMessageW(settings_, kCtlQuality, CB_SETCURSEL, q, 0);
    SendDlgItemMessageW(settings_, kCtlSaver, CB_SETCURSEL, s.batterySaver.load(), 0);
    SendDlgItemMessageW(settings_, kCtlMode, CB_SETCURSEL, CurrentMode(), 0); // -1 selects nothing
    SendDlgItemMessageW(settings_, kCtlOrient, CB_SETCURSEL, s.orientation.load(), 0);
}

void Tray::OnSettingsCommand(int id, int code) {
    Settings& s = AppSettings();
    int oldQuality = s.qualityPct, oldOrientation = s.orientation;
    auto checked = [&](int ctl) { return IsDlgButtonChecked(settings_, ctl) == BST_CHECKED; };
    switch (id) {
    case kCtlStream: s.streamOn = checked(kCtlStream); break;
    case kCtlTouch: s.touch = checked(kCtlTouch); break;
    case kCtlPen: s.pen = checked(kCtlPen); break;
    case kCtlMoveBack: s.moveWindowsBack = checked(kCtlMoveBack); break;
    case kCtlRestoreWindows: s.restoreWindows = checked(kCtlRestoreWindows); break;
    case kCtlAutostart: {
        bool want = checked(kCtlAutostart);
        if (Settings::ApplyStartWithWindows(want)) s.startWithWindows = want;
        break;
    }
    case kCtlQuality: {
        if (code != CBN_SELCHANGE) return;
        LRESULT i = SendDlgItemMessageW(settings_, kCtlQuality, CB_GETCURSEL, 0, 0);
        if (i < 0 || i > 2) return;
        s.qualityPct = kQualityValues[i];
        break;
    }
    case kCtlSaver: {
        if (code != CBN_SELCHANGE) return;
        LRESULT i = SendDlgItemMessageW(settings_, kCtlSaver, CB_GETCURSEL, 0, 0);
        if (i < Settings::kSaverOnBattery || i > Settings::kSaverOff) return;
        s.batterySaver = int(i);
        break;
    }
    case kCtlMode: {
        if (code != CBN_SELCHANGE) return;
        LRESULT i = SendDlgItemMessageW(settings_, kCtlMode, CB_GETCURSEL, 0, 0);
        if (i == 0 || i == 1) SetMode(int(i));
        return;
    }
    case kCtlRestart:
        if (cb_.onRestart) cb_.onRestart();
        return;
    case kCtlRestartTablet:
        if (cb_.onRestartTablet) cb_.onRestartTablet();
        return;
    case kCtlOrient: {
        if (code != CBN_SELCHANGE) return;
        LRESULT i = SendDlgItemMessageW(settings_, kCtlOrient, CB_GETCURSEL, 0, 0);
        if (i < 0 || i > 2) return;
        s.orientation = int(i);
        break;
    }
    case kCtlLog: OpenLog(); return;
    case kCtlUpdates: CheckForUpdates(); return;
    case kCtlFeedback: SendFeedback(); return;
    case kCtlDriverFix: FixDriver(); return;
    case IDCANCEL: DestroyWindow(settings_); return;
    default: return;
    }
    SettingsChanged(oldQuality, oldOrientation);
}

LRESULT CALLBACK Tray::SettingsProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
    }
    auto* self = reinterpret_cast<Tray*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!self) return DefWindowProcW(hwnd, msg, wp, lp);
    switch (msg) {
    case WM_COMMAND:
        if (self->settings_) self->OnSettingsCommand(LOWORD(wp), HIWORD(wp));
        return 0;
    case WM_CTLCOLORSTATIC: { // labels on the window background; the status line in gray
        HDC dc = reinterpret_cast<HDC>(wp);
        SetBkMode(dc, TRANSPARENT);
        int id = GetDlgCtrlID(reinterpret_cast<HWND>(lp));
        COLORREF color = GetSysColor(id == kCtlStatus || id == kCtlVersion ? COLOR_GRAYTEXT : COLOR_WINDOWTEXT);
        if (id == kCtlDriver) color = self->driverOk_ ? GetSysColor(COLOR_GRAYTEXT) : RGB(196, 43, 28);
        SetTextColor(dc, color);
        return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_WINDOW));
    }
    case WM_TIMER:
        SetDlgItemTextW(hwnd, kCtlStatus, self->CurrentStatus().c_str());
        EnableWindow(GetDlgItem(hwnd, kCtlRestartTablet), self->cb_.tabletConnected && self->cb_.tabletConnected());
        if (++self->timerTicks_ % 3 == 0) self->RefreshDriver();
        return 0;
    case WM_DPICHANGED: {
        auto* r = reinterpret_cast<RECT*>(lp);
        SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        if (self->settings_) self->LayoutSettings();
        return 0;
    }
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        KillTimer(hwnd, kStatusTimer);
        self->settings_ = nullptr;
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// Shows the display driver's state, with a button when the user can fix it.
void Tray::RefreshDriver() {
    if (!settings_) return;
    driver::Health h = driver::CheckHealth();
    driverOk_ = h.Ok();
    std::wstring text = h.Summary();
    if (h.Ok() && h.screen == 1 && !AppSettings().streamOn)
        text = L"Display driver: running, tablet screen off while streaming is paused";
    if (!h.Ok()) text += L". " + h.Advice() + L".";
    wchar_t old[512] = {};
    GetDlgItemTextW(settings_, kCtlDriver, old, 512);
    if (text != old) SetDlgItemTextW(settings_, kCtlDriver, text.c_str());
    driverFix_ = h.restartPending                   ? 1  // restart now
                 : h.state == driver::State::Missing ? 2 // install
                 : !h.Ok()                           ? 3 // Device Manager
                                                     : 0;
    static const wchar_t* labels[] = {L"", L"Restart PC", L"&Install display driver", L"Open &Device Manager"};
    HWND button = GetDlgItem(settings_, kCtlDriverFix);
    SetWindowTextW(button, labels[driverFix_]);
    ShowWindow(button, driverFix_ ? SW_SHOW : SW_HIDE);
}

void Tray::FixDriver() {
    switch (driverFix_) {
    case 1:
        if (MessageBoxW(settings_, L"Restart your PC now? Save your work first.", L"Tandem",
                        MB_OKCANCEL | MB_ICONQUESTION) == IDOK &&
            !driver::RestartWindows())
            MessageBoxW(settings_, L"Windows did not restart. Please restart it yourself.", L"Tandem", MB_ICONWARNING);
        break;
    case 2:
        EnableWindow(GetDlgItem(settings_, kCtlDriverFix), FALSE);
        if (driver::RunElevatedInstall())
            MessageBoxW(settings_,
                        L"The display driver is installed. Restart your PC to finish; until then the tablet "
                        L"mirrors the main screen.",
                        L"Tandem", MB_ICONINFORMATION);
        else
            MessageBoxW(settings_, L"The display driver was not installed.", L"Tandem", MB_ICONWARNING);
        if (settings_) EnableWindow(GetDlgItem(settings_, kCtlDriverFix), TRUE);
        break;
    case 3: ShellExecuteW(settings_, L"open", L"devmgmt.msc", nullptr, nullptr, SW_SHOWNORMAL); break;
    }
    RefreshDriver();
}

// ---- updates --------------------------------------------------------------

void Tray::SendFeedback() {
    int choice = Choose(settings_, L"Send feedback or report a problem",
                        L"Tandem opens a GitHub page in your browser with a report already filled in: the versions, "
                        L"your screens, the display driver's state and the last lines of the log (without your "
                        L"Windows user name and the tablet's serial number).\n\n"
                        L"Nothing is sent until you press Submit there, and you can edit everything first. You need "
                        L"a GitHub account. To attach the whole log, drag host.log from the log folder into the report.",
                        {L"Open the report page", L"Open log folder", L"Cancel"});
    if (choice == 1) ShellExecuteW(nullptr, L"open", Settings::DataDir().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    if (choice != 0) return;
    std::wstring url = ReportUrl(cb_.reportInfo ? cb_.reportInfo() : L"", cb_.tabletSerial ? cb_.tabletSerial() : "");
    Log("Feedback: opening the report page (%zu characters)", url.size());
#ifdef TANDEM_TEST // write the link to a file instead of opening the browser
    wchar_t file[MAX_PATH];
    if (GetEnvironmentVariableW(L"TANDEM_TEST_FEEDBACK_FILE", file, MAX_PATH)) {
        FILE* f = nullptr;
        if (_wfopen_s(&f, file, L"wb") == 0 && f) {
            fputs(Narrow(url).c_str(), f);
            fclose(f);
        }
        return;
    }
#endif
    if (reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL)) <= 32)
        MessageBoxW(settings_, L"Could not open the browser. You can report the problem at\n"
                               L"github.com/sykrmzffr/Tandem/issues", L"Tandem", MB_ICONWARNING);
}

void Tray::CheckForUpdates(bool manual) {
    if (updateBusy_.exchange(true)) return;
    if (settings_) EnableWindow(GetDlgItem(settings_, kCtlUpdates), FALSE);
    HWND hwnd = hwnd_;
    std::thread([hwnd, manual] {
        auto* r = new UpdateResult;
        r->manual = manual;
        r->ok = updater::Latest(r->release, r->error);
        if (!PostMessageW(hwnd, kUpdateCheckedMsg, 0, reinterpret_cast<LPARAM>(r))) delete r;
    }).detach();
}

void Tray::OnUpdateChecked(LPARAM lp) {
    std::unique_ptr<UpdateResult> r(reinterpret_cast<UpdateResult*>(lp));
    HWND owner = settings_;
    auto done = [this] {
        updateBusy_ = false;
        if (settings_) EnableWindow(GetDlgItem(settings_, kCtlUpdates), TRUE);
    };
    const UINT flags = MB_SETFOREGROUND;
    // The check Tandem makes on its own says nothing unless there is a newer version.
    if (!r->ok) {
        Log("Update check failed: %s", Narrow(r->error).c_str());
        if (r->manual)
            MessageBoxW(owner, (L"Could not check for updates: " + r->error + L".").c_str(), L"Tandem",
                        flags | MB_ICONWARNING);
        return done();
    }
    const std::wstring& version = r->release.version;
    Log("Update check: latest is %s, this is " FD_VERSION_STR, Narrow(version).c_str());
    if (!updater::IsNewer(version)) {
        if (r->manual)
            MessageBoxW(owner, L"You have the latest version of Tandem (" FD_VERSION_WSTR L").", L"Tandem",
                        flags | MB_ICONINFORMATION);
        return done();
    }
    if (!r->manual) {
        // Not for a version the user skipped, and once a day at most.
        const std::wstring offered = version + L" " + std::to_wstring(time(nullptr) / 86400);
        if (version == Settings::ReadText(L"UpdateSkipped") || offered == Settings::ReadText(L"UpdateOffered")) {
            Log("Update %s: skipped by the user or already offered today", Narrow(version).c_str());
            return done();
        }
        Settings::WriteText(L"UpdateOffered", offered);
    }
    const bool canInstall = !r->release.exeUrl.empty();
    std::wstring text = L"You have " FD_VERSION_WSTR L". ";
    text += canInstall ? L"Tandem downloads and installs the update, restarts by itself and keeps your settings; "
                         L"the tablet app updates the next time it connects."
                       : L"Download it from the release page.";
    if (!r->release.pageUrl.empty()) text += L"\n\n<a href=\"" + r->release.pageUrl + L"\">What's new</a>";
    int choice = Choose(owner, L"Tandem " + version + L" is available", text,
                        {canInstall ? L"Install now" : L"Open the download page", L"Later", L"Skip this version"});
    if (choice == 2) {
        Settings::WriteText(L"UpdateSkipped", version);
        Log("Update %s: skipped by the user", Narrow(version).c_str());
    }
    if (choice == 0 && !canInstall)
        ShellExecuteW(nullptr, L"open", r->release.pageUrl.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    if (choice != 0 || !canInstall) return done();

    SetStatus(L"Downloading Tandem " + r->release.version + L"...");
    wchar_t temp[MAX_PATH];
    GetTempPathW(MAX_PATH, temp);
    r->path = std::wstring(temp) + L"Tandem-" + r->release.version + L".exe";
    HWND hwnd = hwnd_;
    UpdateResult* job = r.release();
    std::thread([hwnd, job] {
        job->ok = updater::Download(job->release.exeUrl, job->path, job->error);
        if (!PostMessageW(hwnd, kUpdateDownloadedMsg, 0, reinterpret_cast<LPARAM>(job))) delete job;
    }).detach();
}

void Tray::OnUpdateDownloaded(LPARAM lp) {
    std::unique_ptr<UpdateResult> r(reinterpret_cast<UpdateResult*>(lp));
    updateBusy_ = false;
    if (settings_) EnableWindow(GetDlgItem(settings_, kCtlUpdates), TRUE);
    if (!r->ok) {
        Log("Update download failed: %s", Narrow(r->error).c_str());
        MessageBoxW(settings_, (L"Could not download the update: " + r->error + L".").c_str(), L"Tandem",
                    MB_SETFOREGROUND | MB_ICONWARNING);
        return;
    }
    // The new copy installs itself over this one (asking this copy to quit) and starts.
    Log("Update: installing %s", Narrow(r->release.version).c_str());
    if (reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", r->path.c_str(), L"--update", nullptr,
                                                SW_SHOWNORMAL)) <= 32)
        MessageBoxW(settings_, L"Could not start the downloaded update.", L"Tandem", MB_SETFOREGROUND | MB_ICONWARNING);
}

// ---- tray window ----------------------------------------------------------

LRESULT CALLBACK Tray::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    static UINT taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    auto* self = reinterpret_cast<Tray*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (self) {
        if (msg == kTrayMsg) {
            UINT event = LOWORD(lp);
            if (event == WM_CONTEXTMENU) self->ShowMenu();
            else if (event == WM_LBUTTONUP || event == NIN_SELECT || event == NIN_KEYSELECT) self->ShowSettings();
            return 0;
        }
        if (msg == kStatusMsg) {
            self->ApplyStatus();
            return 0;
        }
        if (msg == WM_DISPLAYCHANGE) {
            if (self->settings_) self->RefreshSettings(); // e.g. Win+P changed the screen mode
            if (self->cb_.onDisplayChanged) self->cb_.onDisplayChanged();
            return 0;
        }
        if (msg == WM_TIMER && wp == kUpdateTimer) {
            UINT everyMs = kUpdateEveryMs;
#ifdef TANDEM_TEST
            wchar_t fake[16];
            if (GetEnvironmentVariableW(L"TANDEM_TEST_UPDATE_SECONDS", fake, 16)) everyMs = UINT(_wtoi(fake)) * 1000;
#endif
            SetTimer(hwnd, kUpdateTimer, everyMs, nullptr);
            // Not on the very first start: Tandem was installed a moment ago.
            if (Settings::ReadText(L"UpdateChecks").empty()) Settings::WriteText(L"UpdateChecks", L"1");
            else self->CheckForUpdates(false);
            return 0;
        }
        if (msg == kUpdateCheckedMsg) {
            self->OnUpdateChecked(lp);
            return 0;
        }
        if (msg == kUpdateDownloadedMsg) {
            self->OnUpdateDownloaded(lp);
            return 0;
        }
        if (msg == kShowSettingsMsg) { // Tandem was started again (e.g. from the Start menu)
            self->ShowSettings();
            return 0;
        }
        if (msg == WM_ENDSESSION && wp) { // Windows ends this process once we return
            if (self->cb_.onEndSession) self->cb_.onEndSession();
            return 0;
        }
        if (msg == installer::kQuitMsg) { // an installer or uninstaller wants us gone
            if (wp && self->cb_.onHandover) self->cb_.onHandover();
            if (self->cb_.onQuit) self->cb_.onQuit();
            PostQuitMessage(0);
            return 0;
        }
        if (msg == taskbarCreated) { // Explorer restarted: put the icon back
            NOTIFYICONDATAW nid{sizeof nid};
            nid.hWnd = hwnd;
            nid.uID = kIconId;
            nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP | NIF_SHOWTIP;
            nid.uCallbackMessage = kTrayMsg;
            nid.hIcon = self->icon_;
            wcscpy_s(nid.szTip, L"Tandem");
            Shell_NotifyIconW(NIM_ADD, &nid);
            nid.uVersion = NOTIFYICON_VERSION_4;
            Shell_NotifyIconW(NIM_SETVERSION, &nid);
            self->ApplyStatus();
            return 0;
        }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int Tray::Run() {
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (settings_ && IsDialogMessageW(settings_, &msg)) continue; // Tab, Esc, mnemonics
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}
