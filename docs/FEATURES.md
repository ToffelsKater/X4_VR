# Feature tracking: Windows mod and Linux port

One row per feature. **How** each works is in [HOW_IT_WORKS.md](HOW_IT_WORKS.md) (section in
brackets); measurements and history are in [LINUX_FINDINGS.md](LINUX_FINDINGS.md). Update a row
when a feature changes or is tested.

Linux status:
- **Works**: confirmed in the headset (Steam Frame, SteamVR, X4 9.00).
- **Untested**: built and unit-tested, not yet confirmed in the headset.
- **Missing**: Windows has it, Linux doesn't yet.
- **Not needed**: Windows has it, Linux doesn't need it (reason given).
- **Linux only**: not in the Windows mod.
- **Planned**: agreed as a future improvement, not built yet.

Last updated: 2026-10-05.

## Rendering and VR

| Feature | Windows | Linux | Notes |
|---|---|---|---|
| Alternate-eye stereo through X4's head tracking [1] | Yes | Works | |
| Vulkan layer: copy each eye, submit to SteamVR with its pose [4] | Yes | Works | Linux shares X4's queue (RADV has one). |
| Frame half: which eye a frame is [5] | Yes | Works | `half_xor_present` 1 on Windows, 0 on Linux. |
| Eye at use (offset added at the tracker read) [7] | Yes | Works | |
| Async submission thread, pacing, late-frame fallback [4] | Yes | Works | |
| Shared pose per eye pair [7] | No | Linux only, works | Fixes the Steam Frame's right-eye ghosting: its link reprojects both eyes with the left eye's pose. |
| Image size from SteamVR's recommended resolution [4, 12] | No | Linux only, works | Also sets X4's resolution to match. |
| Wait for the headset at start [4] | No | Linux only, works | VR_Init failing with "headset not found / not connected yet" (108, 126, 215) is retried each second, up to `X4VR_HEADSET_WAIT` s (120). |
| OpenXR runtime [3] | Yes | Missing | Only matters without SteamVR (Monado, WiVRn). |
| Turn compensation (mouse turns) [4, 9] | Yes | Not needed | No double vision with the shared pose; Windows' method can't work on the Frame. |

## Head tracking and X4 patches

| Feature | Windows | Linux | Notes |
|---|---|---|---|
| Head pose feed [6] | FreeTrack DLL (`FTGetData`) | Works | OpenTrack UDP, sequence number in roll. |
| Head smoothing off (strength 1) [6] | Yes | Works | |
| Backward clamp patch (lean back) [8] | Yes | Works | |
| Rival tracker patch (TrackIR, Tobii) [8] | Yes | Not needed | The Linux build only has OpenTrack. Don't run the opentrack app alongside. |
| Code found by byte pattern (survives X4 updates) [8] | Yes, 8.00 and 9.00 | Works | Patterns and RTTI instead of fixed addresses: all 6 sites found on 9.00 (51 ms at startup). Check a new build with `x4vr patterns`. |
| Recentre: hotkey, settings counter, first pose [11] | Yes | Works | |
| Recentre from SteamVR's own menu [11] | No | Linux only, works | |

## On foot

| Feature | Windows | Linux | Notes |
|---|---|---|---|
| On-foot zeroing patch [8, 9] | Yes | Works | |
| Camera::GetOffset patch [8, 9] | Yes | Works | Linux `0x1929f70`, the same check as Windows'. |
| Walking detection (stays in stereo) [9] | Yes | Works | |
| On-foot timing (prediction, `delay_walk`) [9] | Yes | Works | |
| On-foot eye flip [9] | Yes | Works | Without it the eyes were swapped on foot. |
| Snap turning | No | Not needed | Turning the mouse more gently is enough. |

## Menus, screen, input

| Feature | Windows | Linux | Notes |
|---|---|---|---|
| Theater screen for menus and views without ship controls [10] | SteamVR overlay | Works | Drawn into the eye images: overlays don't show on the Frame. |
| Theater toggle (Ctrl+F11) [10, 11] | Yes | Works | Also `x4vr ctl flat`. |
| Mouse cursor in VR [11] | SteamVR overlays | Works | Read over XCB, copied into the image. |
| Own window class in VR (`X4VR`) | No | Linux only, works | `SDL_APP_ID` set by `x4vr-run`: window manager rules for VR only. Hyprland Lua rule confirmed; Sway, i3 and `hyprland.conf` rules untested. Menu: Setup > Tiling window manager rules. |
| SteamVR "Exit game" closes X4 [11] | No | Linux only, works | |

