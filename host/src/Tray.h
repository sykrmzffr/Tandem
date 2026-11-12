#pragma once
#include <windows.h>
#include <atomic>
#include <functional>
#include <string>

// Posted to the tray window by a second copy of Tandem: show the settings window.
constexpr UINT kShowSettingsMsg = WM_APP + 4;

// Notification-area icon, its menu and the settings window. Runs on the UI thread.
class Tray {
public:
    struct Callbacks {
        std::function<void()> onQualityChanged; // the stream must restart (new size or orientation)
        std::function<void()> onQuit;
        std::function<void()> onHandover;       // quitting for a new copy that starts right after (update)
        std::function<void()> onDisplayChanged; // monitors added/removed/moved (e.g. Win+P)
        std::function<void()> onRestart;        // start a fresh copy, then quit
        std::function<void()> onRestartTablet;  // close and reopen the app on the tablet
        std::function<bool()> tabletConnected;  // a tablet is attached (its app can be restarted)
        std::function<void()> onEndSession;     // Windows is shutting down or signing out
        std::function<std::wstring()> reportInfo; // lines about the stream and the tablet for a problem report
        std::function<std::string()> tabletSerial; // kept out of problem reports
    };

    bool Create(HINSTANCE inst, Callbacks cb);
    void Destroy();

    // Thread-safe: updates the tooltip / status line, optionally with a balloon.
    void SetStatus(const std::wstring& status, bool notify = false);

    // Pumps messages until Quit is chosen or WM_QUIT arrives.
    int Run();

private:
    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
    static LRESULT CALLBACK SettingsProc(HWND, UINT, WPARAM, LPARAM);
    void ShowMenu();
    void ApplyStatus();
    std::wstring CurrentStatus();

    // Saves settings after a change from the menu or the settings window and
    // restarts the stream if the picture size changed.
    void SettingsChanged(int oldQuality, int oldOrientation);

    // Settings window (created on demand, one at a time).
    void ShowSettings();
    void LayoutSettings();
    void RefreshSettings(); // controls <- settings
    void OnSettingsCommand(int id, int code);

    // Display driver line and its fix button (install, Device Manager, restart).
    void RefreshDriver();
    void FixDriver();
    bool driverOk_ = true;
    int driverFix_ = 0;
    unsigned timerTicks_ = 0;

    // Check for updates: asks GitHub on a worker thread, then offers to download
    // and install a newer release (the new Tandem.exe replaces this copy).
    // `manual` is false for the check Tandem makes on its own (30 s after it starts,
    // then daily): that one stays silent unless a newer version is out.
    void CheckForUpdates(bool manual = true);

    // Opens a GitHub issue form in the browser, filled in with the versions, screens,
    // driver state and the end of the log. Nothing is sent until the user submits it.
    void SendFeedback();
    void OnUpdateChecked(LPARAM result);
    void OnUpdateDownloaded(LPARAM result);
    std::atomic<bool> updateBusy_{false};

    HWND hwnd_ = nullptr;
    HWND settings_ = nullptr;
    HFONT font_ = nullptr, titleFont_ = nullptr;
    HINSTANCE inst_ = nullptr;
    HICON icon_ = nullptr;
    Callbacks cb_;
    std::wstring status_ = L"Starting...";
    bool notifyPending_ = false;
    CRITICAL_SECTION cs_{};
};
