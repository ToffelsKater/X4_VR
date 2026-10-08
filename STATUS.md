# Implementation status — 2026-09-26 (working VR; async submission)

**Native stereoscopic, head-tracked VR works in retail X4 9.00 on the Varjo Aero.**
The user confirmed a stable world (no swimming or warping on look, pitch, roll or
lean), correct depth and scale.

## Working pipeline

1. **Head pose into X4's own camera path.** X4 has built-in FreeTrack support. It
   reads `HKCU\Software\FreeTrack\FreeTrackClient\Path` and loads
   `<Path>\FreeTrackClient64.dll` (`FTGetData`, `FTGetDllVersion`, `FTProvider`).
   Our `FreeTrackClient64.dll` (`src/freetrack_client.cpp`) returns the OpenVR seated
   head pose from the shared runtime (`x4_openvr.dll`). Because this goes through the
   game's own head tracker, culling, object transforms, lighting and shadows all
   follow. Earlier attempts that patched downstream camera records only moved lighting.
2. **Stereo by alternate-eye rendering (AFR).** Each game frame renders one eye: the
   pose source adds that eye's `GetEyeToHeadTransform`. The eye is chosen per frame,
   never per call (X4 calls `FTGetData` several times per frame). With `eye_from_half=1`
   (default) it comes from X4's per-frame render-data half (global RVA `0x6b66280`,
   signature-checked at `0x77a47f`), which follows a frame from pose to present whatever
   the queue depth; otherwise from the present counter plus `delay`.
3. **Presentation.** The Vulkan layer (`src/observe_layer.cpp`, presenter section)
   copies each presented swapchain image into that eye's texture (a ring of 3 per eye).
   The textures are padded to span each eye's whole frustum at the game's
   pixels-per-tangent. A **submission thread** on a layer-private queue (one extra queue
   requested at `vkCreateDevice`) runs `WaitGetPoses` → `Submit(both eyes, bounds,
   Submit_TextureWithPose)` every compositor frame with each eye's
   newest finished image (waiting at most `submit_budget_ms`, default 2, for an in-flight
   copy; SteamVR on the Aero latches ~3.2 ms after `WaitGetPoses`, and the old 8 ms budget
   let copies that were a few ms late push Submit past it, a missed frame and a flash). The
   game's present only copies, then waits for the thread's frame tick. So a late game
   frame repeats the previous image instead of reaching SteamVR late. `async_submit=0`
   restores inline submission from the present.
   Since 2026-09-28 (docs/STUTTER_RESEARCH.md, fix plan items 1-5) nothing between
   `WaitGetPoses` and `Submit` waits without a deadline: OpenVR's copies of earlier
   submissions are tracked by a fence per submission (their images stay held) instead of
   being waited for; the lock shared with the present hook is taken with the submit budget
   (on timeout the last frame is sent again, `resubmit` column); the present hook releases
   it before the driver's present. The thread runs at time-critical priority. All
   diagnostic file I/O (stats, `stereo.txt` reload, request files, `events.jsonl`,
   `head.txt`) runs on one background thread in `x4_openvr.dll`. Shader modules, pipelines
   and descriptor layouts are logged only with `observe.ps1 -Shaders` (needed by
   `reflect_capture.py`/`analyze_uniforms.py`); otherwise only pipeline creations over 2 ms
   (`slow_pipelines`). Live keys: `handoff=1` restores `PostPresentHandoff` after Submit;
   `release_late=0` makes a game frame that ends after a tick wait for the next one again
   (it halved 80-89 fps to 45; `x4_late` column counts released frames). Default 1 since
   2026-10-01: with 10 ms CPU load per frame, 45 vs 67 fps on OpenVR and OpenXR
   (`flicker_ab.py … cpu`; the hitch injector busy-waits, `Sleep` rounded to the timer).
   `submit.request` writes the thread's last 2048 frames to `submit_trace.txt`
   (`tools/submit_trace.py` requests and summarizes it; `flicker_ab.py … stutter` A/Bs the keys).

## OpenXR backend (2026-09-27, experimental)

