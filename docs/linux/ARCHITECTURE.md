# X4 VR architecture: Windows and Linux

For maintainers: how each part works on Windows, and how the Linux port (`-DX4VR_LINUX=ON`) does
it differently. Addresses are X4 9.00's. Feature by feature: `FEATURE_COMPARISON.md`. Windows
measurements: `../STUTTER_RESEARCH.md`.

## Overview

X4 has no VR. The mod renders alternate eyes: X4 draws one frame per eye in turn, with its camera
moved through X4's own head-tracking input (headset pose plus that eye's offset). A Vulkan layer
copies each finished frame into that eye's texture and submits it to SteamVR with the pose it was
rendered with. A few one-byte code patches remove limits in X4's head-tracking code. Menus and
views without ship controls go to a flat screen.

| Part | Windows | Linux |
|---|---|---|
| Head-tracking input | X4 calls `FTGetData` in our `FreeTrackClient64.dll` | We send OpenTrack UDP packets to X4 |
| Vulkan layer | `x4vr_observe.dll` | `libx4vr.so` (layer and runtime in one library) |
| Start | `X4VRLauncher.exe` and `crash_watch.exe` | `x4vr` terminal menu, `x4vr-run` as Steam launch option |
| VR runtime | OpenVR or OpenXR | OpenVR |
| Finding X4 code | Byte signatures in `X4.exe` | Byte patterns in X4's loaded code, RTTI for the tracker class |

## Code layout

- The top-level `CMakeLists.txt` hands over to `linux/CMakeLists.txt` with `X4VR_LINUX=ON`, before
  any Windows line. Without it the Windows build is unchanged.
- Windows files are never edited for Linux. Portable files are shared (`math.cpp`, `session.cpp`,
  `eye_targets.cpp`, `tools/launcher/launcher_settings.hpp`, `hud_mod.hpp`).
- `src/linux/observe_layer.cpp` and `src/linux/runtime_bootstrap.cpp` are copies of the files with the
  same names in `src/` with the Win32 calls replaced. Each names its original and commit in
  its first comment; carry Windows fixes over by diffing the original against that commit.
- Which files are shared, copied or Linux-only, function by function: `CODE_COMPARISON.md`.
- Linux-only code: `src/linux/` (OpenTrack client, code scan, cursor), `tools/linux/` (`x4vr`),
  `linux/` (CMake, `x4vr-run`, layer manifest, Nix package), `config/linux/`, `tests/linux/`, `docs/linux/`.
- `libx4vr.so` links libstdc++ and OpenVR statically and exports only the Vulkan entry points:
  X4 runs in Steam's runtime container, whose libraries may differ.

## Starting X4

**Windows:** the launcher points X4's FreeTrack registry path at the mod's DLL and starts X4
through `crash_watch.exe` with the Vulkan layer enabled by environment variables.

**Linux:** the user sets X4's Steam launch option to `x4vr-run %command%` once. The menu's
*Launch X4 in VR* starts SteamVR if needed, writes a request file and runs `steam -applaunch`.
`x4vr-run` then:

1. finds the game directory in Steam's command (`X4VR_GAME_DIR`);
2. without a fresh request runs Steam's command untouched (Steam's Play stays 2D), after putting
   2D settings back if a VR session crashed and moving the HUD extension aside (`hud --park`);
3. turns off Steam's VR streaming of the window and Steam's overlay (both made X4 quit);
4. enables the layer (`VK_ADD_LAYER_PATH`, `VK_INSTANCE_LAYERS`) and sets `SDL_APP_ID=X4VR`, so the
   VR window has its own class for window manager rules;
5. swaps in X4's VR settings and, only if that worked, fixes them; puts the HUD extension back,
   applies the menu's factor and rebuilds it after a game update (`hud --refresh`);
6. runs X4 with `-skipintro -nocputhrottle`, then swaps the 2D settings back and moves the HUD
   extension aside again.

The mod waits up to 2 minutes for the headset when SteamVR is up before it (Steam Frame).

## Head-tracking feed

