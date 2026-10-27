#pragma once
#include <windows.h>
#include <string>
#include <utility>

// Installs and removes the Virtual Display Driver (MttVDD), which provides the
// extra monitor. Tandem.exe carries the unmodified, signed driver package
// (host/third_party/vdd). Installing needs admin rights, so the elevated parts
// run in a second copy of this executable started with a UAC prompt.
//
// After an install Windows must be restarted before the new monitor is captured:
// capturing a VDD monitor whose driver was started while Windows was running
// froze the test laptop, while after a fresh boot it always worked.
namespace driver {

// True when a VDD device exists (installed by us or by its own installer).
bool Present();

// The VDD device as Device Manager shows it.
enum class State { Missing, Disabled, Failed, Running };

// Whether the driver can give the tablet its own screen, and what to do if not.
struct Health {
    State state = State::Missing;
    ULONG problem = 0;           // Device Manager problem code when Disabled or Failed
    bool restartPending = false; // installed or turned on since Windows started
    int screen = 0;              // VirtualMonitor::ScreenState()
    // Ready (the screen itself may be off the desktop until a tablet connects).
    bool Ok() const { return state == State::Running && !restartPending && screen > 0; }
    std::wstring Summary() const; // one line for the settings window and the log
    std::wstring Advice() const;  // what the user should do; empty when Ok()
};

// Checks the device and its monitor (a few milliseconds).
Health CheckHealth();

// True when Tandem installed the driver and Windows has not restarted since.
bool RestartPending();

// True when the driver was installed by Tandem (so uninstalling may remove it).
bool InstalledByUs();

// Start an elevated copy (UAC prompt) and wait for it. False if declined or failed.
bool RunElevatedInstall();
bool RunElevatedUninstall();

// True when the driver's settings were written by an older Tandem (a file the
// user made is never touched). New settings take effect after a restart.
bool SettingsOutdated();

// Rewrites Tandem's driver settings; 0 on success. Works without admin rights
// when the folder allows it, else use RunElevatedUpdateSettings (--update-driver-settings).
int UpdateSettings();
bool RunElevatedUpdateSettings();

// The virtual monitor resolution Tandem uses for a w x h tablet: its size in
// landscape (portrait is offered too), limited to 4096 wide.
std::pair<int, int> ModeFor(int w, int h);

// Makes sure the driver settings offer ModeFor(w, h), adding it as one of the
// last few tablets' sizes if needed (no admin rights required when the settings
// folder allows it). New modes appear after a restart. True if the settings now
// have the size; false if they are the user's own or could not be written.
bool AddResolution(int w, int h);

// Elevated entry points (--install-driver / --uninstall-driver). Return exit codes.
int Install();
int Uninstall();

// Asks Windows to restart. Returns false if it refused.
bool RestartWindows();

} // namespace driver
