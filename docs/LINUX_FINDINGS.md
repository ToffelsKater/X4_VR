# Linux port: Phase 0 findings

Measurements for `docs/LINUX_PORT_PLAN.md`. One section per Phase 0 step; each records the raw
facts first, then what they mean for the plan.

Machine: NixOS, AMD Ryzen 9 7900X3D (with integrated RDNA2 GPU), AMD Radeon RX 7900 XT,
Mesa 26.2.3 (RADV), Steam Frame through SteamVR. X4 9.00 native Linux build from Steam.

---

## 0.3 The game binary (2026-10-04)

### Facts

- `X4`: `ELF 64-bit LSB executable, x86-64`, **not PIE**, dynamically linked, **stripped**.
  BuildID `77cf7ac0ff32d88ffd3db7eb6ea2e025aecb711d`.
- SHA-256 `113f73d74570084f6ce18a659b365009eccf579fb66dbabb5dad2b1718045f20`, `version.dat` 900.
- Linked dynamically, among others: `libSDL3.so.0`, `libSDL3_ttf.so.0`, `libluajit-5.1.so.2`,
  `libvulkan.so.1`, `libsteam_api.so`, `libopenal.so.1`, `libpthread.so.0`, `libdl.so.2`,
  FFmpeg (`libavcodec.so.55` and others).
- 28,644 dynamic exports. Every export the Windows mod uses is present:
  `Get/SetActiveHeadTrackerHeadFilterStrength`, `IsFullscreenMenuDisplayed`,
  `IsPlayerControllingShip`, `IsHeadTrackingActive`, `IsFullscreenCutsceneActive`, plus
  `IsVRMode` and the other `ActiveHeadTracker` getters and setters.
- Tracker strings: `HEADTRACK_{NONE,DISABLE,FREETRACK,OPENTRACK,TOBII,TRACKIR,DUMMYVR,DUMMY_NOVR}`,
  `OpenTrackState` with `OPENTRACK_{DISABLED,PENDING,CONNECTED,ERROR,SHUTDOWN}`,
  `GetOpenTrackConnectionStatus`, `Get/SetOpenTrackSupportOption`, `IsOpenTrackEnabled`,
  `enableopentrack`, `OpenTrackThread`.
- C++ RTTI type names are still in the binary: `N2VR9OpenTrackE` (`VR::OpenTrack`),
  `N2VR17OpenTrackRunnableE` (`VR::OpenTrackRunnable`), `N1U23HeadTrackerCameraBridgeE`
  (`U::HeadTrackerCameraBridge`).

### Consequences

1. **Not PIE:** the executable always loads at its link address. Signature hits are absolute
   addresses; no load-base arithmetic is needed. Keep the signature checks anyway.
2. **RTTI gives the vtables directly.** In the Itanium C++ ABI a vtable's slot -1 points to the
   class's `type_info`, which points to the name string. So `VR::OpenTrack`'s vtable can be found
   from its name: string → `type_info` → vtable. The Windows hooks swap slots **in the vtable**
   (`swap_slot`), not per object, so the Linux hooks don't need the tracker object at all. The
   object arrives as `this` when X4 calls the hooked slot. This replaces the plan's "find the
   tracker from the recv buffer" step.
3. **`U::HeadTrackerCameraBridge`** is the class whose code holds the backward clamp, the
   still check and the on-foot zeroing on Windows. Its vtable, found the same way, leads to its
   methods, which makes stage D much easier.
4. **OpenTrack is read on its own thread** (`OpenTrackThread`, `OpenTrackRunnable`). This is the
   "separate reader thread" case. With eye at use it doesn't affect which eye is shown, but it
   matters in two places:
   - the per-frame logic Windows runs in `FTGetData` (game thread) needs another game-thread
     hook. Candidate: the `VR::OpenTrack` vtable method the game thread calls to fetch the latest
     pose, the counterpart of the Windows code that calls `FTGetData`;
   - the read-time "pull" idea in the plan (section 6.2) gives no benefit. The shim only needs to
     send centre-pose packets.
5. **SDL3, not SDL2,** linked dynamically. Hotkeys: wrap SDL3's `SDL_PollEvent` (and
   `SDL_PeepEvents`) in the preload shim. Note that SDL3's event struct and key codes differ from
   SDL2's.
6. **All needed exports exist,** so smoothing off and the theater decision (stage E) need no
   reverse engineering.
7. FreeTrack, TrackIR and Tobii still appear as enum names. Whether the Linux build can use any of
   them is checked in step 5 (the options screen). The rival-trackers patch is probably unneeded.

---

## 0.2 Vulkan queue families (2026-10-04)

### Facts

`vulkaninfo` lists three devices: the **RX 7900 XT** (RADV NAVI31), the **CPU's integrated GPU**
(RADV RAPHAEL_MENDOCINO) and `llvmpipe`. On the 7900 XT:

| Family | Count | Flags | Present |
|---|---|---|---|
| 0 | **1** | graphics, compute, transfer, sparse | yes |
| 1 | 4 | compute, transfer, sparse | yes |
| 2 | 1 | video decode | no |
| 3 | 1 | video encode | no |
| 4 | 1 | sparse only | no |

(`libvulkan_dzn.so` fails to load with `-9`: that's Mesa's Direct3D 12 driver for WSL, harmless.)

### Consequences

1. **The private-queue problem is confirmed.** The layer only adds a queue to a graphics family
   with a free slot (`observe_layer.cpp:388`). Family 0 has one queue, so on this GPU:
   - with OpenVR, asynchronous submission silently turns off;
   - with OpenXR, the layer stops with "OpenXR needs a private queue".
