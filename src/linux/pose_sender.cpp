// OpenTrack pose sender: the Linux counterpart of src/freetrack_client.cpp at commit be68c82
// (docs/LINUX_PORT_PLAN.md, sections 4 and 6). Windows answers X4's FTGetData calls; Linux X4 reads
// OpenTrack UDP packets on its own thread (docs/LINUX_FINDINGS.md, 0.6), so this sends one packet
// after every present: the frame X4 builds next uses it. The pose logic is the Windows one (theater
// decision, recentring, synthetic calibration poses, eye offsets, reprojection poses).
//
// Stage B of plan section 7 (eye at use), as on Windows: packets carry the head centre, and a hook on
// VR::OpenTrack's position accessor adds the eye offset of the frame X4 builds when its camera
// reads the tracker (docs/LINUX_FINDINGS.md, stage B). Without the hook (other X4 version,
// X4VR_EYE_AT_USE=0) it falls back to stage A: the eye is chosen when the packet is sent.
// The frame half (C) and the backward clamp (D) need further X4 code patches, later.
//
// Signs and units measured with x4vr udp-send (docs/LINUX_FINDINGS.md, 0.5): OpenTrack +yaw turns
// right, +pitch looks up, +roll tilts left, +x moves left, +y up, -z forward. Positions are sent
// in OpenTrack centimetres. Overrides without rebuilding: X4VR_OT_YAW, _PITCH, _ROLL, _X, _Y, _Z.
#include "linux_runtime.hpp"
#include "opentrack.hpp"
#include <arpa/inet.h>
#include <dlfcn.h>
#include <sys/mman.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <array>
#include <atomic>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>

