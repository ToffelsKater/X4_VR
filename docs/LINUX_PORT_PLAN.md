# X4_VR: native Linux port plan

This plan is for porting X4_VR to the **native Linux build of X4: Foundations** (not the Windows
build under Proton), started from Steam on **NixOS** and built and installed through Nix. The port
lives in this fork and is meant to be offered back to the original repository as a pull request,
so it must not change the Windows build in any way.

Read `README.md` and `STATUS.md` first. They describe the Windows pipeline, the measured X4 quirks
and the calibration values this plan refers to. Anything marked **verify** is an assumption that
has not yet been checked against the code or the game.

Revision 2 (2026-10-04), updated with the first Phase 0 results (`docs/LINUX_FINDINGS.md`). It replaces the first draft. Main changes:

- The port follows the **current** Windows design: eye at use, frame half, code patches. It does not
  fall back to an older eye-per-read scheme.
- The Linux build is opt-in through a flag (section 3). The Windows build stays the default and
  unchanged.
- The target hardware is now known (section 1).

---

## Status (2026-10-04)

**Phase 0 done** (docs/LINUX_FINDINGS.md). **Phase 1 built, awaiting the first headset test**
(docs/LINUX_PHASE1_TEST.md): stage A of section 7, i.e. the Windows pipeline on Linux without X4
code patches. Design decisions taken while building it, which supersede the sections below where
they differ:

- **One library, `libx4vr.so`,** holds the Vulkan layer (`VK_LAYER_X4VR`), the VR runtime and the
  OpenTrack pose sender. The Vulkan loader loads it into X4 (`x4vr-run` sets the layer variables);
  **no `LD_PRELOAD`** is needed for stage A (sections 6.1 and 9.1). VR only starts in the X4 process.
  The library pins itself (X4 creates and destroys instances while starting).
- **Its own C++ runtime, nothing else exported.** libstdc++ and libgcc are linked in statically and a
  version script exports only the three layer entry points; X4's container may provide older or
  different libraries, and X4 exports thousands of symbols of its own.
- **OpenVR's client library is compiled in from source** (nixpkgs `openvr.src`): the packaged
  `libopenvr_api.so` loads libstdc++ and jsoncpp dynamically.
- **The pose sender runs on its own thread** and sends one OpenTrack packet after each present (X4
  reads OpenTrack on its own thread anyway, findings 0.6). X4's UI exports (menu, ship control,
  head tracking active, smoothing) are called from the present hook, which runs on X4's main thread.
- **AMD queue (section 8.2):** a spare graphics queue if there is one (as on Windows); otherwise the
  layer **shares X4's graphics queue** and serialises every use of it (X4's submits and presents,
  the layer's, SteamVR's through the loader). Chosen over a compute queue because SteamVR may need
  graphics operations on the queue it's given. If sharing costs too much frame time, the compute
  queue (option 1) is the next step.
- **OpenXR is deferred:** the Linux build is OpenVR only (`openxr_runtime_stub.hpp`).
- **No cursor overlay yet** (X4 draws an X11 cursor that never reaches the swapchain), **no in-game
  hotkeys yet:** `x4vr ctl recenter|flat` edits `stereo.txt`, bindable to desktop keys.
- **Settings and logs** both live in `~/.local/state/x4vr/` (`stereo.txt` read from the capture
  directory, as on Windows), not split between config and state (section 9.2).
- **Existing test suites** that are platform-independent run on Linux unchanged (launcher logic, eye
  camera, eye targets, camera sampling, extension merge) next to the Linux ones: 8 suites in the Nix
  check phase.

Linux copies of Windows files (section 3): `src/linux/runtime_bootstrap.cpp` (from 5064391),
`src/linux/vr_layer.cpp` (from `src/observe_layer.cpp` at 62569df), `src/linux/pose_sender.cpp`
(counterpart of `src/freetrack_client.cpp` at be68c82).

---

## 1. Target setup

