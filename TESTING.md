# Release checklist

Real-life scenarios to run before a release. Each line says what to do and what
should happen. The log (`%APPDATA%\Tandem\host.log`, or **Open log**) shows what
Tandem decided; the tablet's own log is `adb logcat -s Tandem`.

## Install and update

| # | Scenario | Expected |
|---|---|---|
| 1 | Fresh PC without Virtual Display Driver: run `Tandem.exe`, choose Yes | Driver offered, one admin prompt, restart requested; after restart the tablet screen works |
| 2 | PC with FreeDisplay 1.2.0 installed: run `Tandem.exe`, choose Yes | FreeDisplay closed and removed (shortcut, autostart, Apps entry, files); settings carried over; driver kept |
| 3 | Older Tandem installed: run the new `Tandem.exe` | Replaced in place, settings kept |
| 4 | **Check for updates** with the latest version installed | "You have the latest version" |
| 5 | **Check for updates** with a newer release published | Offers download; Yes downloads, installs, restarts Tandem with its window open; settings kept |
| 6 | **Check for updates** with no internet | Clear error message, nothing changed |
| 7 | Open Tandem from the Start menu | Settings window opens |
| 8 | Sign in to Windows (start with Windows on) | Tray icon only, no window |
| 9 | Open Tandem again while it runs | The running copy shows its window; no second copy |
| 10 | Uninstall from Settings > Apps | Removed; driver removal offered only if Tandem installed it |

## Connecting

| # | Scenario | Expected |
|---|---|---|
| 11 | Plug in a tablet with USB debugging allowed | Tablet app installs or updates itself, opens, shows the second screen within a few seconds |
| 12 | Plug in a tablet that has not allowed this PC yet | Status asks to tap Allow on the tablet; works after Allow |
| 13 | Unplug the tablet | Within ~3 s the extra screen turns off (PC screen only); windows and pointer on the main screen |
| 14 | Plug it back in | Previous mode (Extend/Duplicate) back; windows return to their places on the tablet |
| 15 | Close the tablet app, cable still in | After 3 s same as unplugging; reopening the app brings everything back |
| 16 | Use it for an hour | No flashes of the waiting screen; picture sharp once still |
| 17 | PC goes to sleep and wakes up with the tablet plugged in | Stream resumes by itself (or after a replug) |
| 18 | Unplug and replug quickly several times | Ends up streaming; no stuck screen mode |
| 19 | Tablet whose decoder rejects low-latency settings (e.g. Huawei) | Streams with the plain settings; if nothing works, the tablet shows why |

## Screens and monitors

| # | Scenario | Expected |
|---|---|---|
| 20 | Laptop + tablet, Extend | Tablet is a second screen |
| 21 | Win+P Duplicate while connected | Tablet mirrors the main screen within ~2 s |
| 22 | Win+P "Second screen only" (tablet is the only screen), then unplug | Laptop screen comes back on (never left with no screen); replug restores "Second screen only" |
| 23 | Laptop + external monitor + tablet, unplug the tablet | Both real screens stay on; windows from the tablet move to the main screen |
| 24 | Tablet set as main display, unplug | Windows move to the largest real screen |
| 25 | Tablet placed left of or above the main screen | Touch and pen land where you tap |
| 26 | Main screen and tablet at different scaling (e.g. 125% / 200%) | Windows moved back keep a sensible size |
| 27 | Desktop PC (no built-in screen) + monitor + tablet, unplug | Monitor stays on; windows moved back (the empty extra screen stays until replug) |

## Tablets and settings

| # | Scenario | Expected |
|---|---|---|
| 28 | Orientation Landscape / Portrait / Auto, turning the tablet | Picture upright; touch and pen follow |
| 29 | Tablet with a resolution Tandem has not seen | Scaled picture, restart hint; exact fit after one restart |
| 30 | Phone (20:9) instead of a tablet | Works, letterboxed or exact after restart |
| 31 | Android 8 device | Works (no low-latency extras) |
| 32 | Video quality Sharp / Balanced / Fastest | Stream restarts at the new size |
| 33 | Battery saver: on battery vs plugged in | 60 Hz on battery (default), up to 120 Hz plugged in |
| 34 | Touch and pen switches off | Tablet input ignored |
