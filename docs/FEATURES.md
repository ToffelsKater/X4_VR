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
| SteamVR "Exit game" closes X4 [11] | No | Linux only, works | |

## HUD and X4 settings

| Feature | Windows | Linux | Notes |
|---|---|---|---|
| HUD distance extension [12] | Launcher box | Works | `x4vr hud <factor>`. |
| HUD keeps its size (`.xpl` scripts) [12] | Not needed (`.lua` only) | Linux only, works | Linux X4 loads precompiled `.xpl`; the patched text goes there too. Protected UI Mode off. |
| HUD rebuilt after game updates [12] | Yes, on Play | Works | `x4vr-run` runs `x4vr hud --refresh`. |
| X4 settings check and fix [12] | Launcher buttons | Works | `x4vr check` / `fix-settings`, run by `x4vr-run`. Windowed mode on Linux. |
| `pos_scale` / gain calibration | Calibrated | Not needed | Uses Windows' values; the cockpit looks slightly large but fine in use. |

## Tools and convenience

| Feature | Windows | Linux | Notes |
|---|---|---|---|
| Launcher window (profiles, world scale, status panel) [2] | Yes | Missing | `stereo.txt`, `x4vr ctl` and the hotkeys instead; same settings. |
| One-step start [2] | Launcher "Play" | Works | Steam launch option `x4vr-run %command%`. |
| Bug report button (zip logs, GitHub issue) [2] | Yes | Missing | Logs in `~/.local/state/x4vr`. |
| Crash recorder (minidumps, debug log) [2] | `crash_watch` | Missing | Exit status logged; system crash dumps via `coredumpctl`. |
| Install / uninstall scripts | `install.ps1` / `uninstall.ps1` | Missing | Build with Nix; remove the launch option and `x4vr hud remove`. |
| Diagnostics (traces, dumps, pair stats) [13] | Full set | Partial | Linux: trace, submit trace, eye dump, pair stats, head watch (`X4VR_WATCH_HEAD`). No probe images or camera trace. |
| Windows build check after Linux changes | — | Missing | Shared files changed by hand only; needs a Windows or CI build. |
