# How X4 VR works: Windows mod and Linux port

A working technical reference. For each part of the mod it describes how the Windows mod
(the original, `ToffelsKater/X4_VR`) does it, and how the Linux port (opt-in,
`-DX4VR_LINUX=ON`) does it differently. Addresses are for X4 9.00 (Windows: RVAs in `X4.exe`;
Linux: absolute addresses in the non-PIE `X4` binary). Keep it current when a part changes.
Measurements and history are in `STUTTER_RESEARCH.md` (Windows) and `LINUX_FINDINGS.md` (Linux).
Feature status at a glance: `FEATURES.md`.

Contents:

1. Overview
2. Build, install and loading
3. Runtime bootstrap and settings
4. Vulkan layer: copying eyes, submission, pacing
5. Frame half: which eye a frame is
6. Head-tracking feed
7. Eye at use and shared pose
8. Code patches
9. On foot (walking)
10. Theater screen
11. Cursor, hotkeys, recentre, quit
12. HUD distance and X4 settings
13. Diagnostics
14. Settings and environment reference
15. Status of the Linux port
16. Address reference

---

## 1. Overview

X4 has no VR. The mod turns its normal flat rendering into stereo:

- **Alternate-eye rendering.** X4 renders one frame for the left eye, the next for the right
  eye. The mod moves X4's camera to each eye through X4's own head-tracking input, which X4
  supports for TrackIR/OpenTrack-style head trackers. The headset pose plus the eye offset goes
  in as a "head tracker" pose.
