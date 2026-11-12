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
| 10 | Uninstall from Settings > Apps | Removed; driver removal offered only if Tandem installed it. Nothing is left: no files in `%LOCALAPPDATA%\Programs\Tandem`, `%APPDATA%\Tandem` or `%LOCALAPPDATA%\Tandem`, no `HKCU\Software\Tandem` key, Run entry, Apps entry or shortcut; the tablet's screen is off |

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
| 26 | Main screen and tablet at different scaling (e.g. 125% / 200%) | Windows moved back keep a sensible size; back on the tablet they have their old size |
| 27 | Desktop PC (no built-in screen) + monitor + tablet, unplug | Monitor stays on; windows moved back (the empty extra screen stays until replug) |

## Tablets and settings

| # | Scenario | Expected |
|---|---|---|
| 28 | Orientation Landscape / Portrait / Auto, turning the tablet | Picture upright; touch and pen follow |
| 29 | Tablet with a resolution Tandem has not seen | Scaled picture, restart hint; exact fit after one restart |
| 30 | Phone (20:9) instead of a tablet | Works, letterboxed or exact after restart |
| 31 | Android 8 device | Works (no low-latency extras) |
| 32 | Video quality Sharp / Balanced / Fastest | Stream restarts at the new size |
| 33 | Battery saver: on battery vs plugged in | Off by default (up to 120 Hz); "When on battery": 60 Hz on battery, up to 120 Hz plugged in |
| 34 | Touch and pen switches off | Tablet input ignored |

## Display driver

| # | Scenario | Expected |
|---|---|---|
| 35 | Settings window, everything working | "Display driver: running, tablet screen on" (or "off until a tablet connects") |
| 36 | Tablet screen off with a monitor on (e.g. Tandem restarted after an unplug, or a monitor plugged in meanwhile), connect the tablet | The tablet screen turns on by itself in its old place; no mirroring of the monitor; other screens keep their resolution |
| 37 | Driver not installed | Line in red, **Install display driver** button; the tablet mirrors meanwhile |
| 38 | Driver turned off in Device Manager | Line in red, **Open Device Manager**; status says to turn it on and restart |
| 39 | Driver turned on while Tandem runs | Asks for a restart; does not capture the new screen until then |

## Stream switch and tablet app

| # | Scenario | Expected |
|---|---|---|
| 40 | **Stream to tablet** off while streaming (settings window, then again from the tray menu) | Stream stops; windows move to the main screen; the tablet's screen is off; within 2 s the tablet shows "Paused on the PC"; status and tray tooltip say Paused; **Screen mode** is greyed out |
| 41 | **Stream to tablet** on again | The tablet's screen, the stream and the windows come back within a few seconds |
| 42 | Paused: restart Tandem, restart the PC, unplug and replug | Still paused each time; the tablet shows "Paused on the PC" once its app connects |
| 43 | Paused: leave the tablet alone | It goes to sleep like any idle tablet; switching the stream on wakes it (unlock it if it asks) |
| 44 | **Restart tablet app** while streaming, while paused, and with no tablet connected | App closes and reopens; streaming (or "Paused") is back within 2 s and the tablet's screen is not turned off in between; without a tablet the button is greyed out |
| 45 | Tablet app updated on the tablet to a newer version than the PC has | Tandem keeps using it and logs that the PC needs the update (it does not try to install the older app) |

## Closing Tandem

| # | Scenario | Expected |
|---|---|---|
| 46 | Quit from the tray menu while streaming | Windows move to the main screen and the tablet's screen turns off; the next start brings it back when the tablet connects |
| 47 | **Restart app**, and installing an update | The tablet's screen and the windows on it stay; streaming again within a few seconds |
| 48 | Turn the tablet's screen on yourself (Win+P Extend) before starting Tandem, then quit Tandem | Log says the screen was on before; it is still on after Tandem quits |
| 49 | Shut down or sign out of Windows while streaming | The tablet's screen is off at the next sign-in until the tablet connects (also when Tandem does not start with Windows) |
| 50 | Laptop + monitor + tablet: unplug the tablet, then the monitor, then plug the tablet back in | The tablet's screen comes back as a second screen (not a mirror of the laptop) |

## Updates and feedback

| # | Scenario | Expected |
|---|---|---|
| 51 | Leave Tandem running with a newer release published | About 30 s after start (and once a day) a box offers **Install now**, **Later**, **Skip this version** |
| 52 | **Later** | No box again that day for that version; it returns the next day |
| 53 | **Skip this version** | No box for that version again; **Check for updates** still offers it |
| 54 | First start right after installing; no internet; latest version installed | No box in any of these (errors only in the log) |
| 55 | **Send feedback** > **Open the report page** | Browser opens a new GitHub issue with versions, GPU/encoder, screens, driver state, tablet and the last log lines; the link is under 7,000 characters; no Windows user name and no tablet serial number in it |
| 56 | **Send feedback** > **Open log folder** | Explorer opens `%APPDATA%\Tandem` |
| 57 | Tablet waiting or paused screen > **Report a problem** | Explains, then opens a GitHub issue with the app version, device, Android version, screen and decoder |

## Test build

Some cases cannot be staged by hand on one PC. A test-only build has switches
for them; it is never released (`dist\Tandem.exe` is built without them).

```
set CL=/DTANDEM_TEST
host\build.bat
```

- `%TEMP%\tandem-test.txt` holding one word changes what Tandem sees, also
  while it runs: `missing`, `disabled`, `failed`, `noscreen` (cases 37, 38) and
  `late` (case 39) for the display driver; `othermonitor` makes it act as if
  another monitor were attached (cases 23, 27, 36, 50). Delete the file to end.
- `TANDEM_TEST_DPI=96..240`: lays the settings window out as at that scaling.
- `TANDEM_TEST_UPDATE_URL`: where to ask for the latest release (a local server
  with a made-up newer release for cases 5 and 51-53, an address that does not
  exist for cases 6 and 54). `TANDEM_TEST_UPDATE_SECONDS`: seconds until the
  first automatic check and between checks.
- `TANDEM_TEST_APP_VERSION=14`: Tandem acts as if it carried an older tablet app
  than the one on the tablet (case 45).
- `TANDEM_TEST_FEEDBACK_FILE`: the report link is written to this file instead
  of opening the browser (case 55).
- Tablet app, debug build only (`gradle assembleDebug`):
  `adb shell am start -n com.freedisplay.client/.MainActivity --ei rejectFormats 1`
  makes every decoder reject the low-latency settings, `2` everything (case 19).
- Cases 30 and 31 can run in the Android emulator: start the app there after
  `adb -s emulator-5554 reverse tcp:27184 tcp:27184` (wake the emulator first).
