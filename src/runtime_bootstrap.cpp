#include <x4vr/code_scan.hpp>
#include <x4vr/runtime_bootstrap.hpp>
#include "openxr_runtime.hpp"
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <functional>
#include <iomanip>
#include <map>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

namespace x4vr {
namespace {
thread_local bool runtime_thread = false;
struct RuntimeCall {
    bool previous = runtime_thread;
    RuntimeCall() { runtime_thread = true; }
    ~RuntimeCall() { runtime_thread = previous; }
};
std::mutex bootstrap_mutex;
std::weak_ptr<RuntimeBootstrap> bootstrap;
}
bool is_runtime_bootstrap_thread() { return runtime_thread; }
RuntimeBootstrap::RuntimeBootstrap() {
    RuntimeCall call;
    char backend[16]{};
    if (GetEnvironmentVariableA("X4VR_RUNTIME", backend, sizeof(backend)) && !_stricmp(backend, "openxr")) {
        OutputDebugStringA("X4VR bootstrap: OpenXR backend\n");
        xr_ = std::make_unique<OpenXRRuntime>();
        return;
    }
    OutputDebugStringA("X4VR bootstrap: initialize begin\n");
    session_.initialize();
    OutputDebugStringA("X4VR bootstrap: initialize complete\n");
}
RuntimeBootstrap::~RuntimeBootstrap() {
    std::lock_guard lock(mutex_);
    RuntimeCall call;
    session_.adopt_bootstrap_thread();
    session_.shutdown();
}
std::vector<std::string> RuntimeBootstrap::instance_extensions() {
    if (xr_) { RuntimeCall call; return xr_->instance_extensions(); }
    std::lock_guard lock(mutex_);
    RuntimeCall call;
    session_.adopt_bootstrap_thread();
    OutputDebugStringA("X4VR bootstrap: instance extensions begin\n");
    auto result = session_.instance_extensions();
    OutputDebugStringA("X4VR bootstrap: instance extensions complete\n");
    return result;
}
std::vector<std::string> RuntimeBootstrap::device_extensions(VkPhysicalDevice_T* physical) {
    if (xr_) { RuntimeCall call; return xr_->device_extensions(); }
    std::lock_guard lock(mutex_);
    RuntimeCall call;
    session_.adopt_bootstrap_thread();
    OutputDebugStringA("X4VR bootstrap: device extensions begin\n");
    auto result = session_.device_extensions(physical);
    OutputDebugStringA("X4VR bootstrap: device extensions complete\n");
    return result;
}
VkPhysicalDevice_T* RuntimeBootstrap::output_device(VkInstance_T* instance) {
    if (xr_) { RuntimeCall call; return xr_->output_device(instance); }
    std::lock_guard lock(mutex_);
    RuntimeCall call;
    session_.adopt_bootstrap_thread();
    OutputDebugStringA("X4VR bootstrap: output GPU begin\n");
    auto result = session_.output_device(instance);
    OutputDebugStringA("X4VR bootstrap: output GPU complete\n");
    return result;
}
std::shared_ptr<RuntimeBootstrap> acquire_runtime_bootstrap() {
    std::lock_guard lock(bootstrap_mutex);
    auto result = bootstrap.lock();
    if (!result) { result = std::make_shared<RuntimeBootstrap>(); bootstrap = result; }
    // Pin for the process lifetime: X4 creates and destroys temporary Vulkan
    // instances at startup, and VR_Init/VR_Shutdown churn (vrclient DLL load/unload
    // plus its threads) raced a loader-lock deadlock with overlay hooks. Deliberately
    // leaked: no VR_Shutdown from static destruction; the OS reclaims at exit.
    static auto* pinned = new std::shared_ptr<RuntimeBootstrap>(result);
    (void)pinned;
    return result;
}
std::string RuntimeBootstrap::start_session(const XrVulkanContext& vulkan) {
    if (!xr_) return {};
    RuntimeCall call; // the runtime may create Vulkan objects of its own in here
    return xr_->start_session(vulkan);
}
void RuntimeBootstrap::end_session(VkDevice_T* device) { if (xr_) xr_->end_session(device); }
FrameStatus RuntimeBootstrap::sample_tracking(Matrix& tracking_from_head) {
    if (xr_) return xr_->predicted_tracking(tracking_from_head, 0);
    std::lock_guard lock(mutex_);
    RuntimeCall call;
    session_.adopt_bootstrap_thread();
    return session_.sample_tracking(tracking_from_head);
}
FrameStatus RuntimeBootstrap::predicted_tracking(Matrix& tracking_from_head, float seconds) {
    // Deliberately lock-free: the game thread must never wait behind the render
    // thread's WaitGetPoses. IVRSystem pose queries are safe from any thread.
    if (xr_) return xr_->predicted_tracking(tracking_from_head, seconds);
    tracking_from_head = {};
    vr::TrackedDevicePose_t head{};
    session_.system_->GetDeviceToAbsoluteTrackingPose(vr::TrackingUniverseSeated, seconds, &head, 1);
    const auto pose = from_pose(head.mDeviceToAbsoluteTracking);
    if (!head.bDeviceIsConnected || !head.bPoseIsValid || !is_rigid(pose)) return FrameStatus::tracking_unavailable;
    tracking_from_head = pose;
    return FrameStatus::ready;
}
RuntimeBootstrap::EyeSetup RuntimeBootstrap::eye_setup() {
    if (xr_) return xr_->eye_setup();
    std::lock_guard lock(mutex_);
    RuntimeCall call;
    session_.adopt_bootstrap_thread();
    EyeSetup setup{};
    for (int i = 0; i < 2; ++i) {
        const auto eye = static_cast<vr::EVREye>(i);
        setup.head_from_eye[i] = from_pose(session_.system_->GetEyeToHeadTransform(eye));
        auto& t = setup.tangents[i];
        session_.system_->GetProjectionRaw(eye, &t[0], &t[1], &t[2], &t[3]);
    }
    return setup;
}
// Frame protocol: WaitGetPoses -> Submit(both) -> PostPresentHandoff. Waiting first also
// paces the caller to the headset refresh.
std::string RuntimeBootstrap::submit_stereo(const std::array<vr::VRVulkanTextureData_t, 2>& images,
                                            const std::array<vr::VRTextureBounds_t, 2>& bounds,
                                            const std::array<Matrix, 2>& poses, bool with_pose) {
    auto error = wait_frame();
    return error.empty() ? submit_frame(images, bounds, poses, with_pose) : error;
}
std::string RuntimeBootstrap::wait_frame() {
    if (xr_) { RuntimeCall call; return xr_->wait_frame(); }
    std::lock_guard lock(mutex_);
    RuntimeCall call;
    session_.adopt_bootstrap_thread();
    if (session_.poll_quit()) return "OpenVR quit requested";
    std::array<vr::TrackedDevicePose_t, 1> tracked{};
    const auto error = session_.compositor_->WaitGetPoses(tracked.data(), 1, nullptr, 0);
    if (error != vr::VRCompositorError_None) return "WaitGetPoses failed: OpenVR compositor error " + std::to_string(error);
    return {};
}
std::string RuntimeBootstrap::submit_frame(const std::array<vr::VRVulkanTextureData_t, 2>& images,
                                           const std::array<vr::VRTextureBounds_t, 2>& bounds,
                                           const std::array<Matrix, 2>& poses, bool with_pose, bool handoff,
                                           SubmitMarks* marks) {
    if (xr_) {
        RuntimeCall call;
        auto error = xr_->submit_frame(images, bounds, poses, with_pose);
        if (marks) marks->fill(std::chrono::steady_clock::now());
        return error;
    }
    std::lock_guard lock(mutex_);
    RuntimeCall call;
    session_.adopt_bootstrap_thread();
    vr::EVRCompositorError error{};
    auto descriptors = images;
    for (int i = 0; i < 2; ++i) {
        vr::VRTextureWithPose_t texture{};
        texture.handle = &descriptors[i]; texture.eType = vr::TextureType_Vulkan; texture.eColorSpace = vr::ColorSpace_Auto;
        for (int r = 0; r < 3; ++r) for (int c = 0; c < 4; ++c) texture.mDeviceToAbsoluteTracking.m[r][c] = poses[i].m[r][c];
        error = session_.compositor_->Submit(static_cast<vr::EVREye>(i), &texture, &bounds[i],
                                             with_pose ? vr::Submit_TextureWithPose : vr::Submit_Default);
        if (error != vr::VRCompositorError_None) return "Submit failed: OpenVR compositor error " + std::to_string(error);
        if (marks) (*marks)[i] = std::chrono::steady_clock::now();
    }
    if (handoff) session_.compositor_->PostPresentHandoff();
    if (marks) (*marks)[2] = std::chrono::steady_clock::now();
    return {};
}
bool RuntimeBootstrap::frame_timing(vr::Compositor_FrameTiming& timing, uint32_t frames_ago) {
    if (xr_) return false;
    std::lock_guard lock(mutex_);
    RuntimeCall call;
    session_.adopt_bootstrap_thread();
    timing = {};
    timing.m_nSize = sizeof(timing);
    return session_.compositor_->GetFrameTiming(&timing, frames_ago);
}
std::string RuntimeBootstrap::show_theater(const vr::VRVulkanTextureData_t* image, const vr::VRTextureBounds_t& bounds,
                                           const Matrix& seated_from_screen, float width) {
    if (xr_) { RuntimeCall call; return xr_->show_theater(image, bounds, seated_from_screen, width); }
    std::lock_guard lock(mutex_);
    RuntimeCall call;
    session_.adopt_bootstrap_thread();
    auto* overlay = vr::VROverlay();
    if (!overlay) return "OpenVR overlay interface unavailable";
    if (theater_ == vr::k_ulOverlayHandleInvalid) {
        const auto error = overlay->CreateOverlay("x4vr.theater", "X4 VR theater", &theater_);
        if (error != vr::VROverlayError_None) { theater_ = vr::k_ulOverlayHandleInvalid; return "CreateOverlay failed: OpenVR overlay error " + std::to_string(error); }
    }
    vr::HmdMatrix34_t transform{};
    for (int r = 0; r < 3; ++r) for (int c = 0; c < 4; ++c) transform.m[r][c] = seated_from_screen.m[r][c];
    overlay->SetOverlayWidthInMeters(theater_, width);
    overlay->SetOverlayTransformAbsolute(theater_, vr::TrackingUniverseSeated, &transform);
    overlay->SetOverlayTextureBounds(theater_, &bounds);
    if (image) {
        auto data = *image;
        vr::Texture_t texture{&data, vr::TextureType_Vulkan, vr::ColorSpace_Auto};
        const auto error = overlay->SetOverlayTexture(theater_, &texture);
        if (error != vr::VROverlayError_None) return "SetOverlayTexture failed: OpenVR overlay error " + std::to_string(error);
    }
    overlay->ShowOverlay(theater_);
    return {};
}
std::string RuntimeBootstrap::show_cursor(const uint8_t* rgba, uint32_t width, uint32_t height, bool on_screen,
                                          const Matrix& placement, float width_m) {
    if (xr_) { RuntimeCall call; return xr_->show_cursor(rgba, width, height, on_screen, placement, width_m); }
    std::lock_guard lock(mutex_);
    auto* overlay = vr::VROverlay();
    if (!overlay) return "OpenVR overlay interface unavailable";
    if (rgba) {
        const auto key = std::hash<std::string_view>{}({reinterpret_cast<const char*>(rgba), size_t(width)*height*4})^(size_t(width) << 16 | height);
        const auto found = std::find_if(cursors_.begin(), cursors_.end(), [&](const auto& c) { return c.first == key; });
        auto handle = vr::k_ulOverlayHandleInvalid;
        std::string error;
        if (found != cursors_.end()) { handle = found->second; cursors_.erase(found); }
        else {
            // ponytail: 16 images; X4 uses a handful. Past that the least recently shown is uploaded again,
            // which spends another block: a texture overlay (SetOverlayTexture) if X4 ever cycles more shapes.
            if (cursors_.size() < 16) {
                const auto name = "x4vr.cursor." + std::to_string(cursors_.size());
                if (const auto created = overlay->CreateOverlay(name.c_str(), "X4 VR cursor", &handle); created != vr::VROverlayError_None)
                    return "CreateOverlay (cursor) failed: OpenVR overlay error " + std::to_string(created);
                overlay->SetOverlaySortOrder(handle, 1); // above the theater screen
            } else {
                handle = cursors_.front().second;
                cursors_.erase(cursors_.begin());
            }
            // Kept even if the upload fails, so a failing image isn't retried every frame.
            if (const auto uploaded = overlay->SetOverlayRaw(handle, const_cast<uint8_t*>(rgba), width, height, 4); uploaded != vr::VROverlayError_None)
                error = "SetOverlayRaw failed: OpenVR overlay error " + std::to_string(uploaded);
            static unsigned uploads{};
            OutputDebugStringA(("X4VR presenter: cursor image " + std::to_string(++uploads) + " uploaded\n").c_str());
        }
        cursors_.emplace_back(key, handle);
        if (cursor_ != handle && cursor_ != vr::k_ulOverlayHandleInvalid) overlay->HideOverlay(cursor_);
        cursor_ = handle;
        if (!error.empty()) return error;
    }
    if (cursor_ == vr::k_ulOverlayHandleInvalid) return "no cursor image yet";
    vr::HmdMatrix34_t transform{};
    for (int r = 0; r < 3; ++r) for (int c = 0; c < 4; ++c) transform.m[r][c] = placement.m[r][c];
    overlay->SetOverlayWidthInMeters(cursor_, width_m);
    if (on_screen) overlay->SetOverlayTransformAbsolute(cursor_, vr::TrackingUniverseSeated, &transform);
    else overlay->SetOverlayTransformTrackedDeviceRelative(cursor_, vr::k_unTrackedDeviceIndex_Hmd, &transform);
    overlay->ShowOverlay(cursor_);
    return {};
}
void RuntimeBootstrap::hide_cursor() {
    if (xr_) return xr_->hide_cursor();
    std::lock_guard lock(mutex_);
    if (auto* overlay = vr::VROverlay(); overlay && cursor_ != vr::k_ulOverlayHandleInvalid) overlay->HideOverlay(cursor_);
}
void RuntimeBootstrap::hide_theater() {
    if (xr_) return xr_->hide_theater();
    std::lock_guard lock(mutex_);
    if (auto* overlay = vr::VROverlay(); overlay && theater_ != vr::k_ulOverlayHandleInvalid) overlay->HideOverlay(theater_);
}
namespace {
std::atomic_uint64_t present_frames{};
struct TaggedPose { uint64_t tag = ~0ull; Matrix head; uint32_t eye = 0; bool flat = false, walking = false; };
std::mutex poses_mutex;
// In record order, several per tag: with the eye chosen at use, X4's camera thread can record the
// next frame's pose before the present that ends this tag (long frames), which one slot per tag lost.
std::array<TaggedPose, 32> poses;
uint64_t pose_count = 0;
Matrix published_origin;
bool origin_known = false;
std::mutex settings_mutex;
StereoSettings cached;
std::string capture_dir() {
    char root[1024]{};
    return GetEnvironmentVariableA("X4VR_CAPTURE_DIR", root, sizeof(root)) ? std::string(root) : std::string();
}
StereoSettings read_settings() {
    StereoSettings next;
    const auto root = capture_dir();
    if (root.empty()) return next;
    std::ifstream file(root+"/stereo.txt");
    for (std::string line; std::getline(file, line);) {
        const auto split = line.find('=');
        if (split == std::string::npos) continue;
        const auto key = line.substr(0, split);
        const auto value = std::atof(line.c_str()+split+1);
        if (key == "stereo") next.stereo = value != 0;
        else if (key == "delay") next.delay = static_cast<int>(value);
        else if (key == "ipd_scale") next.ipd_scale = static_cast<float>(value);
        else if (key == "yaw_gain") next.yaw_gain = static_cast<float>(value);
        else if (key == "pitch_gain") next.pitch_gain = static_cast<float>(value);
        else if (key == "roll_gain") next.roll_gain = static_cast<float>(value);
        else if (key == "recenter") next.recenter = static_cast<int>(value);
        else if (key == "pos_scale") next.pos_scale = static_cast<float>(value);
        else if (key == "predict") next.predict = static_cast<float>(value);
        else if (key == "game_tan_y") next.game_tan_y = static_cast<float>(value);
        else if (key == "synth") next.synth = value != 0;
        else if (key == "pace") next.pace = value != 0;
        else if (key == "pair") next.pair = value != 0;
        else if (key == "pair_wait") next.pair_wait = value != 0;
        else if (key == "submit_pose") next.submit_pose = static_cast<int>(value);
        else if (key == "async_submit") next.async_submit = value != 0;
        else if (key == "submit_budget_ms") next.submit_budget_ms = static_cast<float>(value);
        else if (key == "handoff") next.handoff = value != 0;
        else if (key == "release_late") next.release_late = value != 0;
        else if (key == "hitch_ms") next.hitch_ms = static_cast<float>(value);
        else if (key == "hitch_every") next.hitch_every = static_cast<int>(value);
        else if (key == "eye_from_half") next.eye_from_half = value != 0;
        else if (key == "theater") next.theater = static_cast<int>(value);
        else if (key == "theater_distance") next.theater_distance = static_cast<float>(value);
        else if (key == "theater_width") next.theater_width = static_cast<float>(value);
        else if (key == "external_vr") next.external_vr = value != 0;
        else if (key == "shared_pose") next.shared_pose = value != 0;
        else if (key == "cursor") next.cursor = static_cast<int>(value);
        else if (key == "cursor_distance") next.cursor_distance = static_cast<float>(value);
        else if (key == "turn_comp") next.turn_comp = static_cast<int>(value);
        else if (key == "half_xor_render") next.half_xor_render = static_cast<int>(value);
        else if (key == "half_xor_present") next.half_xor_present = static_cast<int>(value);
        else if (key == "eye_at_use") next.eye_at_use = value != 0;
        else if (key == "half_xor_use") next.half_xor_use = static_cast<int>(value);
        else if (key == "walk_at_use") next.walk_at_use = value != 0;
        else if (key == "half_xor_walk") next.half_xor_walk = static_cast<int>(value);
        else if (key == "delay_walk") next.delay_walk = static_cast<int>(value);
        else if (key == "valve_bounds") next.valve_bounds = value != 0;
        else if (key == "synth_rate") next.synth_rate = static_cast<float>(value);
        else if (key == "synth_base" || key == "synth_alt") {
            auto& target = key == "synth_base" ? next.synth_base : next.synth_alt;
            std::istringstream in(line.substr(split+1));
            for (auto& v : target) in >> v;
        }
    }
    return next;
}
struct Background {
    std::mutex mutex;
    std::condition_variable wake;
    std::deque<std::tuple<std::filesystem::path, std::string, bool>> writes; // path, text, append
    std::map<std::string, bool, std::less<>> requests; // polled file names -> seen, not yet taken
};
Background& background() { static auto* value = new Background; return *value; }
void background_loop() {
    auto& b = background();
    const auto root = capture_dir();
    auto poll = std::chrono::steady_clock::now();
    for (;;) try {
        decltype(b.writes) writes;
        {
            std::unique_lock lock(b.mutex);
            b.wake.wait_until(lock, poll, [&] { return !b.writes.empty(); });
            writes.swap(b.writes);
        }
        std::ofstream out;
        std::filesystem::path current;
        for (auto& [path, text, append] : writes) { // consecutive appends to one file share one open
            if (!out.is_open() || !append || path != current) {
                out.close();
                out.open(path, append ? std::ios::app : std::ios::trunc);
                current = path;
            }
            out << text;
        }
        out.close();
        if (std::chrono::steady_clock::now() < poll) continue;
        poll = std::chrono::steady_clock::now()+std::chrono::milliseconds(500);
        const auto next = read_settings();
        { std::lock_guard lock(settings_mutex); cached = next; }
        if (root.empty()) continue;
        std::vector<std::string> names;
        {
            std::lock_guard lock(b.mutex);
            for (const auto& [name, seen] : b.requests) if (!seen) names.push_back(name);
        }
        for (const auto& name : names) // a file still open by its writer isn't deleted: seen next time
            if (DeleteFileA((root+"/"+name).c_str())) { std::lock_guard lock(b.mutex); b.requests[name] = true; }
    } catch (...) {
        Sleep(100);
    }
}
void start_background() {
    static std::once_flag started;
    std::call_once(started, [] {
        { const auto first = read_settings(); std::lock_guard lock(settings_mutex); cached = first; }
        HMODULE self{}; // the thread runs this module's code until the process exits
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                           reinterpret_cast<LPCWSTR>(&background_loop), &self);
        std::thread(background_loop).detach();
    });
}
}
StereoSettings stereo_settings() {
    start_background();
    std::lock_guard lock(settings_mutex);
    return cached;
}
void write_file_later(std::filesystem::path path, std::string text, bool append) {
    start_background();
    auto& b = background();
    {
        std::lock_guard lock(b.mutex);
        b.writes.emplace_back(std::move(path), std::move(text), append);
    }
    b.wake.notify_one();
}
bool take_request(const char* name) {
    start_background();
    auto& b = background();
    std::lock_guard lock(b.mutex);
    const auto found = b.requests.find(std::string_view(name));
    if (found == b.requests.end()) { b.requests.emplace(name, false); return false; }
    return std::exchange(found->second, false);
}
namespace {
// X4 double-buffers per-frame render data in two halves selected by a global that flips once
// per frame (9.00: RVA 0x6b66280; the producer writes half^1, see code at 0x77a47f).
// The half follows a frame from pose sampling to presentation whatever the queue depth.
const volatile int32_t* frame_half_global() {
    static const volatile int32_t* global = []() -> const volatile int32_t* {
        const unsigned char* found = nullptr;
        for (const auto* reader : code::find_all(GetModuleHandleW(nullptr), code::parse(code::x4::frame_half))) {
            const auto target = code::rip_target(reader, 3, 7);
            if (found && target != found) return nullptr; // readers disagree: not the code we know
            found = target;
        }
        return reinterpret_cast<const volatile int32_t*>(found);
    }();
    return global;
}
}
int frame_half() { const auto global = frame_half_global(); return global ? (*global & 1) : -1; }
uint32_t render_eye() {
    const auto s = stereo_settings();
    const int half = s.eye_from_half ? frame_half() : -1;
    if (half >= 0) return static_cast<uint32_t>((half ^ s.half_xor_render) & 1);
    return static_cast<uint32_t>((present_frames.load()+s.delay) & 1);
}
void record_render_pose(const Matrix& head, uint32_t eye, bool flat, bool walking) {
    const auto tag = present_frames.load();
    std::lock_guard lock(poses_mutex);
    poses[pose_count++ % poses.size()] = {tag, head, eye, flat, walking}; // several per frame: the newest wins
}
void publish_view_origin(const Matrix& origin) {
    std::lock_guard lock(poses_mutex);
    published_origin = origin; origin_known = true;
}
bool view_origin(Matrix& origin) {
    std::lock_guard lock(poses_mutex);
    if (origin_known) origin = published_origin;
    return origin_known;
}
namespace {
struct TraceEntry { uint64_t tick; uint64_t value; int half; char kind; uint32_t thread; float a, b, c; };
std::array<TraceEntry, 16384> trace_ring{}; // camera binds add several events per frame
std::atomic_uint64_t trace_index{};
}
void trace_event(char kind, uint64_t value, float a, float b, float c) {
    LARGE_INTEGER now{}; QueryPerformanceCounter(&now);
    trace_ring[trace_index++ % trace_ring.size()] = {uint64_t(now.QuadPart), value, frame_half(), kind, GetCurrentThreadId(), a, b, c};
    if (kind != 'P' || !take_request("trace.request")) return;
    LARGE_INTEGER frequency{}; QueryPerformanceFrequency(&frequency);
    std::ofstream out(capture_dir()+"/trace.txt");
    const auto end = trace_index.load();
    for (auto i = end > trace_ring.size() ? end-trace_ring.size() : 0; i < end; ++i) {
        const auto& e = trace_ring[i % trace_ring.size()];
        out << e.kind << ' ' << e.value << ' ' << e.half << ' ' << std::fixed << std::setprecision(1)
            << double(e.tick)*1e6/double(frequency.QuadPart) << ' ' << e.thread << ' ' << std::setprecision(4)
            << e.a << ' ' << e.b << ' ' << e.c << '\n';
    }
}
uint64_t next_present() { const auto n = present_frames++; trace_event('P', n); return n; }
uint64_t frame_tag() { return present_frames.load(); }
bool presented_frame(uint64_t present, uint32_t& eye, Matrix& head, bool& flat, bool& walking) {
    const auto s = stereo_settings();
    const int half = s.eye_from_half ? frame_half() : -1;
    std::lock_guard lock(poses_mutex);
    if (half >= 0) {
        eye = static_cast<uint32_t>((half ^ s.half_xor_present) & 1);
        // Pose: the newest sample for this eye at least delay-1 frames old (the exact
        // frame distance can vary by one; the eye itself is exact). On foot: delay_walk.
        const uint64_t start = s.delay > 1 ? uint64_t(s.delay-1) : 0, walk_start = s.delay_walk > 1 ? uint64_t(s.delay_walk-1) : 0;
        const uint64_t oldest = present > 15 ? present-15 : 0;
        for (uint64_t i = pose_count; i-- > (pose_count > poses.size() ? pose_count-poses.size() : 0);) {
            const auto& entry = poses[i % poses.size()];
            if (entry.tag+(entry.walking ? walk_start : start) <= present && entry.tag >= oldest && entry.eye == eye) {
                head = entry.head; flat = entry.flat; walking = entry.walking; return true;
            }
        }
        return false;
    }
    const auto tag = present - static_cast<uint64_t>(s.delay);
    for (uint64_t i = pose_count; i-- > (pose_count > poses.size() ? pose_count-poses.size() : 0);) {
        const auto& entry = poses[i % poses.size()];
        if (entry.tag != tag) continue;
        eye = static_cast<uint32_t>(present & 1);
        head = entry.head; flat = entry.flat; walking = entry.walking;
        return true;
    }
    return false;
}
}