`X4VR_RUNTIME=openxr` (the launcher's *VR runtime* box) switches `RuntimeBootstrap` to
`src/openxr_runtime.cpp`; everything else in the pipeline is shared. `XR_KHR_vulkan_enable` (v1)
supplies the extensions the layer adds to X4's instance and device. The session is created at the
first present on X4's device and the layer's private queue, and destroyed in `vkDestroyDevice`.
Each frame the submission thread copies both eye images into runtime swapchains (8-bit UNORM goes
to its SRGB twin) and sends each eye at the head pose it was rendered with; eyes without a pose
(theater mode fills only eye 0) use the runtime's own pose. Theater screen and cursor are quad
layers. The runtime reaches our `vkGetDeviceQueue` hook inside `xrCreateSession` while the present
holds the presenter lock, so the queue map has its own lock. Tools: `openxr_probe` (runtime
capabilities), `runtime_smoke` (drives either backend without X4). Tested on Varjo Base's runtime
and SteamVR's OpenXR runtime (probe only).

## Flicker investigation (2026-09-26)

Symptom: dark/grey flashes (first seen in one eye, later both) whenever X4 fell below a
steady 90 fps; "extreme ghosting" under an injected 35 ms stall. SteamVR's own reprojection
and motion smoothing are off on this setup (Varjo driver defaults), so every late app frame
reached the display as a flash. Ruled out by measurement: eye association (0 half-phase
slips), texture reuse (ring of 3 made no difference), per-eye submit order and texture
identity, submitted poses, and X4 content (per-frame brightness probe: no dark frames).
SteamVR's cumulative dropped/timed-out counters did not register these gaps at all.
Fixed by asynchronous submission: with a 35 ms stall injected every second, inline
submission showed 44 ms gaps and was unplayable; async stayed at 11.6 ms and was clean.
Tools: `tools/flicker_ab.py` (timed beep A/B of stereo.txt modes, prints `pair_stats.txt`),
`hitch_ms`/`hitch_every` (stall injector), `frames.request` (last 1024 frames' 8x8 patch
grid → `frames.raw`/`frames.txt`).

## X4 quirks found and compensated (all measured, see tools/vr_calibrate.py)

| Quirk | Where | Compensation |
| --- | --- | --- |
| Tracker EMA smoothing, alpha = 1/N, menu minimum N = 5 | FreeTrack vtbl `+0x1c0`, field `+0x110` | call exported `SetActiveHeadTrackerHeadFilterStrength(1)` |
| Yaw/pitch normalized by pi, then scaled by 85 deg (1.4835) | `0x97fe50` uses `[[0x6cf1538]+0x678/0x67c]` | `yaw_gain = pitch_gain = pi/1.4835 = 2.1177` |
| Roll passed as angle/pi without scaling | same | `roll_gain = pi` |
| Position: mm × 0.2 / 180 → clamp ±1 → × 0.25 m | `0xf377b0`, range `0xf36330` | `pos_scale = 3.6`, recentre so the head sits near 0 |
| Backward head position (z > 0) zeroed | bridge `0x9fdb4f` | in-memory `jae`→`jmp` at `0x9fdb4d`, signature-checked |
| Position applied in the ship frame (not head-rotated) | measured | send seated/tracking-frame positions |
| X axis mirrored, pitch inverted | measured with the user | `X4VR_FT_X = -1000`, `X4VR_FT_PITCH = -1` defaults |
| Background throttle to ~11 fps when unfocused | game option | launch with `-nocputhrottle` |
| Render-to-present pipeline = 2 presents | star-field ramp test | `delay = 2` |
| OpenVR raw top/bottom flipped vs texture v | Valve convention | `vMin = 0.5-0.5*b/tanY`, `vMax = 0.5-0.5*t/tanY` |
| Startup loader-lock deadlock risk from VR_Init/VR_Shutdown churn | overlay hooks | OpenVR runtime pinned for the process lifetime |

Verified numerically against live camera records and eye-texture dumps:

- Rotation: 1:1 on every axis, with correct Euler order.
- Translation: 1:1 in the ship frame, independent of head yaw.
- Image projection: pinhole, f = 572 px/tan (star-field fit).

## How to run

User-facing instructions (NVIDIA/in-game settings included) are in `README.md`. Players use
`X4VRLauncher.exe` in the repository root (built from `tools/launcher/`). It keeps profiles in `config/profiles/`,
writes the live `stereo.txt` (config/stereo.txt defaults plus the profile's keys), checks and
fixes X4's `config.xml`, and starts `crash_watch` → X4 with the same environment as
`observe.ps1`. Its logic is tested in `tests/launcher_tests.cpp`. Scripted route:
`scripts/install.ps1` (bootstrap, build, FreeTrack registry path, default `stereo.txt`), then
`scripts/play.ps1` (runs `observe.ps1 -Target Game -OpenVRBootstrap -CrashWatch -GameArgs
"-skipintro -nocputhrottle"`). Recentre with Ctrl+F12 or by bumping `recenter=` in
`reports/captures/stereo.txt`. Resolution: fullscreen 3840x2160 via NVIDIA DSR 4x gives
~1245 px/tan at a steady 90 fps (RTX 3090, DLSS/AA off). Swapchain resizes rebuild the eye
textures live. Game FOV must be at maximum (120 deg = `game_tan_y` 0.8675).

Calibration tools (X4 running, cockpit loaded): `tools/vr_calibrate.py --pid <pid>`
(translation, rotation, FOV, latency), `tools/orient_check.py <pid>`, `tools/star_fov.py <pid>`.
Synthetic poses: `synth=1`, `synth_base=x y z yaw pitch roll`, `synth_alt=...`, `synth_rate=deg/frame`, `pace=0`.
Eye dump: create `reports/captures/dump.txt`, which writes `eye-0.raw`, `eye-1.raw` and `eye-dump.txt`.

## Known limits / next work

- AFR gives 45 Hz per eye, and stale-eye parallax is not reprojected. Temporal AA/DLSS history
  crosses eyes.
- Pair mode (`pair=1`: both eyes rendered back to back, 90 Hz per eye) needs X4 at 180 fps.
  At 1440p on the RTX 3090 it is GPU-bound: the newest image often misses the submit budget
  (up to ~27% of frames repeat one eye) and SteamVR's own frame timing stalls, so flashes
  remain. Needs GPU headroom (lower resolution/settings) before it is usable.
- Eye images come from the swapchain. At 4K via DSR they reach ~86% of the Aero's native
  pixel density; each eye uses ~70% of the frame width. HUD is part of the image.
- X4's max FOV (tan 0.8675 vertical) leaves a black band at the bottom (the Aero needs 1.116).
- Needs the launcher (PATH to `build/Release`, Vulkan layer env). A normal Steam launch
  would fail to load `x4_openvr.dll` dependencies.
- **Cursor bug (2026-09-28, OpenVR; fixed):**
  after a while of opening and closing menus the cursor went invisible. Cause: every
  `SetOverlayRaw` keeps one of SteamVR's memory blocks, and the cursor overlay uploaded its
  image on every shape change (arrow, hand, ...). At ~200 blocks vrclient refuses more
  ("201 blocks are already outstanding", `D:/Steam/logs/vrclient_X4.previous.txt`); every upload
  then failed and the layer retried each frame (18,651 failures in one session). Fix: one
  overlay per cursor image (`RuntimeBootstrap::show_cursor`), uploaded once, then only shown
  or hidden; each upload is logged ("cursor image N uploaded"). Verified: 5 minutes of menus,
  5 uploads in the whole session, 0 failures, cursor visible throughout.
- **Black flicker on foot (issue #10), checked 2026-10-08 with 9a4393a:** the user saw no
  black flash in 47 s of fast mouse turns on foot. No camera-space view reached the turn
  compensation. events.jsonl still had 287 `turn_dropped`, all real turns: 30.0 to 42.6 degrees
  between consecutive frames, forward vectors chaining from one event to the next. Those frames
  went out uncompensated. The limit was 30 degrees then and is 60 now, above those flicks and
  below the 90-degree wrong match seen on 2026-10-07. Run with 60 (53 s of hard flicks on foot,
  X4 at 90 fps): every submit compensated, mean correction 16 to 25 degrees per 2 s, largest
  55.4; 2 `turn_dropped` (73.6 and 79.1 degrees), both real turns where one eye was on its
  older image. User: no black flash, no black edge, nothing to complain about.
- **UI scale cap located (issue #18, 2026-10-08):** the cap is in X4's code, not in config.xml
  or the Lua slider alone. 9.00: the stored scale is the float at RVA 0x2f4b6f0 (config key
  `uiscale`). `GetUIScaleFactor` (0xaff590) and `GetUIScale` (0xaff4e0) both return
  max(minimum, stored <= 1.8 ? stored : 1.8): `comiss stored, [1.8]`, then
  `cmovbe rax, rcx` (48 0f 46 c1) picks the stored value or a stack copy of 1.8
  (`mov dword [rsp+8], 0x3fe66666`). `GetUIScaleFactorRange` (0xb07d80) returns the slider range
  with the same 1.8. `SetUIScaleFactor` (0xb01a20) only rejects values <= 0. So a higher value in
  config.xml is clamped on read. A way past it: replace the two `cmovbe` with `mov rax, rcx; nop`
  (48 8b c1 90) at runtime and set `uiscale` in config.xml or through `SetUIScaleFactor`. Open:
  whether X4 reads the scale before the mod's first FTGetData (the patch may have to run from
  the Vulkan layer), whether the UI is usable above 1.8, and the same bytes in 8.00. Not tried
  in the game yet. Original report: a player found that X4's
  in-game UI scale at 1.8 makes the UI big enough to read in the headset, and wants to go
  higher, but 1.8 is the slider maximum. The cap is X4's, not ours (no UI scale setting in
  the layer). Look at raising the limit, for example a UI mod that lifts the slider maximum,
  or our own scale on the HUD/menus.
- **Added, headset-checked on the Aero — external views (F2/F3) on a view-filling screen,
  issue #17 (2026-10-08):** `external_vr=1` (launcher checkbox). Measured first: while piloting,
  X4 still reports ship controls in F2/F3 (`ship=1`, state.txt now logs `external=`), and the
  theater comes on as "no head pose": the external camera never reads the tracker position, so
  eye at use records nothing. head_sync in F2: camera/head rotation ratio 0.000, eye offset 0,
  so X4 ignores the head there and stereo is not available without patching X4's external
  camera. Two ways to send the picture as eye images failed in the headset: with the head pose
  it lagged behind the head (X4 at ~70 fps), and from one fixed pose each eye ran out of picture
  on its own side when looking left or right (each eye image only covers its own frustum).
  Shipped: the theater path, with the screen sized to X4's field of view
  (width = 2 * `external_distance` * `game_tan_y` * aspect, default 20 m away). FTGetData
  publishes `IsExternalViewActive` without cutscene or fullscreen menu
  (`publish_external_view`), and the submission thread sizes the screen and the cursor from it.
  User: fills the view, same area in both eyes, cursor right, map and cockpit unchanged.
  Original request (player, 2026-10-04, Quest 3, OpenXR): early builds stayed in VR in the F2/F3 external camera ("floating in space", good
  for watching fights). Since theater mode (6a9611f) any view without ship controls goes to the
  theater screen: `theater=1` tests `!(walking || at_ship_controls())` in
  `src/freetrack_client.cpp` (FTGetData). Wanted: a setting (launcher checkbox + stereo.txt)
  that keeps theater for fullscreen menus and cutscenes but leaves external views in stereo.
  Today's only workaround is `theater=0`, which also puts the menus in stereo. Check first how
  to tell an external view from a cutscene (`IsFullscreenCutsceneActive` is already read for
  the debug line), and that head tracking still moves the external camera.
  Same player: OpenVR on Quest 3 showed "scuba masking" (early builds: left eye lagging on
  head turns); OpenXR works well. Likely the Steam Link issue, see the README note to use
  Virtual Desktop.
- **Added, untested on Steam Link, measured on the Aero — right-eye jitter fix, `shared_pose` (issue #4, 2026-10-08):**
  ported from the Linux port, opt-in (launcher checkbox, default off). FTGetData gives a
  right-eye frame the head pose of the left-eye frame before it (`pair_head`), and the
  submission thread picks images with equal poses (`match_pair` in `math.hpp`, tested in
  backend_tests). Every 4000 stereo submits events.jsonl gets a `shared_pose` line with
  `stepped_back` and `differing` (Linux on the Frame: about 50% and 1%). Measured on the Aero in
  a scene where X4 misses 90 fps (~168 late frames and ~145 fallbacks per 2 s): the port alone
  gave 44% stepped back and 38% differing, because an eye that falls back to its older image
  breaks the pair. Now the other eye steps back too if its older image has that pose: 50%
  stepped back, 3 of 4000 differing. Windows only; the Linux layer does not have this step.
  User on the Aero: cockpit and on foot look normal with it on. On foot turn compensation
  still changes one eye's pose during mouse turns; those pairs count as differing.
  Needs a Steam Link or Steam Frame tester (coleblooded1 offered in #4).
  Plan as written 2026-10-05:
  Cully-Curwen (Linux port, github.com/Cully-Curwen/X4_VR_Linux) confirmed the cause: SteamVR's
  streaming link reprojects both eyes with the left eye's pose. Submitting both eyes with the
  right pose moved the ghosting to the left eye. Their fix needs no reprojection shader: one head
  pose per eye pair. (1) FTGetData keeps returning the previous head pose while X4 builds a
  right-eye frame (the eye is already known at `src/freetrack_client.cpp:148`); the eye offset is
  still added at use. (2) The submit thread (`src/observe_layer.cpp`, near the `submit_pose`
  code) only submits pairs with identical poses; if one eye is a frame ahead, it submits that
  eye's previous image instead (needs a one-image history per eye). On Steam Frame: ghosting
  gone, ~50% of submits stepped back, ~1% still mismatched. Their reference:
  `src/linux/pose_sender.cpp` and `compositor_loop` in `src/linux/vr_layer.cpp`. Make it an
  opt-in setting (`shared_pose`, default off): it costs one frame of latency on the eye that
  steps back and halves head sampling, and the Aero and Virtual Desktop do not need it. Test
  with a Steam Link user (dshaughnessy5, the reporter, uses Quest + Steam Link). Cheaper than
  the planned right-eye warp (1-2 days).

---

# Earlier history (2026-09-24)

# Implementation status — 2026-09-24

Goal remains: native, geometrically stereoscopic VR in retail X4 using OpenVR,
targeting the user's Varjo Aero. Engine source is not available in this workspace.

## Evidence

- Installed `version.dat`: `900`.
- X4.exe SHA-256:
  `19750a6563889a970f434b5566eb396c6b2dc29ff814bd3e336f838176ad6891`.
- `IsVRMode`, `IsVRVersion`, and VR controller-active exports share RVA `0x000b38c0`:
  `32 c0 c3` (`xor al,al; ret`). They are constant false in this build.
- `SetVRWindowMode` and other VR setters share RVA `0x000963b0`:
  `c2 00 00` (return without work).
- Vulkan is imported; OpenVR is not imported. Selected OpenVR runtime/interface
  literal searches find no matches. This does not prove the absence of every hidden
  rendering path, but the VR settings exports themselves are not an activation path.
- Full static report: `reports/x4_binary.json`. Regenerate with
  `python tools/inspect_x4.py ../X4.exe --output reports/x4_binary.json`.
- X4Native reference revision `fc4b8e26d74365ca332c3b0749eb9bbe167c76a1` provides native
  DLL loading and game-function hooks, but no verified per-eye scene rendering API
  was found. No X4Native components were installed into the game.
- The inspected x4-triple-screen project changes Windows window behavior and size;
  its own limitations state it does not implement multiple camera projections.
- Nine local renderer sources extracted successfully to
  `reference/local-render-sources/`, with source catalog offsets and SHA-256 hashes
  in `extraction.json`. They are inspection copies, not active overrides.
- `common.glsl` declares `BLOCK_BUFFER_BINDING_SLOT_CAMERA` at binding 0 of the
  symbolic `BUFFER_BINDING_SLOT_CAMERA` descriptor set, with view/projection/inverse
  and temporal matrices. `common_vert.glsl` uses both precomputed per-object
  world-view-projection and shared view-projection for instancing. Updating only one
  global matrix cannot cover every geometry path.
- Live X4 observation now resolves those symbolic sets: camera = set **1**, binding
  **0**; world/object = set **3**, binding **0**. Matrices are column-major with a
  16-byte matrix stride. Camera view/projection/view-projection/inverse-view offsets
  are 0/64/448/512 bytes; world WVP/world/previous-WVP offsets are 0/64/128 bytes.
  See `reports/x4_shader_reflection.json` for all observed member offsets.
- Captured game process 40156 completed normally (exit 0); device and instance
  destruction are in the trace. Its capture contains 446 modules (229 unique), 1134
  graphics pipeline events, 16 compute pipelines, and successful presentation through
  sampled frame 4200. Capture: `reports/captures/process-40156-134345817889829613/`.
- Optional bounded mapped-memory tracing now works in retail X4. Process 42904
  completed normally (exit 0); its main capture is
  `reports/captures/process-42904-134346636046115899/`. It contains 44 CPU snapshots
  through present count 1680, with 1792-byte camera and 768-byte object ranges.
  Camera samples 8–11 satisfy `projection * view = viewprojection` and
  `view * inverseview = identity`; other sampled camera matrices are zero.
  Sample 10 errors are approximately 1.49e-9 and 5.96e-8 respectively.
  Its projection is positive-Z-forward, Y-flipped, infinite reversed depth with
  near coefficient 0.1. The 90-degree symmetric projection could be an auxiliary
  view; this is **not** a verified cockpit/gameplay convention or unit scale.
- `reports/x4_uniform_analysis.json` includes the matrices and native stack RVAs.
  Live camera binds pass through `0x1216770` (range ending `0x12184cc`). Static
  inspection shows this routine clearing a 0x700-byte temporary block, filling it,
  copying 0x700 bytes to mapped storage at `0x121843b`, then binding at `0x1218463`.
  It is a camera-uniform assembly/upload candidate, not a verified stereo scene API.
  Static view loads at `0x1216f85..0x1216fb2` copy from a candidate camera object
  at `[R14+8]`, offset 0x40, into uniform offset zero. R14 is the first argument
  preserved from RCX. Pointer identity and lifetime still require live verification.
  Full bounded disassembly: `reports/camera_bind_function.asm`.
- No scene-render entry point, gameplay camera units, or per-eye rendering has yet
  been verified. Unwind ranges can describe fragments rather than full functions.
- Native register reconstruction is now verified at return RVA `0x1218468` in
  process 30648 (normal exit 0). Capture:
  `reports/captures/process-30648-134346640045473046/`; analysis:
  `reports/x4_native_camera_analysis.json`. Of 24 camera samples, eight initial
  samples lack this upload frame; the remaining 16 recover readable source camera
  objects. All 16 stack-local 0x700-byte blocks match the bound uniform exactly.
  The object's matrix at offset 0x40 matches uniform view; offset 0 matches inverse
  view. `YFlip * camera[0x1c0] * camera[0x140]` matches uniform projection with zero
  measured error. Some camera objects are stack-local, so pointers cannot be kept
  past the observed call. This is a verified uniform assembly path, not yet proof
  of the gameplay camera or a safe render-twice entry point.

## Verification performed

- Windows x64 Release build succeeded with MSVC 19.39 and Windows SDK 10.0.26100.
- 50 checks passed: asymmetric frustum boundaries, normal/reversed Vulkan depth,
  rigid pose inversion, metre/IPD preservation, canted eyes, recentering, rejection
  of invalid poses and image metadata, and calls made before session initialization.
- Real OpenVR presence query: runtime installed = true; HMD present = true.
- Real scene initialization and Vulkan instance extension query succeeded.
- After the user enabled tracking, the live pose query succeeded: runtime model
  string `hedy`, recommended extent **3292 x 2820 per eye**, eye positions approximately
  `(-0.031375, 0.00280214, 0.000080742)` and
  `(0.031375, -0.00280214, -0.000080742)` metres relative to the seated origin.
- A transient OpenVR initialization error 306 was traced to SteamVR blocking the
  Varjo driver after a safe-mode event. The runtime subsequently restarted while work
  continued; a later scene initialization and tracking query worked.
  No SteamVR/Varjo settings were changed by this project.
- Live OpenVR image submission is established by the standalone test below;
  in-headset visual verification and X4 eye-image submission remain unproven.
- The separate Vulkan submission diagnostic now submits 90 gray frame pairs
  successfully with RGBA8 and BGRA8 using automatic color space on the Aero.
  It is not visual verification or game integration.
- Vulkan observation smoke test passed on the real RTX 3090: instance/device creation,
  shader capture, descriptor update/bind, compute dispatch, queue completion and cleanup.
- Three reflection parser tests pass, including rejection of malformed instruction
  streams, reading non-default offsets/sets, and refusing to infer camera identity
  from stripped shaders.
- 18 mapped-memory safety checks pass, including nonzero offsets, bounded partial
  mappings, stale handles, unmapping/freeing, pool reset, and inaccessible pointers.
  A real GPU smoke capture reproduced its 64-byte matrix fixture exactly.
- Eight CTest suites pass: backend, reflection, mapped-memory safety, uniform analysis,
  native camera safety, camera sampling, external crash recorder, Vulkan extension
  merge contract. The recorder test
  checks first/second-chance exception handling and the saved dump's exception code.
  Native tests reconstruct a live caller and reject an
  unsupported executable. Real X4 tests additionally establish register/pointer
  correspondence, which standalone tests cannot establish.

## Next implementation steps

1. Identify the gameplay camera among the verified upload source objects.
   User-loaded process 5736 reached gameplay, but the old sampler kept selecting
   a zero UI pass. The user confirmed that X4 then crashed unexpectedly; no normal
   teardown, new local crash dump, or corresponding recent Windows X4 fault record
   was found. Process 17548 then also crashed; its external recorder captured
   `captures/debug-8e727a2a6e0d4043ad17bf4a14e5335f/crash-6.dmp`.
   Exception: access violation 0xc0000005 in X4.exe RVA 0x123b9e3, thread 4604
   (camera/render samples were thread 41708). Instruction `cmp byte ptr [rcx+rax],0`
   attempts to read RCX=0x6b6d5f31305f7265, RAX=0: an invalid, text-like address.
   Surrounding static code identifies XML merge/patch processing. A 64-byte filename
   string's pointer is on the stack, but its heap contents were not included in this
   small dump. The saved return address, located using this routine's verified
   prologue, is RVA 0x595d80; it immediately follows a call to 0x123b570.
   Analysis: `reports/x4_crash_analysis.json`, `reports/crash_function.asm`.
   Immediate fault location is established; the upstream cause is not. Do not
   assume tracing is innocent or blame installed mods. The user has been asked
   whether this save is stable under a normal Steam launch. The user confirms it
   normally loads reliably, so tracing must be treated as a regression until isolated.
   A new comparison run uses `-Target Game -CrashWatch` WITHOUT `-Memory` or
   `-NativeCamera`: API observation only, no mapped-memory sampling/native unwind.
   Debug reports: `captures/debug-8cc7b011f162463ba9d1af6d8d3136ae/`.
   Process 41400 remained responsive through 9,000+ presentations and the user
   reported it runs. The user clarified this is a NEW GAME, not the previous save.
   This establishes a gameplay baseline only, not a controlled reproduction;
   The user explicitly chose to abandon the old-save comparison and continue VR
   implementation with the new game. Do not ask for that save again; its corruption
   remains a hypothesis, not a diagnosis, and the tracing regression is unresolved.
   Review found ordinary memory sampling also called CaptureStackBackTrace.
   That is now separately opt-in via `-StackTrace`; memory-only mode does no
   stack walking, while `-NativeCamera` still uses its explicit register unwind.
   New capture headers record all modes. No change is applied to the running game.
   All four tracing mode combinations passed the real-GPU compute fixture after
   separation. This does not establish native-camera tracing stability in gameplay.
   A tested native sampler update skips zero/nonfinite views, accepts up to 16
   distinct matrices per 120 presents, bounds attempts to 512 per interval and
   camera records to 4096 per process. Obtain a new gameplay capture before
   inferring the player-camera convention. Native stack
   reconstruction now recovers R14 and its camera pointer without changing code;
   it is gated to the exact executable and all copies use checked process reads.
   The user prefers loading the cockpit themselves. Do not close their loaded
   session without coordinating. Earlier short test sessions were our own.
2. Identify and validate the engine's camera-update and scene-render entry points for
   the exact executable hash. Native extension loading alone is insufficient.
3. Implement an adapter that runs both scene views from one simulation tick, with
   correct eye-specific render targets, culling, motion vectors, and post-processing.
   Hook Vulkan instance/device creation early enough to enable compositor extensions.
   Initial hook integration is now built behind `-OpenVRBootstrap`: a shared
   initialization-only OpenVR scene session queries instance/device extensions, appends
   them without dropping game requests, and verifies the chosen headset GPU.
   It is initialization-only (no queue/compositor submission or scene rendering)
   and passed real RTX 3090 startup/compute/normal teardown in process 5352,
   debug reports `captures/debug-77d1d5daf1114aaf984e3884dd1f21a5`.
   Worker-thread extension queries hung (last marker: instance extensions begin).
   Queries now run serialized on Vulkan's calling thread, with private bootstrap-only
   ownership adoption; the rendering Session's ownership rule is unchanged.
   GPU comparison uses device UUIDs, not loader/next-layer wrapped handle equality.
   Abrupt host exits do not invoke runtime shutdown from DLL static destruction.
   Eight contract suites and all four passive GPU modes pass in the main `build`.
   User authorized closing X4; process 41400 exited normally with code 0.
   New X4 startup run: `-Target Game -OpenVRBootstrap -CrashWatch` (no memory/native
   tracing), reports `captures/debug-675349af2d294d0ab35fdd6883dd9665`.
   Process 6876 successfully created its device with the original plus runtime
   extensions, passed GPU identity verification, presented frames, and reached the
   intro after 67 seconds. Capture: `process-6876-134346664913626457`.
   The user confirmed being in the cockpit. This run reached 12,000 recorded
   presentations and then exited normally (code 0) at 21:58:58 on September 23,
   with device/instance destruction and VR_Shutdown recorded. No X4 process is
   currently running. Do not relaunch solely to repeat the pending look test.
4. Connect headset movement to the cockpit camera with verified units and handedness.
   CPU-side `make_x4_eye_camera` is implemented, not injected: base view/inverse,
   native projection and jitter are read at the verified offsets. It converts
   runtime eye poses using a Z-basis reflection, explicit units/metre, asymmetric
   per-eye projection and observed infinite reverse-Z depth. It preserves the
   captured base view instead of rebuilding its large translation; matrix products
   accumulate in double precision. Unsupported input is rejected.
   The two global records were read externally from live process 6876 without
   writes, thread suspension or stack tracing. Reports: `camera-peek-6876-initial`
   and `camera-peek-6876-followup`. Both show positive-Z infinite reverse depth,
   near coefficient 0.1, and inverse-view positions around (-7225,267,31451).
   Absolute 1e-3 inverse-error checks were too strict at these translations;
   componentwise float-error bounds accept the observed pairs and reject a tested
   one-unit mismatch. Equal repeated reads are not atomicity guarantees.
   Eye-camera tests pass on synthetic inputs and both live native records.
   Physical scale and cockpit ownership remain unverified; the user was asked
   to confirm cockpit readiness for a controlled left/right camera-look test.
   Initial work built separately in `build-next`, leaving the active game untouched.
   After the game exited, the main build was updated successfully: all ten suites
   pass. The eye-camera test additionally passes on all 16 centered-cockpit records,
   including reproduction of native finite projection and finite projection*view.
   The user confirmed cockpit readiness and a centered baseline was captured in
   `camera-peek-6876-cockpit-center`. The left-look response was not received
   before the game exited; camera ownership remains unverified.
   Further static analysis identifies +0xc0 as the finite forward-depth projection,
   +0x100 as that projection times view, and +0x180 as reverse-depth projection
   times view. +0xcc0/+0xcc4 contain near/far distances (0.1/400000 in this capture).
   `make_x4_eye_camera` now also constructs the finite per-eye projection and six
   normalized world-space visibility planes. Far-plane construction uses scalar
   distances to avoid float-coefficient cancellation. Lateral planes include the
   current jitter margin; binocular sphere visibility accepts either eye.
   This remains CPU-side code, NOT a replacement for native X4 culling/draw calls.
   Static cross-references now identify source selection at 0xf76620..0xf76c3f:
   selected object+0x10 is stored at 0x6d1b920 and its 0xd10-byte camera copied
   into BOTH globals. The derived setup at 0xf7e980..0xf7f7f4 then calls
   0xf41000 at 0xf7f462 with the second global (0x6d1d660) and a computed pose.
   Pose update 0xf41000 copies the pose, rebuilds inverse view, and tail-calls
   projection/visibility refresh 0xf414c0. Camera-mode semantics remain unverified.
   A private MinHook-backed pose detour's call behavior is tested against owned
   assembly code with the same ten-byte prologue. It is not linked into
   the Vulkan observation layer. Selected-camera filtering, callback rejection,
   recursion, 8000 concurrent fixture calls, return preservation, code restoration
   and reinstall pass. Removal requires caller-provided quiescence; no safe hot
   unload is claimed. Thirteen Release and Debug test suites now pass. Debug
   fixture linking disables incremental thunks to test the actual assembly entry.
   The native ABI and
   actual cockpit identity still require controlled runtime validation.
   A separate `x4vr_native_pose.dll` is now available through `-PoseHook` (opt-in,
   startup only, implies CrashWatch). The debugger holds its new child at the
   PE entry point after loader initialization, loads the DLL and invokes its
   startup export outside DllMain, then resumes the primary thread. A temporary
   one-byte entry breakpoint is restored before resumption. Default recorder
   mode remains read-only. Startup fixture tests prove ordering and rejection
   for failed initialization, missing exports and an unsupported executable.
   Fourteen suites pass in both Release and Debug. The native module verifies
   the pinned executable/signatures, observes only camera 0x6d1d660, and always
   returns false from its callback (no pose replacement). It is process-lifetime
   pinned and performs no unhook or OpenVR shutdown from DLL teardown.
   Trial PID 38392: debug-cddab3d8409c42098dd6fd4a24a23411; capture directory
   pose-hook-38392-2333546. Hook installed successfully at startup; the user
   explicitly confirmed closing X4. Exit was 0xffffffff at 10:44:20 on Sept 24,
   without a recorded unhandled exception. pose-input.bin is empty. Do not infer
   a crash, a validated camera call, or gameplay stability from this run.
   User is now ready for an in-game test; a fresh launch is in progress under
   debug-5bf4bab613ed4a35a6175fb86f1b138a (-PoseHook -NoLayer).
   This fresh run is PID 16860 (recorder 5456), with capture
   pose-hook-16860-265343. The selected-camera callback has now run in X4:
   first inspection saw 71 complete records through sequence 840, thread 6152,
   camera address 0x7ff652ced660 (base+0x6d1d660). X4 remained responsive.
   External read-only checks in camera-peek-16860-hook-startup found both global
   view/inverse pairs consistent. These are startup-scene observations, not yet
   a user-confirmed cockpit/control-response test. No pose replacement occurred.
   Subsequently the user confirmed a centered cockpit and performed controlled
   left, right and upward free-look. Captures are camera-peek-16860-cockpit-
   {center,left,right,up}; each contains 8 samples of each global, all with
   consistent inverse pairs. Capturing waited two seconds after each direction
   reply so the user could tab back into X4. Relative to the last centered pose,
   source camera 0x6d1c950 gives yaw -64.999032 (left), +64.998992 (right), and
   pitch -34.998905 (up) degrees, with cross-axis terms below 0.001 degrees.
   Derived camera 0x6d1d660 follows the same directions with small additional
   offsets (roughly 0.2-0.4 degrees). This verifies cockpit free-look response
   and axis signs in this new-game mode. World position drifted during the
   captures, so translation scale is not established. X4 remained responsive.
   No headset pose was applied, no native projection changed, and no stereo
   scene was rendered. Other camera modes and long-run stability remain unproven.
   Next stage is now implemented in build-next (main build remains the earlier
   observation-only binaries): `HeadLook` composes OpenVR orientation with each
   fresh native pose using the verified Z-basis reflection. Translation is kept
   bit-for-bit; yaw recenter preserves gravity-relative pitch/roll. It does not
   accumulate previous output, replace projection or submit images. A current
   seated pose query uses IVRSystem, not WaitGetPoses, for this desktop test.
   `x4_openvr` is now a shared DLL so the native module and Vulkan layer share
   one Session/RuntimeBootstrap. Mutex-serialized tracking calls use the same
   pre-render ownership adoption as extension queries; no queue/frame is active.
   `-HeadLook` implies PoseHook + OpenVRBootstrap and starts disabled. A separate
   process-specific control tool sends enable/disable/recenter/shutdown requests
   consumed at camera callbacks. Disable/shutdown override pending enables.
   Tracking loss, background focus and errors forward the original native pose.
   Release build-next passes 15 suites, including axis signs, recenter, lost
   tracking, unchanged translation, non-accumulation and control-event lifetime.
   Live `openvr_probe --tracking` read a valid Aero pose through two shared owners.
   Shared-runtime Vulkan smoke on RTX3090 passed, process15260, report
   debug-e8b1e896dbb342a4abb6a2e65bfc2996. No image submission was exercised.
   User authorized closing/relaunching X4. PID16860 exited normally (code0).
   New PID45976, recorder28504, uses build-next -HeadLook; reports
   debug-7de3dc6ce3aa48b4bba9a590095e9e21 and pose-hook-45976-1213234.
   Exactly one runtime initialization is logged; the native startup and Vulkan
   extension/device setup succeeded, selected-camera calls arrived, and the game
   was responsive. After the user's readiness confirmation, enable was acknowledged
   and pose application logged. Captures camera-peek-45976-headlook-{disabled,active}
   show coherent modified derived matrices. However, the user reported NO visible
   cockpit camera movement. This experiment therefore failed its visible outcome;
   it does not establish working head tracking. On recheck September25 X4 was no
   longer running; no exit cause is inferred from the incomplete recorder log.
   Static tracing shows the renderer's per-view wrappers point at heap camera
   objects, whereas this hook modifies only a global copy. A new optional
   peek_camera.py --render-links captures those fixed paths read-only for the next
   normal-launch cockpit comparison. No new native mutation target is enabled.
   Normal-launch PID62052 is now live. The user confirmed cockpit readiness and
   a left-looking view. Read-only render-links captures distinguish a selected
   class U::Zone scene camera from lensflare and Anark UI cameras. Scene/context
   matrices follow approximately -65deg left yaw; some cockpit-anchored UI follows
   too, whereas other UI stays fixed. Selected scene storage alternates addresses.
   A replacement is now implemented at producer camera-record copy 77a376,
   before rendering, not the ineffective derived global. -SceneHeadLook selects
   this startup-only path instead of the old detour; it starts disabled. One
   call instruction is redirected through a private nearby RX relay, not a global
   copy-function hook. The original copy runs first; pool half/slot/count/label
   guards select Zone views, and a native rebuild on a private record precedes
   commit. Translation/source stack/other-camera paths remain unchanged.
   Final Release and Debug builds each pass all 16 suites, including page-boundary
   and pool-overflow rejection. The user closed PID62052 and authorized launch.
   New PID22936, recorder55364, runs build-next -SceneHeadLook; reports
   debug-529b65f466584deda24cded51cf1f957 and pose-hook-22936-19415125.
   The scene-copy hook installed, startup resumed and OpenVR Vulkan setup
   completed. X4 is responsive while loading; tracking remains DISABLED pending
   cockpit/headset readiness. The first producer callback and first guarded Zone
   camera capture are now logged on thread4744; the original copy completed.
   Visible tracking, object WVP/history and UI remain unverified.
   Six camera-reader tests pass, including pointer changes, inaccessible pointers
   and optional bounded view labels. These do not validate native mutation.
5. Integrate HUD/menu/map presentation and input; retain existing flight controls.
   Independent stereo attachment ownership is implemented in `x4_eye_targets`:
   two color images and two depth images with dedicated device-local allocations
   and views, on a borrowed Vulkan 1.1 device. No implicit device/queue ownership
   or hidden synchronization. The caller must finish GPU/compositor use before
   teardown. All 20 injected partial-initialization failures clean up successfully.
   The RTX 3090 `--targets-only` test renders clear values into two separate
   3292x2820 color/depth framebuffers, reuses them, and verifies pixel/depth readback:
   left RGBA(10,10,10,255)/depth0, right RGBA(20,20,20,255)/depth0.5.
   No OpenVR startup or X4 launch occurs in this test. Eleven CTest suites pass.
   These targets are not yet connected to X4's geometry, intermediate render
   targets, postprocessing, HUD, or temporal history. The updated gray submission
   diagnostic uses this render-pass path, but headset submission has not been
   retested since that change; earlier 90-pair success refers to copy/clear images.
6. Validate actual OpenVR Vulkan image submission and headset output, then measure frame timing and
   correct stereo/depth across cockpit, on-foot, map and external camera modes.
   Standalone submission now succeeds for 90 distinct eye-image pairs at 3292x2820
   per eye on RTX 3090/headset `hedy`, for both RGBA8 and BGRA8 with ColorSpace_Auto.
   RGBA8 with forced ColorSpace_Linear previously returned error 105. No visual
   correctness or X4 image submission is established by these gray-image runs.

## Completion gates (all currently unproven)

- X4 scene geometry is rendered separately for each eye using runtime eye transforms.
- Head position and orientation affect the game view at correct scale and latency.
- Native Vulkan textures reach OpenVR and display correctly on the Varjo Aero.
- HUD, menus, map and existing controls are usable in VR.
- Tracking loss, recentering, focus changes, game loading and shutdown are handled.
- A repeatable enable/disable path works without corrupting the retail installation.
- In-game runtime checks establish correct stereo and practical frame timing.

The backend library and passing unit checks do **not** satisfy these gates.

## Primary references

- https://github.com/ValveSoftware/openvr/wiki/Vulkan
- https://github.com/ValveSoftware/openvr/wiki/IVRSystem::GetProjectionRaw
- https://github.com/eg3r/X4Native
- https://github.com/hubertdungen/x4-triple-screen
- https://support.varjo.com/hc/en-us/my-headset-does-not-work-with-steamvr-openvr-or-openxr-applications
- https://www.egosoft.com/download/x4/bonus_en.php
- https://github.com/KhronosGroup/Vulkan-Loader/blob/main/docs/LoaderLayerInterface.md
- https://github.com/KhronosGroup/SPIRV-Headers/blob/main/include/spirv/unified1/spirv.hpp
- https://learn.microsoft.com/en-us/windows/win32/api/winnt/nf-winnt-rtlvirtualunwind