namespace x4vr::linux_port {
namespace {
constexpr double degrees = 180/3.14159265358979323846;
std::mutex state_mutex;
GameState state;

// X4's exported UI queries (its Lua API); the executable exports them, so dlsym finds them.
template<class F> F game_export(const char* name) { return reinterpret_cast<F>(dlsym(RTLD_DEFAULT, name)); }
float setting(const char* name, float fallback) {
    const char* text = std::getenv(name);
    return text && *text ? std::strtof(text, nullptr) : fallback;
}

// OpenVR convention: +X right, +Y up, -Z forward; yaw about +Y (positive = left),
// pitch about +X (positive = up), roll about -Z... composed as T * Ry * Rx * Rz.
x4vr::Matrix synthetic_pose(const float v[6]) {
    const double d = 1/degrees, y = v[3]*d, p = v[4]*d, r = v[5]*d;
    auto ry = x4vr::Matrix::identity(), rx = ry, rz = ry;
    ry.m[0][0] = float(std::cos(y)); ry.m[0][2] = float(std::sin(y)); ry.m[2][0] = float(-std::sin(y)); ry.m[2][2] = float(std::cos(y));
    rx.m[1][1] = float(std::cos(p)); rx.m[1][2] = float(-std::sin(p)); rx.m[2][1] = float(std::sin(p)); rx.m[2][2] = float(std::cos(p));
    rz.m[0][0] = float(std::cos(r)); rz.m[0][1] = float(-std::sin(r)); rz.m[1][0] = float(std::sin(r)); rz.m[1][1] = float(std::cos(r));
    auto pose = x4vr::multiply(x4vr::multiply(ry, rx), rz);
    pose.m[0][3] = v[0]; pose.m[1][3] = v[1]; pose.m[2][3] = v[2];
    return pose;
}

// ---- Stage B: eye at use ------------------------------------------------------------------
// Linux X4 9.00 (non-PIE), VR::OpenTrack, from its disassembly (docs/LINUX_FINDINGS.md, stage B):
// - slot 2 (0x1a1b720, the update), once per frame on the game thread: copies the newest packet's six doubles
//   to +0x78..+0xa0 and sets +0xa8 (none new: zeroes them, clears +0xa8), position = xyz * scale
//   (+0x114) smoothed into +0xd0 (strength 1: the newest), angles into +0x100.
// - slot 34 (0x1a0dda0), the position accessor: +0xd0..+0xd8 / 180, clamped to [-1, 1].
// Each packet carries a sequence number in the low mantissa bits of its roll (a 1e-14 relative
// change). The accessor hook (same frame, after the update) notes which packet X4 took, adds that
// packet's eye offset for the frame being built, calls the original and restores the centre, and
// records the packet's headset pose for the layer.
constexpr uintptr_t opentrack_vtable = 0x3c62520, opentrack_type_info = 0x3c61bb0;
constexpr size_t position_slot = 34;
constexpr uintptr_t position_code_address = 0x1a0dda0;
constexpr unsigned char position_code[] = {0xf3, 0x0f, 0x10, 0x87, 0xd0, 0x00, 0x00, 0x00}; // movss 0xd0(%rdi),%xmm0
constexpr size_t field_position = 0xd0, field_scale = 0x114, field_roll = 0xa0, field_fresh = 0xa8;

struct SentPacket { x4vr::Matrix head; std::array<std::array<float, 3>, 2> delta{}; bool flat{}, valid{}; };
std::mutex packets_mutex;
std::array<SentPacket, 256> packets; // by sequence number (1..255)
uint32_t taken_seq{}; // the packet X4's updates took last (game thread only)
std::atomic<bool> eye_hook{false};
using TrackerPosition = void (*)(void*, float*, float*, float*);
TrackerPosition original_position{};

template<class T> T field(void* tracker, size_t offset) { T v; std::memcpy(&v, static_cast<char*>(tracker)+offset, sizeof v); return v; }
uint32_t packet_seq(double roll) { uint64_t bits; std::memcpy(&bits, &roll, 8); return uint32_t(bits & 0xff); }
double with_seq(double roll, uint32_t seq) {
    uint64_t bits; std::memcpy(&bits, &roll, 8);
    bits = (bits & ~uint64_t(0xff)) | seq;
    std::memcpy(&roll, &bits, 8);
    return roll;
}

void position_at_use(void* tracker, float* x, float* y, float* z) {
    if (field<uint8_t>(tracker, field_fresh)) taken_seq = packet_seq(field<double>(tracker, field_roll));
    SentPacket packet;
    if (taken_seq) { std::lock_guard lock(packets_mutex); packet = packets[taken_seq]; }
    if (!packet.valid) return original_position(tracker, x, y, z);
    const auto eye = x4vr::render_eye(); // the frame X4 builds now
    auto* position = reinterpret_cast<float*>(static_cast<char*>(tracker)+field_position);
    const float scale = field<float>(tracker, field_scale);
    const std::array<float, 3> centre{position[0], position[1], position[2]};
    for (int i = 0; i < 3; ++i) position[i] += packet.delta[eye][i]*scale;
    original_position(tracker, x, y, z);
    for (int i = 0; i < 3; ++i) position[i] = centre[i];
    x4vr::record_render_pose(packet.head, eye, packet.flat, false);
    x4vr::trace_event('U', x4vr::frame_tag(), float(eye), float(taken_seq), float(field<uint8_t>(tracker, field_fresh))); // use
}

bool swap_slot(void** slot, void* replacement, void** original) {
    const long page = sysconf(_SC_PAGESIZE);
    auto* start = reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(slot) & ~uintptr_t(page-1));
    if (mprotect(start, size_t(page), PROT_READ | PROT_WRITE) != 0) return false;
    *original = *slot;
    __atomic_store_n(slot, replacement, __ATOMIC_SEQ_CST);
    mprotect(start, size_t(page), PROT_READ);
    return true;
}
void install_eye_hook() {
    const char* off = std::getenv("X4VR_EYE_AT_USE");
    if (off && *off == '0') { log("X4VR pose: eye at use off (X4VR_EYE_AT_USE=0); eyes chosen when packets are sent"); return; }
    auto** vtable = reinterpret_cast<void**>(opentrack_vtable);
    const char expected_name[] = "N2VR9OpenTrackE";
    bool ok = in_executable(opentrack_vtable-8, 8*(position_slot+2)) && in_executable(opentrack_type_info, 16) &&
              reinterpret_cast<uintptr_t>(vtable[-1]) == opentrack_type_info;
    if (ok) {
        const auto name = *reinterpret_cast<const char* const*>(opentrack_type_info+8);
        ok = in_executable(reinterpret_cast<uintptr_t>(name), sizeof expected_name) && !std::memcmp(name, expected_name, sizeof expected_name);
    }
    ok = ok && reinterpret_cast<uintptr_t>(vtable[position_slot]) == position_code_address &&
         in_executable(position_code_address, sizeof position_code) &&
         !std::memcmp(reinterpret_cast<const void*>(position_code_address), position_code, sizeof position_code);
    if (!ok) { log("X4VR pose: eye-at-use hook doesn't match this X4 (only Linux 9.00 is known); eyes chosen when packets are sent"); return; }
    void* original{};
    if (!swap_slot(&vtable[position_slot], reinterpret_cast<void*>(&position_at_use), &original)) {
        log("X4VR pose: eye-at-use hook not installed (vtable not writable)"); return;
    }
    original_position = reinterpret_cast<TrackerPosition>(original);
    eye_hook = true;
    log("X4VR pose: eye-at-use hook installed (X4 9.00 VR::OpenTrack slot 34)");
}

void sender_loop() {
    const int port = int(setting("X4VR_OPENTRACK_PORT", opentrack::default_port));
    const int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) { log("X4VR pose: no UDP socket; head tracking disabled"); return; }
    sockaddr_in to{};
    to.sin_family = AF_INET;
    to.sin_port = htons(uint16_t(port));
    to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    // OpenVR seated pose -> OpenTrack packet: sign per axis, metres to centimetres for positions.
    const float sy = setting("X4VR_OT_YAW", -1), sp = setting("X4VR_OT_PITCH", 1), sr = setting("X4VR_OT_ROLL", 1);
    const float sx = setting("X4VR_OT_X", -100), sh = setting("X4VR_OT_Y", 100), sz = setting("X4VR_OT_Z", 100);
    // Recentre: X4 clamps head position (on Windows to 0.25 m), so the seated origin must sit at the
    // user's actual head, with yaw facing the cockpit forward. The first tracked pose recentres.
    auto origin_inverse = x4vr::Matrix::identity();
    int recentered = INT_MIN;
    uint64_t last_tag = ~0ull;
    bool logged = false, recorded_any = false;
    uint32_t seq = 0;
    float ramp_rate = 0; uint64_t ramp_start = 0;
    log("X4VR pose: OpenTrack sender started (UDP 127.0.0.1:"+std::to_string(port)+"); turn on OpenTrack Support in X4's controls");
    for (;;) try {
        // Once per present (the next frame X4 builds reads this packet), else every 12 ms.
        const auto until = std::chrono::steady_clock::now()+std::chrono::milliseconds(12);
        while (x4vr::frame_tag() == last_tag && std::chrono::steady_clock::now() < until)
            std::this_thread::sleep_for(std::chrono::microseconds(250));
        last_tag = x4vr::frame_tag();
        const auto runtime = existing_runtime();
        if (!runtime) continue;
        const auto settings = x4vr::stereo_settings();
        const auto game = game_state();
        auto head = x4vr::Matrix::identity();
        const bool tracked = runtime->predicted_tracking(head, settings.predict) == x4vr::FrameStatus::ready;
        if (!tracked && !settings.synth) continue;
        if (!tracked) head = x4vr::Matrix::identity();
        const auto eye = x4vr::render_eye(); // one game frame renders one eye
        // A fullscreen menu goes to the theater screen, and so does any view without ship controls.
        const bool flat = settings.theater == 2 || (settings.theater == 1 && game.sampled && (game.fullscreen_menu || !game.controlling_ship));
        const auto tracking_head = head; // reprojection pose (tracking space)
        if (recentered != settings.recenter) {
            recentered = settings.recenter;
            const auto origin = x4vr::seated_origin(head);
            origin_inverse = x4vr::inverse_rigid(origin);
            x4vr::publish_view_origin(origin); // the theater screen is placed in front of it
            log("X4VR pose: head position/yaw recentred");
        }
        head = x4vr::multiply(origin_inverse, head);
        std::array<x4vr::Matrix, 2> eye_head{head, head}, recorded{tracking_head, tracking_head};
        if (flat) head = eye_head[0] = eye_head[1] = x4vr::Matrix::identity(); // steady, centred view for the virtual screen
        else if (settings.synth) { // calibration: dumps carry the synthetic pose
            if (settings.synth_rate != ramp_rate) { ramp_rate = settings.synth_rate; ramp_start = x4vr::frame_tag(); }
            const auto ramp = static_cast<float>(ramp_rate*double(x4vr::frame_tag()-ramp_start));
            float v[6];
            for (int side = -1; side <= 1; ++side) { // -1 left, 0 centre, 1 right
                for (int i = 0; i < 6; ++i) v[i] = settings.synth_base[i] + float(side)*settings.synth_alt[i];
                v[3] += ramp;
                (side ? eye_head[side > 0] : head) = synthetic_pose(v);
            }
            recorded = eye_head;
        } else if (settings.stereo) {
            // Alternate-eye rendering: this game frame renders one eye; the layer submits the
            // presented image to the same eye (shared counter). The eye positions are SteamVR's
            // (its IPD), re-read about once a second so an IPD change reaches X4 while playing.
            static auto eyes = runtime->eye_setup();
            static auto eyes_read = std::chrono::steady_clock::now();
            if (const auto now = std::chrono::steady_clock::now(); now-eyes_read > std::chrono::seconds(1)) {
                eyes_read = now;
                const auto fresh = runtime->eye_setup();
                const float before = eyes.head_from_eye[1].m[0][3]-eyes.head_from_eye[0].m[0][3];
                const float after = fresh.head_from_eye[1].m[0][3]-fresh.head_from_eye[0].m[0][3];
                if (std::fabs(after-before) > 0.0005f)
                    log("X4VR pose: SteamVR IPD changed from "+std::to_string(int(std::lround(before*1000)))+" to "+
                        std::to_string(int(std::lround(after*1000)))+" mm");
                eyes = fresh;
            }
            for (uint32_t e = 0; e < 2; ++e) {
                auto head_from_eye = eyes.head_from_eye[e];
                for (int r = 0; r < 3; ++r) head_from_eye.m[r][3] *= settings.ipd_scale;
                eye_head[e] = x4vr::multiply(head, head_from_eye);
            }
        }
        // Eye at use: the packet carries the centre; the accessor hook adds the eye's offset (in
        // packet units here, times X4's position scale there) and records the pose.
        const bool at_use = eye_hook && !settings.synth;
        // Shared pose: no new packet before a right-eye frame, so X4 builds it from the left eye's packet.
        if (at_use && settings.stereo && !flat && shared_pose() && eye == 1) continue;
        seq = seq % 255+1;
        if (at_use) {
            SentPacket packet{tracking_head, {}, flat, true};
            const float sign[3] = {sx, sh, sz};
            for (uint32_t e = 0; e < 2; ++e)
                for (int i = 0; i < 3; ++i) packet.delta[e][i] = settings.pos_scale*sign[i]*(eye_head[e].m[i][3]-head.m[i][3]);
            std::lock_guard lock(packets_mutex);
            packets[seq] = packet;
        } else {
            head = eye_head[eye];
            std::lock_guard lock(packets_mutex);
            packets[seq].valid = false;
        }
        // Only frames X4 builds with tracker input carry a pose; the rest (main menu, loading) go to
        // the theater screen, as on Windows where X4 then doesn't call FTGetData.
        if (at_use) recorded_any = true;
        else if (game.head_tracking) {
            x4vr::record_render_pose(settings.synth && !flat ? recorded[eye] : tracking_head, eye, flat, false);
            recorded_any = true;
        }
        const auto& m = head.m; // row-major, OpenVR seated: +X right, +Y up, -Z forward, metres
        // Y(yaw)-X(pitch)-Z(roll) intrinsic decomposition of R = Ry*Rx*Rz.
        const double yaw = std::atan2(m[0][2], m[2][2])*degrees;
        const double pitch = std::asin(std::fmax(-1.f, std::fmin(1.f, -m[1][2])))*degrees;
        const double roll = std::atan2(m[1][0], m[1][1])*degrees;
        // As on Windows, X4 scales angles down (85 of 180 degrees); the gains undo it.
        // ponytail: position scale reuses the Windows pos_scale; calibrate on Linux (docs/LINUX_FINDINGS.md, 0.5).
        const opentrack::Pose pose{settings.pos_scale*sx*m[0][3], settings.pos_scale*sh*m[1][3], settings.pos_scale*sz*m[2][3],
                                   settings.yaw_gain*sy*yaw, settings.pitch_gain*sp*pitch, settings.roll_gain/3.14159265f*sr*roll};
        auto sent = pose;
        sent.roll = with_seq(sent.roll, seq);
        const auto packet = opentrack::encode(sent);
        sendto(fd, packet.data(), packet.size(), 0, reinterpret_cast<const sockaddr*>(&to), sizeof(to));
        if (!logged && recorded_any) { logged = true; log("X4VR pose: first headset pose delivered to X4"); }
    } catch (const std::exception& error) {
        log(std::string("X4VR pose: ")+error.what());
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}
}

