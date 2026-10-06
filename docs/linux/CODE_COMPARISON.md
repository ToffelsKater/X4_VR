# Code comparison: Windows and Linux

Which Windows files the Linux build uses, which it copies, and where the copies differ, function by
function. Update this when a copy changes. How the parts work: `ARCHITECTURE.md`.

## Files

| Kind | Files |
|---|---|
| **Shared, unchanged** (both builds compile them) | `src/math.cpp`, `src/session.cpp`, `src/eye_targets.cpp`, `src/x4_camera.cpp` (tests only), `include/x4vr/` `runtime_bootstrap.hpp`, `eye_targets.hpp`, `vulkan_extensions.hpp`, `math.hpp`, `session.hpp`, `x4_camera.hpp`, `tools/launcher/hud_mod.hpp`, `tools/launcher/launcher_settings.hpp`, the shared test suites in `tests/` |
| **Linux copy of a Windows file** | `src/linux/observe_layer.cpp` ← `src/observe_layer.cpp` (commit `62569df`); `src/linux/runtime_bootstrap.cpp` ← `src/runtime_bootstrap.cpp` (`5064391`) |
| **Linux counterpart** (same job, different mechanism) | `src/linux/opentrack_client.cpp` ↔ `src/freetrack_client.cpp` (OpenTrack UDP instead of FreeTrack); `src/linux/code_scan.hpp` ↔ `include/x4vr/code_scan.hpp` (ELF instead of PE); `tools/linux/x4vr_cli.cpp` ↔ `tools/launcher/launcher.cpp` (terminal menu instead of window) |
| **Windows only** | `src/` `openxr_runtime.cpp`, `native_camera.cpp`, `native_pose_module.cpp`, `pose_detour.cpp`, `copy_call_hook.cpp`, `head_look.cpp`; their `include/x4vr/` headers; `tools/crash_watch.cpp`, `scripts/*.ps1` |
| **Linux only** | `src/linux/` (`x11_cursor`, `elf_classes`, `opentrack.hpp`, `md5.hpp`, `settings_control.hpp`, `linux_runtime.hpp`, `openxr_runtime_stub.hpp`), `tools/linux/` (`terminal_ui.hpp`, `steam_config.hpp`, and the menu's tested parts: `launch_option.hpp`, `settings_swap.hpp`, `state_files.hpp`, `gpu_status.hpp`), `linux/` (with `nix/`), `config/linux/`, `tests/linux/`, `docs/linux/`, `.github/workflows/linux.yml` |

The Windows originals haven't changed upstream since the copies were made (upstream `main`: `be68c82`).

## `src/linux/observe_layer.cpp` ← `src/observe_layer.cpp`

| Status | Functions |
|---|---|
| **Identical** | `Instance`, `instance_for`, `device_for`, `key`, `link_info`, `check`, `dump_prepare`, `barrier`, `eye_bounds`, `eye_texture`, `Ticks`, `ticks`, `SubmitRecord`, `presenter`, `pace_to_compositor`, `presenter_submit` |
| **Platform calls only** (logging, clock, sleep, `EXPORT` dropped) | `vkDestroyInstance`, `capture_root`, `dump_write`, `report_submit`, `wait_mid_frame`, `write_timeline`, `stop_submission`, `vkCreateSwapchainKHR` (one more log line), `vkGetDeviceQueue`, `vkGetDeviceQueue2`, `vkGetPhysicalDeviceProcAddr` |
| **Changed for Linux** | `Device`, `vkCreateDevice`, `vkDestroyDevice`, `make_black`: the shared graphics queue (RADV has one). `vkCreateInstance`, `vkCreateDevice`: when VR can't start, X4 runs flat (Windows fails the call). `vkCreateInstance`: X4 process check, pins the library, starts the OpenTrack client. `Presenter`, `presenter_initialize`, `readback_prepare`: SteamVR's pixel density, cursor buffers; no OpenXR session. `presenter_copy`: scaled copy, cursor drawn into the image, frame half logged, `async_submit` as at the first frame. `pair_stats`: that `async_submit`. `update_theater`: 30 flat frames before switching (X4's hints flickered). `compositor_loop`: flat screen drawn into the eye images, shared-pose pairs (the fallback image is older than the chosen one), SteamVR events, the shared-queue lock around submits only. `vkQueuePresentKHR`: shared-queue lock, game state sampled. `device_intercept`, `x4vrNegotiateLoaderLayerInterfaceVersion`: the `x4vr_*` entry points. |
| **Windows only** | Turn compensation (`MainView`, `main_view`, `track_camera`, `sample_uniform`, `turn_stats`), overlay cursor (`Cursor`, `largest_window`, `game_window`, `cursor_image`, `update_cursor`), capture and diagnostics (`Capture`, `log`, `observe`, `*_enabled`, `slow_pipelines`, `executable_stack`, `extension_list`, `probe_write`, `handle`, `quote`), the hooks those need (`vkCreateShaderModule`, pipelines, descriptor sets, buffers and memory), exported `vkGetInstanceProcAddr` / `vkGetDeviceProcAddr` |
| **Linux only** | Shared queue (`lock_if_shared`, `lock_vr_queue`, `vkQueueSubmit`, `vkQueueSubmit2`, `vkQueueSubmit2KHR`, `vkQueueBindSparse`, `vkQueueWaitIdle`, `vkDeviceWaitIdle`), `draw_cursor`, `screen_rects`, `async_submit`, `pin_library`, `raise_thread_priority`, `public_properties2`, `join`, `milliseconds`, `pause_cpu`, `x4vr_GetInstanceProcAddr`, `x4vr_GetDeviceProcAddr` |

## `runtime_bootstrap.cpp` ← `runtime_bootstrap.cpp`

| Status | Functions |
|---|---|
| **Identical** | `RuntimeCall`, `Background`, `acquire_runtime_bootstrap`, `is_runtime_bootstrap_thread`, `stereo_settings`, `write_file_later`, `take_request`, `frame_half`, `render_eye`, `record_render_pose`, `publish_view_origin`, `view_origin`, `next_present`, `frame_tag`; `RuntimeBootstrap::` `start_session`, `end_session`, `eye_setup`, `frame_timing`, `predicted_tracking`, `sample_tracking`, `submit_frame`, `submit_stereo`, `show_theater`, `hide_theater`, `hide_cursor` |
| **Platform calls only** | `RuntimeBootstrap::` `instance_extensions`, `device_extensions`, `output_device`, `show_cursor`; `capture_dir`, `background_loop`, `start_background`, `trace_event` |
| **Changed for Linux** | `RuntimeBootstrap::RuntimeBootstrap`: waits for the headset. `RuntimeBootstrap::wait_frame`: SteamVR's recenter and *Exit game* events. `read_settings`: the `shared_pose` key, Linux's `roll_gain` default (2.5, degrees). `frame_half_global`: from the X4 scan. `presented_frame`: a trace event. |
| **Windows only** | none |
| **Linux only** | `log`, `is_x4_process`, `in_executable`, `scan_x4`, `x4_sites`, `control`, `shared_pose`, `existing_runtime` |

## `opentrack_client.cpp` ↔ `freetrack_client.cpp`

Same order and names where the job is the same. Windows answers X4's `FTGetData` calls; Linux
sends one OpenTrack packet per present from its own thread.

| Status | Functions |
|---|---|
| **Identical** | `fullscreen_menu`, `game_flag` |
| **Same job, adapted** | `synthetic_pose`, `setting`, `game_export` (`dlsym`), `at_ship_controls`, `patch_code` and `log_mismatch` (the site comes from the scan), `unclamp_backward_position`, `enable_on_foot_tracking`, `camera_on_foot` (Linux offsets), `position_at_use` (finds the packet X4 used), `swap_slot` |
| **Windows only** | `FTGetData` and the FreeTrack exports (`FreeTrackData`, `FTGetDllVersion`, `FTProvider`, `FTReportName`), `EyeOffsets`, `eye_hook_ready`, `still_at_use` (Linux X4's still check is a stub), `disable_rival_trackers` (Linux X4 has only OpenTrack), `ctrl_pressed` (Linux hotkeys are in `x11_cursor.cpp`) |
| **Linux only** | `sender_loop` (the pose maths of `FTGetData`), `start_opentrack_client`, `apply_patches`, `install_eye_hook`, `sample_game_state`, `game_state`, `walk_flip`, `field`, `packet_seq`, `with_seq` |

## `code_scan.hpp` ↔ `include/x4vr/code_scan.hpp`

`parse` and `matches` are identical; the Windows header can't be included on Linux (`<windows.h>`).
`find_all` and `rip_target` work on an ELF image instead of a mapped PE. The signatures differ
(other compiler): Windows keeps a row per X4 version (8.00, 9.00), Linux has 9.00's, and each pins
the struct offsets the mod reads.
