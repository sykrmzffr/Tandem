#pragma once
#include <windows.h>
#include <string>

// Helpers for the Virtual Display Driver (VDD) monitor.
//
// Attaching/detaching the monitor through registry-backed display changes
// (CDS_UPDATEREGISTRY) hung VDD and froze the whole system in testing, so it only
// leaves and joins the desktop through Windows' display configuration (Win+P,
// SetScreen), and only the in-place mode switch below changes its mode, before
// capture starts.
// Disabling/re-enabling the VDD device works, but capturing its monitor afterwards
// hung the graphics stack too, so when the tablet goes away its windows are moved
// back to the main screen instead.
namespace VirtualMonitor {

// GDI name (\\.\DISPLAYn) of the VDD monitor; empty if VDD is not installed.
std::wstring Find();

// True when the VDD monitor is part of the desktop.
bool IsAttached();

// Switches the VDD monitor to the offered mode that best fits a tablet of
// tabletW x tabletH (same orientation, closest shape, closest size) at the refresh
// rate closest to `hz`. Uses a non-persistent ChangeDisplaySettingsEx call (no
// registry update). Returns true when the mode was changed.
bool MatchTablet(const std::wstring& gdiName, UINT tabletW, UINT tabletH, UINT hz);

// True when the VDD monitor offers w x h (in that orientation).
bool HasMode(const std::wstring& gdiName, UINT w, UINT h);

// Projection as in Win+P: 0 Extend, 1 Duplicate, 2 PC screen only, 3 Second screen
// only, -1 other.
int Topology();

// Same as picking that Win+P option (done by Windows, which keeps the VDD device
// running; only the monitor leaves the desktop). True on success.
bool SetTopology(int mode);

// True when the active screens are built-in panel(s) plus the VDD monitor and
// nothing else, so "PC screen only" turns off just the tablet's screen.
bool OnlyVddBesidesBuiltIn();

// True when the VDD monitor is the only active screen (Win+P "Second screen only"
// with no other monitor): without the tablet, nothing would be visible.
bool OnlyVddActive();

// The VDD monitor in Windows' display setup: 0 none (driver not running, or it made
// no monitor), 1 connected but off the desktop, 2 on the desktop.
int ScreenState();

// Puts the VDD monitor on the desktop or takes it off, leaving the other screens
// as they are, like "Extend these displays" / "Disconnect this display" in
// Settings > Display. Windows keeps the VDD device running either way, as with
// Win+P. Turning on uses the place and mode Windows remembers for this set of
// screens. True when it is in the requested state.
bool SetScreen(bool on);

// Desktop area of the VDD monitor; false when it is not on the desktop.
bool Bounds(RECT& out);

// Moves windows left entirely off-screen inside `area` (where the VDD monitor was)
// onto the primary monitor. Windows normally does this when a monitor goes away,
// but some apps (e.g. Flutter ones) put themselves back at their old position.
int MoveStrandedWindows(const RECT& area);

// Moves the windows that MoveWindowsToPrimary / MoveStrandedWindows took off the
// VDD monitor back to their places on it (scaled if its size changed), unless they
// were closed or the user moved, minimized or maximized them since. With apply
// false, only forgets them. Returns the number of windows moved back.
int RestoreWindows(bool apply);

// Moves app windows (and the pointer) off the VDD monitor onto the primary
// monitor, keeping their state (normal/maximized/minimized) and their place
// relative to the screen. Returns the number of windows moved.
int MoveWindowsToPrimary();

} // namespace VirtualMonitor