## HUD and X4 settings

| Feature | Windows | Linux | Notes |
|---|---|---|---|
| HUD distance extension [12] | Launcher box | Works | Menu setting (HUD Scaled, HUD Scale Ratio), applied at the next VR launch, confirmed; also `x4vr hud <factor>`. |
| HUD keeps its size (`.xpl` scripts) [12] | Not needed (`.lua` only) | Linux only, works | Linux X4 loads precompiled `.xpl`; the patched text goes there too. Protected UI Mode off. |
| HUD rebuilt after game updates [12] | Yes, on Play | Works | `x4vr-run` runs `x4vr hud --refresh`. |
| X4 settings check and fix [12] | Launcher buttons | Works | `x4vr check` / `fix-settings`, run by `x4vr-run`. Windowed mode on Linux. |
| 2D and VR X4 settings kept apart [12] | No | Linux only, works | `config.xml.x4vr-2d` / `-vr` swapped at VR launch and exit (also after a crash: restored at the next start); HUD extension on only in VR. |
| `pos_scale` / gain calibration | Calibrated | Not needed | Uses Windows' values; the cockpit looks slightly large but fine in use. |

## Tools and convenience

| Feature | Windows | Linux | Notes |
|---|---|---|---|
| Launcher window (status, settings, play) [2, 11b] | Win32 window | Works | `x4vr` terminal menu: status with live frame rate, notices, launch, recentre/flat while running, settings tagged live / next launch. |
| Settings profiles [11b] | No | Linux only, works | Built-in "Steam Frame" plus the player's own (Save as profile, switch, delete). |
| X4 resolution choice [12] | Launcher (`x4_width`/`x4_height`) | Works | Menu: automatic, a 16:9 size or custom; same keys as Windows. Custom 2640x1588 (5:3) confirmed on the Steam Frame. |
| Automatic resolution in the headset's own shape [12] | No | Planned | Automatic rounds up to a 16:9 size; it could compute the size matching the eyes' view from SteamVR (Steam Frame: about 2640x1588, ~10% fewer pixels than 2880x1620), wide enough that the off-centre eye views leave no strip at the outer edge. Then the Steam Frame profile stays on automatic and follows SteamVR's resolution slider. |
| HUD size from X4's own UI scale [12] | No | Planned | Observed: X4 reported "modified" only because of the HUD's scale changes (the UI script edits), not for moving it. X4's own UI scale (also enlarges menu fonts) says it doesn't apply to the HUD above 1. Plan: (1) test that a move-only extension (anchor positions) keeps X4 unmodified; (2) find where X4 keeps the HUD out of its UI scale and lift that with a byte patch, like the other X4 patches, so X4's UI scale sizes the HUD. Then no UI script edits, no "modified" flag, Protected UI Mode can stay on. Open: the scale's `config.xml` name and range; whether its maximum covers factors like 2.5-3.5. |
| Curved flat screen [10] | No | Planned | The flat screen for menus and the map as a cylinder around the head, so every part faces the player straight on: the corners as easy to read as the centre, instead of seen at an angle. Linux draws the screen into the eye images (`screen_rects`), so the curve goes there; radius = the screen distance. |
| In-game settings checklist [12] | README tables | Linux only, works | Menu screen by X4 settings page, marked from `config.xml`. |
| Notices (known issues, tips) [11b] | No | Linux only, works | `share/x4vr/notices.txt`. |
| One-step start [2, 11b] | Launcher "Play" | Works | Menu "Launch X4 in VR": SteamVR if needed, then `steam -applaunch`. Steam's own Play starts the normal game. |
| Bug report button (zip logs, GitHub issue) [2] | Yes | Works | Menu / `x4vr report`: `~/x4vr-report-<time>.tar.gz` and the issue link. |
| Crash recorder (minidumps, debug log) [2] | `crash_watch` | Missing | Exit status logged; system crash dumps via `coredumpctl`. |
| Install / uninstall | `install.ps1` / `uninstall.ps1` | Works | CMake install (any distro) or Nix; menu "Copy the launch option" and "Add to the app launcher" (rofi) confirmed; menu / `x4vr uninstall`. Steam's launch option is set and cleared by the user. |
| Diagnostics (traces, dumps, pair stats) [13] | Full set | Partial | Linux: trace, submit trace, eye dump, pair stats, head watch (`X4VR_WATCH_HEAD`). No probe images or camera trace. |
| Windows build check after Linux changes | — | Missing | Shared files changed by hand only; needs a Windows or CI build. |