| Item | Value | Consequence |
|---|---|---|
| Headset | Steam Frame, streamed through **SteamVR on Linux** | **OpenVR is the main backend**, as on Windows (it's the default and the best-tested path). OpenXR through SteamVR's runtime stays available with `X4VR_RUNTIME=openxr`, as on Windows. Monado and WiVRn are out of scope. |
| GPU | AMD Radeon RX 7900 XT (RADV, Mesa) | No DSR: high render resolution comes from gamescope. **RADV usually exposes only one graphics queue**, which breaks the layer's private submission queue (section 8.2). |
| Game | Native Linux X4 9.00 from Steam | The same version as the Windows mod's signature tables, but a different binary (ELF, GCC/Clang code, Itanium C++ ABI). Every Windows address and signature has to be found again. |
| OS | NixOS, classic Nix (no flakes) | Built with `pkgs.callPackage` from `configuration.nix` (section 10). |

---

## 2. Goal and scope

### First milestone: cockpit parity with Windows

Seated cockpit flight in stereo with 6DOF head tracking, working **the way the Windows mod works
today**:

- head pose through X4's own head tracker, so culling, lighting and shadows follow;
- alternate-eye rendering, with the eye chosen **at use**: X4 receives the head centre, and
  the tracker's position accessor adds the offset of the eye X4 is building at that moment;
- the eye at present time taken from X4's **frame half** global (`eye_from_half=1`);
- X4's backward head-position clamp removed, so the rear eye isn't pinned when looking sideways;
- tracker smoothing off (`SetActiveHeadTrackerHeadFilterStrength(1)`);
- the "pose barely changes" check neutralised (`still_at_use`);
- the asynchronous submission thread on a private queue;
- the theater screen for menus, Ctrl+F11 (flat-screen toggle) and Ctrl+F12 (recenter);
- the `stereo.txt` live keys, stats and trace files.

Each Windows mechanism already falls back to a weaker mode when its signature doesn't match.
The port keeps those fallbacks, and the work is staged so that each stage is playable (section 7).

### Second milestone

- **On-foot head tracking.** Windows does this with two more patches (`enable_on_foot_tracking`).
- The graphical launcher (the CLI covers the first milestone).
- The calibration and debug tools in `tools/*.py` (memory reading through `process_vm_readv`).

### Later

- Both eyes from the same simulation step (world gating, section 12).
- The view-matrix eye check as an extra diagnostic (section 12).

### Hard constraints

1. **The Windows build doesn't change** (section 3). Its output, behaviour and defaults stay
   identical, and the Windows tests still pass with a manual MSVC build. There is no CI in the
   repository today, so this has to be checked by hand, or with a Windows GitHub Actions job
   added early.
2. **No Egosoft files in the repository.** Same rule as the Windows mod.
3. **Never patch game code without checking its bytes first.** Every Linux patch and hook goes
   through the same signature check as Windows (`code_scan.hpp`), with a fallback on mismatch.
4. **Measure before building.** Phase 0 results go into `docs/LINUX_FINDINGS.md`.

---

## 3. Keeping Windows untouched: the `X4VR_LINUX` flag

### The flag

The platform is a **build-time** choice: a Windows build and a Linux build are different
binaries, so there is no runtime flag that switches between them. The opt-in is a CMake
option:

```
cmake -B build -DX4VR_LINUX=ON      # Linux build
cmake -B build                      # Windows build, exactly as today
```

- `X4VR_LINUX` defaults to `OFF`. Without it, the top-level `CMakeLists.txt` behaves byte for
  byte as now: Windows x64 only, MSVC, MASM, pinned SDKs. On a non-Windows host it still stops
  with the existing error message, extended to say "pass -DX4VR_LINUX=ON for the Linux build".
- With `X4VR_LINUX=ON`, the top-level file hands over to `linux/CMakeLists.txt` **before** any
  Windows-specific line (`project(... ASM_MASM)`, MinHook, MSVC runtime flags) runs. Change
  to the top-level file: a few lines at the very top.
- The Nix package (section 10) always passes `-DX4VR_LINUX=ON`. Steam players on Linux never
  type it.

### Where Linux code goes

| Kind of file | Rule |
|---|---|
| Windows-only entry points: `freetrack_client.cpp`, `native_pose_module.cpp`, `pose_detour.cpp`, `copy_call_hook.cpp`, `crash_watch.cpp`, `startup_module.cpp`, `tools/launcher/launcher.cpp`, `scripts/*.ps1` | **Not touched.** |
| Already portable (no Win32 calls): `math.cpp`, `session.cpp`, `x4_camera.cpp`, `head_look.cpp`, `eye_targets.cpp`, `launcher_settings.hpp`, `hud_mod.hpp` | Compiled by the Linux build as they are. |
| Windows files with Win32 calls that Linux needs: `runtime_bootstrap.cpp`, `openxr_runtime.cpp`, `observe_layer.cpp`, `freetrack_client.cpp` (its pose logic), `include/x4vr/code_scan.hpp` (PE section walk) | **Not touched.** Linux gets its own copy under `src/linux/` (decision below), with the Win32 calls replaced. Each copy's header names the Windows file and the commit it was copied from. |
| New Linux-only code | `src/linux/` (pose shim, ELF scan, patches), `linux/` (CMake, layer manifest, wrapper script), `config/linux/` (Linux `stereo.txt` defaults), `tools/linux/` (the `x4vr` CLI), `tests/linux/`, `nix/` and the root `default.nix`. Tools are C++ in the same build, so they share code with the mod (the OpenTrack packet, the ELF scan) and run in the Nix check phase. |
| X4 signature tables | Linux tables live next to the Windows ones in `code_scan.hpp`, under `x4vr::code::x4_linux`. The Windows rows are not edited. |

**Decision (2026-10-04): separate Linux copies for now.** Windows files stay byte-for-byte
unchanged; the Linux build has its own copies of the files it needs to change. The cost is
duplication: a fix to the presenter, the stutter handling or the overlays in a Windows file has to
be carried over to the Linux copy by hand. To keep that manageable:
- each copy starts with a comment naming its Windows original and the commit it was copied from;
- copies change only what Linux needs (Win32 calls, paths, the AMD queue, the pose input), so a
  diff against the original stays small and readable;
- when Windows files change upstream, diff them against the recorded commit and port the change.

If the port goes upstream, the copies can be merged back into one code base with a small platform
header (the alternative considered: one copy, each Win32 call behind a function with a Windows and
a Linux version).

---

## 4. Windows mechanisms and their Linux equivalents

Taken from the current code, not the README.

| Mechanism (Windows code) | What it does | Linux equivalent |
|---|---|---|
| `FTGetData` (`freetrack_client.cpp`), called by X4 several times per frame on the game thread | Sends the head-centre pose as FreeTrack data (radians, millimetres, with gains). On its first call: acquires the runtime, applies the patches, turns smoothing off. Also handles hotkeys, theater state, recentering and diagnostics. | X4 on Linux reads **OpenTrack UDP** (127.0.0.1:4242, 6 little-endian doubles; **verify** units, signs and scale in 0.5). `libx4vr_pose.so`, loaded with `LD_PRELOAD`, provides the centre pose over that socket. The once-only setup and per-frame logic move to a hook point that runs on the game thread (section 6.3). |
| Locating the tracker object: `tracker = data - layout.data`, where `data` is the buffer X4 passes to `FTGetData` | Finds X4's FreeTrack tracker so its vtable can be hooked | Not needed: the binary keeps C++ RTTI, so `VR::OpenTrack`'s vtable is found from its type name (findings 0.3). Swapping slots in the vtable hooks every instance; the object arrives as `this`. Slots are still signature-checked. |
| Eye at use: vtable slot `0x108` (tracker position accessor) → `position_at_use` | Adds the eye offset of the frame X4 is building **now**, then restores the centre. Avoids the 2–38 % wrong-eye frames of choosing the eye at the read. | The same hook on the Linux tracker class's position accessor. The slot offset will differ (Itanium ABI vtable layout, different class) and must be found. |
| Vtable slot `0x28` (tracker "still" check) → `still_at_use` | Stops X4 from ignoring a centre pose that barely moves | Same, with the Linux slot. |
| Frame half global (RVA `0x6b66280`, reader signature at `0x77a47f`), `frame_half()` | The eye for a frame, from pose to present, whatever the queue depth. Used by both the pose side and the layer. | Find the same global in the ELF: same mechanism, ELF scan, Linux signature. Without it, the fallback is the present counter plus `delay` (re-measured, see 0.7). |
| Backward clamp patch (`jae`→`jmp` at `0x9fdb4d`) | Stops X4 from zeroing backward head position. **Without it, the rear eye is pinned whenever you look sideways**, not only when leaning back. | Must be in the first milestone. Find the bridge code in the ELF and use the same kind of one-byte patch, signature-checked. |
| Rival trackers patch (TrackIR, Tobii) | Stops another tracker DLL from replacing the headset pose | Probably not applicable on Linux (no TrackIR/Tobii DLLs). Check in 0.3 whether the Linux build even has them. |
| On-foot patches (`enable_on_foot_tracking`) | Head tracking while walking | Second milestone. |
| `SetActiveHeadTrackerHeadFilterStrength(1)` via `GetProcAddress` | Turns tracker smoothing off | `dlsym(RTLD_DEFAULT, …)`, if exported (0.3). Must run on the game thread. |
| `IsFullscreenMenuDisplayed`, `IsPlayerControllingShip` (and `IsHeadTrackingActive`, `IsFullscreenCutsceneActive` for diagnostics) | Theater screen for menus and views without ship controls | `dlsym`, if exported (0.3). |
| Ctrl+F11 / Ctrl+F12 via `GetAsyncKeyState` (pose DLL and `observe_layer.cpp:1361`) | Flat-screen toggle, recenter | X4 links SDL3 dynamically: wrap `SDL_PollEvent` in the preload shim (section 9.3). |
| `x4_openvr.dll`, shared by the pose DLL and the layer | One VR session per process | `libx4vr_runtime.so`, a shared library loaded by both. |
| Vulkan layer (`observe_layer.cpp`) | Capture, per-eye texture ring, submission thread on a private queue | Same code behind the platform header, plus a Linux layer manifest. |
| Launcher → `crash_watch` → X4 | Environment, debugger | `x4vr-run %command%` as the Steam launch option. No debugger: a preloaded library's constructor runs before `main`. Core dumps come from systemd-coredump. |
| `X4VR_CAPTURE_DIR` (`reports/captures/` in the game folder) | Settings, logs, traces | `$XDG_CONFIG_HOME/x4vr/` (settings, profiles) and `$XDG_STATE_HOME/x4vr/` (logs, captures). `x4vr-run` sets `X4VR_CAPTURE_DIR`, so the shared code needs no change. |
| `OutputDebugStringA` | Log lines | Same text written to a log file and to stderr. Keep the exact line wording (`X4VR freetrack: …`), so the README troubleshooting table only needs a Linux column. |

---

## 5. Phase 0: measurements on your machine

This needs your machine with X4 running: in the main menu for some checks, and **in a game,
sitting in a ship's cockpit** for others. Session 1 needed only shell commands. Session 2 uses the
probe kit built with `nix-build` (`docs/LINUX_PHASE0.md` has the step-by-step instructions):
`x4vr udp-send` (OpenTrack sender), `x4vr vr-check` (SteamVR check), `x4vr elf-classes` (RTTI
class and vtable list), the socket probe `libx4vr_probe.so` and the logging layer
`VK_LAYER_X4VR_probe`, loaded by the `x4vr-probe-run` launch wrapper. Results go into
`docs/LINUX_FINDINGS.md`.

In the order to run them:

### 0.1 SteamVR and the Steam Frame on Linux (no X4 needed)

Build `openvr_probe` and `runtime_smoke` for Linux first (they come out of Phase 1's first step).
Check that the OpenVR runtime path resolves (`~/.config/openvr/openvrpaths.vrpath`), that
`openvr_probe --tracking` reads the Frame's head pose, and that `runtime_smoke` shows grey
frames in the headset. Repeat with `X4VR_RUNTIME=openxr`.
*Pass:* pose and frames on both backends, or a written reason why one doesn't work.

### 0.2 Vulkan queue families on the 7900 XT (no X4 needed)

Run `vulkaninfo --summary` and look at the queue family list. If the graphics family has
`queueCount = 1`, the layer can't add its private queue there (`observe_layer.cpp:388`). The
result decides the design in section 8.2.

### 0.3 Exports and linked libraries

```
nm -D --defined-only "<X4 dir>/X4" | grep -E 'HeadTracker|IsFullscreenMenuDisplayed|IsPlayerControllingShip|IsHeadTrackingActive|IsFullscreenCutsceneActive|VRMode'
ldd "<X4 dir>/X4"
file "<X4 dir>/X4"; sha256sum "<X4 dir>/X4"; cat "<X4 dir>/version.dat"
```

Record which of the exports the Windows mod uses are present, whether SDL2 is linked dynamically
(hotkeys), which Vulkan loader is used, whether the binary is PIE, and whether it is stripped.
Record the SHA-256 for the signature tables.

### 0.4 How Steam runs native X4 on NixOS

With X4 at the main menu: `cat /proc/$(pidof X4)/maps | grep -E 'libc\.so|libvulkan'`,
`ls -l /proc/$(pidof X4)/root`, and the process environment (`PRESSURE_VESSEL_*`,
`STEAM_RUNTIME*`, `LD_LIBRARY_PATH`). Find out whether it runs directly in the NixOS Steam FHS
environment, under the scout `LD_LIBRARY_PATH` runtime, or in a pressure-vessel container, and
whether `/nix/store` and `~/.config/openvr` are visible from inside.

### 0.5 OpenTrack UDP head tracking (in the cockpit)

Enable OpenTrack in X4's options (Controls → Head Tracking). Run `x4vr udp-send`
and set one axis at a time. For each axis, record the units, sign, scale (does 30° sent give
30°?), clamping and smoothing, and whether X4 recenters on its own. Also test, as on Windows:
- **backward position:** send +z (backwards). Is it zeroed, as with FreeTrack?
- **a still pose:** send a constant pose for a few seconds, then move. Does X4 stop applying it
  (the "still" check)?
- **smoothing:** does `SetActiveHeadTrackerHeadFilterStrength` change the UDP path's lag?

*Pass:* a written mapping table, with the Windows quirk table (`STATUS.md`) row by row answered
for UDP.

### 0.6 How X4 reads the socket, and where the tracker object is (in the cockpit)

1. `strace -f -tt -e trace=socket,bind,recvfrom,recvmsg,recvmmsg,recv,read,poll,ppoll,select,epoll_wait -p $(pidof X4)`.
   Record which thread binds and reads, how often it reads, whether reads are non-blocking or
   drain until `EAGAIN`, and whether it waits for readiness first.
2. With the socket probe preloaded (`libx4vr_probe.so`, the analogue of Windows'
   `ftgetdata_caller.txt`): the **buffer address** X4 reads into, the thread, the call pattern
   and a backtrace with absolute addresses (X4 isn't PIE).

This is less critical than in the first draft. With eye at use, read timing only affects the
centre pose's latency, not which eye is shown. It is still needed to find the tracker object and
the bridge code.

### 0.7 A trivial Vulkan layer loads (main menu)

The layer logs `vkCreateInstance`, `vkCreateDevice` (with the queue create infos), the present
thread ID, and the swapchain format, extent and WSI (X11/XWayland or Wayland). Load it with the
wrapper (section 9.1). *Pass:* the log appears.

### 0.8 Camera uniform layout (only needed for section 12)

Same as the Windows notes: set 1, binding 0; offsets 0/64/448/512. Uniform layout comes from the
shaders, so it is likely identical (**verify** later).

---

## 6. Phase 2: pose input (`libx4vr_pose.so`)

### 6.1 Loading and process filtering

Loaded with `LD_PRELOAD` from `x4vr-run`. Steam's launch chain inherits it, so the constructor
checks that `/proc/self/exe` is the X4 binary and does nothing in any other process. Real
functions come from `dlsym(RTLD_NEXT, …)`.

### 6.2 Delivering the centre pose

Simplest option first: the shim **sends real UDP packets** to X4's port from its own thread at
the headset rate, carrying the **head centre** (`predicted_tracking`, recentred, gains applied).
No read interception is needed for the pose itself, because eye at use adds the eye offsets
later. Wrapping `recvfrom`/`recvmsg` on X4's tracker socket is still needed, for three reasons:
- to find the tracker object from the buffer address (section 4);
- to run the per-frame logic on X4's game thread, if X4 reads there (0.6). Windows does
  recentering, the theater decision and the smoothing call in `FTGetData`;
- optionally, to fill the buffer with a fresh pose at read time, for lower latency.

If 0.6 shows the reads happen on a separate network thread, the per-frame logic moves into the
position-accessor hook (it runs on X4's camera thread) and exported game calls are made only
where they are safe.

### 6.3 Shared logic

The body of `FTGetData` (theater decision, recentering, synthetic poses, eye offsets, `head.txt`
and `state.txt` traces) is mostly platform-free, apart from the FreeTrack packing and Win32
calls. Following the decision in section 3, the Linux pose library gets its own copy of this logic,
marked as mirroring `freetrack_client.cpp` at a given commit; `freetrack_client.cpp` is not changed.

### 6.4 Gains and defaults

Keep the keys (`yaw_gain`, `pitch_gain`, `roll_gain`, `pos_scale`, axis signs as `X4VR_FT_*`
environment variables). Defaults come from 0.5 and live in `config/linux/stereo.txt`, not in
`config/stereo.txt`, which stays the Windows default.

### 6.5 Logging

Same lines as Windows: `X4VR freetrack: first headset pose delivered to X4`,
`… eye-at-use hook installed (X4 9.00 linux code)`, `… signature mismatch; left unchanged`.

---

## 7. Phase 3: ELF signatures and code patches (part of the first milestone)

These follow the Windows mod today, so they are no longer deferred. Do them in stages. Each stage
is playable, and each falls back exactly as Windows does when its signature doesn't match.

| Stage | Adds | Fallback without it |
|---|---|---|
| A | Pipeline: UDP centre pose + eye per frame from the present counter (`eye_from_half=0`, `eye_at_use=0`) | — (proves frames, poses and SteamVR work; expect some eye swaps) |
| B | Tracker object + `position_at_use` + `still_at_use` hooks (eye at use) | Stage A eye choice |
| C | Frame half global (`eye_from_half=1`) | present counter plus `delay` |
| D | Backward clamp patch | rear eye pinned when looking sideways |
| E | Smoothing off, theater exports | smoothing at the menu minimum; theater only through Ctrl+F11 |

### Method

- Use the Windows analysis (`STATUS.md`, `reports/`, the comments in `code_scan.hpp`) as the map.
  Anchors in the ELF: the float constants 1.4835 (85°) and 0.2, the 0.25 m clamp, the
  `HeadTracker` export names and their callers, and the recv call found in 0.6.
- If the binary isn't stripped, symbol names make this much easier (0.3).
- Ghidra (headless, `nix-shell -p ghidra`) or `objdump` for disassembly. Matches between MSVC and GCC code are
  fuzzy; work from data flow, not byte shape.
- The bytes of the game binary never enter the repository. Signatures are short masked byte
  patterns, like the Windows tables.

### Mechanism

- `code_scan.hpp`: add an ELF variant of `find_all` that walks the executable `PT_LOAD` segments
  of the main program (from `dl_iterate_phdr`). The Windows PE walk is unchanged.
- Code patches: verify the bytes, `mprotect` the page writable, write, restore protection, and
  flush (`__builtin___clear_cache`).
- Vtable swaps: vtables are in `.data.rel.ro`, which is read-only after RELRO, so `mprotect`
  those too. Same checks as `swap_slot`.
- Calling conventions: the hooks are plain SysV functions on Linux. Check argument order against
  the disassembly, because the Windows `position_at_use(tracker, x, y, z)` signature may not match
  the GCC-compiled accessor.
- `tests/code_scan_tests.cpp` gains a Linux mode that checks the Linux tables against an X4 ELF
  given on the command line, like the Windows test does for `X4.exe`.

---

## 8. Phase 4: Vulkan layer port

### 8.1 General

- Win32 calls behind the platform header (section 3). The logic stays the same.
- Linux layer manifest (`linux/VkLayer_x4vr.json`). Enabled by the wrapper with
  `VK_ADD_LAYER_PATH` (loader 1.3.234 or newer; otherwise `VK_LAYER_PATH`) and
  `VK_INSTANCE_LAYERS`. Check the loader version found in 0.3.
- Export only `vkGetInstanceProcAddr`, `vkGetDeviceProcAddr` and
  `vkNegotiateLoaderLayerInterfaceVersion` (`-fvisibility=hidden`).
- Submission thread priority: try `SCHED_FIFO` (needs `RLIMIT_RTPRIO` or rtkit), else a lower
  nice value. Log which one was applied. Never fail on it.
- Log the WSI (X11 or Wayland), the swapchain format and the final extension lists.
- Swapchain resizes: test under gamescope.

### 8.2 The private queue on AMD (depends on 0.2)

The layer adds one queue to the game's **graphics** family and gives up if the family is full
(`observe_layer.cpp:388`). RADV usually exposes a single graphics queue, so on the 7900 XT:
- with OpenVR, `vr_queue` is null and **asynchronous submission silently turns off**, bringing back
  the flicker it fixed;
- with OpenXR, the layer throws ("OpenXR needs a private queue").

0.2 confirmed it: the 7900 XT's graphics family has one queue; family 1 has four compute queues. Option 1 is the chosen route (findings 0.2). Options considered:
1. A queue from a **compute** family for the copies and submission. The copy is a plain
   image copy and doesn't need graphics. Needs queue-family ownership transfers between the
   game's graphics queue and the compute queue, and a check that SteamVR accepts a compute queue
   in `VRVulkanTextureData_t::m_pQueue` / `XrGraphicsBindingVulkanKHR`.
2. Share the game's graphics queue with a mutex around every `vkQueueSubmit`/`vkQueuePresentKHR`
   the layer sees. Simpler, but the layer would have to intercept all of the game's queue use.
3. `RADV_PERFTEST`/driver options that expose more graphics queues, if any exist for this GPU
   (**verify**). This would be environment-only, with no code change.

The change has to stay behind a condition that is false on Windows (a family with spare graphics
queues), so NVIDIA on Windows keeps the current path.

---

## 9. Phase 5: wrapper, settings and tools

### 9.1 `x4vr-run` (first milestone)

Steam launch option: `x4vr-run %command%`. It:
- prepends `libx4vr_pose.so` to `LD_PRELOAD`, sets the Vulkan layer variables, `X4VR_RUNTIME`
  (default `openvr`), and `X4VR_CAPTURE_DIR`;
- adds `-skipintro -nocputhrottle` (**verify** on Linux);
- creates `stereo.txt` from `config/linux/stereo.txt` plus the chosen profile if missing;
- handles a pressure-vessel container if 0.4 finds one (copy libraries to a visible path, set
  `PRESSURE_VESSEL_FILESYSTEMS_RO`/`RW`);
- `exec "$@"`.

### 9.2 Settings and logs

`$XDG_CONFIG_HOME/x4vr/` and `$XDG_STATE_HOME/x4vr/`, overridable by environment variable.
Nothing is written to the Nix store.

### 9.3 Hotkeys (Ctrl+F11, Ctrl+F12)

Windows polls with `GetAsyncKeyState` (confirmed in the code). On Linux, in order of preference:
1. X4 links **SDL3** dynamically (findings 0.3): wrap SDL3's `SDL_PollEvent`/`SDL_PeepEvents` in the preload shim.
2. `x4vr ctl recenter|flat`, which bumps keys in `stereo.txt` (Windows already has `recenter=`;
   add a matching `flat=` counter). Bindable to a desktop shortcut or a SteamVR binding.
3. evdev (needs `input` group). Avoid.

### 9.4 CLI (`x4vr`)

`x4vr check` / `fix-settings` (port of the `config.xml` logic, already tested in
`launcher_tests.cpp`; Linux path `~/.config/EgoSoft/X4/<id>/config.xml`, **verify**),
`x4vr hud <factor>|remove` (port of `hud_mod.hpp`), `x4vr report` (bug-report packer: logs,
`stereo.txt`, `config.xml`, kernel, Mesa and RADV version, SteamVR version, X4 version,
NixOS generation). DSR advice becomes gamescope advice.

### 9.5 Calibration tools (second milestone)

`tools/*.py` memory reading through `process_vm_readv`. Needs ptrace permission
(`kernel.yama.ptrace_scope`); document it, don't set it.

---

## 10. Phase 6: Nix packaging (no flakes)

- `nix/package.nix`: `callPackage`-style, builds with CMake and `-DX4VR_LINUX=ON`. The Linux
  build always takes its dependencies from the system (`vulkan-headers`, `openvr`, later
  `openxr-loader`, from nixpkgs); it never downloads. `doCheck = true` runs the portable CTest suites. Outputs: `lib/libx4vr_runtime.so`,
  `lib/libx4vr_pose.so`, the layer `.so` and its manifest, `bin/x4vr-run`, `bin/x4vr`,
  `share/x4vr/config/`.
- `default.nix` for `nix-build` from the repository root. No `shell.nix`: plain `nix-shell` in the
  repository gives the build environment from `default.nix`, and one-off tools come from
  `nix-shell -p strace gdb …`.
- `nix/module.nix`: `programs.x4vr.enable` adds the package to `environment.systemPackages`.
- Install documentation (README Linux section): `pkgs.callPackage /path/to/fork/nix/package.nix {}`,
  or the module, from a local path or `fetchFromGitHub` pinned to a commit.

### glibc and libstdc++

**Phase 0.4 result:** X4 runs in pressure-vessel (sniper), but the process uses the host's glibc 2.42 and
Vulkan loader from `/nix/store`, and `/nix/store` is visible inside the container. Mitigation 1
is therefore enough on this setup, and the copy step in 3 isn't needed. See findings 0.4.

The injected libraries load into X4's process, which uses the Steam environment's glibc.
1. Build with the system's own `pkgs`, the same nixpkgs as `programs.steam`.
2. Link `libstdc++`/`libgcc` statically into the injected libraries. Check the needed glibc symbol
   versions with `readelf -V`.
3. If 0.4 finds a container without `/nix/store`, `x4vr-run` copies the libraries to a visible path.

SteamVR's own `vrclient.so` is also loaded into X4's process by `libopenvr_api.so`, and it expects
the Steam Runtime. Check in 0.1 and 0.7 that this works from inside the game's environment.

---

## 11. Testing

- **Without X4:** portable CTest suites; `openvr_probe` and `runtime_smoke` against SteamVR with the
  Frame (0.1); `vulkan_smoke` with the layer loaded; a socket test program that binds 4242 and reads
  in the pattern found in 0.6, for the shim.
- **With X4, in order:** the trivial layer loads; Stage A (mono, `stereo=0`, then stereo); Stage B
  (no eye swaps: near objects not doubled); Stage C; Stage D (look 90° sideways, stereo stays
  correct); Stage E; menus to the theater screen and back; recenter; resize under gamescope; clean
  quit with no hang in the submission thread.
- **Windows:** a manual MSVC build of the fork after every change to a shared file, with all
  Windows suites passing, and a short play test before the upstream PR.

---

## 12. Later work

### Open items after Phase 1 (2026-10-05)

Working: stereo in the cockpit with the right eye at use (stages B, C), no ghosting on the Steam
Frame (shared pose per eye pair), the virtual screen drawn into the eye images, X4's settings and
resolution fixed before each start, eye images at SteamVR's recommended size.

Features the Windows mod has:
1. **Mouse cursor in VR**: done (2026-10-05, confirmed in the headset); read over XCB, copied into
   the game image (`docs/LINUX_FINDINGS.md`). Possible polish: blended edges, size matched to the
   scaled game image.
2. **In-game hotkeys**: done (2026-10-05): Ctrl+F12 / Ctrl+F11 watched over XCB while X4 has focus,
   and SteamVR's recentre (`VREvent_SeatedZeroPoseReset`) recentres the mod too.
3. **On foot**: head tracking while walking (Windows: on-foot patches); Linux shows the virtual screen.
4. **Stage D, leaning back**: paused, the zeroing is downstream of the head-tracker bridge
   (`docs/LINUX_FINDINGS.md`); needs a runtime approach.
5. **OpenXR**: Linux is OpenVR only; not needed for the Frame.

Tuning:
6. **Calibrate `pos_scale` and the turn gains** (Windows values; the cockpit feels slightly large).

Known limits, accepted for now:
7. **HUD distance** (`x4vr hud`): the HUD moves back but shrinks (X4 loads the precompiled `.xpl`).
8. **Tiling window managers** resize X4's window; float it (class `X4`) to keep the resolution.

Before an upstream PR:
9. Done: diagnostic switches removed (results kept in `docs/LINUX_FINDINGS.md`).
10. Done: the probe builds with newer GCC (all 9 tests pass).
11. Done: README section "Linux (native X4, experimental)".
12. Windows: outside the Linux folders only the top-level `CMakeLists.txt` (an opt-in branch that
    returns before the Windows build) and `.gitignore` changed since `be68c82`; still to build on Windows.


- **Both eyes from one simulation step:** clock gating in the preload shim (`clock_gettime` on the
  main thread, `world_gate=0/1`), or a hook on the simulation step. `pair=1` stays the
  no-gating alternative (needs 180 fps).
- **View-matrix eye check:** camera uniform at set 1, binding 0, compared against recorded poses;
  `eye_mismatch` counter. With eye at use and the frame half, it's a diagnostic, not a
  requirement. Useful on both platforms.

---

## 13. Open questions (answers go into `docs/LINUX_FINDINGS.md`)

1. Do SteamVR and the Steam Frame work on Linux with OpenVR from a native game's environment? (0.1)
2. How many graphics queues does RADV expose on the 7900 XT, and which private-queue option fits? (0.2, 8.2)
3. Which of the Windows mod's exports exist in the Linux binary; is it stripped; SDL2 dynamic? (0.3)
4. FHS, scout or pressure-vessel; is `/nix/store` visible? (0.4)
5. UDP units, scaling, backward clamp, still check, smoothing. (0.5)
6. Which thread reads the socket, and does the read buffer lead to the tracker object? (0.6)
7. ~~Platform header or Linux copies?~~ Linux copies for now (section 3); revisit if the port goes upstream.
8. Do `-nocputhrottle` and focus throttling behave the same on Linux?
9. Does Linux X4 use the same `config.xml` keys, path and extension layout?

---

## 14. Definition of done (first milestone)

- [ ] Phase 0 written up in `docs/LINUX_FINDINGS.md`.
- [ ] Windows build without the flag unchanged and passing its suites (manual MSVC build).
- [ ] Linux CTest suites pass in `nix-build`.
- [ ] Installed through `pkgs.callPackage` from `configuration.nix`; `x4vr-run %command%` starts
      native X4 with the layer and shim loaded and no glibc errors.
- [ ] Stages A to E working, each logging its "installed" or "signature mismatch" line.
- [ ] Cockpit flight in stereo with 6DOF on the Steam Frame through SteamVR (OpenVR): stable world
      when looking around, including 90° sideways; no eye swaps over 10 minutes; asynchronous
      submission active on the 7900 XT.
- [ ] Theater screen for menus, recenter, flat-screen toggle.
- [ ] README Linux section: requirements, NixOS setup, launch option, X4 settings (OpenTrack UDP on,
      FOV 120°, AA and upscaler advice as on Windows), gamescope for resolution, troubleshooting
      log lines.
