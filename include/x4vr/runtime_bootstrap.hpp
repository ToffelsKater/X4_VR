#pragma once
#include <x4vr/session.hpp>
#include <chrono>
#include <filesystem>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

namespace x4vr {
class OpenXRRuntime;
// The Vulkan layer's device, for the OpenXR session (OpenVR takes these per submitted texture).
// physical: the layer's own handle; output_physical: the runtime's handle from output_device().
using VulkanFunction = void (*)();
struct XrVulkanContext {
    VkInstance_T* instance{};
    VkPhysicalDevice_T* physical{}, *output_physical{};
    VkDevice_T* device{};
    VkQueue_T* queue{}; // the layer-private queue
    uint32_t family{}, queue_index{};
    VulkanFunction (*gipa)(VkInstance_T*, const char*){};
    VulkanFunction (*gdpa)(VkDevice_T*, const char*){};
    int (*set_loader_data)(VkDevice_T*, void*){};
};
// Initialization and unpaced tracking bridge: no queue access, tracking wait,
// or image submission. One shared-library instance for both native/Vulkan modules.
// Calls are serialized on Vulkan's caller thread: dispatching extension queries
// to a worker while inside vkCreateInstance can invert loader locks.
// Backend: OpenVR, or OpenXR when the launcher sets X4VR_RUNTIME=openxr (read once).
class RuntimeBootstrap {
    std::mutex mutex_;
    Session session_;
    std::unique_ptr<OpenXRRuntime> xr_;
public:
    RuntimeBootstrap();
    ~RuntimeBootstrap();
    bool openxr() const { return xr_ != nullptr; }
    // OpenXR: create the session on the game's device (first present); OpenVR: nothing to do.
    std::string start_session(const XrVulkanContext& vulkan);
    void end_session(VkDevice_T* device); // before that device is destroyed
    std::vector<std::string> instance_extensions();
    std::vector<std::string> device_extensions(VkPhysicalDevice_T* physical);
    VkPhysicalDevice_T* output_device(VkInstance_T* instance);
    FrameStatus sample_tracking(Matrix& tracking_from_head);
    // Stereo presentation (alternate-eye rendering). Seated pose predicted
    // `seconds` ahead; eye setup is constant per session.
    FrameStatus predicted_tracking(Matrix& tracking_from_head, float seconds);
    struct EyeSetup { std::array<Matrix, 2> head_from_eye; std::array<std::array<float, 4>, 2> tangents; }; // l,r,t,b
    EyeSetup eye_setup();
    // Submit both eye textures (with bounds), hand off, then block in WaitGetPoses
    // for pacing. Caller serializes the queue. Returns an empty string on success.
    // poses: seated head pose each eye image was rendered with (Submit_TextureWithPose);
    // with_pose false submits without poses (compositor assumes the WaitGetPoses pose).
    std::string submit_stereo(const std::array<vr::VRVulkanTextureData_t, 2>& images,
                              const std::array<vr::VRTextureBounds_t, 2>& bounds,
                              const std::array<Matrix, 2>& poses, bool with_pose = true);
    // The same protocol split in two, for a dedicated submission thread. handoff: call
    // PostPresentHandoff after Submit (OpenVR). marks (diagnostics): when Submit(left),
    // Submit(right) and the handoff returned.
    using SubmitMarks = std::array<std::chrono::steady_clock::time_point, 3>;
    std::string wait_frame();
    std::string submit_frame(const std::array<vr::VRVulkanTextureData_t, 2>& images,
                             const std::array<vr::VRTextureBounds_t, 2>& bounds,
                             const std::array<Matrix, 2>& poses, bool with_pose = true, bool handoff = true,
                             SubmitMarks* marks = nullptr);
    // SteamVR's timing of a recent compositor frame (OpenVR only; false for OpenXR).
    bool frame_timing(vr::Compositor_FrameTiming& timing, uint32_t frames_ago);
    // Theater mode: the flat game image on a world-fixed virtual screen (an OpenVR overlay),
    // `width` metres wide, centred at seated_from_screen. A null image keeps the last one.
    std::string show_theater(const vr::VRVulkanTextureData_t* image, const vr::VRTextureBounds_t& bounds,
                             const Matrix& seated_from_screen, float width);
    void hide_theater();
    // Mouse cursor overlay (X4 uses the Windows cursor, which never reaches the swapchain).
    // rgba: new image (null keeps the last). placement: cursor centre in seated space
    // (on_screen, on the theater screen) or relative to the headset; width in metres.
    std::string show_cursor(const uint8_t* rgba, uint32_t width, uint32_t height, bool on_screen,
                            const Matrix& placement, float width_m);
    void hide_cursor();
private:
    vr::VROverlayHandle_t theater_{vr::k_ulOverlayHandleInvalid};
    // One overlay per cursor image, uploaded once. Every SetOverlayRaw keeps a SteamVR memory
    // block for good, and vrclient refuses new ones past ~200 ("201 blocks are already
    // outstanding", vrclient_X4.txt). Uploading on each shape change used them up after a while
    // in menus: from then on every upload failed and the cursor stayed invisible.
    std::vector<std::pair<size_t, vr::VROverlayHandle_t>> cursors_; // image hash, overlay; most recently shown last
    vr::VROverlayHandle_t cursor_{vr::k_ulOverlayHandleInvalid}; // the one shown
};
// Alternate-eye bookkeeping shared by the FreeTrack pose source and the Vulkan layer.
// Tunables are re-read from %X4VR_CAPTURE_DIR%/stereo.txt ("key=value" per line).
// delay = presents between the game reading a pose and presenting that frame.
struct StereoSettings { bool stereo = true; int delay = 2, recenter = 0; float ipd_scale = 1, pos_scale = 3.6f, yaw_gain = 2.1177f, pitch_gain = 2.1177f, roll_gain = 3.14159f, predict = 0.035f, game_tan_y = 0.8675f; // 0.8675 = X4 FOV slider at maximum (120 deg)
    // Calibration only: synthetic head pose (x y z metres, yaw pitch roll degrees, relative to
    // the recentred origin) plus a +/- delta alternating per game frame like the eyes.
    bool synth = false, pace = true, valve_bounds = true, pair = false;
    // Pair mode: hold the left eye's present until mid-way through the compositor frame.
    bool pair_wait = true;
    // Pose submitted with the eye images: 0 none, 1 each eye's own, 2 newest of the two for both
    // (2: inline submission only).
    int submit_pose = 1;
    // Submit from a dedicated thread every compositor frame (a late game frame repeats the
    // previous image instead of reaching SteamVR late), waiting at most submit_budget_ms
    // for the newest image's copy to finish. 0 = submit inline from the game's present.
    // SteamVR on the Aero latches the frame ~3.2-4.5 ms after WaitGetPoses returns (measured
    // 2026-09-28): a Submit after that misses a whole compositor frame, which shows as a flash.
    bool async_submit = true; float submit_budget_ms = 2;
    // PostPresentHandoff after each asynchronous Submit. openvr.h: only needed when WaitGetPoses
    // can't follow the present, and here it follows at once (openvr #1401: the call itself blocked).
    bool handoff = false;
    // 1: a game frame that ends after a compositor tick releases X4 at once instead of waiting
    // for the next tick. 0 snaps every frame between 11.1 and 22.2 ms to 45 fps: with 10 ms CPU
    // load per frame (2026-10-01, OpenVR and OpenXR) 45 vs 67 fps, 0 missed compositor frames
    // either way, and 1 looked smoother in the headset.
    bool release_late = true;
    // Diagnostics: stall the game's render thread for hitch_ms once every hitch_every presents.
    float hitch_ms = 0; int hitch_every = 90;
    // Eye association by X4's per-frame render-data half instead of present counting.
    bool eye_from_half = false; int half_xor_render = 0, half_xor_present = 0;
    // Eye offset added when X4's camera reads the tracker position, not at the tracker read
    // (FreeTrack client, EyeOffsets); half_xor_use maps the frame half then to the eye.
    // half_xor_use 1: measured in the headset 2026-09-28 (0 swapped the eyes: near objects doubled).
    bool eye_at_use = true; int half_xor_use = 1;
    // The same on foot, measured 2026-09-28: the camera bridge reads the tracker right after a
    // present and that frame uses it (in the cockpit: right before a present, two frames later),
    // so the eye there is half ^ 0 and the reprojection pose is found with delay 1 (delay 2
    // submitted a pose two frames old: stutter when turning the head).
    bool walk_at_use = true; int half_xor_walk = 0, delay_walk = 1;
    float synth_rate = 0; std::array<float, 6> synth_base{}, synth_alt{};
    // Theater mode (flat game image on a virtual screen): 0 off, 1 while X4 shows a fullscreen
    // menu or sends no head poses, 2 always. Screen distance and width in metres.
    int theater = 1; float theater_distance = 2.f, theater_width = 2.2f;
    // 1: with theater=1 the external camera (F2/F3) stays in stereo instead of going to the screen.
    bool external_vr = false;
    // One head pose per eye pair, for SteamVR's streaming link (Steam Link, Steam Frame), which
    // reprojects both eyes with the left eye's pose (issue #4). X4 builds the right eye from the
    // left eye's head pose, and the submission thread pairs images with equal poses. The eye that
    // steps back is one frame older, and the head is sampled once per pair instead of per image.
    bool shared_pose = false;
    // Mouse cursor overlay (1 on, 0 off); over the stereo view it sits cursor_distance metres ahead.
    int cursor = 1; float cursor_distance = 5.f;
    // Turn compensation: eye images are rendered one game frame apart, so a mouse or body turn
    // between them splits the eyes. Submit each eye's pose rotated by the game camera's turn
    // since the newest image (SteamVR then aligns both). 0 off, 1 on foot, 2 always (in the
    // cockpit it misaligns the interior during ship turns instead).
    int turn_comp = 1; };
StereoSettings stereo_settings(); // cached; a background thread re-reads stereo.txt every 500 ms
// File I/O on that background thread, in call order: X4's threads and the submission thread
// never wait on the disk. append false replaces the file.
void write_file_later(std::filesystem::path path, std::string text, bool append);
// True once each time `name` appeared in %X4VR_CAPTURE_DIR% (deleted when seen; polled every
// 500 ms from the first call on).
bool take_request(const char* name);
// Pose source (may be called several times per game frame): eye the frame being
// simulated now will be presented to, and the head pose it was rendered with.
uint32_t render_eye();
uint64_t frame_tag();   // present count when the game samples its pose
// flat: the frame shows a fullscreen menu (or theater mode is forced); it goes to the virtual screen.
// walking: the player is on foot (turn compensation applies).
void record_render_pose(const Matrix& head, uint32_t eye, bool flat = false, bool walking = false);
// Recentred seated origin (position + yaw), published by the pose source; false until set.
void publish_view_origin(const Matrix& origin);
bool view_origin(Matrix& origin);
int frame_half(); // X4 per-frame double-buffer half (0/1), -1 if unavailable
// Layer: once per present. Returns the present number; pose lookup by number.
// Diagnostics: create trace.request to dump the last 16384 events to trace.txt
// ("kind value half time_us thread a b c"). Kinds: P present, L/R pose read for that eye,
// C main camera bound (a b c: camera world position).
void trace_event(char kind, uint64_t value, float a = 0, float b = 0, float c = 0);
uint64_t next_present();
// Eye, head pose, flat and walking flags of the frame being presented now (false: no pose known).
bool presented_frame(uint64_t present, uint32_t& eye, Matrix& head, bool& flat, bool& walking);
// Avoid recursively bootstrapping if runtime initialization itself uses Vulkan.
bool is_runtime_bootstrap_thread();
std::shared_ptr<RuntimeBootstrap> acquire_runtime_bootstrap();
}