**Windows (`freetrack_client.cpp`):** X4 calls `FTGetData` once per frame. On the first call it
applies the patches and sets X4's head smoothing to 1. Each call predicts the headset pose, picks
the eye, and converts to FreeTrack units (angle gains 180/85, position × `pos_scale`).

**Linux (`src/linux/opentrack_client.cpp`):** Linux X4 reads OpenTrack UDP (6 doubles: x, y, z in cm,
yaw, pitch, roll in degrees) on port 4242. A thread sends one packet per present with the same
pose maths, its own axis signs, and a packet sequence number in the low bits of the roll. X4's
game state (menu open, controlling a ship, walking) is read on X4's main thread through its
exported functions. SteamVR's IPD is re-read every second (Windows: once).

The gains undo X4's scaling of tracker angles. Windows sends FreeTrack radians (yaw and pitch
180/85, roll π); Linux sends degrees, so each gain is a plain factor: yaw and pitch 2.1177, roll
2.5. Roll was measured on the Steam Frame (stars still with the head tilted 30°): X4's tracker
scales all three angles alike, so its camera likely gives roll about 72° of range where yaw and
pitch get 85°. `pos_scale` 3.6 is Windows' and was checked by leaning (the cockpit stays put).

## Which eye: frame half, eye at use, shared pose

- **Frame half:** X4 keeps a per-frame global that alternates 0/1 from pose to present. The layer
  uses it to know which eye a presented frame is (Windows `half_xor_present=1`, Linux 0).
- **Eye at use:** the feed sends the head centre, and a hook on the tracker's position accessor
  adds the offset of the eye X4 is building at that moment. Windows hooks the FreeTrack tracker
  (slot `0x108`, plus a "still" check); Linux hooks `VR::OpenTrack` slot 34, found through RTTI.
  Linux identifies the packet X4 used by its sequence number.
- **Shared pose (Linux only):** the Steam Frame reprojects both eyes with the left eye's pose, so
  the right eye ghosted. The sender skips the packet before a right-eye frame, and the layer submits
  pairs with matching poses (`shared_pose=1`).

## Vulkan layer

Common: copy each frame into a ring of 3 textures per eye, and submit from a dedicated thread
every compositor frame. A late game frame repeats the previous image instead of flashing.
X4's render thread is paced to the compositor (`async_submit=1`, the default; `0` submits inline
and skips the flat screen and, on Linux, the shared pose).

Linux differences:
- **The VR queue:** like Windows, the layer asks the driver for a spare graphics queue of its own
  (NVIDIA's has several). AMD's RADV has one, and Intel's likely too, so there the layer shares
  X4's queue behind one recursive lock, around every `vkQueue*` call and the runtime's submits;
  waits for the GPU and X4's game-state queries stay outside it. Only the shared path is tested
  (AMD); the log and the menu's GPU line show which one ran.
- Eye images use SteamVR's recommended pixel density; the ideal X4 resolution is saved for the
  menu (`x4_resolution.txt`).
- The flat screen and the mouse cursor are drawn into the eye images, not shown as SteamVR
  overlays (see "Flat screen, cursor, hotkeys").
- SteamVR's recenter and *Exit game* events are handled.
- `async_submit` is read once at the first frame (switching it live froze the headset).

## X4 code patches

Each is one byte, checked against a pattern that must match exactly once. Linux scans X4's loaded
code at start (`scan_x4`); `x4vr patterns` runs the same scan on a file. The patterns also pin the
struct offsets the mod reads, as Windows' per-version signatures do: the tracker fields (`+0xd0`
position, `+0xa0` roll, `+0xa8` new-packet flag, `+0x114` position scale) through the slot 34 and
slot 2 (update) patterns, and the camera fields (`+0x18`, player `+0x3e8`, mode `+0x880`) through
the camera-offset and walking-check patterns. A moved field turns its feature off instead of
reading the wrong memory. The on-foot patches go in together or not at all.