2. **Plan option 1 fits:** family 1 has four compute queues. The layer's work there is image
   copies and submission, which need no graphics. Needed: queue-family ownership transfers (or
   `VK_SHARING_MODE_CONCURRENT` eye textures) between family 0 and family 1, and a check that
   SteamVR accepts a compute-family queue in `VRVulkanTextureData_t`. Selection rule: use the
   game's graphics family if it has a free queue (unchanged Windows path); otherwise a
   compute family.
3. **Two RADV GPUs.** X4 and SteamVR must use the 7900 XT. The layer already compares device
   UUIDs with the runtime's output device. Step 4 checks which GPU X4 picks.
   `MESA_VK_DEVICE_SELECT` can force it from the wrapper if needed.

---

## 0.1 SteamVR and the Steam Frame (2026-10-04, partial)

### Facts

- SteamVR runs with the Steam Frame on this machine, but **does not start on its own** when an
  application asks for it (a known Linux issue). It has to be started by hand first. SteamVR's
  desktop sharing doesn't work either.
- `~/.config/openvr/openvrpaths.vrpath` registers the runtime at
  `~/.local/share/Steam/steamapps/common/SteamVR`, with config and logs under
  `~/.local/share/Steam/{config,logs}`. No external drivers.
- `~/.config/openxr/1/active_runtime.json` points to SteamVR
  (`SteamVR/bin/linux64/vrclient.so`), so `X4VR_RUNTIME=openxr` will also reach SteamVR.

### Consequences

1. Launch order is always SteamVR first, then X4. `x4vr-run` checks that SteamVR is running
   (`vrserver` process) before starting X4 and says so clearly if it isn't. Without SteamVR
   the mod can't find a headset, and X4 runs flat as without the mod.
2. Desktop sharing isn't used by the mod. The theater screen and cursor are the mod's own
   OpenVR overlays (or OpenXR quad layers). Their behaviour on Linux SteamVR is tested in 0.1
   with `runtime_smoke`.
3. Both runtime files live under the home directory, so a Steam container normally sees them.
   Step 0.4 confirms this.

---

## 0.4 How Steam runs native X4 on NixOS (2026-10-04)

### Facts

- Compatibility setting: default, **Steam Linux Runtime 3.0 (sniper)**.
- Launch chain: NixOS Steam FHS `bwrap` → `steam` → `reaper SteamLaunch AppId=392160` →
  `srt-bwrap` → `pv-adverb` → `testandlaunch` (an Egosoft **bash script** in the game folder) →
  the game, started as `./X4` from the game folder. The game names its main thread `Main()`, so
  `pgrep -x X4` finds nothing; use `pgrep -x 'Main\(\)'`.
- `PRESSURE_VESSEL_RUNTIME=sniper_platform_3.0.20260805.254768`, `PRESSURE_VESSEL_COPY_RUNTIME=1`.
- Loaded from the **host's Nix store** inside the container: `libc.so.6` from
  `glibc-2.42-84`, `libvulkan.so.1.4.357` (vulkan-loader 1.4.357), and Mesa 26.2.3's
  `libvulkan_radeon.so`. pressure-vessel took the host's newer glibc and graphics stack.
- `/nix/store` is visible inside the container, and so is `~/.config/openvr/openvrpaths.vrpath`.
- Environment set by pressure-vessel: `VK_LAYER_PATH` and `VK_IMPLICIT_LAYER_PATH` pointing at
  `/usr/lib/pressure-vessel/overrides/share/vulkan/…`, `VK_ICD_FILENAMES`/`VK_DRIVER_FILES`, and
  `LD_LIBRARY_PATH=lib:/usr/lib/pressure-vessel/overrides/…` (the game's own `lib/` first; the
  game ships its own libraries there, SDL3 among them).
- `LD_PRELOAD` already holds Steam's overlay (`/tmp/pressure-vessel-libs-…/${PLATFORM}/gameoverlayrenderer.so`).
  pressure-vessel rewrites preload entries into paths it makes visible.
- Session: Hyprland (Wayland), with Xwayland on `:0`. The game has both `WAYLAND_DISPLAY` and
  `DISPLAY` set; which one SDL3 uses is checked in 0.7.
- GPU: X4 opened `/dev/dri/renderD128`, which is PCI `0000:03:00.0`. The other GPU is
  `0000:13:00.0` (`renderD129`).

### Consequences

1. **The glibc risk is gone for this setup.** The game process uses NixOS's own glibc 2.42, so
   libraries built from the system's nixpkgs load without symbol-version problems. Static
   `libstdc++` is still worth doing, because the game's `lib/` comes first in `LD_LIBRARY_PATH`.
   This holds while pressure-vessel keeps choosing the host glibc (it picks the newer one), so
   `x4vr-run` should log the glibc version it sees.
2. **`/nix/store` paths work inside the container.** No copying to the home directory is needed.
   `x4vr-run` can point `LD_PRELOAD` and the layer path straight at the Nix package.
3. **Vulkan layer loading:** the loader is 1.4.357, so `VK_ADD_LAYER_PATH` is supported. It adds
   to pressure-vessel's own `VK_LAYER_PATH` instead of replacing it. Whether pressure-vessel
   passes `VK_ADD_LAYER_PATH` and `VK_INSTANCE_LAYERS` through unchanged is tested in 0.7.
