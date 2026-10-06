// OpenTrack client: the Linux counterpart of src/freetrack_client.cpp at commit be68c82
// (docs/linux/ARCHITECTURE.md, "Head-tracking feed"). Windows answers X4's FTGetData calls; Linux X4 reads
// OpenTrack UDP packets on its own thread, so this sends one packet after every present: the frame
// X4 builds next uses it. The pose logic is the Windows one (theater decision, recentring,
// synthetic calibration poses, eye offsets, reprojection poses).
//
// Eye at use, as on Windows: packets carry the head centre, and a hook on VR::OpenTrack's position
// accessor adds the eye offset of the frame X4 builds when its camera reads the tracker. Without
// the hook (pattern not found, X4VR_EYE_AT_USE=0) the eye is chosen when the packet is sent.
//
// Signs and units, measured in X4 9.00: OpenTrack +yaw turns
// right, +pitch looks up, +roll tilts left, +x moves left, +y up, -z forward. Positions are sent
// in OpenTrack centimetres. Overrides without rebuilding: X4VR_OT_YAW, _PITCH, _ROLL, _X, _Y, _Z.
#include "linux_runtime.hpp"
#include "code_scan.hpp"
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
#include <vector>

namespace x4vr::linux_port {
namespace {
constexpr double degrees = 180/3.14159265358979323846;
std::mutex state_mutex;
GameState state;

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

// ---- Code patches: backward clamp and on foot ------------------------------------------------
// X4 9.00's per-frame camera input function (0xfeb...) reads the tracker's angles (slot 33) and position (slot 34, called from
// 0xfec248) and hands them to the camera controller (0x1933d00). The Windows mod's patches have
// counterparts there; each is one byte, so it is atomic while X4 runs:
// - Backward clamp: `if (tracker type != 7 && z > 0) z = 0` (0xfeb72b..0xfeb749) pinned leaning
//   back and the rear eye. Its `jbe` past the zeroing (0xfeb73f) becomes `jmp`.
// - On foot: with camera mode 0 and no ship (0xfec070, a call), X4 hands the camera a zero pose
//   unless the tracker is an eye tracker. The `je` to the normal path after the call (0xfec077)
//   becomes `jno`, always taken after `test`.
// - On foot, camera offset (Windows' second on-foot patch): Camera::GetOffset (0x1929f70, Windows
//   0x97a300) exits without a movement controller (camera +0x18, Windows +0x20; null on foot)
//   before composing the head offset (camera +0x5a0..+0x5df, Windows +0x590). Its `je` to the
//   exit (0x1929ffd -> 0x192a216) goes to the block that composes the offset without it
//   (0x192a284 -> 0x192a15e, Windows 0x97a5a8) instead: displacement 0x213 -> 0x281.
// Changes one byte at site+at from `from` to `to`; true once it is in place (patched now or
// earlier). The site comes from the X4 scan (0: its signature isn't in this X4). Windows:
// patch_code, which searches the signature itself.
bool patch_code(const char* what, uint64_t site, size_t at, unsigned char from, unsigned char to) {
    const auto address = uintptr_t(site)+at;
    if (!site || !in_executable(address, 1)) return false;
    auto* target = reinterpret_cast<unsigned char*>(address);
    if (*target == to) { log(std::string("X4VR patch: ")+what+" already patched"); return true; }
    if (*target != from) { log(std::string("X4VR patch: ")+what+": unexpected byte; left unchanged"); return false; }
    const long page = sysconf(_SC_PAGESIZE);
    auto* start = reinterpret_cast<void*>(address & ~uintptr_t(page-1));
    if (mprotect(start, size_t(page), PROT_READ | PROT_WRITE | PROT_EXEC) != 0) { log(std::string("X4VR patch: ")+what+": code not writable"); return false; }
    __atomic_store_n(target, to, __ATOMIC_SEQ_CST);
    mprotect(start, size_t(page), PROT_READ | PROT_EXEC);
    log(std::string("X4VR patch: ")+what+" patched");
    return true;
}
void log_mismatch(const char* what) {
    log(std::string("X4VR patch: ")+what+": signature mismatch; left unchanged");
}
// X4's camera input zeroes backward head position (z > 0), pinning leaning back and the rear eye
// when looking sideways: its `jbe` past the zeroing becomes `jmp`.
void unclamp_backward_position(const code::X4Sites& sites) {
    if (!patch_code("backward head-position clamp", sites.backward_clamp, code::x4::backward_clamp_at, 0x76, 0xeb))
        log_mismatch("backward head-position clamp");
}
// Both on-foot patches in place: walking counts as stereo (Windows: on_foot_tracking). The camera
// offsets camera_on_foot reads (+0x3e8, +0x880, +0x18) are pinned by the scan's signatures.
std::atomic<bool> on_foot_tracking{false};
// X4's player global (9.00: 0x3db6948), from the camera-offset site; 0 if not found.
std::atomic<uintptr_t> player_global{0};
// Head tracking on foot: both on-foot patches (the zeroing's `je` -> `jno`, GetOffset's exit
// displacement 0x213 -> 0x281), then walking detection through the player global.
bool enable_on_foot_tracking(const code::X4Sites& sites) {
    namespace x4 = code::x4;
    if (!sites.onfoot_zeroing || !sites.camera_offset || // both or neither
        !patch_code("on-foot head-pose zeroing", sites.onfoot_zeroing, x4::onfoot_zeroing_at, 0x84, 0x81) ||
        !patch_code("on-foot camera offset", sites.camera_offset, x4::camera_offset_at, 0x13, uint8_t(x4::camera_offset_jump)) ||
        !sites.player_global || !in_executable(uintptr_t(sites.player_global), 8)) {
        log_mismatch("on-foot head-pose zeroing or camera offset");
        return false;
    }
    player_global = uintptr_t(sites.player_global);
    return true;
}
// The rendered camera ([[player global]+0x3e8], the global both on-foot patch sites use) in mode 0
// (+0x880) without a movement controller (+0x18): the player walking. Windows: [[manager]+0x3d0],
// +0x868, +0x20 (camera_on_foot). X4's main thread only.
bool camera_on_foot() {
    const auto global = player_global.load();
    const auto player = global ? *reinterpret_cast<const uintptr_t*>(global) : 0;
    const auto camera = player ? *reinterpret_cast<const uintptr_t*>(player+0x3e8) : 0;
    return camera && !*reinterpret_cast<const uintptr_t*>(camera+0x18) && !*reinterpret_cast<const int32_t*>(camera+0x880);
}
// Windows does these on FTGetData's first call; here once the scan is done.
void apply_patches(const code::X4Sites& sites) {
    const char* off = std::getenv("X4VR_PATCHES");
    if (off && *off == '0') { log("X4VR patch: code patches off (X4VR_PATCHES=0)"); return; }
    unclamp_backward_position(sites);
    on_foot_tracking = enable_on_foot_tracking(sites);
}

// ---- Eye at use ---------------------------------------------------------------------------
// Linux X4 9.00 (non-PIE), VR::OpenTrack, from its disassembly:
// - slot 2 (0x1a1b720, the update), once per frame on the game thread: copies the newest packet's six doubles
//   to +0x78..+0xa0 and sets +0xa8 (none new: zeroes them, clears +0xa8), position = xyz * scale
//   (+0x114) smoothed into +0xd0 (strength 1: the newest), angles into +0x100.
// - slot 34 (0x1a0dda0), the position accessor: +0xd0..+0xd8 / 180, clamped to [-1, 1].
// Each packet carries a sequence number in the low mantissa bits of its roll (a 1e-14 relative
// change). The accessor hook (same frame, after the update) notes which packet X4 took, adds that
// packet's eye offset for the frame being built, calls the original and restores the centre, and
// records the packet's headset pose for the layer.
// The vtable, by RTTI, and its slot 34 code are found by the X4 scan (code_scan.hpp). The field
// offsets below are pinned there too: +0xd0 by the slot 34 signature, the others by slot 2's
// (x4::opentrack_update), so on an X4 build where they moved the hook isn't installed.
constexpr size_t position_slot = code::x4::opentrack_position_slot;
constexpr size_t field_position = 0xd0, field_scale = 0x114, field_roll = 0xa0, field_fresh = 0xa8;

struct SentPacket { x4vr::Matrix head; std::array<std::array<float, 3>, 2> delta{}; bool flat{}, walking{}, valid{}; };
std::mutex packets_mutex;
std::array<SentPacket, 256> packets; // by sequence number (1..255)
uint32_t taken_seq{}; // the packet X4's updates took last (game thread only)
std::atomic<bool> eye_hook{false};
using TrackerPosition = void (*)(void*, float*, float*, float*);
std::atomic<TrackerPosition> original_position{};

template<class T> T field(void* tracker, size_t offset) { T v; std::memcpy(&v, static_cast<char*>(tracker)+offset, sizeof v); return v; }
uint32_t packet_seq(double roll) { uint64_t bits; std::memcpy(&bits, &roll, 8); return uint32_t(bits & 0xff); }
double with_seq(double roll, uint32_t seq) {
    uint64_t bits; std::memcpy(&bits, &roll, 8);
    bits = (bits & ~uint64_t(0xff)) | seq;
    std::memcpy(&roll, &bits, 8);
    return roll;
}

// On foot X4 uses the pose one game frame sooner than in the cockpit, so the eye a frame half
// means flips (Windows: half_xor_use 1 in the cockpit, half_xor_walk 0 on foot). Linux keeps
// Windows' difference: on foot the eye is flipped by half_xor_use ^ half_xor_walk (1 by default;
// half_xor_walk=1 turns the flip off).
uint32_t walk_flip(bool walking) {
    if (!walking) return 0;
    const auto s = x4vr::stereo_settings();
    return uint32_t((s.half_xor_use ^ s.half_xor_walk) & 1);
}

void position_at_use(void* tracker, float* x, float* y, float* z) {
    if (field<uint8_t>(tracker, field_fresh)) taken_seq = packet_seq(field<double>(tracker, field_roll));
    SentPacket packet;
    if (taken_seq) { std::lock_guard lock(packets_mutex); packet = packets[taken_seq]; }
    const auto original = original_position.load();
    if (!packet.valid) return original(tracker, x, y, z);
    const auto eye = x4vr::render_eye()^walk_flip(packet.walking); // the frame X4 builds now
    auto* position = reinterpret_cast<float*>(static_cast<char*>(tracker)+field_position);
    const float scale = field<float>(tracker, field_scale);
    const std::array<float, 3> centre{position[0], position[1], position[2]};
    for (int i = 0; i < 3; ++i) position[i] += packet.delta[eye][i]*scale;
    original(tracker, x, y, z);
    for (int i = 0; i < 3; ++i) position[i] = centre[i];
    x4vr::record_render_pose(packet.head, eye, packet.flat, packet.walking);
    x4vr::trace_event('U', x4vr::frame_tag(), float(eye), float(taken_seq), float(field<uint8_t>(tracker, field_fresh))); // use
}

// `on_original` gets the slot's old value before the swap: X4 may call the slot at once.
bool swap_slot(void** slot, void* replacement, void (*on_original)(void*)) {
    const long page = sysconf(_SC_PAGESIZE);
    auto* start = reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(slot) & ~uintptr_t(page-1));
    if (mprotect(start, size_t(page), PROT_READ | PROT_WRITE) != 0) return false;
    on_original(*slot);
    __atomic_store_n(slot, replacement, __ATOMIC_SEQ_CST);
    mprotect(start, size_t(page), PROT_READ);
    return true;
}
// Windows: eye_hook_ready, checked on each FTGetData call; here installed once after the scan.
void install_eye_hook(const code::X4Sites& sites) {
    const char* off = std::getenv("X4VR_EYE_AT_USE");
    if (off && *off == '0') { log("X4VR pose: eye at use off (X4VR_EYE_AT_USE=0); eyes chosen when packets are sent"); return; }
    auto** vtable = reinterpret_cast<void**>(sites.opentrack_vtable);
    const bool ok = sites.opentrack_vtable && in_executable(sites.opentrack_vtable, 8*(position_slot+1)) &&
                    reinterpret_cast<uintptr_t>(vtable[position_slot]) == sites.opentrack_position;
    if (!ok) { log("X4VR pose: eye-at-use hook: VR::OpenTrack's position accessor not found in this X4; eyes chosen when packets are sent"); return; }
    if (!swap_slot(&vtable[position_slot], reinterpret_cast<void*>(&position_at_use),
                   [](void* original) { original_position.store(reinterpret_cast<TrackerPosition>(original)); })) {
        log("X4VR pose: eye-at-use hook not installed (vtable not writable)"); return;
    }
    eye_hook = true;
    log("X4VR pose: eye-at-use hook installed (VR::OpenTrack slot 34)");
}

float setting(const char* name, float fallback) {
    const char* text = std::getenv(name);
    return text && *text ? std::strtof(text, nullptr) : fallback;
}
// X4's exported UI queries (its Lua API); the executable exports them, so dlsym finds them.
template<class F> F game_export(const char* name) { return reinterpret_cast<F>(dlsym(RTLD_DEFAULT, name)); }
bool fullscreen_menu() { // X4 9.00: first argument true = any fullscreen menu, name ignored
    static const auto query = game_export<bool (*)(bool, const char*)>("IsFullscreenMenuDisplayed");
    return query && query(true, nullptr);
}
bool game_flag(const char* name) {
    const auto query = game_export<bool (*)()>(name);
    return query && query();
}
bool at_ship_controls() {
    static const auto query = game_export<bool (*)()>("IsPlayerControllingShip");
    return !query || query();
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
        // On foot X4 takes the head pose one game frame later than in the cockpit (Windows,
        // docs/STUTTER_RESEARCH.md "On foot"): predict one frame further.
        // ponytail: one frame = 1/90 s, as on Windows; read the headset refresh if other rates matter.
        const bool walking = game.walking && !settings.synth;
        const bool tracked = runtime->predicted_tracking(head, settings.predict+(walking ? 1.f/90 : 0.f)) == x4vr::FrameStatus::ready;
        if (!tracked && !settings.synth) continue;
        if (!tracked) head = x4vr::Matrix::identity();
        const auto eye = x4vr::render_eye()^walk_flip(walking); // one game frame renders one eye
        // A fullscreen menu goes to the theater screen, and so does any view without ship controls.
        const bool flat = settings.theater == 2 ||
                          (settings.theater == 1 && game.sampled && (game.fullscreen_menu || !(walking || game.controlling_ship)));
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
        // As on Windows, stereo.txt's eye_at_use=0 (and walk_at_use=0 on foot) turn it off.
        const bool at_use = eye_hook && !settings.synth && settings.eye_at_use && (!walking || settings.walk_at_use);
        // Shared pose: no new packet before a right-eye frame, so X4 builds it from the left eye's packet.
        if (at_use && settings.stereo && !flat && shared_pose() && eye == 1) continue;
        seq = seq % 255+1;
        if (at_use) {
            SentPacket packet{tracking_head, {}, flat, walking, true};
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
            x4vr::record_render_pose(settings.synth && !flat ? recorded[eye] : tracking_head, eye, flat, walking);
            recorded_any = true;
        }
        const auto& m = head.m; // row-major, OpenVR seated: +X right, +Y up, -Z forward, metres
        // Y(yaw)-X(pitch)-Z(roll) intrinsic decomposition of R = Ry*Rx*Rz.
        const double yaw = std::atan2(m[0][2], m[2][2])*degrees;
        const double pitch = std::asin(std::fmax(-1.f, std::fmin(1.f, -m[1][2])))*degrees;
        const double roll = std::atan2(m[1][0], m[1][1])*degrees;
        // As on Windows, X4 scales angles down (85 of 180 degrees); the gains undo it. In degrees
        // here, so each gain is a plain factor (Windows' roll_gain pi is for FreeTrack's radians).
        // pos_scale 3.6 as on Windows: leaning, the cockpit stays in place.
        const opentrack::Pose pose{settings.pos_scale*sx*m[0][3], settings.pos_scale*sh*m[1][3], settings.pos_scale*sz*m[2][3],
                                   settings.yaw_gain*sy*yaw, settings.pitch_gain*sp*pitch, settings.roll_gain*sr*roll};
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
    GameState next;
    next.sampled = true;
    next.head_tracking = game_flag("IsHeadTrackingActive");
    if (next.head_tracking) {
        next.fullscreen_menu = fullscreen_menu();
        next.controlling_ship = at_ship_controls();
        next.walking = on_foot_tracking && !next.controlling_ship && camera_on_foot();
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
    if (next.head_tracking && (next.fullscreen_menu != state.fullscreen_menu || next.controlling_ship != state.controlling_ship ||
                               next.walking != state.walking)) {
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
        log("X4VR game: " + std::to_string(ms) + " ms: fullscreen_menu=" + std::to_string(next.fullscreen_menu) +
            " controlling_ship=" + std::to_string(next.controlling_ship) + " walking=" + std::to_string(next.walking));
    }
    state = next;
}
GameState game_state() {
    std::lock_guard lock(state_mutex);
    return state;
}
void start_opentrack_client() {
    static std::once_flag once;
    std::call_once(once, [] {
        std::thread([] {
            scan_x4(); // the X4 code the hook and patches use, found by pattern (a fraction of a second)
            const auto* sites = x4_sites();
            apply_patches(*sites);
            install_eye_hook(*sites);
            sender_loop();
        }).detach();
    });
}
}