| Patch | Purpose | Windows 9.00 | Linux 9.00 |
|---|---|---|---|
| Backward clamp | X4 zeroed backward head position | `jae` → `jmp` at `0x9fdb4d` | `jbe` → `jmp` at `0xfeb73f` |
| On-foot zeroing | X4 gave the camera a zero pose without a ship | `je` → `jmp` at `0x9fd9ae` | `je` → `jno` at `0xfec078` |
| On-foot camera offset | `Camera::GetOffset` skipped the head offset on foot | `0x97a413` | `0x1929fff` (exit `je` → offset block) |
| Rival trackers | TrackIR/Tobii replaced the headset pose | `0xfa34fc`, `0xfa353c` | Not needed (OpenTrack only) |

## On foot

Both on-foot patches, plus walking detection (the rendered camera has no movement controller and
camera mode 0). Walking stays in stereo. On foot X4 uses the pose one frame sooner, so the pose is
predicted 1/90 s further, the eye mapping flips, and the reprojection uses `delay_walk=1`.
Windows also compensates mouse turns (`turn_comp`); Linux doesn't need it with the shared pose.

## Flat screen, cursor, hotkeys

- Flat when `theater=2`, or `theater=1` and a fullscreen menu is open or the player is neither
  walking nor flying. The screen is 2.2 m wide, 2 m ahead of the recentred origin.
- Windows shows the screen and the cursor as SteamVR overlays. Linux draws both into the eye
  images:
  - **Screen:** the flat image is blitted into black eye textures at the screen's place, with the
    head pose of when it was placed, so SteamVR's reprojection keeps it fixed in space.
  - **Cursor:** X4's cursor is the X server's (Xwayland), not in its swapchain. A thread reads it
    over XCB (`x11_cursor.cpp`): X4's window by `WM_CLASS` (`X4`, or `X4VR` in VR) or
    `_NET_WM_PID`, the pointer position, the image through XFixes. Its opaque pixels (alpha ≥ 128;
    a copy can't blend) are copied into the game image, clipped to the image's area.
- **Why not overlays:** on the Steam Frame the overlay first showed nothing (black). Later it
  showed, but shimmered and wobbled: SteamVR shrinks the overlay's large image (about 2800 px
  wide, whatever X4's resolution) to the headset's pixels every display frame without mipmaps,
  while drawn in it is shrunk once per game frame and then only reprojected. An overlay cursor
  over a drawn screen would also be placed by a different method and could swim against it.
  `X4VR_THEATER_OVERLAY=1` still shows the screen as an overlay, for comparison.
- Ctrl+F12 recenters, Ctrl+F11 toggles the flat screen (Windows: `GetAsyncKeyState`; Linux: XCB
  key state while X4 has focus). `x4vr ctl recenter|flat` does the same.

## HUD and X4 settings

- **HUD distance** (`hud_mod.hpp`, shared): an extension built from the player's game files scales
  the HUD anchors back by a factor (1-6) and the HUD's scale factors in X4's UI scripts to keep its
  size. Linux X4 loads precompiled `.xpl` scripts, so the patched text goes there too; Protected UI
  Mode must be off. Linux applies the menu's factor at the next VR launch.
- **X4 settings** (`launcher_settings.hpp`, shared): FOV maximum, no temporal anti-aliasing,
  upscaling or frame generation, VSync and frame limit off, OpenTrack on. Linux also sets windowed
  mode at the VR resolution: Linux X4 ignores its resolution in fullscreen.
- **2D and VR apart (Linux):** a VR launch saves `config.xml` as `config.xml.x4vr-2d` and puts
  `config.xml.x4vr-vr` in its place; X4's exit swaps them back, and so does the next start after a
  crash. The HUD extension is only in `extensions/` during VR sessions; for 2D it is moved next
  to it (`x4vr_hud.off`), so 2D isn't "modified" (X4 often has no per-user `content.xml` to turn
  it off in). Saves made in VR stay flagged.

## The `x4vr` menu (Linux)

