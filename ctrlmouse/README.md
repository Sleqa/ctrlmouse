# ctrlmouse

Use a game controller as your mouse on Windows. Native C++, a single ~250 KB
exe, no dependencies, no drivers, no installer.

Works with DualSense / DualShock and other DirectInput controllers as well as
XInput pads.

## Controls

| Controller | Action |
|---|---|
| Left stick | Move the mouse cursor |
| Right stick (up/down) | Scroll wheel (stick up scrolls down) |
| Cross / A | Left click (hold to drag) |
| Circle / B | Right click |
| Square / X | Play / pause — **hold** for fullscreen |
| D-pad up / down | Volume up / down (hold to keep changing, speeds up) |
| D-pad left / right | Seek back / forward — hold to fast-forward or rewind |
| Triangle / Y | Open / close the on-screen keyboard |
| Options *(hold)* | Open the app launcher |
| D-pad *(launcher open)* | Move between apps |
| Cross *(launcher open)* | Launch the highlighted app |
| Circle *(launcher open)* | Close the launcher |
| D-pad up *(launcher, app running)* | Ask whether to close that app |
| Cross *(close prompt)* | Close the app |
| D-pad down / Circle *(close prompt)* | Back out |
| D-pad *(keyboard open)* | Move between keys (hold to repeat) |
| Cross *(keyboard open)* | Type the highlighted key |
| Circle *(keyboard open)* | Backspace |
| Touchpad click *(customizable)* | Toggle the mapping on / off |

Volume and play/pause use the system media keys, so they reach whichever app
owns playback even in the background. Seeking sends left/right arrow keys —
the way video players expect — so it applies to the focused window.

## Features

- **On-screen keyboard** — dark themed, animated, driven entirely by the
  controller. It never steals focus, so keys go to the app you're working in.
- **Game auto-pause** — detects fullscreen games (exclusive and borderless)
  and pauses the mapping so your sticks don't fight the game. Checked at most
  every 2 seconds with a couple of API calls; effectively zero cost. Can be
  toggled off, and the controller toggle button overrides it in-game.
- **Customizable toggle bind** — bind any controller button to enable/disable
  the mapping from the couch.
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
| `toggle_button` | Controller button index for the toggle bind | `13` (touchpad) |
| `game_pause` | Auto-pause in fullscreen games | `true` |
| `mouse_curve` | Cursor response curve; 1 = linear, higher = finer near centre (1-3) | `2.0` |
| `fullscreen_key` | Shortcut sent on hold: 0 = F11, 1 = Alt+Enter, 2 = F | `0` |

## License

[MIT](LICENSE)
