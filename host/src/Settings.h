#pragma once
#include <atomic>
#include <string>

// User preferences, persisted in %APPDATA%\Tandem\settings.ini and shared
// between the tray menu (UI thread) and the streaming thread.
struct Settings {
    std::atomic<bool> touch{true};          // inject finger input
    std::atomic<bool> pen{true};            // inject stylus input
    std::atomic<int> qualityPct{75};        // encode size as % of the monitor (100/75/50)
    std::atomic<bool> startWithWindows{false};
    std::atomic<bool> moveWindowsBack{true}; // turn off the tablet screen (or move windows back) when it goes away
    std::atomic<bool> restoreWindows{true}; // put windows back on the tablet screen when it returns
    std::atomic<int> restoreMode{-1}; // Win+P mode to restore when the tablet returns (0 extend, 1 duplicate, 3 second screen only)
    std::atomic<bool> streamOn{true}; // "Stream to tablet": off pauses the stream and turns the tablet screen off
    std::atomic<bool> screenOurs{true}; // the tablet screen is on the desktop because of Tandem (see HostLoop)
    enum Saver : int { kSaverOnBattery = 0, kSaverAlways = 1, kSaverOff = 2 };
    std::atomic<int> batterySaver{kSaverOff}; // 60 Hz instead of up to 120 Hz
    std::atomic<int> orientation{0}; // 0 follow the tablet, 1 landscape, 2 portrait (proto::Orientation)

    // True when battery saver applies right now (setting + power source).
    bool SaverActive() const;

    void Load();
    void Save() const;

    // Adds or removes the HKCU Run entry that starts the host at sign-in.
    static bool ApplyStartWithWindows(bool enable);

    static std::wstring DataDir(); // %APPDATA%\Tandem, created on demand

    // Text values kept next to the settings (used on the UI thread only): UpdateSkipped,
    // the version the user chose to skip, and UpdateOffered, "<version> <day>" of the
    // last update popup.
    static std::wstring ReadText(const wchar_t* key);
    static void WriteText(const wchar_t* key, const std::wstring& value);
};

Settings& AppSettings();