A terminal menu (`tools/linux/x4vr_cli.cpp`, `terminal_ui.hpp`) in place of the Windows launcher:
status (SteamVR, X4 build support, launch option, live frame rate), notices
(`config/linux/notices.txt`), settings with profiles (`config/linux/profiles/`), the in-game
settings checklist, tiling window manager rules, bug report and uninstall. Settings tagged *live*
go to `stereo.txt`, which the mod re-reads every 0.5 s; *next launch* ones are applied by
`x4vr-run`. The GPU line shows the last VR session's GPU and queue, or why VR was off. Uninstall
removes only the mod's own files by name (`tools/linux/state_files.hpp`), whatever directory
`X4VR_DIR` names. A few subcommands remain for `x4vr-run` and for when the menu can't help
(`x4vr help`).

## Settings and diagnostics

- `stereo.txt` (Linux: `~/.local/state/x4vr/`) holds the settings; defaults in
  `runtime_bootstrap.hpp`, shipped values in `config/linux/stereo.txt`.
- Linux defaults that differ from Windows, each measured in the headset: `half_xor_present=0`
  (1 doubled the HUD), `shared_pose=1` (the Frame's ghosting), `roll_gain=2.5` (degrees). The
  last is set in Linux's `read_settings`, so an older `stereo.txt` without the line gets it too.
- Request files in that directory produce `trace.txt` (`trace.request`), `submit_trace.txt`
  (`submit.request`) and an eye-image dump (`dump.txt`). `pair_stats.txt` is written every 2 s.
- Linux logs to `x4vr.log` and `stderr.log` in the same directory.

Linux environment variables, set in front of `x4vr-run` in the launch option:

| Variable | Effect |
|---|---|
| `X4VR_ALWAYS=1` | Every Steam launch starts in VR |
| `X4VR_FIX_SETTINGS=0` | Leave X4's `config.xml` alone |
| `X4VR_RESOLUTION=WxH` | X4's VR resolution (`0`: leave it) |
| `X4VR_HEADSET_WAIT=<s>` | Headset wait at start (default 120, `0`: none) |
| `X4VR_APP_ID=<name>` | VR window class (default `X4VR`) |
| `X4VR_HOTKEYS=0`, `X4VR_PATCHES=0`, `X4VR_EYE_AT_USE=0` | Turn those parts off |
| `X4VR_THEATER_OVERLAY=1` | Flat screen as a SteamVR overlay (shimmers; for comparison) |
| `X4VR_DIR`, `X4VR_GAME_DIR`, `X4VR_GAME_ARGS` | State directory, game directory, X4 arguments |
| `X4VR_OT_YAW`, `_PITCH`, `_ROLL`, `_X`, `_Y`, `_Z` | Sign and unit per OpenTrack axis (Windows: `X4VR_FT_*`); the gains in `stereo.txt` scale on top |
| `X4VR_OPENTRACK_PORT` | X4's OpenTrack port (default 4242) |

## Building and tests (Linux)

- **CMake:** `-DX4VR_LINUX=ON`; OpenVR's sources are fetched at configure time (v2.15.6, the
  version nixpkgs has) unless `OPENVR_SOURCE_DIR` names a copy. Warnings are errors.
- **Nix:** `nix-build linux/nix` (`linux/nix/default.nix`), from the channel's nixpkgs, which
  passes its OpenVR sources and runs the tests.
- **Tests** (`tests/linux/`, `ctest`): the code scan on a stand-in for X4's code, ELF and RTTI
  reading, MD5, the OpenTrack packet, Steam's config files, the launch option check, the 2D/VR
  settings swap (crash and stop cases) and the HUD extension's placement, uninstall's file list,
  plus the shared Windows suites that build on Linux.
- **CI** (`.github/workflows/linux.yml`): both routes on Ubuntu 24.04 when files the Linux build
  reads change.

## Linux addresses (X4 9.00)

Found by pattern at start; listed for reading disassembly.

| What | Address |
|---|---|
| Frame half global | `0x72a0fa0` |
| Camera input function (reads the tracker) | `0xfeb000..0xfed400` |
| `Camera::GetOffset` | `0x1929f70` |
| Player global (camera controller `+0x3e8`, mode `+0x880`) | `0x3db6948` |
| `VR::OpenTrack` vtable, slot 34 (position) | `0x3c62520`, `0x1a0dda0` |
