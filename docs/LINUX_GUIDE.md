# X4 VR on Linux: install and play

Stereo VR for the native Linux build of X4: Foundations (9.00), through SteamVR. Tested with a
Steam Frame on an AMD GPU (Mesa RADV). Experimental, and not affiliated with Egosoft.

How it works: [HOW_IT_WORKS.md](HOW_IT_WORKS.md). Feature status: [FEATURES.md](FEATURES.md).

## What you need

- X4: Foundations, the native Linux version from Steam, started at least once. Steam from your
  distribution's package or Valve's installer: the Flatpak version isn't supported (its sandbox
  can't run the mod's launcher).
- SteamVR with your headset working.
- To build: a C++20 compiler (GCC 13+ or Clang 16+), CMake 3.24+, git, and the Vulkan headers
  (package `vulkan-headers` or `libvulkan-dev`, depending on the distribution).

## 1. Build and install

```bash
git clone https://github.com/Cully-Curwen/X4_VR_Linux ~/x4vr-src
git clone --depth 1 https://github.com/ValveSoftware/openvr ~/x4vr-src/external/openvr
cd ~/x4vr-src
cmake -S . -B build -DX4VR_LINUX=ON -DCMAKE_BUILD_TYPE=Release \
      -DOPENVR_SOURCE_DIR=$PWD/external/openvr -DCMAKE_INSTALL_PREFIX=$HOME/.local
cmake --build build -j
ctest --test-dir build          # all tests should pass
cmake --install build
```

This installs `x4vr` (the menu) and `x4vr-run` (the launcher Steam uses) in `~/.local/bin`, and
the mod and its defaults in `~/.local/share`. OpenVR's client library is built into the mod from
that source, so no OpenVR package is needed. Any install prefix works; the paths below follow it.

A Nix expression is also included (`nix-build` in the source directory).

## 2. First-time setup

Start the menu in a terminal:

```bash
~/.local/bin/x4vr
```

The menu is one screen in sections: **Status**, **Play**, **Notices** (known issues and tips),
**Settings**, **X4 settings for VR** and **Setup**. Arrow keys move, Enter selects, types a
value or opens a list of the options to pick from, Space ticks a box, ←→ change a value. The
selected item is explained at the bottom. Only the **Quit** item closes the menu (Esc and Ctrl+C
don't, so it can't be closed by accident while you play).

1. **Copy the launch option**, then paste it in Steam: X4 > Properties > General > Launch
   options. It looks like `/home/you/.local/bin/x4vr-run %command%`. The menu copies it to the
   clipboard (wl-copy, xclip or xsel if installed, else through the terminal).
   This doesn't change normal play: **Steam's Play button still starts the normal game.** Only a
   launch from the menu starts VR.
2. **Add to the app launcher** puts "X4 VR" in your desktop's app menu (and rofi, wofi, etc.), so
   the menu opens in a terminal from there.
3. Optional, under **Settings**: **HUD Scaled**. Out of the box X4's cockpit HUD sits a
   hand's width from your face. Turn it on and set the **HUD Scale Ratio** (2.5 is a good start: 2.5 times
   farther, same apparent size; Enter to type any value from 1.0 to 6.0). It is applied at the
   next VR launch.
   - X4 then counts as **modified** while the extension is on, and saves made then are flagged,
     like with any extension. The mod only turns it on during VR sessions, so 2D play and 2D
     saves aren't affected.
   - In X4's Extension Settings, turn **Protected UI Mode off**, else the HUD only moves back and
     looks smaller.

## 3. Play in VR

1. In the menu: **Launch X4 in VR**. It starts SteamVR if it isn't running (put the headset on),
   then X4 through Steam with the mod. If the headset hasn't connected yet (a wireless one takes
   a few seconds after SteamVR), X4 waits for it up to 2 minutes before showing its window.
2. The main menu appears on a flat screen in front of you. Look straight ahead and press
   **Ctrl+F12** to recentre (or use SteamVR's own recentre).
3. Load a game: in the pilot seat and on foot the view is 3D and follows your head. Menus and the
   map go to the flat screen.

While X4 runs, with its window focused:

| Key | |
|---|---|
| Ctrl+F12 | Recentre (also SteamVR's recentre, or the menu) |
| Ctrl+F11 | Flat screen on / automatic |

SteamVR's "Exit game" closes X4.

### 2D and VR settings

VR needs some X4 settings 2D play doesn't want (windowed at the headset's resolution, FOV 120°,
no temporal anti-aliasing, OpenTrack on, ...). The mod keeps two sets:

- At a VR launch it saves X4's `config.xml` as `config.xml.x4vr-2d` and puts the VR copy
  (`config.xml.x4vr-vr`) in its place, fixed for VR.
- When X4 exits, the VR settings go back to `config.xml.x4vr-vr` and your 2D settings return.
- If X4 crashed in VR, the next start of either kind restores the 2D settings first.

So change 2D settings while playing in 2D, and VR settings while playing in VR; each is kept.

**In-game settings checklist** (menu, X4 settings for VR) lists every X4 setting VR needs, by
where it is in X4's settings, with what X4's VR settings have now: ✓ right, ✗ wrong (fixed at
the next VR launch), · recommended, - check it by hand in X4, the mod can't read it (Controls >
OpenTrack: head rotation and position factors at 100 %; with HUD Scaled on, Extensions >
Protected UI Mode off). Head Motion Smoothing isn't listed: the mod turns it off itself.

