# Feature comparison: Windows and Linux

Every Windows feature and whether Linux has it, plus Linux's own extras. Linux tested with a
Steam Frame, SteamVR and X4 9.00.

**Key:** ✅ has it · ❌ doesn't have it yet · ➖ not needed there · 🟠 partly. The note says where
Linux does it differently.

## VR and rendering

| Feature | Windows | Linux | Note |
|---|:---:|:---:|---|
| Alternate-eye stereo | ✅ | ✅ | |
| Eye images submitted with their pose, async submission | ✅ | ✅ | |
| Eye chosen at the tracker read (eye at use) | ✅ | ✅ | |
| Shared pose per eye pair | ➖ | ✅ | Fixes the Steam Frame's right-eye ghosting. |
| Eye images at SteamVR's recommended size | ➖ | ✅ | |
| Waits for the headset at start | ➖ | ✅ | Up to 2 minutes. |
| OpenXR | ✅ | ❌ | Only needed without SteamVR. |
| Turn compensation | ✅ | ➖ | The shared pose avoids the double image. |

## Head tracking and X4 patches

| Feature | Windows | Linux | Note |
|---|:---:|:---:|---|
| Head tracking | ✅ | ✅ | Windows: FreeTrack DLL. Linux: OpenTrack over UDP. |
| Smoothing off, backward clamp patch | ✅ | ✅ | |
| X4 code found by byte pattern | ✅ | ✅ | `x4vr patterns` checks a new X4 build. |
| TrackIR / Tobii patch | ✅ | ➖ | Linux X4 only has OpenTrack. |
| Recenter | ✅ | ✅ | Linux: also SteamVR's recenter. |
| Stereo and head tracking on foot | ✅ | ✅ | Same patches as Windows. |

## Menus, screen and input

| Feature | Windows | Linux | Note |
|---|:---:|:---:|---|
| Flat screen for menus, Ctrl+F11 | ✅ | ✅ | Windows: SteamVR overlay. Linux: drawn into the eye images (an overlay shimmers on the Frame). |
| Mouse cursor | ✅ | ✅ | Windows: SteamVR overlay. Linux: read from X4's X11 window and drawn into the eye images. |
| SteamVR's *Exit game* closes X4 | ➖ | ✅ | |
| Own window class in VR (`X4VR`) | ➖ | ✅ | For tiling window manager rules. |

## HUD and X4 settings

| Feature | Windows | Linux | Note |
|---|:---:|:---:|---|
| HUD distance | ✅ | ✅ | Linux also patches X4's precompiled `.xpl` UI scripts. |
| X4 settings checked and fixed | ✅ | ✅ | Linux: windowed mode. |
| 2D and VR settings kept apart | ➖ | ✅ | HUD extension in VR only; restored after a crash too. |
| In-game settings checklist | 🟠 | ✅ | Windows: in the README. Linux: a menu screen. |

## Launcher and tools

| Feature | Windows | Linux | Note |
|---|:---:|:---:|---|
| Launcher | ✅ | ✅ | Windows: a window. Linux: terminal menu, `x4vr`. |
| Profiles | ✅ | ✅ | Built-in *Steam Frame*. |
| X4 resolution, custom size | ✅ | ✅ | |
| Notices (known issues, tips) | ➖ | ✅ | |
| Bug report | ✅ | ✅ | |
| Install and uninstall | ✅ | ✅ | |
| Crash recorder | ✅ | ❌ | Linux logs the exit status. |
| Diagnostics | ✅ | 🟠 | Linux: traces, eye dump, pair stats. |