- **Capture and submit.** A Vulkan layer copies each finished frame (from X4's swapchain) into
  that eye's texture and submits the pair to SteamVR (OpenVR) with the pose it was rendered
  with, so SteamVR's reprojection corrects for the time in between.
- **Code patches** remove limits in X4's head-tracking code (backward lean, on foot).
- Menus and other views without ship controls go to a flat **theater screen** in VR.

Both builds have the same three parts: a head-tracking feed, a Vulkan layer and a shared
runtime. What differs is how each reaches X4:

| Part | Windows | Linux |
|---|---|---|
| Head-tracking input to X4 | X4 calls `FTGetData` in our `FreeTrackClient64.dll` (FreeTrack protocol, pull) | We send OpenTrack UDP packets to X4's OpenTrack receiver (push) |
| Vulkan layer | `x4vr_observe.dll`, layer `VK_LAYER_X4VR_observe` | `libx4vr.so`, layer `VK_LAYER_X4VR` |
| Shared runtime | `x4_openvr.dll`, loaded by both DLLs | Linked into `libx4vr.so` (one library) |
| Start | `X4VRLauncher.exe` (+ `crash_watch.exe`) | The `x4vr` terminal menu: Launch in VR runs `steam -applaunch`; the Steam launch option `x4vr-run %command%` applies the mod only then |
| VR runtime | OpenVR (default) or OpenXR | OpenVR only (OpenXR is a stub) |
| Finding X4 code | Byte signatures searched in `X4.exe` (`include/x4vr/code_scan.hpp`) | Byte patterns searched in X4's loaded code, and RTTI for the tracker's vtable (`src/linux/code_scan.hpp`); `x4vr patterns` checks a build |

---

## 2. Build, install and loading

### Windows

- Top-level `CMakeLists.txt`, x64 MSVC (static runtime, shared UCRT so DLLs share one heap).
  The OpenXR loader is linked statically.
- Shipped files (`scripts/package.ps1`):
  - `x4_openvr.dll`: the runtime (`math.cpp session.cpp runtime_bootstrap.cpp
    openxr_runtime.cpp`, plus the unused `x4_camera.cpp head_look.cpp`).
  - `FreeTrackClient64.dll`: `freetrack_client.cpp`.
  - `x4vr_observe.dll`: the layer (`observe_layer.cpp`, `native_camera.cpp`), with
    `layer/VkLayer_x4vr_observe.json`.
  - `X4VRLauncher.exe`, `crash_watch.exe` and `openvr_api.dll`.
- **Loading:**
  - X4 loads `FreeTrackClient64.dll` from the folder in
    `HKCU\Software\FreeTrack\FreeTrackClient\Path` when "OpenTrack Support" is on. The
    launcher and `install.ps1` set that value; `uninstall.ps1` restores it.
  - The layer is not installed globally. The launcher starts X4 with
    `VK_ADD_LAYER_PATH=<bin>`, `VK_INSTANCE_LAYERS=VK_LAYER_X4VR_observe`,
    `X4VR_OPENVR_BOOTSTRAP=1`, `X4VR_RUNTIME`, `X4VR_CAPTURE_DIR=<root>\reports\captures`
    and `X4VR_GAME_ARGS="-skipintro -nocputhrottle"`.
- `crash_watch.exe` runs X4 as a debuggee:
  - it collects every "X4VR …" `OutputDebugString` line into `debug-events.log`;
  - it writes a minidump on a crash;
  - it sets `_NO_DEBUG_HEAP=1`, otherwise loading takes over 7 minutes.
- **Experimental, not in the shipped path:**
  - `x4vr_native_pose.dll`, `pose_detour.cpp`, `copy_call_hook.cpp` and
    `scene_camera_guard.hpp`: an earlier camera-hook approach, loaded only by
    `crash_watch_dev --startup-module`. It gave no visible cockpit movement
    (`DEVELOPMENT.md`).
  - `head_look.cpp` and `x4_camera.cpp` are compiled but not called.

### Linux

- Top-level `CMakeLists.txt`: with `X4VR_LINUX=ON` it goes to `linux/CMakeLists.txt` and
  returns before the Windows project, so the Windows build is unchanged. Without the flag, a
  non-Windows host stops with an error.
- Targets:
  - `libx4vr.so` (`x4vr_mod`): `src/linux/vr_layer.cpp`, `runtime_bootstrap.cpp`,
    `pose_sender.cpp`, `x11_cursor.cpp`, `head_watch.cpp`, and the shared `src/eye_targets.cpp`,
    `src/math.cpp` and `src/session.cpp`.
    - It is linked with static libstdc++/libgcc and `--exclude-libs,ALL`. The version script
      `linux/x4vr.map` exports only the three Vulkan loader entry points.
    - OpenVR is built from source as a static library (`x4vr_openvr_api`), so it can't clash
      with libraries in X4's Steam runtime container.
  - `x4vr` CLI (`tools/linux/x4vr_cli.cpp`). It reuses the Windows launcher logic
    `tools/launcher/hud_mod.hpp` and `launcher_settings.hpp`, which are header-only.
  - Phase 0 probes: `libx4vr_probe.so` and `libVkLayer_x4vr_probe.so`.
  - Tests (`ctest`): the shared suites plus `opentrack_tests`, `md5_tests`,
    `elf_classes_tests` and `probe_preload`.
- Install: the layer manifest `VkLayer_x4vr.json` goes to `share/vulkan/explicit_layer.d`;
  `x4vr-run` and `x4vr` go to `bin`; the default `stereo.txt` goes to `share/x4vr`.
- **Starting:** the `x4vr` menu (section 11b) launches VR: SteamVR first if needed, then a
  request file (`launch.request` in the state folder) and `steam -applaunch 392160`. Steam
  starts X4 with its usual chain (reaper, the Steam Linux Runtime) and the launch option
  `x4vr-run %command%`, which the user sets once (the menu copies it). The mod has to be loaded
  there: `-applaunch` only asks the running Steam client, and the game inherits Steam's
  environment.
- **Loading:** `linux/x4vr-run.in`:
  0. Without a fresh request (under 2 minutes; `X4VR_ALWAYS=1` skips the check) it runs
     Steam's command untouched (`exec`): Steam's Play button starts the normal game. A VR
     session that didn't restore X4's 2D settings (crash) gets them back first.
  1. Creates `~/.local/state/x4vr` (or `$X4VR_DIR`), copies the default `stereo.txt` there,
     and rotates `x4vr.log` and `stderr.log`.
  2. `unset StreamForOpenVR SteamStreamingVRPairedInvite`. With SteamVR running, Steam
     otherwise streams X4 to a virtual headset screen, takes its window, and X4 quits.
  3. Removes `gameoverlayrenderer.so` from `LD_PRELOAD` and sets
     `DISABLE_VK_LAYER_VALVE_steam_overlay_1=1`. Steam's overlay capture made X4 quit at the
     first submit. `X4VR_STEAM_OVERLAY=1` keeps the overlay.
  4. Exports `X4VR_CAPTURE_DIR`, `X4VR_RUNTIME=openvr`, `VK_ADD_LAYER_PATH`,
     `VK_INSTANCE_LAYERS=VK_LAYER_X4VR` and `VK_LOADER_LAYERS_ENABLE`.
  5. Warns in the log if `vrserver` isn't running; SteamVR doesn't start on its own on Linux.
  6. Switches X4 to its VR settings (`x4vr settings-mode vr`, section 12), runs
     `x4vr fix-settings --auto` and `x4vr hud --refresh` (the game folder from the
     `testandlaunch` argument).
  7. Appends `-skipintro -nocputhrottle`, or `$X4VR_GAME_ARGS`.
  8. Runs X4 (not with `exec`) and logs its exit status. 0 is a clean quit; 134, 139, 143 and
     137 are an abort, a crash or a kill.
  9. Switches X4 back to its 2D settings (`x4vr settings-mode 2d`).
- **No crash_watch.** Logs go to `~/.local/state/x4vr/x4vr.log` (`linux_port::log`) and
  `stderr.log`.

---

## 3. Runtime bootstrap and settings

### Common

`include/x4vr/runtime_bootstrap.hpp` is shared by both builds. The Linux copy of the `.cpp` is
`src/linux/runtime_bootstrap.cpp`.

- `acquire_runtime_bootstrap()` keeps one runtime per process and pins it for the process
  lifetime. X4 creates and destroys temporary Vulkan instances while starting, and repeated
  `VR_Init`/`VR_Shutdown` caused a deadlock.
- OpenVR: `VR_Init(VRApplication_Scene)`, seated tracking universe (`session.cpp`).
- `predicted_tracking()` is lock-free (`GetDeviceToAbsoluteTrackingPose(Seated, t)`), so the
  pose feed never waits behind the render thread's `WaitGetPoses`.
- Other calls:
  - `eye_setup()`: `GetEyeToHeadTransform` and `GetProjectionRaw`.
  - `wait_frame()`: `WaitGetPoses`.
  - `submit_frame()`: per eye, `Submit` with `VRTextureWithPose_t` and `Submit_TextureWithPose`.
- Pose records: `record_render_pose()` keeps a ring of 32 entries `{present tag, head, eye,
  flat, walking}`. `presented_frame()` finds the pose a presented frame was rendered with
  (section 5).
- Settings (`StereoSettings`): `stereo.txt` in `X4VR_CAPTURE_DIR`, one `key=value` per line,
  re-read every 500 ms by `background_loop()`. The same thread does deferred file writes and
  watches for request files (section 13).

### Differences on Linux

- OpenVR only. `X4VR_RUNTIME=openxr` logs and falls back; `openxr_runtime_stub.hpp` throws.
- `wait_frame()` also polls SteamVR events (`PollNextEvent`):
  - `VREvent_Quit` → acknowledge, then close X4 (section 11);
  - `VREvent_SeatedZeroPoseReset` / `StandingZeroPoseReset` → recentre.
- Linux helpers in `linux_port`:
  - `log`;
  - `is_x4_process` (`/proc/self/exe` is named `X4`, or `X4VR_ANY_PROCESS=1`);
  - `in_executable` (address range within X4's loaded segments, used before reading or
    patching X4 code);
  - `control`;
  - `shared_pose`.
- The Linux `stereo.txt` (`config/linux/stereo.txt`) differs from Windows':
  `half_xor_present=0` (Windows 1) and `shared_pose=1`, a Linux-only key.

---

## 4. Vulkan layer: copying eyes, submission, pacing

Windows: `src/observe_layer.cpp`. Linux: `src/linux/vr_layer.cpp`, a copy reduced to what
ships. Shader, memory and camera capture, the stack walks and the frame probe are left out.

### Common design

- **Instance:** acquire the runtime and add the runtime's instance extensions. A failure
  leaves X4 flat: "VR runtime unavailable, X4 runs without VR".
  - Linux only: when `VR_Init` fails because the headset isn't there yet (108 not found, 126
    presence failed, 215 wireless headset not connected), the bootstrap retries every second
    for up to `X4VR_HEADSET_WAIT` s (120). SteamVR can be up before a Steam Frame connects.