### VR settings

The menu's **Settings** are saved in `~/.local/state/x4vr/stereo.txt`. Each is tagged with when
it applies:
- **live**, at once, also while X4 runs: 3D, shared pose (keep it on for the Steam Frame), world
  scale, head prediction, mouse cursor, flat screen mode, distance and width;
- **next launch**, at the next VR start: stutter protection, HUD Scaled / HUD Scale Ratio and X4's resolution in VR
  (automatic picks the smallest 16:9 size that covers what SteamVR renders; Custom adds Resolution
  Width and Height rows for any size; it needn't be 16:9).

Keep **Stutter protection** on: off is for troubleshooting only, and loses the flat screen for
menus and the Steam Frame's shared pose (right-eye ghosting).

**Profiles** hold a set of these settings. "Steam Frame" is built in; change settings and use
**Save as profile** to keep your own (in `~/.local/state/x4vr/profiles/`). ←→ on Profile loads
another; "· changed" means the settings differ from the profile's.

While X4 runs in VR, Status shows the headset's frame rate and late or repeated frames.

## 4. After an X4 update

The mod finds the X4 code it changes by its bytes, so small updates usually keep working. The
menu's **X4 build** line shows whether this X4 is supported (`x4vr patterns` lists each part).
If something isn't found, that feature stays off and X4 still runs. Please report it.

The HUD extension is rebuilt from the new game files at the next VR launch.

## 5. Problems

- **Nothing in the headset, X4 on the monitor:** SteamVR or the headset wasn't ready. The log
  says why: `~/.local/state/x4vr/x4vr.log`.
- **X4 closes right after starting in VR:** make a bug report (below).
- **Black screen in the headset:** Ctrl+F11 shows the flat screen.
- **Tiling window managers** (Hyprland, Sway, i3): X4 renders at its window's size, and a tiling
  window manager resizes it or makes it fullscreen at the monitor's size. In VR its window has its
  own class, `X4VR` (2D keeps `X4`), so a rule can float only the VR window. The menu's
  **Setup > Tiling window manager rules** shows the rule for each, with a copy button. Hyprland
  0.55+ (Lua config, confirmed working):

  ```lua
  hl.window_rule({ name = "x4-vr", match = { class = "^(X4VR)$" }, float = true,
                   suppress_event = "fullscreen maximize", fullscreen_state = "0 0" })
  ```

  Check with `hyprctl clients` while X4 runs in VR: class `X4VR`, `floating: 1`, `fullscreen: 0`.

**Bug reports:** the menu's **Make a bug report** packs logs, settings and a summary into
`~/x4vr-report-<time>.tar.gz`. Attach it to a new issue at
https://github.com/Cully-Curwen/X4_VR_Linux/issues.

## 6. Uninstall

1. In the menu: **Uninstall** (or `x4vr uninstall`). It removes, as ticked:
   - the desktop entry;
   - the HUD extension (X4 closed);
   - the mod's copies of X4's settings (your 2D settings stay);
   - the mod's settings, profiles and logs;
   - the installed program files (`x4vr`, `x4vr-run`, the mod and its data under `~/.local` or
     wherever `x4vr` runs from), ticked when any are found. A Nix install in `/nix/store` goes
     with the GitHub clone directory instead.

   "Restore X4's settings from before the mod" is off by default: it brings back the settings
   from before the first VR launch, undoing 2D changes made since.
2. In Steam: clear X4's launch option (X4 > Properties > General > Launch options). If you added
   a tiling window manager rule for `X4VR`, remove it from that config too.
3. Delete the GitHub clone directory; the uninstall screen shows its path (e.g. `rm -rf ~/x4vr-src`).

Saves made while the HUD extension was on stay flagged as modified; that is X4's own rule.

## Commands

The menu's actions are also commands (`x4vr help` lists all):

| Command | |
|---|---|
| `x4vr` | The menu |
| `x4vr launch` | Launch X4 in VR |
| `x4vr launch-option status` / `copy` | Check / copy the Steam launch option |
| `x4vr ctl recenter` / `flat` | Recentre / flat screen while X4 runs |
| `x4vr hud <factor>` / `remove` / `status` | HUD distance now (X4 closed) |
| `x4vr settings-mode status` | Whether X4 has its 2D or VR settings now |
| `x4vr patterns` | Whether this X4 build is supported |
| `x4vr report` | Bug report |
| `x4vr install-desktop` | App launcher entry |
| `x4vr uninstall` | Remove what the mod set up |

Environment variables for special cases (in front of the launch option's command):
`X4VR_ALWAYS=1` starts every Steam launch in VR; `X4VR_FIX_SETTINGS=0` leaves X4's settings alone;
`X4VR_RESOLUTION=WxH` sets X4's VR resolution (`0`: leave it); `X4VR_HEADSET_WAIT=<seconds>` how
long X4 waits for the headset at start (default 120, `0`: don't wait); `X4VR_HOTKEYS=0` turns the keys
off; `X4VR_PATCHES=0` turns the code patches off.
