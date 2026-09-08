# ctrlmouse

Use a game controller as your mouse on Windows. Native C++, a single ~250 KB
exe, no dependencies, no drivers, no installer.

Works with DualSense / DualShock and other DirectInput controllers as well as
XInput pads.

## Controls

Every action below is rebindable: open **Button layout** in the settings
window and press the button you want to change - the page names it, and
everything on it then applies to that button. More than one action can share
a button when one is a tap and the other a hold. The D-pad is bindable too:
its four directions are actions like any other, and just start out on volume
and seek.

A button can also send a keyboard shortcut instead of, or as well as, an
action - a single key like `F`, or a combination like `Ctrl+Shift+Tab`.

The mapping pauses while that page is open, so pressing a button to bind it
cannot also fire whatever is already on it.

| Controller | Action |
|---|---|
| Left stick | Move the mouse cursor |
| Right stick (up/down) | Scroll wheel (stick up scrolls down) |
| Cross / A | Left click (hold to drag) |
| Circle / B | Right click |
| Square / X | Play / pause — **hold** for the fullscreen flyout |
| R1 / L1 | Forward / back, as the mouse side buttons |
| D-pad up / down | Volume up / down (hold to keep changing, speeds up) |
| D-pad left / right | Seek back / forward — hold to fast-forward or rewind |
| Triangle / Y | Open / close the on-screen keyboard |
| Triangle *(hold)* | Open the keyboard with an app search bar |
| Options | Open / close the app launcher |
| D-pad *(launcher open)* | Move between apps |
| Cross *(launcher open)* | Launch the highlighted app |
| Circle *(launcher open)* | Close the launcher |
| D-pad up *(launcher, top row)* | Reach the show-desktop and Settings icons |
| D-pad up *(launcher, app running)* | Ask whether to close that app |
| Cross *(close prompt)* | Close the app |
| D-pad down / Circle *(close prompt)* | Back out |
| D-pad *(keyboard open)* | Move between keys (hold to repeat) |
| Cross *(keyboard open)* | Type the highlighted key. Enter also closes the keyboard |
| Circle *(keyboard open)* | Backspace |
| Touchpad click *(customizable)* | Toggle the mapping on / off, with a flyout saying which |

Volume and play/pause use the system media keys, so they reach whichever app
owns playback even in the background. Seeking sends left/right arrow keys —
the way video players expect — so it applies to the focused window.

## Features

- **On-screen keyboard** — dark themed, animated, driven entirely by the
  controller. It never steals focus, so keys go to the app you're working in.
- **Search on hold** — holding the keyboard button opens a search. Set it to
  *Third party* and it presses a hotkey you choose, summoning a launcher you
  already use (PowerToys Command Palette, PowerToys Run, or anything else with
  a hotkey); the keyboard types into it and the shoulder buttons walk its
  results. Set it to *Built-in* for a simple list of your installed apps.
- **Game auto-pause** — detects fullscreen games (exclusive and borderless)
  and pauses the mapping so your sticks don't fight the game. Checked at most
  every 2 seconds with a couple of API calls; effectively zero cost. Can be
  toggled off, and the controller toggle button overrides it in-game.
- **Customizable toggle bind** — bind any controller button to enable/disable
  the mapping from the couch.
- **Run at login** — an optional toggle registers a `Run` entry that starts
  ctrlmouse minimised to the tray.
- **App launcher** — hold Options for a grid of apps with their real icons.
  Launching one that is already open switches to it instead of starting a
  second copy, and D-pad up on a running app offers to close it. A Settings
  tile opens Windows Settings.
- **System tray** — closing the window sends it to the tray; the mapping keeps
  running. Right-click the tray icon to restore or quit.
- **Settings window** — sensitivity, scroll speed, and deadzone sliders with
  live values. Settings persist in a `config.json` next to the exe.

## Download & run

1. Grab `ctrlmouse.exe` from the [latest release](../../releases/latest).
2. Double-click it. That's it — the settings window opens and the mapping is
   live. Close the window to send it to the tray.

> **Windows SmartScreen / antivirus note:** the exe is not code-signed, so
> SmartScreen may show "Windows protected your PC" on first run (click
> *More info → Run anyway*), and some antivirus products may flag it — an app
> that synthesizes mouse input looks suspicious to heuristics by nature. The
> full source is in this repo; if in doubt, build it yourself (below).

## Build from source

Requires the MSVC Build Tools (any recent Visual Studio / Build Tools install).

From a *x64 Native Tools Command Prompt for VS*:

```bat
compile.bat
```

Or with CMake:

```bat
cmake -B build
cmake --build build --config Release
```

Everything is one source file ([ctrlmouse.cpp](ctrlmouse.cpp)) plus an icon and
a version resource. Win32 + DirectInput + GDI only — no third-party libraries.

## Configuration

Settings are saved automatically to `config.json` in
`%APPDATA%\ctrlmouse`, alongside `apps.txt` (the launcher list, one path per
line). Settings written by older builds are moved there on first run.

| Key | Meaning | Default |
|---|---|---|
| `mouse_sensitivity` | Cursor speed at full stick deflection | `18` |
| `scroll_sensitivity` | Scroll speed at full deflection | `1.0` |
| `deadzone` | Stick travel ignored near centre (0–0.5) | `0.15` |
| `enabled` | Master on/off | `true` |
| `bind_lclick` / `bind_rclick` / `bind_keyboard` / `bind_playpause` / `bind_fullscreen` / `bind_launcher` / `bind_toggle` / `bind_forward` / `bind_back` / `bind_volume_up` / `bind_volume_down` / `bind_seek_fwd` / `bind_seek_back` | Controller button index per action, or -1 for none; set from Button layout. 0-13 are the pad's buttons, 16-19 the D-pad directions | `1, 2, 3, 0, 0, 9, 13, 5, 4, 16, 18, 17, 19` |
| `scN_btn` / `scN_mods` / `scN_vk` | Keyboard shortcut on a button: which button, its `MOD_*` bits, and its virtual-key code. Only slots in use are written | *(none)* |
| `game_pause` | Auto-pause in fullscreen games | `true` |
| `mouse_curve` | Cursor response curve; 1 = linear, higher = finer near centre (1-3) | `2.0` |
| `fullscreen_key` | Last shortcut picked in the fullscreen flyout: 0 = F11, 1 = Alt+Enter, 2 = F | `0` |
| `search_mode` | Search opened on hold: 0 = built-in list, 1 = press a hotkey for your own launcher | `1` if PowerToys is installed |
| `search_mods` / `search_vk` | The hotkey sent in third-party mode; set it from the settings window | `Alt+Space` |

## License

[MIT](LICENSE)