void sample_game_state() {
    // Only exports that null-check their game objects outside a game (Windows notes: IsHUDActive
    // crashes at the main menu); the menu and ship queries only once X4 applies head tracking.
    static const auto tracking = game_export<bool (*)()>("IsHeadTrackingActive");
    static const auto menu = game_export<bool (*)(bool, const char*)>("IsFullscreenMenuDisplayed");
    static const auto controlling = game_export<bool (*)()>("IsPlayerControllingShip");
    GameState next;
    next.sampled = true;
    next.head_tracking = tracking && tracking();
    if (next.head_tracking) {
        next.fullscreen_menu = menu && menu(true, nullptr); // X4 9.00: first argument true = any fullscreen menu
        next.controlling_ship = !controlling || controlling();
        // X4 smooths tracker input with alpha = 1/strength; its menu minimum is 5, which lags
        // rotation and averages alternating eye offsets away. The exported setter accepts 1.
        static bool smoothing = false;
        if (!smoothing) {
            smoothing = true;
            const auto set = game_export<void (*)(int64_t)>("SetActiveHeadTrackerHeadFilterStrength");
            const auto get = game_export<int64_t (*)()>("GetActiveHeadTrackerHeadFilterStrength");
            if (set && get) {
                set(1);
                log(get() == 1 ? "X4VR pose: head smoothing disabled (strength 1)" : "X4VR pose: head smoothing change rejected");
            }
        }
    }
    std::lock_guard lock(state_mutex);
    if (next.head_tracking != state.head_tracking)
        log(next.head_tracking ? "X4VR pose: X4 applies head tracking (cockpit)" : "X4VR pose: X4 doesn't apply head tracking (menu, loading or on foot)");
    // What sends the cockpit to the theater screen (fullscreen_menu, !controlling_ship), logged
    // on each change with a millisecond clock: short pop-ups (hints) showed up as either.
    if (next.head_tracking && (next.fullscreen_menu != state.fullscreen_menu || next.controlling_ship != state.controlling_ship)) {
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
        log("X4VR game: " + std::to_string(ms) + " ms: fullscreen_menu=" + std::to_string(next.fullscreen_menu) +
            " controlling_ship=" + std::to_string(next.controlling_ship));
    }
    state = next;
}
GameState game_state() {
    std::lock_guard lock(state_mutex);
    return state;
}
void start_pose_sender() {
    static std::once_flag once;
    std::call_once(once, [] { install_eye_hook(); std::thread(sender_loop).detach(); });
}
}
