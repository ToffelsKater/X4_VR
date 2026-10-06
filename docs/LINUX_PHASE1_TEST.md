# Linux port: first headset test (Phase 1, stage A)

> Historical: the first test of 2026-10-04. To install and play, see [LINUX_GUIDE.md](LINUX_GUIDE.md).

What this build does: X4 in the Steam Frame through SteamVR, head tracking through X4's OpenTrack
input, alternate-eye stereo, menus on a virtual screen. Known limits of stage A (plan section 7):

- **Eye swaps:** the eye is chosen when the pose is sent, not when X4 builds the camera, so some
  frames show the other eye's offset (on Windows this was 2 % at a steady 90 fps, more when the
  frame rate wobbles). Fixed by stage B (eye at use).
- **Looking sideways:** X4 zeroes backward head position, which pins the rear eye when you turn
  your head. Fixed by stage D (backward clamp patch).
- **Scale and angles are not calibrated yet:** the Windows values are used. Turning your head may
  turn the view more or less than your head; leaning may move too far or too little.
- OpenVR only (no OpenXR yet).

Everything below is safe to stop at any point: removing the launch option gives a normal X4.

---

## 1. Update and build

```bash
cd ~/code/X4_VR_Linux
git pull
nix-build
```

It ends with `100% tests passed out of 8` and a `/nix/store/...-x4vr-0.2.0-phase1` path.

## 2. Recentre and flat screen

As on Windows, while X4 has focus:

- **Ctrl+F12**: puts "forward" where you're looking now, and the virtual screen in front of you.
  SteamVR's own recentre (the Frame's "recenter view", or the SteamVR dashboard) does the same.
- **Ctrl+F11**: switches the flat virtual screen on (everything shown flat, like a big monitor)
  and back to automatic (stereo in the cockpit, screen for menus).

X4 still gets the keys (the mod only watches them). The same from a terminal or a desktop key
binding: `x4vr ctl recenter`, `x4vr ctl flat`. `X4VR_HOTKEYS=0` in front of the launch option turns
the keys off.

## 3. X4 settings (automatic)

`x4vr-run` sets what VR needs in X4's `config.xml` before every start (X4 isn't running yet at that
point, so it can't overwrite them): FOV 120°, anti-aliasing not temporal, upscaling and frame
generation off, VSync off, frame-rate limit off, OpenTrack Support on, chromatic aberration off,
and the resolution: `x4vr-run` asks SteamVR (already running) for the size it uses (its render
resolution setting, per app too) and sets X4 to the smallest common 16:9 mode at least that large
(2880x1620 for the Frame at SteamVR's default), in Windowed mode: Linux X4 renders fullscreen and
borderless at the desktop size whatever its resolution setting says. Without SteamVR it uses the
size the mod saved last time (`~/.local/state/x4vr/x4_resolution.txt`). `X4VR_RESOLUTION=WxH` in
front of the launch option picks one, `X4VR_RESOLUTION=0` leaves resolution and display mode alone.
Tiling window managers (Hyprland, Sway) resize the window to their tile, and X4 then renders that
size: make X4's window (class `X4`) floating with a window rule. `X4VR_RESOLUTION=WxH` in front of the launch
option picks one, `X4VR_RESOLUTION=0` leaves X4's resolution alone.
Everything else stays as it is; the first change keeps the original as `config.xml.x4vr-backup`
next to it. What it changed is listed at the top of `x4vr.log`.

To see where you stand first (X4 closed or running, both fine):

```bash
~/code/X4_VR_Linux/result/bin/x4vr check
```

`FIX` lines get changed at the next `x4vr-run` start (or now with `x4vr fix-settings`, X4 closed);
`tip` lines are recommended only. A line saying "not in this config.xml" means Linux X4 stores that
setting under another name: set it in the game's options instead. To leave `config.xml` alone,
add `X4VR_FIX_SETTINGS=0` in front of the launch option's command.

Your borderless window at 4K is fine on Linux (the Windows mod's "fullscreen" rule is for NVIDIA DSR).

## 3b. HUD distance (optional)

Out of the box X4 puts the cockpit HUD about a hand's width from your face. With X4 closed:

```bash
~/code/X4_VR_Linux/result/bin/x4vr hud 2.5
```

moves it 2.5 times further away at the same apparent size (any factor from 1 to 6;
`x4vr hud remove` undoes it, `x4vr hud status` shows it). This writes the extension
`extensions/x4vr_hud` in X4's folder, built from your own game files, as the Windows launcher does;
`x4vr-run` rebuilds it after a game update. Small cockpit pop-ups (the interaction menu) stay at
X4's distance.

**X4 then counts as modified**, like with any third-party extension: it says "Modified game
detected", turns off its online features (Ventures, online leaderboards), and **every savegame
saved while the mod is on is flagged permanently**, even if later loaded without it. Use a
separate save for VR, or `x4vr hud remove` (X4 closed) before playing saves you want to keep
online.

X4 also asks about **Protected UI Mode**: turn it **off** (Extension Settings), else X4 doesn't
load the extension's HUD scripts and the HUD moves back but looks smaller. Linux X4 9.00 loads
precompiled `.xpl` copies of the HUD scripts, so the extension puts the patched script text at
those paths too, so the HUD keeps its size at any factor.

## 4. Launch option

X4 → Properties → General → Launch Options:

```
/home/cully/code/X4_VR_Linux/result/bin/x4vr-run %command%
```

(This replaces the probe wrapper.) It adds `-skipintro -nocputhrottle` to X4's arguments.

## 5. Run

1. Start SteamVR with the Frame, as for any VR game.
2. Start X4 from Steam.
3. Put the headset on. The main menu should appear on a virtual screen about 2 m in front of you
   (theater mode). Press Ctrl+F12 (or `x4vr ctl recenter`) if it's behind you or off to the side.
4. Load a game and sit in the pilot seat. The view should switch to stereo (the cockpit around
   you), following your head.
5. Open the map or another fullscreen menu: it should go back to the virtual screen, then return to
   the cockpit when closed.

Play for a few minutes. Things to notice and note down:

- Does the cockpit look 3D (depth), and the right size?
- Turning your head: does the view turn with it, too much, or too little?
- Leaning forward/back and sideways: does the view follow?
- Flicker, doubled images, or "swimming" (the world moving when you turn your head).
- Smoothness: how it feels compared with Elite Dangerous.

## 6. Send back

Quit X4, then:

```bash
~/code/X4_VR_Linux/result/bin/x4vr check
cd ~/.local/state/x4vr
cat x4vr.log
tail -5 pair_stats.txt
grep -E 'ERROR|error' stderr.log | head -20
```

plus your notes. The settings for this test are in `~/.local/state/x4vr/stereo.txt`; you can edit
it while X4 runs (it's re-read every half second), but leave it as it is for this first run.

## If something goes wrong

- **X4 runs flat on the monitor, nothing in the headset:** `x4vr.log` says why (no SteamVR, wrong
  GPU, ...). Check SteamVR was running before X4 started.
- **X4 doesn't start or crashes:** remove the launch option, start X4 normally, and send
  `x4vr.log`, `x4vr.previous.log` and `stderr.log`.
- **Black screen in the headset:** press Ctrl+F11 / `x4vr ctl flat`: the virtual screen should
  appear. Send the logs.
- **The view jumps or spins:** take the headset off, `x4vr ctl flat`, and send the logs and what you
  saw. The sign of an axis may be wrong; it can be flipped without rebuilding.
