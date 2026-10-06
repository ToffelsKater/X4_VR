#pragma once
#include <cstddef>
#include <cstdint>
// Linux-only additions to the shared runtime. include/x4vr/runtime_bootstrap.hpp stays the Windows
// header, unchanged (docs/LINUX_PORT_PLAN.md, section 3); what Linux needs beyond it lives here.
#include <x4vr/runtime_bootstrap.hpp>
#include <memory>
#include <string>

namespace x4vr::linux_port {
// "X4VR ..." diagnostic lines, the counterpart of OutputDebugStringA on Windows: written to
// stderr and appended to $X4VR_CAPTURE_DIR/x4vr.log.
void log(const std::string& line);
// The process is X4 (its executable is named X4). The layer only starts VR in X4.
bool is_x4_process();
// The runtime the layer created, or null before that. The pose sender never creates it itself.
std::shared_ptr<RuntimeBootstrap> existing_runtime();

// X4's state, read through its exported UI functions on X4's main thread (the present hook) and
// used by the pose sender's own thread (Windows reads them in FTGetData, on X4's game thread).
struct GameState {
    bool sampled = false;       // at least one sample taken
    bool fullscreen_menu = false;
    bool controlling_ship = true;
    bool head_tracking = false; // X4 applies tracker input
};
void sample_game_state(); // X4's main thread only
GameState game_state();

// Pose sender (stage A, docs/LINUX_PORT_PLAN.md section 7): sends the head pose to X4's OpenTrack
// socket once per present. Started by the layer once the runtime exists; X4 only.
void start_pose_sender();

// Whether [address, address+size) lies in the main executable's loaded segments (X4 is non-PIE:
// its code and data addresses are fixed). Checked before reading or patching X4 at known addresses.
bool in_executable(uintptr_t address, size_t size);

// SteamVR's link to the Steam Frame applies the left eye's submitted pose to both eyes (measured
// with pose_from_eye, docs/LINUX_FINDINGS.md). With shared_pose (stereo.txt, default 1) X4 builds
// both eyes of a pair from one head pose (the pose sender skips the packet before a right-eye
// frame) and the layer submits matching pairs, so one pose is right for both images.
bool shared_pose();

// The live controls of `x4vr ctl` ("recenter", "flat"), from the mod: Ctrl+F12 / Ctrl+F11 while X4
// has focus (x11_cursor.cpp) and SteamVR's recentre (runtime_bootstrap.cpp). Edits stereo.txt.
void control(const char* action, const char* source);
}
