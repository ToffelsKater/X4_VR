// Diagnostic (X4VR_WATCH_HEAD=1; =2 for the camera input's branches, see path_sites; =3 for the
// readers of the camera controller's head offset, see camera_controller; =4 for their callers): which X4 code uses the head-tracker input, for stage D
// (backward clamp) and on foot (docs/LINUX_FINDINGS.md). X4 9.00's VR::OpenTrack object keeps the
// head position at +0xd0 (3 floats, written by its update, slot 2) and the angles at +0x100; the
// eye-at-use hook (pose_sender.cpp) hands this file the object and its accessor's callers.
// Hardware watchpoints (perf_event_open, own process, no root) on those fields record each
// instruction that touches them, on every thread, in 20 s windows (10, labelled with the game
// state) once the object is known; the log lists the code addresses by count (an address is the
// instruction after the access). The bridge (U::HeadTrackerCameraBridge, static at 0x3e60820)
// turned out unused: its position stayed 0 in the cockpit.
#include "linux_runtime.hpp"
#include <linux/hw_breakpoint.h>
#include <linux/perf_event.h>
#include <asm/perf_regs.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace x4vr::linux_port {
namespace {
constexpr size_t field_position = 0xd0, field_angles = 0x100;

std::atomic<bool> watching{false};
std::atomic<void*> tracker{};
std::mutex callers_mutex;
std::map<uint64_t, uint64_t> callers; // accessor return addresses, this window
constexpr size_t ring_pages = 8; // data pages per event, plus one header page

struct Watch { int fd{-1}; void* ring{}; size_t size{}; };

// `calls`: an execution breakpoint at a function's first instruction that also samples the
// stack pointer and the 8 bytes there, the return address.
int open_watch(pid_t tid, uintptr_t address, std::string& error, bool execute = false, bool calls = false) {
    perf_event_attr attr{};
    attr.type = PERF_TYPE_BREAKPOINT;
    attr.size = sizeof(attr);
    attr.bp_type = execute ? HW_BREAKPOINT_X : HW_BREAKPOINT_RW; // x86 has no read-only data breakpoints
    attr.bp_addr = address;
    attr.bp_len = execute ? sizeof(long) : uint64_t(HW_BREAKPOINT_LEN_8);
    attr.sample_period = 1;
    attr.sample_type = PERF_SAMPLE_IP;
    if (calls) {
        attr.sample_type |= PERF_SAMPLE_REGS_USER | PERF_SAMPLE_STACK_USER;
        attr.sample_regs_user = uint64_t(1) << PERF_REG_X86_SP;
        attr.sample_stack_user = 8;
    }
    attr.exclude_kernel = 1;
    attr.exclude_hv = 1;
    const int fd = int(syscall(SYS_perf_event_open, &attr, tid, -1, -1, PERF_FLAG_FD_CLOEXEC));
    if (fd < 0) error = std::strerror(errno);
    return fd;
}

// Reads the samples a ring holds (PERF_RECORD_SAMPLE: header, then the IP) into `counts`.
// With `calls` (samples from open_watch(..., calls)), also counts (function, return address).
void drain(Watch& w, std::map<uint64_t, uint64_t>& counts, std::map<std::pair<uint64_t, uint64_t>, uint64_t>* calls = nullptr) {
    auto* meta = static_cast<perf_event_mmap_page*>(w.ring);
    const uint64_t head = __atomic_load_n(&meta->data_head, __ATOMIC_ACQUIRE);
    uint64_t tail = meta->data_tail;
    const auto* data = static_cast<const unsigned char*>(w.ring)+meta->data_offset;
    const uint64_t mask = meta->data_size-1;
    while (tail < head) {
        perf_event_header header;
        for (size_t i = 0; i < sizeof header; ++i) reinterpret_cast<unsigned char*>(&header)[i] = data[(tail+i) & mask];
        if (header.type == PERF_RECORD_SAMPLE && header.size >= sizeof header+8) {
            uint64_t ip = 0;
            for (size_t i = 0; i < 8; ++i) reinterpret_cast<unsigned char*>(&ip)[i] = data[(tail+sizeof header+i) & mask];
            ++counts[ip];
            // IP | REGS_USER (abi, sp) | STACK_USER (size, 8 bytes, dyn_size)
            if (calls && header.size >= sizeof header+8*6) {
                uint64_t v[5];
                for (size_t k = 0; k < 5; ++k)
                    for (size_t i = 0; i < 8; ++i) reinterpret_cast<unsigned char*>(&v[k])[i] = data[(tail+sizeof header+8*(k+1)+i) & mask];
                // v: abi, sp, stack size, return address, dyn_size
                if (v[0] && v[2] == 8 && v[4] == 8) ++(*calls)[{ip, v[3]}];
            }
        }
        if (!header.size) break;
        tail += header.size;
    }
    __atomic_store_n(&meta->data_tail, tail, __ATOMIC_RELEASE);
}

// Checks that watchpoints report anything here: one on a variable this thread reads 10 times.
std::string self_test() {
    static volatile uint64_t probe;
    std::string error;
    Watch w;
    w.fd = open_watch(pid_t(syscall(SYS_gettid)), uintptr_t(&probe), error);
    if (w.fd < 0) return "can't open ("+error+")";
    w.size = size_t(sysconf(_SC_PAGESIZE))*(ring_pages+1);
    w.ring = mmap(nullptr, w.size, PROT_READ | PROT_WRITE, MAP_SHARED, w.fd, 0);
    if (w.ring == MAP_FAILED) { close(w.fd); return "can't map"; }
    for (int i = 0; i < 10; ++i) (void)probe;
    std::map<uint64_t, uint64_t> counts;
    drain(w, counts);
    munmap(w.ring, w.size);
    close(w.fd);
    uint64_t hits = 0;
    for (const auto& [ip, n] : counts) hits += n;
    return std::to_string(hits)+" of 10 reads seen";
}

// X4VR_WATCH_HEAD=2: execution breakpoints (4 per thread) on the branch points of X4's camera
// input function (docs/LINUX_FINDINGS.md), two sets in turn, and the camera mode it tests.
struct Site { uintptr_t address; const char* what; };
constexpr Site path_sites[2][4] = {
    {{0xfeb6c5, "camera input reached"}, {0xfec070, "camera mode 0: no-ship check"},
     {0xfec0bc, "zero pose (the on-foot patch skips it)"}, {0xfec202, "tracker angles read"}},
    {{0xfec5c0, "no camera controller"}, {0xfeb6fc, "tracker gate (slot 6)"},
     {0xfec1b0, "tracker slot 23 check"}, {0xfec1f9, "mode 0 recheck passed"}}};
// X4VR_WATCH_HEAD=4: who calls the head offset's readers (return addresses).
constexpr Site call_sites[4] = {
    {0x11d9380, "on-foot head-offset composer"}, {0x1628ce0, "cockpit head-offset reader"},
    {0x1628dd0, "caller of the cockpit reader"}, {0x1dccd30, "camera getter (on-foot path)"}};
const char* site_name(uint64_t ip) {
    for (const auto& set : path_sites) for (const auto& site : set) if (site.address == ip) return site.what;
    for (const auto& site : call_sites) if (site.address == ip) return site.what;
    return nullptr;
}
// X4's camera controller, [[player global]+0x3e8] (0 without one). Its update (0x1933d00, head
// path 0x1935ac1) stores the head position (4 floats) at +0x5a0 and the head rotation at +0x5b0:
// X4VR_WATCH_HEAD=3 watches who reads them (Windows: Camera::GetOffset, head offset +0x590).
uintptr_t camera_controller() {
    const auto player = *reinterpret_cast<const volatile uintptr_t*>(0x3db6948);
    return player ? *reinterpret_cast<const volatile uintptr_t*>(player+0x3e8) : 0;
}
constexpr size_t head_offset_field = 0x5a0;
// The camera mode X4's input function tests: controller +0x880; -1 without a controller.
int camera_mode() {
    const auto camera = camera_controller();
    return camera ? *reinterpret_cast<const volatile int32_t*>(camera+0x880) : -1;
}

std::string address_line(uint64_t ip, uint64_t n) {
    char line[128];
    const char* name = site_name(ip);
    std::snprintf(line, sizeof line, "X4VR watch:   0x%llx  %llu times%s%s", static_cast<unsigned long long>(ip),
                  static_cast<unsigned long long>(n), in_executable(uintptr_t(ip), 1) ? "" : "  (not X4: the mod)",
                  name ? (std::string("  ")+name).c_str() : "");
    return line;
}
void log_counts(const char* what, const std::map<uint64_t, uint64_t>& counts) {
    std::vector<std::pair<uint64_t, uint64_t>> sorted(counts.begin(), counts.end());
    std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
    log("X4VR watch: "+std::to_string(sorted.size())+" "+what+":");
    for (size_t i = 0; i < sorted.size() && i < 40; ++i) log(address_line(sorted[i].first, sorted[i].second));
}

void watcher(int kind) {
    const bool path = kind == 2, offset = kind == 3, callers_mode = kind == 4;
    log("X4VR watch: self-test: "+self_test());
    for (int i = 0; i < 1200 && !tracker.load(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(500));
    if (path || offset || callers_mode) {
        if (!in_executable(path_sites[0][0].address, 1) || !in_executable(0x3db6948, 8)) { log("X4VR watch: not X4 9.00; no watch"); return; }
        log(path ? "X4VR watch: path mode: which branches X4's camera input takes (sets A and B in turn)"
                 : offset ? "X4VR watch: offset mode: which code reads the camera controller's head offset (+0x5a0 position, +0x5b0 rotation)"
                 : "X4VR watch: caller mode: who calls the head offset's readers");
    }
    uintptr_t watched_camera = 0;
    auto* object = static_cast<char*>(tracker.load());
    if (!object) { log("X4VR watch: X4 never asked the tracker for the head position in 10 min (eye-at-use hook off?); no watch"); return; }
    {
        char line[96];
        std::snprintf(line, sizeof line, "X4VR watch: OpenTrack object at 0x%llx", static_cast<unsigned long long>(uintptr_t(object)));
        log(line);
    }
    const uintptr_t base = uintptr_t(object);
    const uintptr_t data_targets[] = {base+field_position, base+field_position+8, base+field_angles, base+field_angles+8};
    const long page = sysconf(_SC_PAGESIZE);
    std::map<pid_t, std::vector<Watch>> watches;
    std::string first_error;
    auto next_scan = std::chrono::steady_clock::now();
    for (int window = 1; window <= 10; ++window) {
        const auto game = game_state();
        const char* label = !game.head_tracking ? "no head tracking (on foot, menu or loading)" :
                            !game.controlling_ship ? "head tracking, not flying" : game.fullscreen_menu ? "cockpit, menu" : "cockpit";
        uintptr_t targets[4];
        const uintptr_t camera = offset ? camera_controller() : 0;
        for (int i = 0; i < 4; ++i)
            targets[i] = path ? path_sites[(window-1) % 2][i].address : offset ? camera+head_offset_field+8*size_t(i) :
                         callers_mode ? call_sites[i].address : data_targets[i];
        if (offset) {
            char line[96];
            std::snprintf(line, sizeof line, "X4VR watch: camera controller at 0x%llx", static_cast<unsigned long long>(camera));
            log(line);
        }
        if (path || (offset && camera != watched_camera)) { // other addresses: reopen on every thread
            for (auto& [tid, list] : watches) for (auto& w : list) { munmap(w.ring, w.size); close(w.fd); }
            watches.clear();
            next_scan = std::chrono::steady_clock::now();
            watched_camera = camera;
        }
        if (offset && !camera) { std::this_thread::sleep_for(std::chrono::seconds(20)); log("X4VR watch: no camera controller"); continue; }
        std::map<int, int> modes; // camera mode -> samples
        log("X4VR watch: window "+std::to_string(window)+" of 10, "+label+(path ? (window % 2 ? ", set A" : ", set B") : "")+": recording 20 s");
        std::map<uint64_t, uint64_t> counts;
        std::map<std::pair<uint64_t, uint64_t>, uint64_t> calls;
        { std::lock_guard lock(callers_mutex); callers.clear(); }
        float last[3];
        std::memcpy(last, object+field_position, sizeof last);
        int changes = 0; // the position changing while it's sampled (every 20 ms)
        const auto end = std::chrono::steady_clock::now()+std::chrono::seconds(20);
        while (std::chrono::steady_clock::now() < end) {
            if (std::chrono::steady_clock::now() >= next_scan) { // new threads appear: watch them too
                next_scan += std::chrono::seconds(2);
                std::error_code ec;
                for (const auto& entry : std::filesystem::directory_iterator("/proc/self/task", ec)) {
                    const pid_t tid = pid_t(std::atoi(entry.path().filename().c_str()));
                    if (!tid || watches.count(tid)) continue;
                    auto& list = watches[tid];
                    for (const uintptr_t address : targets) {
                        std::string error;
                        Watch w;
                        w.fd = open_watch(tid, address, error, path || callers_mode, callers_mode);
                        if (w.fd < 0) { if (first_error.empty()) first_error = error; continue; }
                        w.size = size_t(page)*(ring_pages+1);
                        w.ring = mmap(nullptr, w.size, PROT_READ | PROT_WRITE, MAP_SHARED, w.fd, 0);
                        if (w.ring == MAP_FAILED) { close(w.fd); continue; }
                        list.push_back(w);
                    }
                }
            }
            for (auto& [tid, list] : watches) for (auto& w : list) drain(w, counts, &calls);
            float now[3];
            std::memcpy(now, object+field_position, sizeof now);
            if (std::memcmp(now, last, sizeof now)) { ++changes; std::memcpy(last, now, sizeof now); }
            if (path || offset || callers_mode) ++modes[camera_mode()];
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        for (auto& [tid, list] : watches) for (auto& w : list) drain(w, counts, &calls);
        size_t opened = 0;
        for (auto& [tid, list] : watches) opened += list.size();
        if (!opened) { log("X4VR watch: no watchpoint could be set ("+first_error+"); check /proc/sys/kernel/perf_event_paranoid"); break; }
        char line[160];
        std::snprintf(line, sizeof line, "X4VR watch: window %d: position changed %d times; now %.3f %.3f %.3f; %zu watchpoints", window,
                      changes, double(last[0]), double(last[1]), double(last[2]), opened);
        log(line);
        if (path || offset || callers_mode) {
            std::string seen;
            for (const auto& [mode, n] : modes) seen += " "+std::to_string(mode)+" ("+std::to_string(n)+"x)";
            log("X4VR watch: camera mode (+0x880, -1 no camera):"+seen);
            log_counts(path ? "branch points reached (per frame on the game thread)" : offset ? "code addresses touched the head offset" :
                       "functions called", counts);
            for (const auto& [key, n] : calls) {
                char line[160];
                std::snprintf(line, sizeof line, "X4VR watch:   0x%llx called from 0x%llx  %llu times", static_cast<unsigned long long>(key.first),
                              static_cast<unsigned long long>(key.second), static_cast<unsigned long long>(n));
                log(line);
            }
        } else
            log_counts("code addresses touched the tracker's position or angles", counts);
        std::map<uint64_t, uint64_t> copy;
        { std::lock_guard lock(callers_mutex); copy = callers; }
        log_counts("callers of the position accessor (return addresses)", copy);
    }
    for (auto& [tid, list] : watches) for (auto& w : list) { munmap(w.ring, w.size); close(w.fd); }
    watching = false;
    log("X4VR watch: done");
}
}

void note_tracker_use(void* object, uintptr_t caller) {
    if (!watching.load(std::memory_order_relaxed)) return;
    void* none = nullptr;
    tracker.compare_exchange_strong(none, object);
    std::lock_guard lock(callers_mutex);
    ++callers[caller];
}

void start_head_watch() {
    const char* on = std::getenv("X4VR_WATCH_HEAD");
    if (!on || *on < '1' || *on > '4') return;
    watching = true;
    std::thread(watcher, *on-'0').detach();
}
}