- **Device:**
  - Add a private queue for the VR thread in X4's graphics family.
  - Check that X4's GPU is the headset's GPU (`deviceUUID` equals OpenVR's output device).
  - Add the runtime's device extensions.
- **Swapchain:** record the images, extent and format. On a resize, rebuild the eye targets;
  the old ones are retired, not freed.
- **Present (`presenter_copy`):**
  1. `next_present()`, then `presented_frame()`: which eye this frame is, its head pose, flat
     or not.
  2. `update_theater` decides stereo or theater (section 10).
  3. Copy the image into a ring of 3 targets per eye (`EyeTargets`). The game image is
     centred in an eye texture sized to the larger of the game's and the headset's field of
     view; the rest stays black.
  4. The copy waits on X4's present semaphores and signals a semaphore that the real present
     waits on. Each slot keeps its pose and a `written` fence.
- **Eye texture geometry (`presenter_initialize`):** the span is the larger of X4's FOV
  (`game_tan_y`, aspect-scaled) and the headset's raw tangents. `eye_bounds` crops each eye's
  asymmetric frustum (`valve_bounds`).
- **Submission thread (`compositor_loop`, `async_submit=1`):**
  1. `wait_frame()`, then a tick that releases X4's render thread.
  2. Take the lock within `submit_budget_ms` (2 ms), otherwise resubmit the last frame.
  3. Per eye: the newest image, or the newest finished one as a fallback.
  4. Submit with the pose that image was rendered with.
  5. A ring of fences keeps submitted images alive until SteamVR's reads finish.

  A late frame repeats the previous image instead of flashing grey.
- **Pacing (`pace_to_compositor`):** X4's render thread waits for the compositor tick
  (`release_late=1` lets a late frame go at once). `pair=1` mode, `wait_mid_frame` and the
  inline path (`async_submit=0`) are optional.
  - The inline path only sends the eye images: the flat screen (Windows: SteamVR overlay; Linux:
    drawn into the eye images) and, on Linux, the shared pose live in the submission thread. With
    `async_submit=0` menus get no flat screen (Linux: grey right eye) and the Steam Frame's
    right-eye ghosting returns (2026-10-05). Both menus keep the option, for troubleshooting.

### Windows only

- **Turn compensation** (`turn_comp`): reads X4's camera uniform from mapped memory
  (`track_camera`, descriptor set 1). When both eyes come from different frames during a
  turn, it rotates the older eye's pose by the camera's turn between them. On by default on
  foot.
- OpenXR backend (`openxr_runtime.cpp`): `LOCAL`/`VIEW` spaces, SRGB swapchains, theater and
  cursor as quad layers.
- Diagnostics: probe grid, `events.jsonl`.

### Linux only

- **Shared queue.** RADV has one graphics queue, so the layer shares X4's queue. Every use of
  it is serialised by one recursive mutex (`shared_queue`): X4's submits and presents, the
  layer's work and SteamVR's work.
- **Sizing from SteamVR.** The pixel density is the lower of X4's own and SteamVR's
  recommended density (`GetRecommendedRenderTargetSize` per tangent span). When it is lower,
  the copy is a linear `CmdBlitImage` scale instead of a copy. It writes the ideal X4
  resolution to `x4_resolution.txt` for the CLI. `X4VR_NATIVE_SIZE=1` keeps X4's density.
- **Shared-pose pair matching** (section 7).
- **Theater screen drawn into the eye images** (section 10) and **cursor copied into the
  image** (section 11).
- Thread priority: `SCHED_FIFO` if allowed, else nice -10. Windows uses `TIME_CRITICAL`.
- `sample_game_state()` runs in the present hook on X4's main thread (section 6).
- A frame-half check logs how often the half flipped (after 1000 presents, then every 20000).

---

## 5. Frame half: which eye a frame is

X4 keeps a per-frame "half" global that alternates 0/1 and follows a frame from pose sampling
to presentation, whatever the queue depth. The mod uses it to tell, at present time, which eye
a frame was rendered for.

| | Windows | Linux |
|---|---|---|
| Found by | Signature `48 63 05 ?? ?? ?? ?? 48 83 f0 01 48 69 c8 70 02 00 00`, all matches naming one global | Pattern `8b 05 ?? ?? ?? ?? 83 f0 01 c3` (its reader, 9.00: `0x218e220`), all matches naming one global |
| Global | RVA `0x6b66280` | `0x72a0fa0` |
| `render_eye()` | `half ^ half_xor_render` | same |
| `presented_frame()` | eye = `half ^ half_xor_present`, `half_xor_present=1` | `half_xor_present=0` (measured, stage C) |

Without the half (`eye_from_half=0` or not found), the eye follows the present count and
`delay`.

---

## 6. Head-tracking feed

### Windows: `FTGetData` (`src/freetrack_client.cpp`)

X4 calls `FTGetData(FreeTrackData*)` on its main thread, once per frame, when OpenTrack Support
is on. Each call:

1. On the first call: acquire the runtime, apply the code patches (section 8), and set X4's
   head smoothing to 1 with its export `SetActiveHeadTrackerHeadFilterStrength`. X4's menu
   minimum of 5 lags rotation and averages the alternating eye offsets away.
2. `walking = on_foot_tracking && camera_on_foot()` (section 9). Pose =
   `predicted_tracking(predict + (walking ? 1/90 : 0))`.
3. `eye = render_eye() ^ walking`.
4. `flat` from X4's exports (section 10).
5. Recentre if needed (section 11).
6. Per-eye heads: `head * head_from_eye[e]` (eye translation × `ipd_scale`). Flat uses the
   identity pose.
7. Convert to FreeTrack:
   - angles from a Y-X-Z decomposition, times `yaw/pitch/roll_gain`. X4 maps angle/π to ±1
     and multiplies by 85°, hence 180/85 = 2.1177;
   - position `pos_scale × {-1000, 1000, 1000} × m[k][3]` mm, in seated axes. X4 applies
     tracker translation in the ship frame, not rotated with the head.
8. Fill the eye-at-use offsets (section 7).

It also writes the diagnostics `head.txt`, `state.txt` and `ftgetdata_caller.txt`.

### Linux: OpenTrack UDP (`src/linux/pose_sender.cpp`)

Linux X4 has no FreeTrack DLL loading. Its OpenTrack support (`VR::OpenTrack`) listens on UDP
for 6 little-endian doubles: x, y, z (cm) and yaw, pitch, roll (degrees). Format in
`src/linux/opentrack.hpp`.

- `sender_loop` thread: waits for a new present (up to 12 ms), so it sends **one packet per
  present** to `127.0.0.1:4242` (`X4VR_OPENTRACK_PORT`).
- Pose computation as on Windows: prediction, recentre, flat identity, per-eye heads every
  1 s (IPD changes logged), Y-X-Z angles times the gains, position × `pos_scale`.
- Axis signs differ from Windows because X4's OpenTrack path differs: yaw -1, pitch 1,
  roll 1, x -100, y 100, z 100 (`X4VR_OT_*` overrides).
- **Packet sequence number in roll.** A number 1..255 goes in the low 8 mantissa bits of the
  roll double, about a 1e-14 relative change. That lets the eye-at-use hook tell which packet
  X4's update took.
- **Game state** (`sample_game_state`, called from the layer's present hook on X4's main
  thread): `IsHeadTrackingActive`, `IsFullscreenMenuDisplayed(true, nullptr)` and
  `IsPlayerControllingShip`, via `dlsym(RTLD_DEFAULT)`. The menu and ship queries run only
  while head tracking is active; on Windows, `IsHUDActive` crashed at the main menu. Changes
  are logged.
- Head smoothing set to 1 as on Windows.
- Start order (`start_pose_sender`): `apply_patches()`, `install_eye_hook()`, the sender
  thread, then `start_head_watch()`.

**`VR::OpenTrack` in Linux 9.00:**
- vtable `0x3c62520`, type_info `0x3c61bb0` (`N2VR9OpenTrackE`).
- Slot 2 `0x1a1b720`: update, once per frame. Copies the newest packet to `+0x78..+0xa0`,
  sets the fresh flag `+0xa8`, smooths the position × scale (`+0x114`) into `+0xd0`, and the
  angles into `+0x100`.
- Slot 33 `0x1a0dd60`: angles accessor.
- Slot 34 `0x1a0dda0`: position accessor, `+0xd0/180` clamped to ±1.
- Itanium vtables have two destructor slots, MSVC one, so Linux slot numbers are one higher
  than Windows' (Windows position slot `0x108` = 33).

---

## 7. Eye at use and shared pose

### Why

X4 reads the tracker on its camera thread, not exactly when the pose arrives. If the eye is
chosen when the pose is sent, a read that lands early or late uses the other eye's offset: on
Windows about 2% of frames at 90 fps (up to 38% unpaced) jumped sideways. So the feed sends the
**head centre**, and a hook on the tracker's position accessor adds the offset of the eye X4
is building at that moment (from the frame half).

### Windows

`eye_hook_ready()` finds the tracker from `FTGetData`'s data pointer (`tracker = data -
layout.data`, checks `tracker+get_data == FTGetData`) and swaps two vtable slots:

- `0x108`, position → `position_at_use`:
  - eye = `render_eye() ^ (walking ? half_xor_walk : half_xor_use)`;
  - adds `delta[eye] × scale × gain(0.2)` to the position field, calls the original, restores
    it, and records the pose for reprojection.
- `0x28`, still check → `still_at_use`. X4 stops reading the tracker once 30 reads in a row
  barely changed. The centre alone can be that still, so the tracker never counts as still
  while offsets are added.

9.00 layout: update `0xf37583`, position code `0xf377b0`, still code `0xf376c0`, fields
`data 0x30`, `position 0xc0`, `scale 0x104`. 8.00 is also supported.

### Linux

- `install_eye_hook()` uses the X4 scan: `VR::OpenTrack`'s vtable is found through its RTTI
  name `N2VR9OpenTrackE` (type name → type_info → vtable, `elf_classes.cpp`), and slot 34 must
  hold `f3 0f 10 87 d0 00 00 00` (9.00: vtable `0x3c62520`, slot 34 `0x1a0dda0`). Then it swaps
  slot 34 (`mprotect`, atomic store).
- `position_at_use`:
  - if the fresh flag `+0xa8` is set, reads the packet sequence from the roll at `+0xa0`;
  - looks up that packet's per-eye deltas;
  - adds `delta[render_eye()] × scale(+0x114)` to `+0xd0`, calls the original, restores it,
    and records the packet's headset pose.
- `X4VR_EYE_AT_USE=0` turns it off. The feed then falls back to choosing the eye when sending
  (stage A).
- **No still hook.** Linux `VR::OpenTrack` slot 6 (`0x1398960`) is a stub, and no freeze was
  seen. X4's camera input does call slot 6 (`+0x30`) before reading the tracker, at
  `0xfeb70b`.

### Shared pose (Linux only)

The Steam Frame (SteamVR's link) reprojects **both eyes with the left eye's pose**. A right-eye
image rendered from a newer pose was then shifted wrongly: the right eye ghosted and jittered
in the cockpit.

The fix has two parts:
- **Sender:** with the hook active, in stereo and not flat, the packet before a right-eye frame
  is skipped (`shared_pose` setting, default 1). X4 then renders both eyes of a pair from the
  same packet. Eye at use still adds each eye's own offset.
- **Layer:** `compositor_loop` submits matching pairs. If the two newest eye images have
  different poses (compared with `memcmp`), the eye that is ahead steps back to a ring slot
  with the other eye's pose. Statistics are logged every 4000 submits.

Windows has no shared-pose path. Each eye's pose comes from its own game frame.

---

## 8. Code patches

### Mechanism

- **Windows** (`patch_code`, `code::find_unique`): a byte signature with `??` wildcards is
  searched in X4's code sections and must match exactly once. Already-patched bytes count as
  done. `VirtualProtect`, then `memcpy`, then `FlushInstructionCache`. Versions 8.00 and 9.00
  are supported by rows of signatures.
- **Linux** (`apply_patches` / `apply_patch` in `pose_sender.cpp`, patterns in
  `src/linux/code_scan.hpp`): the same idea as Windows.
  - At startup the pose sender's thread scans X4's loaded code once (`scan_x4`) and logs each
    site (`X4VR scan:` lines). Each pattern must match exactly once. `??` wildcards cover call and
    RIP displacements; struct offsets and short jumps stay literal.
  - The camera-offset patch also checks the block its new jump lands on. The player global is
    read from that site's RIP operand.
  - The patched byte must hold its original value (or the patched one: "already patched").
    Anything else is logged and X4 is left alone.
  - `x4vr patterns [X4 path]` runs the same scan on the file, to check a new X4 build before
    playing.
  - Each patch is **one byte**, written with an atomic store between `mprotect` RWX and R-X,
    so it is safe while X4 runs.
  - `X4VR_PATCHES=0` turns them off.

### The X4 function they patch

Windows notes call it the "head-tracker bridge" (`0x9fd870`). On Linux it is **X4's per-frame
camera input function**, around `0xfeb000..0xfed400`. It was found with the head watch
(section 13): the tracker's position accessor has exactly one caller, the call at `0xfec248`.

Note: Linux also has a class `U::HeadTrackerCameraBridge` (vtable `0x3b1c240`, static object
`0x3e60820`). It is **not** this function, and it isn't used for the cockpit view: its stored
position stays 0.

What the Linux function does:
1. Keyboard look input goes into locals.
2. Camera controller = `[[0x3db6948]+0x3e8]`, where `0x3db6948` is the player global. It
   reads the controller's camera mode `+0x880`.
3. With mode 0: `call 0x1e07060` (no ship). If true and the tracker isn't an eye tracker
   (slot 8), the camera gets an **all-zero pose** (`0xfec0bc..0xfec0db`).
4. Otherwise: tracker = `[0x3db8938]+0x148` (the head-tracking manager's active tracker).
   - Slot 6 (`+0x30`) and slot 23 (`+0xb8`) gate the read.
   - With mode 0: slot 33 (angles) fills yaw/pitch/roll and slot 34 (position) fills x/y/z.
5. `if (tracker slot 19 (+0x98) != 7 && z > 0) z = 0`: the **backward clamp**.
6. Hand the values to the camera controller `0x1933d00`: xmm0-2 angles, xmm3-5 position.

### The patches

| Patch | Windows 9.00 | Linux 9.00 |
|---|---|---|
| **Backward clamp:** X4 zeroes backward head position (z > 0), pinning leaning back and the rear eye when looking sideways | Signature `f3 0f 10 45 67 0f 57 05 ?? ?? ?? ?? 0f 2f c6 73 04 44 89 65 67` at `0x9fdb3e`, `jae` (+15) → `jmp` | `0xfeb73f` `jbe` (`76 08`) → `jmp` (`eb`); 30 bytes from `0xfeb72b` checked |
| **Rival trackers:** TrackIR / Tobii come after FreeTrack in X4's pick; a foreign `NPClient64.dll` (vorpX) or an eye tracker replaced the headset pose | TrackIR `je` +11 and Tobii `je` +18 → `jmp` (`0xfa34fc`, `0xfa353c`) | Not needed: the Linux build has only OpenTrack (no TrackIR or Tobii trackers). The clash to avoid is another program sending to UDP 4242 (the real opentrack app) |
| **On-foot zeroing:** without a ship, X4 hands the camera a zero pose | `je` at +101 of the bridge signature (`0x9fd9ae`) → `jmp` | `0xfec078`: `je` (`0f 84`) → `jno` (`0f 81`), always taken after `test` |
| **On-foot camera offset:** `Camera::GetOffset` applies the head offset (`Camera+0x590`) only through a movement controller (`Camera+0x20`), null on foot | `0x97a413`: `je` displacement `0x276` → `0x18a`, into the offset block | `Camera::GetOffset` = `0x1929f70`: its `je` to the exit (`0x1929ffd`, `0f 84 13 02 00 00`) goes to the offset block `0x192a284` instead (displacement `0x213` → `0x281`, byte `0x1929fff`) |

Both on-foot patches on Windows must reference the same camera-manager global, otherwise
neither counts.

---

## 9. On foot (walking)

### Windows

1. Both on-foot patches (section 8).
2. **Walking detection** (`camera_on_foot`): camera = `[[camera manager]+0x3d0]`. Walking
   when `camera+0x20` (movement controller) is null and `camera+0x868` (mode, 9.00) is 0.
3. Walking stays in stereo: flat = `menu || !(walking || controlling ship)`.
4. **Timing:** on foot, X4 reads the tracker right after a present and the frame being built
   uses it; in the cockpit, the read comes before a present and is used two frames later. So
   on foot:
   - the pose is predicted 1/90 s further (`predict + 1/90`);
   - eye = `render_eye() ^ 1` when sending, and `half_xor_walk=0` at use;
   - the reprojection pose uses `delay_walk=1`.

   Result: cockpit and on foot both smooth (`STUTTER_RESEARCH.md`, "On foot").
5. **Turn compensation** on foot (`turn_comp=1`) keeps both eyes aligned while turning with
   the mouse.

### Linux, status

- **Both Windows patches are ported** (section 8): the on-foot zeroing (`0xfec078`), and
  `Camera::GetOffset`'s "no movement controller" exit (`0x1929fff`). The second is waiting for
  its headset test.
- How the second was found, 2026-10-05:
  - `X4VR_WATCH_HEAD=2`: on foot X4 reads the tracker every frame and hands the pose to the
    camera controller `0x1933d00`, as in the cockpit.
  - The controller stores the head offset at `+0x5a0` (position) and `+0x5b0` (rotation)
    (`0x1935ac1`). Windows: `Camera+0x590`.
  - Modes 3 and 4 found its readers.
  - Windows' commit 8342ef1 showed that `GetOffset` takes the camera as a parameter. A search for
    its check (camera `+0x780` against the player entity `[0x3db6948]+0x238`, fallback
    `0x3db6c00`) found `0x1929f70`.
  - It is called on foot once per frame (`0x11d9660`), and returned before the offset.

  | Windows `Camera::GetOffset` (`0x97a300`) | Linux `0x1929f70` |
  |---|---|
  | `camera+0x20` null → exit `0x97a694` (`je` at `0x97a418`) | `camera+0x18` null → exit `0x192a216` (`je` at `0x1929ffd`) |
  | camera `+0x770` vs `[camera manager+0x230]` | camera `+0x780` vs `[0x3db6948]+0x238` |
  | `[camera manager+0x3d0]` (rendered camera) | `[0x3db6948]+0x3e8` |
  | offset block `0x97a5a8` (`Camera+0x590`) | `0x192a284` → `0x192a15e` (`camera+0x5a0..+0x5df`) |
- Dead ends:
  - The cockpit path (`0x1646510` → `0x1628ce0`, gate `0x1628210`) is the **seat** camera.
  - Forcing it on foot made the view follow the head, but moved the camera to the seat and
    stopped walking.
  - Those experiments are removed.
- Moving the head on foot currently feels laggy. SteamVR reprojects each image with the pose it
  was sent with, but X4 doesn't apply that pose on foot.
- Headset test (2026-10-05): on foot the view follows the head, walking works, the cockpit is
  unchanged; "a tiny bit of lag".
- **Walking detection** (`camera_on_foot`, sampled on X4's main thread, as on Windows): the
  rendered camera `[[0x3db6948]+0x3e8]` in mode 0 (`+0x880`) without a movement controller
  (`+0x18`), not controlling a ship, both on-foot patches in place. Walking stays in stereo with
  `theater=1`; the game-state log line shows `walking=`.
- **Timing, from Windows:** on foot the pose is predicted one frame (1/90 s) further, and poses
  are recorded as walking, so the reprojection pose uses `delay_walk` (1) instead of `delay`.
  Test: the lag is better on foot, and `delay_walk=2` didn't help further.
- **Eye flip on foot**, from Windows: on foot X4 uses the pose one frame sooner, so the eye a
  frame half means flips. Windows maps it with `half_xor_use=1` in the cockpit and
  `half_xor_walk=0` on foot. Linux keeps that difference: on foot the eye (at send and at use)
  is flipped by `half_xor_use ^ half_xor_walk`, 1 by default. `half_xor_walk=1` turns it off.
  Without it the eyes are swapped on foot (headset, 2026-10-05: worse for near objects); with it,
  on foot "feels better" (confirmed).
- **Turn compensation isn't needed on Linux with the shared pose** (`shared_pose=1`, the
  default). The headset test (2026-10-05) showed no double vision when turning with the mouse on
  foot; what felt harsh was X4's own turn speed, set in X4's input options.
  - The Windows method can't work on the Steam Frame anyway: it corrects one eye's submitted
    pose, and the Frame reprojects both eyes with the left eye's pose.
  - If it is ever needed (another headset, `shared_pose=0`), the Linux way would be to shift the
    other eye's image by the camera turn between the two frames while copying it, using
    Windows' camera-uniform reading (`track_camera`).

---

## 10. Theater screen

Views that aren't for stereo (menus, map, loading, cutscenes, views without ship controls) are
shown flat on a virtual screen.

### Common

- The feed decides `flat`:

  ```
  theater == 2                                                  (forced)
  || theater == 1 && (fullscreen menu || !(walking || controlling ship))
  ```

  Linux has the same `walking` term (section 9). While flat, the feed sends the centred
  identity pose, a steady view.
- The layer switches modes in `update_theater`. Switching to the theater screen takes several
  flat frames in a row (Windows 10, Linux 30; X4's pop-up hints flickered with 10 on Linux),
  and switching back takes 3 stereo frames.
- The screen is `theater_width` (2.2 m) wide, `theater_distance` (2 m) in front of the
  recentred origin (`view_origin`), and re-placed on recentre.

### Windows

- An OpenVR overlay (`show_theater`, `"x4vr.theater"`) shows eye 0's image.
- The eyes are submitted black, at full size; a tiny texture lost the Vulkan device.
- Ctrl+F11 toggles forced theater.

### Linux

- SteamVR overlays don't show on the Steam Frame (black). So `draw_screen` blits the flat image
  into both eye textures at the screen's place, using frustum rectangles from `screen_rects`.
- These are submitted with a frozen `screen_origin` pose, so SteamVR's reprojection keeps the
  screen fixed in space.
- `X4VR_THEATER_OVERLAY=1` uses the Windows-style overlay instead.
- Ctrl+F11 and `x4vr ctl flat` toggle `theater` between 2 and 1 in `stereo.txt`.

---

## 11. Cursor, hotkeys, recentre, quit

| | Windows | Linux |
|---|---|---|
| Cursor source | `GetCursorInfo` / `DrawIconEx` over black and over white, the difference giving alpha | XFixes `GetCursorImage` over XCB (loaded at runtime with `dlopen`; X4's window by `WM_CLASS` `X4` or `_NET_WM_PID`), `QueryPointer`/`GetGeometry` |
| Cursor display | OpenVR overlays, one per cursor image (max 16; `SetOverlayRaw` leaks); on the theater screen or at `cursor_distance` along the game ray | Copied into the game image before it is copied to the eye (`draw_cursor`: pixels with alpha ≥ 128, no blending), so it shows in the cockpit and on the theater screen |
| Hotkeys | `GetAsyncKeyState`: Ctrl+F12 recentre, Ctrl+F11 theater, checked in `FTGetData` | XCB `QueryKeymap` while X4's window has focus (watched, not grabbed): Ctrl+F12 recentre, Ctrl+F11 theater. `X4VR_HOTKEYS=0` turns them off |
| Recentre | Ctrl+F12, the `recenter` counter in `stereo.txt` (the launcher's button), the first pose | Same, plus `x4vr ctl recenter` and **SteamVR's own recentre** (`VREvent_SeatedZeroPoseReset`) |
| SteamVR "Exit game" | — | `VREvent_Quit` → `AcknowledgeQuit_Exiting`, then `WM_DELETE_WINDOW` to X4's window (as its close button) |

Recentre (`seated_origin`) takes position and yaw only. X4 clamps normalized head position to
±1 (0.25 m), so the origin must sit at the user's head. X4's own "Reset Head Tracking" key
breaks the calibration.

`x4vr ctl` and the hotkeys edit `stereo.txt` atomically (`settings_control.hpp`, write to `.tmp`
then rename). The mod picks up the change within 0.5 s.

### 11b. The Linux menu (`x4vr`, `tools/linux/x4vr_cli.cpp`, `terminal_ui.hpp`)

A full-screen terminal menu without libraries (termios raw mode, ANSI codes), Linux only. The
counterpart of the Windows launcher window. One screen in sections, rebuilt every 2 s so it
always shows what is true now; the selected item's description shows at the bottom. Nothing is
shown greyed out: what doesn't apply isn't listed. Enter on a choice opens its options as a list
(←→ cycles them in place). Only the Quit item closes it: Esc and Ctrl+C don't.
- **Status:** SteamVR, the X4 build scan (`x4vr patterns`), the launch option (read from
  Steam's `localconfig.vdf`, `steam_config.hpp`), X4's VR settings, and X4: not running, in 2D,
  or in VR with the headset's frame rate, late and repeated frames from the newest
  `pair_stats.txt` line (`parse_stats_line`, the same format and parser as Windows).
- **Play:** Launch X4 in VR; while X4 runs in VR, recentre and flat screen.
- **Notices:** pinned known issues and tips, from `share/x4vr/notices.txt`
  (`config/linux/notices.txt`, lines `issue: ...` / `tip: ...` / `help: ...`, the last in green).
- **Settings**, each tagged with when it applies:
  - `live`: written to `stereo.txt`, which the mod re-reads every half second: 3D, shared pose,
    world scale, head prediction, cursor, flat screen mode, distance and width.
  - `next launch`: stutter protection (`async_submit`, latched by the layer at X4's first frame:
    the OpenVR session can't move between the game's thread and the submission thread once
    frames went through it, so switching while X4 ran froze the headset's image); applied by
    `x4vr-run` at the next VR start: HUD distance (`hud_factor`, 1.0-6.0
    to one decimal, typed or ←→; `x4vr hud --refresh` builds or removes the extension) and X4's
    resolution (`x4_width`/`x4_height`, the Windows launcher's keys; 0: automatic; Custom shows
    Resolution Width and Height rows, like Windows' two boxes. Any aspect works: the layer
    keeps X4's vertical view and derives the horizontal one from the image's aspect).
  - **Profiles:** sets of those settings. Built in: `share/x4vr/profiles/*.txt` (Steam Frame);
    the player's own: `<state>/profiles/*.txt`, made with "Save as profile". The last loaded or
    saved is named in `<state>/profile`; "· changed" marks settings that differ from it.
  - The VR runtime is shown as text: SteamVR (OpenVR) is the only one on Linux.
- **X4 settings for VR:** the checks the VR copy of `config.xml` fails (required / recommended,
  with the current value); `x4vr-run` fixes them at the next VR launch. "In-game settings
  checklist" opens the README's settings tables as a screen, by X4 settings page, each row
  marked from `config.xml` or "check by hand in X4" (Controls > OpenTrack: the factors, which X4
  doesn't save; smoothing isn't listed, the mod sets it;
  Extensions > Protected UI Mode, listed only with HUD Scaled on).
- **Setup:**
  - Copy the launch option: wl-copy, xclip or xsel, else OSC 52. The user pastes it in Steam;
    the tool never writes Steam's files.
  - Add to the app launcher: `~/.local/share/applications/x4vr.desktop`, `Terminal=true`.
  - Bug report: `~/x4vr-report-<time>.tar.gz` with logs, settings, X4's `config.xml` and a
    summary.
  - Uninstall: removes what the mod set up (LINUX_GUIDE.md, section 6, lists the rest).

Each action is also a subcommand (`x4vr help`).

---

## 12. HUD distance and X4 settings

### HUD distance (shared logic, `tools/launcher/hud_mod.hpp`)

X4's HUD anchors sit 0.29-0.44 m in front of the pilot. The tool builds an extension
`extensions/x4vr_hud`:
- `subst_01.cat`/`.dat` with original UI files from X4's catalogs;
- `uianchor_*` positions scaled by a factor of 1-6 (default 2.5), with menus left alone;
- the HUD scaling factors in 9 Lua files scaled to match;
- `content.xml` and `x4vr_hud.txt` (factor and source hash).

It is rebuilt when the game's files change.

- **Windows:** launcher buttons. Refreshed on Play.
- **Linux:** the menu's HUD setting (`hud_factor` in `stereo.txt`), applied by `x4vr-run`'s
  `x4vr hud --refresh` at the next VR start; also `x4vr hud <factor>|remove|status`. Same files
  (MD5 from `md5.hpp`). Limits:
  - Linux X4 9.00 loads precompiled `.xpl` UI scripts, so the Lua scale changes alone don't
    apply: the HUD moved back but got smaller. The extension now also puts the patched script
    text at the `.xpl` paths (Linux only; confirmed 2026-10-05: the HUD keeps its size at any factor).
    Protected UI Mode must be off.
  - X4 reports the game as modified, and warns about Protected UI.

`x4vr game-grep` searches X4's catalogs.

### X4 settings (`launcher_settings.hpp`, `check_x4` / `fix_x4`)

Required:
- `fov ≥ 1.333`;
- non-temporal anti-aliasing;
- no frame generation (`dlssg`, `fsr3g`);
- `upmode=none`;
- `presentmode=immediate`;
- no frame-rate limit;
- OpenTrack on;
- optionally the resolution.

| | Windows | Linux |
|---|---|---|
| Config | `Documents\Egosoft\X4\<id>\config.xml` | `~/.config/EgoSoft/X4/<id>/config.xml` (newest) |
| How | Launcher check/fix buttons, backup `config.xml.bak-<stamp>` | `x4vr check` / `x4vr fix-settings [--auto]`, run by `x4vr-run`; single backup `config.xml.x4vr-backup`; atomic write |
| Display mode | Fullscreen, not borderless | **Windowed** when a resolution is set. Linux X4 honours `res_width`/`res_height` only in windowed mode; tiling window managers (Hyprland) resize the window, so float it: `x4vr-run` sets `SDL_APP_ID=X4VR` in VR (X4 uses SDL3), so a rule can match the VR window's class `X4VR` and leave 2D (`X4`) alone |
| Resolution | Optional | From `X4VR_RESOLUTION=WxH` (0 = leave alone), else a SteamVR background query (`VR_Init(VRApplication_Background)`, recommended density over the headset tangents), else `x4_resolution.txt` written by the mod. Rounded up to 1920x1080, 2560x1440, 2880x1620, 3200x1800 or 3840x2160 |

Both refuse to edit while X4 runs, and fix only keys present in the file.

**2D and VR settings apart (Linux only, `x4vr settings-mode`).** VR needs settings 2D play
doesn't want, so Linux keeps two copies next to `config.xml`:
- **VR launch (`vr`):** `config.xml` → `config.xml.x4vr-2d`, then `config.xml.x4vr-vr` (if
  any) → `config.xml`, then fixed for VR. A marker (`x4-settings.vr` in the state folder, with
  the config path) records the VR session.
- **X4 exits (`2d`):** `config.xml` → `config.xml.x4vr-vr`, `config.xml.x4vr-2d` → `config.xml`,
  marker removed. If X4 crashed, the next start of either kind does this first.
- The HUD extension is switched on for VR and off for 2D in X4's `content.xml`
  (`<extension id="x4vr_hud" enabled=...>`), so 2D play and 2D saves aren't "modified".
- `config.xml.x4vr-backup` stays the copy from before the first fix (uninstall can restore it).

---

## 13. Diagnostics

### Common

Request files in the capture directory are deleted when seen:

| File | Output |
|---|---|
| `trace.request` | `trace.txt`: 16384 events (P present, L/R read, l/r used, …; Linux adds M and U) |
| `submit.request` | `submit_trace.txt`: 2048 frames |
| `dump.txt` | `eye-0/1.raw` and `eye-dump.txt` |

`pair_stats.txt` every ~2 s: submits, stale/late per eye, fallbacks, timings. `hitch_ms` and
`hitch_every` simulate stalls.

### Windows only

- `debug-events.log` (crash_watch), `events.jsonl`, `frames.request` (probe grid), `turn.txt`,
  `head.txt`, `state.txt`, `ftgetdata_caller.txt`.
- The `X4VR_CAPTURE_*` capture runs.
- `code_scan_tests.exe <X4.exe>` checks the signatures offline.
- `tools/*.py` analysis scripts (`head_sync.py`, `eye_phase.py`, …).

### Linux only

- **Logs:** `x4vr.log` with pose, game-state, patch, sizing, pair and frame-half lines.
  `stderr.log` holds X4's own output.
- **Head watch** (`src/linux/head_watch.cpp`): hardware breakpoints and watchpoints through
  `perf_event_open` on the mod's own threads (no root needed; `perf_event_paranoid` ≤ 2).
  - `X4VR_WATCH_HEAD=1`: watchpoints on `VR::OpenTrack` `+0xd0/+0xd8/+0x100/+0x108`. Logs
    which code touches them and the position accessor's callers. This is how the camera input
    function was found.
  - `X4VR_WATCH_HEAD=2`: execution breakpoints on the camera input function's branch points
    (two sets of 4, alternating), plus the camera mode `[[0x3db6948]+0x3e8]+0x880`.
  - `X4VR_WATCH_HEAD=3`: watchpoints on the camera controller's head offset (`+0x5a0..+0x5bf`),
    re-opened when the controller changes.
  - `X4VR_WATCH_HEAD=4`: execution breakpoints at the offset readers' entries that sample the
    stack pointer and the 8 bytes there (the return address), to log who calls them.
  - Both record 10 windows of 20 s, labelled with the game state, and start with a self-test.
- **Phase 0 tools:** `x4vr vr-check`, `udp-send`, `elf-classes` (RTTI → vtables), and the
  probes `x4vr-probe-run`.

---

## 14. Settings and environment reference

### `stereo.txt` keys

The code defaults are in `runtime_bootstrap.hpp`. The shipped files override some of them.

| Key | Default | Meaning | Windows file | Linux file | Used on Linux |
|---|---|---|---|---|---|
| `stereo` | 1 | stereo, else mono | 1 | 1 | yes |
| `delay` | 2 | presents between pose and display (fallback without half) | 2 | 2 | yes |
| `recenter` | 0 | counter; a change recentres | 0 | 0 | yes |
| `ipd_scale` | 1 | eye distance scale | 1 | 1 | yes |
| `pos_scale` | 3.6 | head position scale | 3.6 | 3.6 | yes (not calibrated on Linux) |
| `yaw_gain`, `pitch_gain` | 2.1177 | 180/85 | | | yes |
| `roll_gain` | 3.14159 | | | | yes |
| `predict` | 0.035 | pose prediction (s) | | | yes |
| `game_tan_y` | 0.8675 | X4 FOV 120° | | | yes |
| `valve_bounds` | 1 | v-flip of eye bounds | | | yes |
| `eye_from_half` | 0 | use the frame half | 1 | 1 | yes |
| `half_xor_render` / `half_xor_present` | 0 / 0 | half → eye mapping | – / 1 | – / 0 | yes |
| `eye_at_use`, `half_xor_use` | 1 / 1 | eye at use | | | no (`X4VR_EYE_AT_USE` env) |
| `walk_at_use`, `half_xor_walk`, `delay_walk` | 1 / 0 / 1 | on foot | | | `delay_walk` only |
| `shared_pose` | 1 | pair matching | – | 1 | Linux only |
| `async_submit`, `submit_budget_ms` | 1 / 2 | submission thread | | | yes |
| `pace`, `release_late`, `pair`, `pair_wait`, `submit_pose`, `handoff` | 1/1/0/1/1/0 | pacing / submit | | | yes |
| `theater`, `theater_distance`, `theater_width` | 1 / 2 / 2.2 | theater screen | | | yes |
| `cursor`, `cursor_distance` | 1 / 5 | cursor | | | `cursor` only |
| `turn_comp` | 1 | turn compensation | | | no (not needed with `shared_pose`) |
| `synth`, `synth_rate`, `synth_base`, `synth_alt` | 0 | calibration poses | | | yes |
| `hitch_ms`, `hitch_every` | 0 / 90 | stall simulation | | | yes |

### Linux environment variables

| Variable | Effect |
|---|---|
| `X4VR_DIR` | settings and log directory (default `~/.local/state/x4vr`) |
| `X4VR_ALWAYS=1` | every Steam launch starts in VR (default: only launches from the `x4vr` menu) |
| `X4VR_STEAM_OVERLAY=1` | keep Steam's overlay |
| `X4VR_FIX_SETTINGS=0` | don't touch X4's `config.xml` |
| `X4VR_RESOLUTION=WxH` / `0` | X4 resolution, or leave it alone |
| `X4VR_NO_STEAMVR_QUERY` | skip the SteamVR resolution query |
| `X4VR_GAME_DIR`, `X4VR_GAME_ARGS` | game folder; X4 arguments |
| `X4VR_NATIVE_SIZE=1` | eye images at X4's density |
| `X4VR_THEATER_OVERLAY=1` | SteamVR overlay theater |
| `X4VR_HOTKEYS=0` | no hotkeys |
| `X4VR_EYE_AT_USE=0` | no eye-at-use hook |
| `X4VR_PATCHES=0` | no code patches |
| `X4VR_WATCH_HEAD=1` … `4` | head watch diagnostics |
| `X4VR_OPENTRACK_PORT` | default 4242 |
| `X4VR_OT_YAW/_PITCH/_ROLL/_X/_Y/_Z` | axis sign and scale |

---

## 15. Status of the Linux port

**Working:** stereo in the cockpit on the Steam Frame, with eye at use and shared pose (no
ghosting), the frame half (no eye swaps or HUD doubling), the theater screen for menus, the
cursor, hotkeys, SteamVR recentre and "Exit game", sizing from SteamVR, and the HUD distance
(with the `.xpl` shrink).

**Backward clamp patch:** confirmed in the headset (leaning back works).

**Confirmed on foot (2026-10-05):** walking detection (stereo with `theater=1`), the on-foot
timing (less lag) and the on-foot eye flip.

**Working on foot (2026-10-05):** head tracking with both Windows on-foot patches ported.

**Done:** the HUD size (`.xpl` scripts, section 12).

**Open:**
1. OpenXR (setups without SteamVR).
2. Build the Windows target once, to confirm it is unchanged.

**Not needed for now (user's call, 2026-10-05):**
- `pos_scale` and gain calibration: the cockpit looked slightly large, but it is fine in use.
- Snap turning on foot: turning the mouse more gently is enough.
- Turn compensation: not needed with `shared_pose` (section 9).
- Rival trackers: the Linux build has only OpenTrack. Don't run the opentrack app (UDP 4242)
  alongside.

---

## 16. Address reference

The Linux mod finds these by pattern (section 8); the Linux addresses are X4 9.00's, for
reading disassembly. The head-watch diagnostics (`X4VR_WATCH_HEAD`) still use fixed 9.00
addresses.

| What | Windows 9.00 (RVA) | Linux 9.00 |
|---|---|---|
| Frame half global | `0x6b66280` | `0x72a0fa0` (reader `0x218e220`) |
| Head-tracker input → camera function | bridge `0x9fd870` | camera input `0xfeb000..0xfed400` |
| Backward clamp branch | `0x9fdb4d` (`jae`) | `0xfeb73f` (`jbe`) |
| On-foot zeroing branch | `0x9fd9ae`+101 | `0xfec077` (`je`) |
| Camera::GetOffset | `0x97a300`; exit `je` at `0x97a418`, offset block `0x97a5a8` | `0x1929f70`; exit `je` at `0x1929ffd`, offset block `0x192a284` |
| Tracker position accessor | FreeTrack tracker slot `0x108` (`0xf377b0`) | `VR::OpenTrack` slot 34 `0x1a0dda0` |
| Tracker still check | slot `0x28` (`0xf376c0`) | none (slot 6 is a stub) |
| Tracker pick (rival trackers) | `0xfa34fc`, `0xfa353c` | – |
| Player global | – | `0x3db6948` (camera controller `+0x3e8`, mode `+0x880`) |
| Head-tracking manager | `[manager+0x3b8]` active tracker | `0x3db8938` (active tracker `+0x148`) |
| Camera manager | (signature) `+0x3d0` camera | `0x3daab80` |
| Camera controller update | – | `0x1933d00` (head path `0x1935ac1`; head offset stored at controller `+0x5a0` position, `+0x5b0` rotation) |
| `VR::OpenTrack` vtable / type_info | – | `0x3c62520` / `0x3c61bb0` |
| `U::HeadTrackerCameraBridge` (unused for the cockpit) | – | vtable `0x3b1c240`, object `0x3e60820` |