4. **`LD_PRELOAD`:** `x4vr-run` runs outside the container (as `x4vr-run %command%`, where
   `%command%` includes the runtime's entry point), and prepends to the existing value. The
   preload library will be loaded into `testandlaunch`'s bash, `pv-adverb` and others too. It must
   stay inactive unless `/proc/self/exe` is the X4 binary (plan section 6.1).
5. **SDL3 comes from the game's own `lib/`.** A preloaded `SDL_PollEvent` still takes precedence.
6. The process name is `Main()`, not `X4`. Tools and the wrapper must look for it by
   executable path, not by name.

---

## 0.4 (continued) GPU, launch script, in-game options (2026-10-04)

### Facts

- `0000:03:00.0` is the Navi 31 (RX 7900 XT); `0000:13:00.0` is the Raphael integrated GPU. X4
  opens `renderD128` = the 7900 XT. In-game: *Auto-select GPU* on, graphics card
  "AMD Radeon RX 7900 XT (RADV NAVI31)".
- `testandlaunch` (Egosoft, bash): prepends `lib` to `LD_LIBRARY_PATH`, sets `GTK2_RC_FILES`,
  sources `testcommon` and checks for missing libraries and CA certificates (dialogs on failure).
  If an argument is `-prefer-wayland`, it sets `SDL_VIDEO_DRIVER=wayland,x11`; otherwise SDL3
  chooses. On a Wayland session it unsets `SDL_GAMECONTROLLER_IGNORE_DEVICES` (Steam Input).
  Finally `./X4 "$@"`: every argument reaches the game unchanged.
- Controls → *Head Tracking Support* offers only **OpenTrack Support** (off). FreeTrack, TrackIR
  and Tobii are not offered on Linux. The tracker filter, deadzone and factor options were not
  visible while OpenTrack is off; check them once it's on (0.5).
- Game settings → Camera: *Head Movement Intensity* 100, *VE Goggles Auto Reset* on. Both may act
  on head-tracking input; checked in 0.5.
- Display settings now: borderless window at the desktop's 3840x2160 (27" DP-1), TAA, FSR off,
  VSync off, frame-rate limit 120, **FOV 90°**.

### Consequences

1. OpenTrack UDP is the only tracker input on Linux, as planned. No rival-trackers patch needed.
2. Game arguments (`-skipintro -nocputhrottle`) can be passed through Steam's `%command%`;
   `testandlaunch` forwards them.
3. Whether the game uses Wayland or Xwayland by default is still to be logged in 0.7. The
   `-prefer-wayland` argument forces Wayland if needed.
4. Settings to change before VR tests, as on Windows: FOV to the maximum (120°, the
   `game_tan_y` 0.8675 calibration), anti-aliasing off or non-temporal (TAA history crosses
   eyes with alternate-eye rendering), FSR off. The desktop is already 4K, the resolution
   Windows reaches with DSR, so gamescope supersampling is optional at first.
5. Check whether *Head Movement Intensity* scales tracker input (keep it at 100 for calibration),
   and whether *VE Goggles Auto Reset* recenters on its own (it may need to be off, as X4's own
   reset breaks the calibration on Windows).

---

## 0.1 (continued) OpenVR from a Linux program: `x4vr vr-check` (2026-10-04)

### Facts

- Run directly on NixOS, OpenVR's `VR_IsHmdPresent` says **no headset** although SteamVR runs with
  the Frame: SteamVR's `vrclient.so` expects the Steam runtime's libraries.
- Through **`steam-run`** everything works: `headset_present=1`, model **`Deckard MP`** (the Steam
  Frame), recommended render size **2644x2644 per eye**, eye offsets ±0.034 m (IPD about 68 mm),
  60 of 60 head-pose samples tracked over 15 s, following look, pitch, roll and lean.
- Vulkan instance extensions SteamVR requires: `VK_KHR_external_memory_capabilities`,
  `VK_KHR_get_physical_device_properties2`, `VK_KHR_external_fence_capabilities`,
  `VK_KHR_surface`, `VK_KHR_external_semaphore_capabilities` (the layer already merges
  runtime-required extensions into X4's list).
- The seated-space yaw was around +140° to +170° at the start: SteamVR's seated zero isn't where
  the user faces. The mod recentres on its own at start and with Ctrl+F12, so this needs nothing.

### Consequences

1. **OpenVR works with the Steam Frame on Linux** through the mod's own `Session` code (plan 0.1,
   tracking half). Frames to the headset are tested later with the layer.
2. Programs started outside Steam (the `x4vr` tools) need `steam-run` to talk to SteamVR. X4
   itself runs inside Steam's runtime container, which provides those libraries (Elite Dangerous
   works the same way), so the mod inside X4 should not need anything extra; the layer test
   confirms that.
3. Render target: 2644x2644 per eye at the headset's recommendation. With alternate-eye rendering
   each eye comes from X4's swapchain, so X4's resolution sets the pixel density (as on Windows).

---

## 0.3 (continued) Tracker classes from RTTI: `x4vr elf-classes X4 Track` (2026-10-04)

### Facts

15 classes match "Track", 61 match "Camera". The ones that matter:

| Class | Base | type_info | vtable address point | Slots |
|---|---|---|---|---|
| `VR::OpenTrack` | `VR::TrackerInterface` | `0x3c61bb0` | `0x3c62520` | 74 |
| `VR::OpenTrackRunnable` | `XLib::Runnable` | `0x3c61b98` | `0x3b169a8` | 3 (the reader thread's body) |
| `U::HeadTrackerCameraBridge` | `UI::XAnark::ICamera` | `0x3c62c20` | `0x3b1c240` | 5 |
| `VR::TrackerInterface` | | `0x3c61760` | none (abstract) | |

- `VR::OpenTrack` slots 0 and 1 are the two Itanium destructors (`0x1a22860`, `0x1a1b690`). Many
  slots share small stubs (`0x1089f10`, `0x1089ed0`, `0xb70c60`, `0x9b7140`), typical of the
  default "not supported" answers of a tracker interface that several trackers implement.
- `U::HeadTrackerCameraBridge`'s three own methods are `0x19185c0`, `0x191c330`, `0x191c270`.
  `VR::OpenTrack` slots 32 (`0x19183f0`) and 47 (`0x1918430`) sit in the same code area.
- A few names show a second "type_info" without a vtable at a low address (`0x4c0998`,
  `0x495e88`, ...): data that happens to point at the same name string, not real type_infos.
  They don't affect the classes above.

### Consequences

1. **The tracker vtable is found without the game running** (stage B's first step).
2. Mapping from the Windows hooks, as a hypothesis to verify by disassembly: MSVC puts one
   destructor entry first and Itanium two, so with the destructor declared first, MSVC slot `k`
   would be Itanium slot `k+1`. Windows' position accessor (`0x108`, slot 33) would then be
   **slot 34 (`+0x110`, `0x1a0dda0`)** and the still check (`0x28`, slot 5) **slot 6 (`+0x30`,
   `0x1398960`)**. MSVC also orders overloaded virtuals differently, so this can be off; the
   function bodies decide (what each reads and writes in the tracker object).
3. The bridge's code (backward clamp on Windows) is around `0x1918000`–`0x191d000`, a small area
   to search for stage D.

---

## 0.7 Probe layer inside X4 (2026-10-04)

### Facts

- `x4vr-probe-run` as the Steam launch option works: **the layer loads in X4 inside the Steam
  Linux Runtime container** through `VK_ADD_LAYER_PATH` with `/nix/store` paths, and the
  preloaded socket probe is active in the X4 process (Steam's overlay is preloaded after it).
- Instance: app "X4", API 1.2, surface extensions for Xlib, XCB and Wayland. X4 creates an
  **Xlib surface: X11 through Xwayland** on the Hyprland desktop.
- Device: RX 7900 XT. **X4 takes queue 0 of family 0 (graphics) and queue 0 of family 1
  (compute/transfer, 4 queues)**; it presents from its main thread (`Main()`, tid = pid) on
  family 0, queue 0. Device extensions: `VK_KHR_swapchain`, `VK_KHR_buffer_device_address`,
  `VK_KHR_push_descriptor`, `VK_EXT_memory_budget`, `VK_EXT_memory_priority`,
  `VK_EXT_descriptor_indexing`, `VK_KHR_external_memory_fd`, `VK_KHR_external_semaphore_fd`,
  `VK_KHR_timeline_semaphore`.
- Swapchain: 3840x2160, `B8G8R8A8_UNORM`, colour space sRGB nonlinear, at least 4 images,
  present mode **IMMEDIATE** (VSync off), usage `0x1b` (includes transfer source, so the layer can
  copy from it as on Windows). It is recreated a few times during start-up.
- Frame rate at the main menu: ~100 per second with the window focused, **~12.8 per second
  unfocused** (X4's background throttle, as on Windows without `-nocputhrottle`).
- No UDP socket yet: OpenTrack Support was still off (only a TCP socket for online services).

### Consequences

1. Loading through the wrapper is proven; plan section 9.1's approach holds, no copying of
   libraries needed.
2. **Private queue on AMD:** family 1 has 4 queues and X4 uses only index 0, so the layer can add
   one more queue to X4's family-1 request (index 1) at `vkCreateDevice`. Copies and submission
   run there; eye textures need a queue-family ownership transfer from family 0 or concurrent
   sharing (plan 8.2, option 1).
3. The swapchain format and usage match the Windows capture path. 8-bit UNORM goes to its SRGB
   twin for OpenXR, as on Windows.
4. Launch with `-nocputhrottle` in VR runs (the window isn't focused while wearing the headset).

---

## 0.5 / 0.6 OpenTrack head tracking in the cockpit (2026-10-04)

### How X4 reads the socket (socket probe)

- Turning on *OpenTrack Support* starts thread **`OpenTrackThread`** (an SDL thread). It creates a
  UDP socket and binds it to port 4242 (code at `0x1a22d65`/`0x1a22da4`), then loops:
  **`select()` with a 1 s timeout (`0x1a22e3b`), then one blocking-style `recvfrom` of 48 bytes**
  (`0x1a22e81`) per packet, into a **local buffer** (`0x753268ff5d50`, not the tracker object).
  The loop sits in the function called from `0x1a37276`.
- At 90 packets per second from `x4vr udp-send`: 90 `select` and 90 `recvfrom` per second, every
  packet read, no backlog, no errors, total waiting time under 0.3 ms per 2 s.
- The menu then shows "OpenTrack connection established" and a new **OpenTrack** section:
  *head motion smoothing 5*, *head rotation factor 100%*, *head position factor 100%*.

### What the camera does (by eye, test table in docs/LINUX_PHASE0.md)

| Test | Result |
|---|---|
| Where it works | **Only when piloting the ship.** Standing in the ship: no effect (as on Windows, where X4 zeroes the pose on foot). |
| Focus | Keeps working while the terminal has focus. |
| `yaw +30` / `-30` | **Right** / left |
| `yaw 90` / `yaw 180` | 90 stops short of the shoulder; **180 gives roughly a real 90°**: angles are scaled down, as on Windows (angle/π × 85°) |
| `pitch +20` | **Up** |
| `roll +20` | **Anticlockwise** (head tilts left) |
| `x +5` | Head moves **left** |
| `y +5` | **Up** |
| `z +5`, `z -5` | Too small to see |
| `z -30` / `z +30` | **-z moves the head forward; +z (backward) stays at zero**: the backward clamp exists on Linux too |
| `x 50` | Much further than `x 5` |
| Small change after 5 s still (`yaw 30` → `31`) | Moves: no "still pose" freeze seen |
| `yaw 30` held 30 s | No drift back to centre (*VE Goggles Auto Reset* on has no visible effect) |
| `alt yaw 10` (±10° every packet) | Irregular shaking, not a steady pattern |
| `pause` (no packets) | View stays at the last pose |
| *Head Movement Intensity* 50 | No visible difference: it doesn't scale the tracker |

### Consequences

1. **OpenTrack UDP is a working head-pose input on Linux.** The pull-shim idea is dropped for good:
   the mod only needs to send packets (plan section 6.2).
2. **Signs:** OpenTrack +yaw turns right, +pitch looks up, +roll tilts left, +x moves left, +y up.
   The mod's axis signs for Linux are set from this (the OpenVR seated pose uses +yaw = left).
3. **The Windows quirks carry over.** Angles look scaled by 85/180 (Windows: `yaw_gain =
   pitch_gain = 2.1177`, here in degrees the same factor 180/85), and **backward head position
   (+z) is zeroed**, so stage D (backward clamp patch) is needed on Linux exactly as on Windows.
   The precise numbers still come from reading the camera view matrix, as `tools/vr_calibrate.py`
   does on Windows (0.8, or matrix logging in the layer), before setting Linux defaults.
4. **The irregular shaking with alternating poses** shows that the game samples the latest packet
   at its own frame times, unsynchronised with the reader thread: which packet a frame uses is
   random. So the eye can't be picked per packet; it must be added when X4 builds the camera
   (eye at use, plan stage B), as on Windows. Smoothing 5 also blends the alternation.
5. *Head motion smoothing* can't go below 5 in the menu (as on Windows); the mod sets 1 through
   the exported `SetActiveHeadTrackerHeadFilterStrength`.
6. X4 holds the last pose when packets stop, and doesn't recentre on its own: the mod's own
   recentring (`recenter=`, Ctrl+F12) behaves as on Windows.

### Launch log (stderr.log)

- The loader inserted `VK_LAYER_X4VR_probe` as instance and device layer from the `/nix/store`
  path given by `VK_ADD_LAYER_PATH`; `VK_INSTANCE_LAYERS` was honoured inside the container.
- The only `ERROR` lines are `ld.so ... wrong ELF class ... ignored` for the 32-bit Steam
  helpers on the launch path (the same lines appear for Steam's own overlay library). Harmless.

## Phase 0 status

Done: 0.1 (OpenVR with the Frame, tracking), 0.2, 0.3, 0.4, 0.5 (qualitative), 0.6, 0.7.
Open: 0.5 exact scale factors and 0.8 camera uniform layout, both by reading the camera matrices
once the layer does more than logging. Frame submission to the Frame is tested with the ported
layer in Phase 3.

## Phase 1, first headset runs (stage A)

1. **Steam streams X4 into the headset unless told not to.** With SteamVR running, Steam launches
   non-VR games with `StreamForOpenVR=1` (and `SteamStreamingVRPairedInvite`) and takes over their
   window for its flat "theater" stream; X4 then loses its window (`Base::ShowCursor() - failed
   to show cursor: Invalid window`) and quits, also later when the Frame reconnects. Plain X4
   fails the same way, so it isn't the mod. `x4vr-run` unsets both; X4 must be started from Steam
   on the PC (starting it from the headset's library may stream it again). Later (Steam beta
   client) Steam captured X4 through its overlay anyway: the Frame showed the splash screen in 2D
   for a moment and X4 quit (exit status 130) as soon as the mod submitted its first frame, also
   after a reboot. Without the overlay (`gameoverlayrenderer.so` dropped from `LD_PRELOAD`,
   `DISABLE_VK_LAYER_VALVE_steam_overlay_1=1`) it runs; `x4vr-run` does both
   (`X4VR_STEAM_OVERLAY=1` keeps the overlay).
2. **Stereo in the cockpit works:** 3D, head turning and leaning match the head (Windows gains
   and `pos_scale` 3.6 unchanged). The cockpit may look slightly too large (ipd/scale calibration).
   120 submits/s, each eye 60 new images/s, almost no late frames (`pair_stats.txt`).
3. **The right eye jitters when the head moves** (still: fine; worse with faster turns; with
   `predict=0` or `0.06` worse, ghosting between old and new position; `submit_pose=0` less jitter
   but both eyes misaligned). This is stage A's eye-per-packet limitation (Phase 0 finding 4):
   some right-eye frames are built from another packet than the pose they're submitted with.
   Fixed by stage B (eye at use).
4. **SteamVR's overlay shows nothing on Linux.** The theater overlay is created and
   `SetOverlayTexture` succeeds, but the screen is invisible (black) anywhere in the headset. The
   Linux layer now draws the virtual screen into the eye images instead, submitted with the head
   pose of when it was placed, so SteamVR's reprojection keeps it fixed in space
   (`X4VR_THEATER_OVERLAY=1` for the overlay). The cursor overlay will need the same treatment.
5. **SteamVR asks for 4202x4266 per eye but the Frame link downsamples above 3458x3458**
   (`vrcompositor.txt`): lower X4's per-app resolution in SteamVR, later cap it in the mod.
6. **A wireless dropout leaves SteamVR in standby:** vrlink logged video stream resets, then
   `Connection inactive` and `entering standby`; after reconnecting SteamVR stayed in standby and
   the Frame showed a flat stream. Only restarting SteamVR (and X4) recovered. The mod kept
   submitting (SteamVR blocked each submit ~150-200 ms). Recovering without restarting X4 needs
   the mod to reconnect to a restarted SteamVR.

## Stage B: X4's OpenTrack tracker (Linux 9.00 disassembly)

`VR::OpenTrack` (vtable address point `0x3c62520`, 74 slots):

| Slot | Address | What it does |
|---|---|---|
| 2 (`+0x10`) | `0x1a1b720` | Per-frame update (game thread). If the reader thread left a new packet (`+0x20..+0x48`, flag `+0x50`): copies its six doubles to `+0x78..+0xa0`, sets `+0xa8`; else zeroes them and clears `+0xa8`. Position = xyz * `+0x114` (position scale; setter slot 49 clamps to [0.25, 1.25]) into `+0xb0` (minus `+0xc0` after a recentre, flag `+0x110`); angles = (-yaw, pitch, roll) * a constant (`0x2e27138`) * `+0x118` into `+0xe0`. Smoothing with alpha = 1/`+0x120` (strength) into `+0xd0` (position) and `+0x100` (angles). |
| 33 (`+0x108`) | `0x1a0dd60` | Angle accessor: `+0x108`, -`+0x100`, `+0x104`, each / a constant (`0x2e27154`). |
| 34 (`+0x110`) | `0x1a0dda0` | **Position accessor**: `+0xd0..+0xd8` / 180, clamped to [-1, 1] (so +-180 packet cm at scale 1). |
| 49, 50 / 53, 54 / 57, 58 | | Set/get position scale `+0x114`, angle scale `+0x118`, smoothing strength `+0x120`. |
| 67 (`+0x218`) | `0x1a0dca0` | Recentre: stores the current position/angles into `+0xc0` / `+0xf0`, sets `+0x110`. |
| 6 (`+0x30`) | `0x1398960` | `return false` (a stub; not the Windows still check). |

So the Windows eye-at-use hook maps directly: slot 34 is the position accessor (Windows `0x108`),
and the eye offset in its fields is the packet offset times `+0x114`. The layer hooks slot 34;
packets carry a sequence number in roll's low mantissa bits, read back from `+0xa0` when `+0xa8`
is set, so the hook knows which packet (and headset pose) X4 used. Not found yet: a "still" check
like Windows' (X4 skipping the accessors when the pose barely changes), if Linux X4 has one.

## Stage C: frame half candidate (Linux 9.00)

The Windows reader pattern (`imul 0x270` after `xor 1`) only matches Detour's `dtCrowd` code on
Linux (its agent struct is also 0x270 bytes). Reads of a rip-relative global followed by `xor 1`
leave one fitting candidate: **the int at `0x72a0fa0`**, returned xor 1 by the function at
`0x218e220` (`mov 0x72a0fa0,%eax; xor $1,%eax; ret`), next to a table at `0x454d9a0` indexed by
`xor 1` (`0x218e237`, `0x21988d7`). The layer uses it with `eye_from_half=1` and logs how often
it flips per present ("stage C check"); the xor settings (`half_xor_render`, `half_xor_present`)
are found in the headset as on Windows.

**Confirmed in the headset:** the global flips on 997 of 1000 presents, and `eye_from_half=1`
with `half_xor_present=0` (`half_xor_render=0`) removes the HUD doubling, the A-menu flicker and
the swap flash every few seconds (`half_xor_present=1` doubles the HUD). Linux defaults set to
that. Left: some jitter/ghosting in the right eye on head movement (pose pairing, `delay`).

## Right-eye ghosting: the Frame link uses the left eye's pose for both eyes

With the eyes and poses paired correctly (trace: 98 % of presents matched to the pose one frame
old, the rest split evenly between the eyes; per-eye optics mirror-symmetric, no cant), the right
eye still ghosted on head movement: with no eye offset (`ipd_scale=0`), swapped offsets, swapped
halves, right eye submitted first, and in pair mode; mono (same image and pose in both eyes) was
clean. `pose_from_eye=1` (both eyes submitted with the right eye's pose) moved the ghosting to the
left eye, `pose_from_eye=0` kept it in the right (both diagnostic switches, like `submit_right_first`,
were temporary and are removed): **SteamVR's link to the Frame reprojects both
eyes with the left eye's pose.**

Fix (`shared_pose=1`, default): the pose sender skips the packet before each right-eye frame, so X4
builds both eyes of a pair from one head pose (eye offsets still added at use), and the layer
submits pairs with identical poses, stepping the newer eye back to its image with the other eye's
pose when needed. The log reports how many pairs needed that and how many still went out with
different poses.

## HUD distance extension on Linux X4 9.00

`x4vr hud` (the Windows launcher's HUD mod) works, with two prompts from X4: "Modified game
detected" (online features off, saves made with it stay flagged) and **Protected UI Mode**, which
lists the extension and, while on, doesn't load its replaced Lua scripts: the anchors move (HUD
farther) but the size factors don't (HUD smaller). Disabling protection didn't change the size either: the game catalogs hold each UI script as
`.lua`, `.xpl` and `.sig` signatures for both, and X4 loads the `.xpl`, which is precompiled
bytecode (`x4vr game-grep` finds no source text in any of the 81 UI `.xpl` files). The Linux
Windows launcher's mod only replaces the `.lua`. With protection off
(`<uisafemode>false</uisafemode>`) X4 only logged failed signature checks for the two XML anchor
files, and used them. First left as is to stay with the Windows mod; taken up again 2026-10-05:
`x4vr hud` puts the patched Lua source at each `.xpl` path as well (Lua's loader takes source or
bytecode). `x4vr hud --refresh` rebuilds an installed extension with it, since the source hash
now covers the `.xpl` files. First headset check (2026-10-05): the radar and message HUD look
bigger than without it, so X4 loads the replaced `.xpl` text. Confirmed at other factors: the HUD
keeps its apparent size and moves back. Kept as a Linux-only addition (Windows replaces only the
`.lua`).

## Stage D (backward clamp): search so far, paused

Windows patches a `jae` in the head-tracker bridge that zeroes backward head position. On Linux
9.00 the bridge (`U::HeadTrackerCameraBridge`, vtable `0x3b1c240`: slots 0/1 constants, 2
`0x19185c0` position, 3 `0x191c330` angles, 4 `0x191c270` FOV) has no such check: its position
method calls `VR::OpenTrack` slot 34 and stores `(x, y, -z)` at `+0x10`, nothing else. It is the
only caller of slot 34 (the other `call *0x110` with that shape, `0x15c80b8`, takes four outputs).
Searches for the Windows shape (sign flip with the constant at `0x2e245d0`, compare, zero) found
194 sites, none in camera code; callers of bridge slots 2 then 3 (54 sites) showed no clamp in the
first candidates. The zeroing is downstream, wherever the camera reads the bridge's `+0x10`;
finding it needs a different approach (e.g. watching the value at runtime). Paused 2026-10-04.

**Runtime watch (2026-10-05, `X4VR_WATCH_HEAD=1`, `src/linux/head_watch.cpp`).** Hardware
watchpoints, on every thread, over 10 windows of 20 s (cockpit and other states):
- The bridge's `+0x10` stayed `0,0,0` and nothing touched it, also in the cockpit with the head
  moving: X4 doesn't use the bridge for the cockpit view. The "only caller" above was wrong.
- `VR::OpenTrack` (heap object) `+0xd0` position / `+0x100` angles: written by slot 2's update
  (`0x1a1b8b2..0x1a1b8e2`), read by slot 34 (`0x1a0dda8..0x1a0ddd0`) and the function just before
  it (`0x1a0dd68..0x1a0dd97`, the angles accessor). Nothing else in X4 reads them.
- Slot 34 has **one caller, return address `0xfec24e`**, about once per frame while head tracking
  applies; it stops when it doesn't (window 10). That function is where the cockpit view gets
  the head position, so it is where the backward clamp and the on-foot gating should be.
- With `theater=1` (default), a view without ship controls goes to the theater screen and the
  sender sends the centred pose (position 0). So far, standing in the ship read as "X4 zeroes
  the pose on foot", but the mod sent the zero itself. Windows has a walking check
  (`freetrack_client.cpp`); Linux doesn't yet. Testing on foot needs `theater=0`.

**The caller (`0xfeb...`, X4's per-frame camera input; disassembly 0xfeb000..0xfed400)** is the
Linux counterpart of what the Windows notes call the bridge (Windows `0x9fd870`): keyboard look
input into locals, then the tracker, then the camera controller `0x1933d00` with yaw/pitch/roll
(`-0x100/-0xfc/-0xf8(%rbp)`) and position (`-0xf4/-0xf0/-0xb0`, z last):
- `0xfeb6c5`: camera controller = `[[0x3db6948]+0x3e8]`; its mode `+0x880` == 0 goes to `0xfec070`:
  `call 0x1e07060` (no ship); true and the tracker (manager `0x3db8938`, `+0x148`) not an eye
  tracker (slot 8, `+0x40`) → the camera gets an all-zero pose (`0xfec0bc..0xfec0db`). That is
  the on-foot zeroing. Patch: the `je 0xfeb6fc` at `0xfec077` → `jno` (byte `0xfec078` `0x84` →
  `0x81`; `test` clears OF, so always taken).
- `0xfeb6fc`/`0xfec1b0`: tracker slots 6 (`+0x30`) and 23 (`+0xb8`) gate the read; with mode 0,
  slot 33 (angles) and slot 34 (position, the call at `0xfec248`) fill the locals.
- `0xfeb71d`: `if (tracker slot 19 (+0x98) != 7 && z > 0) z = 0`: the backward clamp. Patch: the
  `jbe` at `0xfeb73f` → `jmp` (`0x76` → `0xeb`).
Both are applied by `apply_patches()` in `src/linux/pose_sender.cpp` (bytes checked first;
`X4VR_PATCHES=0` turns them off). Windows' second on-foot patch (Camera::GetOffset without a
movement controller) still needs its Linux counterpart, if on foot needs it.

**On foot, after the zeroing patch (2026-10-05).** In the headset the view still doesn't follow
the head on foot: the aim point stays centred. `X4VR_WATCH_HEAD=2` shows the same path as in the
cockpit: camera mode 0, the gates pass (`0xfeb6fc`, `0xfec1b0`, `0xfec1f9`), angles and position
read every frame. So the pose reaches the controller `0x1933d00` and is dropped later. The
controller's head path `0x1935ac1` stores the position at controller `+0x5a0` and the rotation
at `+0x5b0` (the counterpart of Windows' `Camera+0x590`). `X4VR_WATCH_HEAD=3` finds their
readers. A search for Windows' `Camera::GetOffset` opening (movement-controller check, then the
camera manager `0x3daab80`) found nothing.

**Head offset readers (`X4VR_WATCH_HEAD=3`).** Written every frame by `0x1935b9b` (position) and
`0x1e02200` (rotation), in the cockpit and on foot. Read:
- In the cockpit by `0x1628ce0` (called from `0x1628dd0`, which runs only when
  `[[0x3db6948]+0x238]` is set). It copies controller `+0x5a0..+0x5df` to the camera
  `+0x280..+0x2b0`, then, if controller `+0x18` is set, hands it to that object's slot `0x138`.
  Without it, it writes identity. This is the shape of Windows' `Camera::GetOffset`
  (controller `+0x20`). It is not called on foot.
- On foot by `0x11d9380` (4 calls per frame). With camera mode 0 and no ship (`0x1e07060`), and
  its `+0x68` flag set, it takes the head offset (via `0x1dccd30`) and composes it into, or
  replaces, the transform it is given. Which transform is unknown: `X4VR_WATCH_HEAD=4` records
  the callers (return addresses) of `0x11d9380`, `0x1628ce0`, `0x1628dd0` and `0x1dccd30`.

## Mouse cursor

X4's cursor is the X server's (Xwayland), not part of its swapchain, as on Windows. The Windows
layer shows it as a SteamVR overlay, which doesn't display on the Frame. The Linux layer reads it
over XCB (loaded at runtime, `src/linux/x11_cursor.cpp`): X4's window by `WM_CLASS` "X4" (or
`_NET_WM_PID`), the pointer over it (`QueryPointer`), the image through XFixes
(`GetCursorImage`; X4 hides the cursor with an empty image). The presenter copies the cursor's
opaque pixels (alpha >= 128; a transfer can't blend) into each copied game image at the pointer's
place, so it shows in the cockpit and on the virtual screen. Tested against Xvfb (window found by
class, position and image read, hidden outside the window). `cursor=0` in stereo.txt turns it off.

**On-foot camera offset found (2026-10-05, `X4VR_WATCH_HEAD=4` + disassembly).** The camera
update `0x1646510` calls the cockpit reader `0x1628ce0` only when `0x1628210` returns true: in a
ship (`[[0x3db6948]+0x238]+0x6aa8` != the invalid id at `0x3daa6f0`), or when controller
`+0x780` is the player entity and the movement controller `+0x18` is set. Then it composes the
offset into the camera (`+0x10..+0x40`) and sets `+0x2c0`; otherwise it takes a previously
applied offset out again. The reader writes identity without a movement controller. Patches
(`apply_patches`): `0x1646545` `0x85` → `0x81` (`jne` → `jno`) and `0x1628d4a` `0x3d` →
`0x73` (to the reader's return `0x1628dbe`). These are Windows' two Camera::GetOffset conditions.

**Gate patch result (2026-10-05).** With both camera-offset patches, on foot the view follows the
head (same amount as the head), but the camera jumps to the cockpit seat and the player can't
walk. Past the gate, `0x1646510` takes the seated path: `0x1628510(this, 0, 0)` re-parents the
camera (its parent `+0x360`, transform `+0x10..+0x40` converted through the parent's
`+0xe0..+0x110`), it sets `+0x2c0`, and `0x164662a` re-attaches it to an object found from
`+0x360` (component types `0x54`/`0x75`, event `0x16c5fe0`). So `0x1646510` is the seat camera,
not a plain "apply the offset". The gate is now opt-in (`X4VR_ONFOOT_GATE=1`); the on-foot view
needs the offset applied in the walking camera's own update instead.

**`X4VR_ONFOOT_GATE=2` result (2026-10-05).** Same as gate 1: the camera floats to the seat, no
walking. So the predicate `0x1628210` is not where Windows' patch acts. Windows' commit 8342ef1
patches inside `Camera::GetOffset` (0x97a300) at +0x113 (0x97a413): `cmp qword [rsi+0x20], 0`
on a camera **parameter**, then the camera manager's `+0x230` entity against `camera+0x770`,
then `[manager+0x3d0]`. The offset block is `0x97a5a8` and the exit `0x97a694`. On foot that
function exits before it reads the head offset, so the read watch (mode 3) can't see its Linux
counterpart. Next: a static search for code that reads `+0x5a0` (and `+0x5b0`) of a camera
after checking a `+0x18`/`+0x20` member of the same object.

**Camera::GetOffset found (2026-10-05).** `0x1929f70(camera, out, …)`:
- checks camera `+0x780`/`+0x788`, then the mode (`+0x880`) and calls `0x18a0090`;
- then `mov 0x18(%rbx),%r12; test; je 0x192a216` (no movement controller → return);
- compares camera `+0x780` with `[0x3db6948]+0x238` (fallback `0x3db6c00`) and the camera with
  `[0x3db6948]+0x3e8`. Equal → `0x192a2dd` (`0x1de5df0`, then `0x1628210`: seated returns).
  Otherwise, through the movement controller's slot `0x138`.
- composes the head offset (`+0x5a0..+0x5df`) into `out` at `0x192a15e`. The blocks
  `0x192a284` and `0x192a2b6` reach it without a movement controller.

This is Windows' 0x97a300 check for check. The patch is the same: the exit `je` (displacement
byte `0x1929fff`, `0x13` → `0x81`) goes to `0x192a284`. The gate experiments
(`X4VR_ONFOOT_GATE`) and the `0x1628d49` reader patch are removed.

**Turn compensation (2026-10-05).** Not needed on Linux with `shared_pose=1`: no double vision
when turning with the mouse on foot. The harshness reported was X4's turn speed, its own input
setting. Windows' method (rotate the older eye's submitted pose by the camera turn) can't work on
the Steam Frame, which reprojects both eyes with the left eye's pose. If ever needed, shift the
other eye's image by the turn while copying it.

**Pattern scan (2026-10-05).** `x4vr patterns` and the mod's startup scan find all six sites
on X4 9.00 at the known addresses (clamp `0xfeb72b`, zeroing `0xfec070`, camera offset
`0x1929ff6`, player global `0x3db6948`, frame half `0x72a0fa0`, VR::OpenTrack vtable `0x3c62520`
with slot 34 `0x1a0dda0`). The in-game scan takes 51 ms. Patches and hook applied; play unchanged.
