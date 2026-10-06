// x4vr: the Linux port's tool. Without arguments, a full-screen menu (checks, VR settings, Launch
// in VR, HUD distance, bug report, uninstall); its actions and the Phase 0 measurements
// (docs/LINUX_PORT_PLAN.md) are also subcommands (x4vr help).
#include "elf_classes.hpp"
#include "code_scan.hpp"
#include "../launcher/hud_mod.hpp"
#include "../launcher/launcher_settings.hpp"
#include "md5.hpp"
#include "settings_control.hpp"
#include "opentrack.hpp"
#include "steam_config.hpp"
#include "terminal_ui.hpp"
#include <x4vr/session.hpp>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <unistd.h>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <functional>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <mutex>
#include <optional>
#include <regex>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {
using namespace std::chrono_literals;
constexpr double pi = 3.14159265358979323846;

void usage() {
    std::cout <<
        "Usage: x4vr [command] [options]\n"
        "\n"
        "  (no command) or menu\n"
        "      The menu: checks, VR settings, Launch X4 in VR, HUD distance, bug report, uninstall.\n"
        "\n"
        "  launch\n"
        "      Launch X4 in VR: starts SteamVR if needed, then X4 through Steam (steam -applaunch) with\n"
        "      the mod. Steam's own Play button starts the normal game.\n"
        "\n"
        "  launch-option status | copy\n"
        "      X4's Steam launch option (x4vr-run %command%), needed for Launch: shows whether it is\n"
        "      set, or copies it to paste in Steam (X4 > Properties > General > Launch options).\n"
        "\n"
        "  settings-mode vr | 2d | status\n"
        "      Keeps X4's settings for 2D and VR apart (config.xml.x4vr-2d / -vr): x4vr-run switches to\n"
        "      VR before a VR launch and back to 2D when X4 exits.\n"
        "\n"
        "  install-desktop\n"
        "      Adds \"X4 VR\" (this menu, in a terminal) to the app launcher (rofi, desktop menus).\n"
        "\n"
        "  report\n"
        "      Packs logs, settings and a summary into ~/x4vr-report-<time>.tar.gz for a bug report.\n"
        "\n"
        "  uninstall\n"
        "      Removes what the mod set up (desktop entry, HUD extension, its settings and copies).\n"
        "\n"
        "  vr-check [--seconds N]\n"
        "      Connects to SteamVR (OpenVR), prints the headset, render size and eye positions,\n"
        "      then the head pose 4 times a second for N seconds (default 10). Start SteamVR first.\n"
        "\n"
        "  udp-send [--host 127.0.0.1] [--port 4242] [--rate 90]\n"
        "      Sends OpenTrack UDP head poses (as X4 expects with OpenTrack Support on) and reads\n"
        "      commands from the terminal. Type 'help' once it runs.\n"
        "\n"
        "  check\n"
        "      Checks X4's settings (config.xml) for VR: FOV, anti-aliasing, upscaling, frame\n"
        "      generation, VSync, frame rate limit, OpenTrack support, and the resolution SteamVR\n"
        "      uses, in windowed mode (X4VR_RESOLUTION=WxH or 0 overrides).\n"
        "\n"
        "  fix-settings [--auto]\n"
        "      Sets what check reports, keeping everything else (backup: config.xml.x4vr-backup,\n"
        "      made once). Refuses while X4 runs: X4 rewrites config.xml when it exits. --auto: quiet\n"
        "      unless something changed (x4vr-run uses it before every start).\n"
        "\n"
        "  hud <factor> | remove | status | --refresh\n"
        "      Moves X4's cockpit HUD <factor> times further away at the same apparent size (1 to 6;\n"
        "      2.5 is a good start): writes the extension extensions/x4vr_hud, built from your own game\n"
        "      files, as the Windows launcher does. X4 must be closed. --refresh applies the factor chosen\n"
        "      in the menu (stereo.txt hud_factor) and rebuilds it after a game update (x4vr-run does\n"
        "      that before every VR start). Game directory: $X4VR_GAME_DIR, else\n"
        "      Steam's default library. X4 then counts as modified (no online features; saves made\n"
        "      with it stay flagged).\n"
        "\n"
        "  patterns [path to the X4 executable]\n"
        "      Finds the X4 code the mod patches and hooks by its bytes, as the mod does at startup,\n"
        "      and prints where (default: X4 in the game directory). A site it doesn't find stays\n"
        "      unpatched in the game; the mod logs the same list (\"X4VR scan\").\n"
        "\n"
        "  game-grep <text-regex> [path-regex]\n"
        "      Searches the game's catalog files (the base game's copy of each file) whose path matches\n"
        "      [path-regex] (default: UI scripts and assets) and prints path:line: text for each match.\n"
        "\n"
        "  ctl recenter | flat\n"
        "      While X4 runs with x4vr-run: recentre the view (and the virtual screen), or switch\n"
        "      the flat virtual screen on/off. Edits stereo.txt in $X4VR_DIR (default\n"
        "      ~/.local/state/x4vr). Bind them to keys in your desktop (Ctrl+F12 / Ctrl+F11 on Windows).\n"
        "\n"
        "  elf-classes <executable> <filter>\n"
        "      Lists C++ classes whose type name contains <filter> (e.g. Track), with their base\n"
        "      classes and vtables, from the RTTI of a non-PIE executable such as Linux X4.\n";
}

template<class T> bool parse_number(std::string_view text, T& value) {
    const auto end = text.data()+text.size();
    const auto [ptr, ec] = std::from_chars(text.data(), end, value);
    return ec == std::errc() && ptr == end;
}

// ---- vr-check -------------------------------------------------------------------------------
void print_pose(const char* label, const x4vr::Matrix& m) {
    // OpenVR seated: +X right, +Y up, -Z forward; R = Ry(yaw) * Rx(pitch) * Rz(roll).
    const double yaw = std::atan2(m.m[0][2], m.m[2][2])*180/pi;
    const double pitch = std::asin(std::fmax(-1.0, std::fmin(1.0, -double(m.m[1][2]))))*180/pi;
    const double roll = std::atan2(m.m[1][0], m.m[1][1])*180/pi;
    std::printf("%s position_m=(%+.3f, %+.3f, %+.3f) yaw=%+7.2f pitch=%+7.2f roll=%+7.2f\n",
                label, m.m[0][3], m.m[1][3], m.m[2][3], yaw, pitch, roll);
}
int vr_check(const std::vector<std::string_view>& args) {
    int seconds = 10;
    for (size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--seconds" && i+1 < args.size() && parse_number(args[i+1], seconds) && seconds >= 0) ++i;
        else { usage(); return 2; }
    }
    std::cout << "runtime_installed=" << vr::VR_IsRuntimeInstalled() << '\n';
    char path[4096]{};
    uint32_t needed = 0;
    if (vr::VR_GetRuntimePath(path, sizeof(path), &needed)) std::cout << "runtime_path=" << path << '\n';
    // Only informative: on Linux this quick check can report no headset while SteamVR has one
    // (streamed headsets, or SteamVR's client library failing to load here). Connecting tells why.
    std::cout << "headset_present=" << vr::VR_IsHmdPresent() << '\n' << std::flush;
    x4vr::Session session;
    try {
        session.initialize();
    } catch (const std::exception& error) {
        std::cerr << "Connecting to SteamVR failed: " << error.what() << "\n"
                     "Is SteamVR running with the headset? If it is, try again through steam-run (docs/LINUX_PHASE0.md, step 3).\n";
        return 4;
    }
    std::cout << "headset_model=" << session.headset_model() << '\n';
    std::cout << "required_instance_extensions=";
    for (const auto& ext : session.instance_extensions()) std::cout << ext << ' ';
    std::cout << '\n';
    x4vr::Frame frame;
    auto status = x4vr::FrameStatus::tracking_unavailable;
    for (auto deadline = std::chrono::steady_clock::now()+5s; std::chrono::steady_clock::now() < deadline;) {
        status = session.begin_frame(frame, 0.05f, 10000.f);
        if (status == x4vr::FrameStatus::ready || status == x4vr::FrameStatus::quit_requested) break;
        std::this_thread::sleep_for(20ms);
    }
    if (status != x4vr::FrameStatus::ready) {
        std::cerr << "frame_unavailable=" << int(status) << ' ' << session.last_error() << '\n';
        return 5;
    }
    std::cout << "per_eye_render_size=" << frame.width << 'x' << frame.height << '\n';
    for (int e = 0; e < 2; ++e) print_pose(e ? "right_eye" : "left_eye ", frame.eyes[e].seated_from_eye);
    std::cout << "Head pose for " << seconds << " s (seated space; move your head):\n" << std::flush;
    int good = 0, bad = 0;
    for (auto end = std::chrono::steady_clock::now()+std::chrono::seconds(seconds); std::chrono::steady_clock::now() < end;) {
        x4vr::Matrix head;
        status = session.sample_tracking(head);
        if (status == x4vr::FrameStatus::quit_requested) break;
        if (status == x4vr::FrameStatus::ready) { ++good; print_pose("head", head); }
        else { ++bad; std::puts("head tracking_unavailable"); }
        std::fflush(stdout);
        std::this_thread::sleep_for(250ms);
    }
    session.shutdown();
    std::cout << "samples_tracked=" << good << " samples_untracked=" << bad << '\n';
    return good > 0 || seconds == 0 ? 0 : 5;
}

// ---- udp-send -------------------------------------------------------------------------------
struct Sender {
    std::mutex mutex;
    x4vr::opentrack::Pose base;
    int alt_axis = -1; double alt_value = 0;               // alternates +value/-value per packet
    int sweep_axis = -1; double sweep_amplitude = 0, sweep_period = 4; // sine on one axis
    bool paused = false;
    std::atomic<uint64_t> sent{0}, failed{0};
};
std::string describe(Sender& s) {
    std::lock_guard lock(s.mutex);
    std::ostringstream out;
    out << "pose:";
    for (int i = 0; i < 6; ++i) out << ' ' << x4vr::opentrack::axis_names[i] << '=' << s.base.axis(i);
    if (s.alt_axis >= 0) out << " | alt " << x4vr::opentrack::axis_names[s.alt_axis] << " +-" << s.alt_value;
    if (s.sweep_axis >= 0) out << " | sweep " << x4vr::opentrack::axis_names[s.sweep_axis] << " +-" << s.sweep_amplitude << " every " << s.sweep_period << " s";
    out << (s.paused ? " | PAUSED" : "") << " | sent " << s.sent << " failed " << s.failed;
    return out.str();
}
void udp_help() {
    std::cout <<
        "Commands (OpenTrack units: x y z in cm, yaw pitch roll in degrees):\n"
        "  <axis> <value>                set one axis, e.g. 'yaw 30' or 'z -5'\n"
        "  zero                          all axes to 0, alternation and sweep off\n"
        "  alt <axis> <value> | alt off  alternate +value/-value every packet (smoothing test)\n"
        "  sweep <axis> <amplitude> <period_s> | sweep off\n"
        "  pause | resume                stop/start sending (what does X4 do without packets?)\n"
        "  show | help | quit\n";
}
int udp_send(const std::vector<std::string_view>& args) {
    std::string host = "127.0.0.1";
    int port = x4vr::opentrack::default_port;
    double rate = 90;
    for (size_t i = 0; i < args.size(); i += 2) {
        const auto key = args[i];
        const auto value = i+1 < args.size() ? args[i+1] : std::string_view();
        if (key == "--host" && !value.empty()) host = std::string(value);
        else if (key == "--port" && parse_number(value, port) && port > 0 && port < 65536) {}
        else if (key == "--rate" && parse_number(value, rate) && rate >= 1 && rate <= 1000) {}
        else { usage(); return 2; }
    }
    sockaddr_in to{};
    to.sin_family = AF_INET;
    to.sin_port = htons(uint16_t(port));
    if (inet_pton(AF_INET, host.c_str(), &to.sin_addr) != 1) { std::cerr << "Not an IPv4 address: " << host << '\n'; return 2; }
    const int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) { std::perror("socket"); return 1; }

    Sender sender;
    std::atomic_bool running{true};
    std::thread thread([&] {
        const auto start = std::chrono::steady_clock::now();
        const auto period = std::chrono::duration<double>(1.0/rate);
        auto next = start;
        for (uint64_t n = 0; running; ++n) {
            next += std::chrono::duration_cast<std::chrono::steady_clock::duration>(period);
            x4vr::opentrack::Pose pose;
            bool paused;
            {
                std::lock_guard lock(sender.mutex);
                pose = sender.base;
                paused = sender.paused;
                if (sender.alt_axis >= 0) pose.axis(sender.alt_axis) += (n % 2 ? -1 : 1)*sender.alt_value;
                if (sender.sweep_axis >= 0) {
                    const double t = std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count();
                    pose.axis(sender.sweep_axis) += sender.sweep_amplitude*std::sin(2*pi*t/sender.sweep_period);
                }
            }
            if (!paused) {
                const auto packet = x4vr::opentrack::encode(pose);
                if (sendto(fd, packet.data(), packet.size(), 0, reinterpret_cast<const sockaddr*>(&to), sizeof(to)) == ssize_t(packet.size())) ++sender.sent;
                else ++sender.failed;
            }
            std::this_thread::sleep_until(next);
        }
    });
    std::cout << "Sending to " << host << ':' << port << " at " << rate << " Hz.\n";
    udp_help();
    std::cout << describe(sender) << "\n> " << std::flush;
    for (std::string line; std::getline(std::cin, line); std::cout << "> " << std::flush) {
        std::istringstream in(line);
        std::string command;
        if (!(in >> command)) { std::cout << describe(sender) << '\n'; continue; }
        if (command == "quit" || command == "exit") break;
        if (command == "help") { udp_help(); continue; }
        bool ok = true;
        {
            std::lock_guard lock(sender.mutex);
            std::string axis_name; double value = 0, period = 0;
            if (command == "zero") { sender.base = {}; sender.alt_axis = sender.sweep_axis = -1; }
            else if (command == "pause") sender.paused = true;
            else if (command == "resume") sender.paused = false;
            else if (command == "show") {}
            else if (command == "alt" || command == "sweep") {
                const bool sweep = command == "sweep";
                in >> axis_name;
                if (axis_name == "off") (sweep ? sender.sweep_axis : sender.alt_axis) = -1;
                else if (const int axis = x4vr::opentrack::axis_index(axis_name); axis >= 0 && (in >> value) && (!sweep || ((in >> period) && period > 0))) {
                    if (sweep) { sender.sweep_axis = axis; sender.sweep_amplitude = value; sender.sweep_period = period; }
                    else { sender.alt_axis = axis; sender.alt_value = value; }
                } else ok = false;
            } else if (const int axis = x4vr::opentrack::axis_index(command); axis >= 0 && (in >> value)) sender.base.axis(axis) = value;
            else ok = false;
        }
        if (!ok) std::cout << "Didn't understand that. Type 'help'.\n";
        std::cout << describe(sender) << '\n';
    }
    running = false;
    thread.join();
    close(fd);
    std::cout << "\nStopped. " << describe(sender) << '\n';
    return 0;
}

// ---- check / fix-settings --------------------------------------------------------------------
// Linux X4 keeps one config per Steam account under ~/.config/EgoSoft/X4/<id>/ (also looked for
// under $XDG_CONFIG_HOME when that is set elsewhere); use the newest.
std::filesystem::path x4_config() {
    const char* home = std::getenv("HOME");
    const char* xdg = std::getenv("XDG_CONFIG_HOME");
    std::vector<std::filesystem::path> bases{std::filesystem::path(home ? home : ".")/".config/EgoSoft/X4"};
    if (xdg && *xdg) bases.push_back(std::filesystem::path(xdg)/"EgoSoft/X4");
    std::filesystem::path best;
    std::filesystem::file_time_type newest{};
    for (const auto& base : bases) {
        std::error_code error;
        for (const auto& entry : std::filesystem::directory_iterator(base, error)) {
            std::error_code time_error;
            const auto config = entry.path()/"config.xml";
            const auto time = std::filesystem::last_write_time(config, time_error);
            if (!time_error && (best.empty() || time > newest)) { best = config; newest = time; }
        }
    }
    return best;
}
bool x4_running() {
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator("/proc", error)) {
        std::error_code link_error;
        if (std::filesystem::read_symlink(entry.path()/"exe", link_error).filename() == "X4") return true;
    }
    return false;
}
// The Windows launcher's checks (tools/launcher/launcher_settings.hpp, tested in launcher_tests),
// minus "fullscreen, not borderless": on Windows that is for NVIDIA DSR; on Linux X4's borderless
// window at the desktop resolution is fine.
// X4's resolution: the smallest common 16:9 mode at least as large as what SteamVR uses (saved by
// SteamVR directly when it runs, else saved by the mod in x4_resolution.txt while X4 ran), so X4
// doesn't render pixels SteamVR throws away. Chosen in the menu: stereo.txt x4_width/x4_height.
// X4VR_RESOLUTION=WxH sets it, =0 leaves it alone.
std::filesystem::path settings_file();
std::filesystem::path settings_marker();
// What the mod computes at startup (vr_layer.cpp, presenter_initialize): X4's image spans
// 2*tan_x by 2*tan_y (tan_y = game_tan_y, X4's FOV), at the pixels per tangent SteamVR recommends.
// Asked as a background app: it doesn't start SteamVR or show up as a running game.
bool steamvr_resolution(const std::string& xml, int& w, int& h) {
    if (std::getenv("X4VR_NO_STEAMVR_QUERY")) return false;
    auto error = vr::VRInitError_None;
    auto* system = vr::VR_Init(&error, vr::VRApplication_Background);
    if (!system || error != vr::VRInitError_None) return false;
    uint32_t rec_w = 0, rec_h = 0;
    system->GetRecommendedRenderTargetSize(&rec_w, &rec_h);
    double want_x = 0, want_y = 0;
    for (int e = 0; e < 2; ++e) {
        float l, r, t, b;
        system->GetProjectionRaw(vr::EVREye(e), &l, &r, &t, &b);
        if (r > l) want_x = std::max(want_x, rec_w/double(r-l));
        if (b != t) want_y = std::max(want_y, rec_h/std::fabs(double(b-t)));
    }
    vr::VR_Shutdown();
    double tan_y = 0.8675; // stereo.txt game_tan_y
    std::ifstream settings(settings_file());
    for (std::string line; std::getline(settings, line);) if (line.rfind("game_tan_y=", 0) == 0) tan_y = std::atof(line.c_str()+11);
    std::string rw, rh;
    const double aspect = x4vr::launcher::xml_value(xml, "res_width", rw) && x4vr::launcher::xml_value(xml, "res_height", rh) &&
                          std::atoi(rh.c_str()) > 0 ? std::atof(rw.c_str())/std::atof(rh.c_str()) : 16.0/9;
    w = int(std::lround(2*tan_y*aspect*want_x));
    h = int(std::lround(2*tan_y*want_y));
    return w > 0 && h > 0;
}
std::pair<int, int> wanted_resolution(const std::string& xml) {
    int w = 0, h = 0;
    if (const char* set = std::getenv("X4VR_RESOLUTION"); set && *set) {
        if (std::sscanf(set, "%dx%d", &w, &h) == 2 && w > 0 && h > 0) return {w, h};
        return {0, 0};
    }
    // Chosen in the menu (stereo.txt x4_width / x4_height, as the Windows launcher); 0: automatic.
    w = std::atoi(x4vr::linux_port::read_setting(settings_file(), "x4_width", "0").c_str());
    h = std::atoi(x4vr::linux_port::read_setting(settings_file(), "x4_height", "0").c_str());
    if (w > 0 && h > 0) return {w, h};
    const auto saved = settings_file().parent_path()/"x4_resolution.txt";
    if (steamvr_resolution(xml, w, h)) {
        std::ofstream(saved, std::ios::trunc) << w << 'x' << h << '\n';
    } else { // SteamVR not running: what the mod saved last time
        std::ifstream in(saved);
        if (!(in >> w) || in.get() != 'x' || !(in >> h) || w <= 0 || h <= 0) return {0, 0};
    }
    static constexpr std::pair<int, int> modes[] = {{1920, 1080}, {2560, 1440}, {2880, 1620}, {3200, 1800}, {3840, 2160}};
    for (const auto& mode : modes) if (mode.first >= w && mode.second >= h) return mode;
    return modes[std::size(modes)-1];
}
std::vector<x4vr::launcher::Check> linux_checks(const std::string& xml) {
    const auto [width, height] = wanted_resolution(xml);
    auto checks = x4vr::launcher::check_x4(xml, width, height);
    std::erase_if(checks, [](const auto& c) { return c.label.rfind("Display mode", 0) == 0; }); // Windows' fullscreen rule
    // Linux X4 renders fullscreen and borderless windows at the desktop size whatever its
    // resolution setting says; only a window keeps it (tiling window managers may resize it).
    if (width > 0 && height > 0) {
        std::string fullscreen = "(missing)", borderless = "(missing)";
        x4vr::launcher::xml_value(xml, "fullscreen", fullscreen);
        x4vr::launcher::xml_value(xml, "borderless", borderless);
        checks.push_back({"Display mode: windowed", true, fullscreen == "false" && borderless == "false",
                          "fullscreen "+fullscreen+", borderless "+borderless, {{"fullscreen", "false"}, {"borderless", "false"}}});
    }
    return checks;
}
std::string read_text(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}
int check_settings(bool fix, bool automatic) {
    const auto path = x4_config();
    if (path.empty()) { std::cerr << "X4's config.xml not found under ~/.config/EgoSoft/X4 (start X4 once).\n"; return automatic ? 0 : 1; }
    const auto xml = read_text(path);
    const auto checks = linux_checks(xml);
    // Only settings that exist in this config.xml are changed: Linux X4 may name some differently,
    // and a key X4 doesn't know would only clutter the file.
    std::vector<x4vr::launcher::Check> fixable;
    for (const auto& c : checks) {
        std::string value;
        const bool present = std::all_of(c.fix.begin(), c.fix.end(), [&](const auto& kv) { return x4vr::launcher::xml_value(xml, kv.first, value); });
        if (!c.ok && present) fixable.push_back(c);
        if (!automatic) std::printf("%-4s %-36s %s%s\n", c.ok ? "ok" : c.required ? "FIX" : "tip", c.label.c_str(), c.current.c_str(),
                                    c.ok || present ? "" : "  (not in this config.xml: change it in the game)");
    }
    if (!fix) {
        if (!automatic) std::cout << (fixable.empty() ? "Nothing to fix." : "Run 'x4vr fix-settings' with X4 closed to fix these.") << '\n';
        return 0;
    }
    if (fixable.empty()) { if (!automatic) std::cout << "Nothing to fix.\n"; return 0; }
    if (x4_running()) { std::cerr << "X4 is running: close it first (it rewrites config.xml when it exits).\n"; return 1; }
    const auto backup = path.string()+".x4vr-backup";
    std::error_code error;
    if (!std::filesystem::exists(backup)) std::filesystem::copy_file(path, backup, error);
    const auto temporary = path.string()+".x4vr-tmp";
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        out << x4vr::launcher::fix_x4(xml, fixable);
        if (!out) { std::cerr << "Can't write " << temporary << '\n'; return 1; }
    }
    std::filesystem::rename(temporary, path);
    for (const auto& c : fixable) std::cout << "x4vr: X4 setting fixed for VR: " << c.label << " (was " << c.current << ")\n";
    std::cout << "x4vr: " << path.string() << " updated; original kept as " << backup << '\n';
    return 0;
}

// ---- hud --------------------------------------------------------------------------------------
// The Windows launcher's HUD distance mod (tools/launcher/hud_mod.hpp, launcher.cpp): the HUD's
// anchor positions and world-space scale factors times k, as a substitution extension.
std::filesystem::path game_dir() {
    if (const char* dir = std::getenv("X4VR_GAME_DIR"); dir && *dir) return dir;
    for (const auto& library : x4vr::steam::libraries()) // every Steam library, also on other drives
        if (const auto candidate = library/"steamapps/common/X4 Foundations"; std::filesystem::exists(candidate/"01.cat")) return candidate;
    return {};
}
// Linux X4 9.00 ships each UI script as .lua and as .xpl, precompiled bytecode, and loads the
// .xpl: replacing only the .lua moved the HUD back but kept its size (2026-10-04). The extension
// therefore also puts the patched Lua source at the .xpl path (Lua's loader takes source or bytecode).
std::string xpl_of(const std::string& lua) { return lua.substr(0, lua.size()-4)+".xpl"; }
std::map<std::string, std::string> hud_originals(const std::filesystem::path& game) {
    std::set<std::string> paths(x4vr::launcher::hud_anchor_files().begin(), x4vr::launcher::hud_anchor_files().end());
    for (const auto& script : x4vr::launcher::hud_scripts()) { paths.insert(script); paths.insert(xpl_of(script)); }
    return x4vr::launcher::read_game_files(game, paths);
}
std::string source_hash(const std::map<std::string, std::string>& originals) {
    std::string all;
    for (const auto& [path, blob] : originals) all += path+x4vr::linux_port::md5_hex(blob);
    return x4vr::linux_port::md5_hex(all);
}
bool write_text(const std::filesystem::path& path, const std::string& text) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
    return bool(out);
}
// x4vr_hud.txt: scale=, source=
std::map<std::string, std::string> installed_hud(const std::filesystem::path& extension) {
    std::map<std::string, std::string> values;
    std::ifstream in(extension/"x4vr_hud.txt");
    for (std::string line; std::getline(in, line);)
        if (const auto split = line.find('='); split != std::string::npos) values[line.substr(0, split)] = line.substr(split+1);
    return values;
}
bool install_hud(const std::filesystem::path& game, double scale, std::string& error) {
    const auto originals = hud_originals(game);
    auto files = x4vr::launcher::hud_files(originals, scale, error);
    if (files.empty()) return false;
    for (const auto& script : x4vr::launcher::hud_scripts())
        if (originals.count(xpl_of(script))) files[xpl_of(script)] = files.at(script);
    const auto extension = game/"extensions/x4vr_hud";
    std::error_code ignored;
    std::filesystem::remove_all(extension, ignored);
    std::filesystem::create_directories(extension, ignored);
    std::ofstream data(extension/"subst_01.dat", std::ios::binary);
    std::string index;
    const auto stamp = std::to_string(std::time(nullptr));
    for (const auto& [path, blob] : files) {
        data.write(blob.data(), std::streamsize(blob.size()));
        index += path+' '+std::to_string(blob.size())+' '+stamp+' '+x4vr::linux_port::md5_hex(blob)+'\n';
    }
    data.close();
    const auto scale_text = x4vr::launcher::format_number(scale);
    const bool ok = data && write_text(extension/"subst_01.cat", index) &&
        write_text(extension/"content.xml", "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
            "<content id=\"x4vr_hud\" name=\"X4 VR HUD distance\" version=\"100\" save=\"0\" enabled=\"1\"\n"
            "  description=\"Generated by x4vr: HUD "+scale_text+"x farther away at the same apparent size, for VR. "
            "Built from your own game files.\">\n</content>\n") &&
        write_text(extension/"x4vr_hud.txt", "scale="+scale_text+"\nsource="+source_hash(originals)+"\n");
    if (!ok) error = "could not write "+extension.string();
    return ok;
}
// X4's per-user content.xml (next to config.xml) remembers extensions turned off in its menu.
// Outside a VR session the extension stays off: a VR launch turns it on (settings-mode).
void enable_hud_in_vr() {
    const auto config = x4_config();
    if (config.empty() || !std::filesystem::exists(settings_marker())) return;
    const auto content_path = config.parent_path()/"content.xml";
    bool disabled = false;
    const auto content = x4vr::launcher::enable_hud_extension(read_text(content_path), disabled);
    if (disabled && !write_text(content_path, content))
        std::cout << "X4 has it turned off: turn on \"X4 VR HUD distance\" in X4's Extensions menu.\n";
}
// The factor chosen in the menu (stereo.txt hud_factor, 0: X4's default), or -1 if never chosen.
double wanted_hud() {
    const auto value = x4vr::linux_port::read_setting(settings_file(), "hud_factor", "");
    return value.empty() ? -1 : std::atof(value.c_str());
}
// `hud <factor>` / `remove` install at once (X4 closed); the menu sets hud_factor in stereo.txt
// and x4vr-run's `hud --refresh` applies it at the next VR launch, also rebuilding the extension
// after a game update.
int hud(const std::vector<std::string_view>& args) {
    if (args.size() != 1) { usage(); return 2; }
    const auto game = game_dir();
    const bool refresh = args[0] == "--refresh";
    if (game.empty()) {
        if (refresh) return 0;
        std::cerr << "X4's game directory not found: set X4VR_GAME_DIR to the directory with 01.cat.\n";
        return 1;
    }
    const auto extension = game/"extensions/x4vr_hud";
    const auto installed = installed_hud(extension);
    const double installed_scale = installed.count("scale") ? std::atof(installed.at("scale").c_str()) : 0;
    if (args[0] == "status") {
        if (installed_scale > 0) std::cout << "HUD distance mod installed: factor " << installed.at("scale") << " (" << extension.string() << ")\n";
        else std::cout << "HUD distance mod not installed (X4's default HUD distance).\n";
        if (const double wanted = wanted_hud(); wanted >= 0 && std::fabs(wanted-installed_scale) > 0.001)
            std::cout << "At the next VR launch: " << (wanted > 0 ? "factor "+x4vr::launcher::format_number(wanted) : std::string("removed")) << '\n';
        return 0;
    }
    if (x4_running()) { std::cerr << "X4 is running: close it first (it loads extensions at startup).\n"; return refresh ? 0 : 1; }
    std::error_code ignored;
    double scale = 0;
    if (refresh) {
        const double wanted = wanted_hud();
        if (wanted == 0 && installed_scale > 0) { std::filesystem::remove_all(extension, ignored); std::cout << "x4vr: HUD distance mod removed\n"; return 0; }
        if (wanted > 0 && std::fabs(wanted-installed_scale) > 0.001) scale = wanted; // changed in the menu
        else if (installed_scale <= 0 || (installed.count("source") && installed.at("source") == source_hash(hud_originals(game)))) return 0;
        else { // after a game update the mod would replace new game files with old copies
            std::string error;
            if (install_hud(game, installed_scale, error)) { std::cout << "x4vr: HUD distance mod rebuilt for the updated game files\n"; return 0; }
            std::filesystem::remove_all(extension, ignored);
            std::cout << "x4vr: HUD distance mod removed: it no longer matches this X4 version (" << error << ")\n";
            return 0;
        }
    } else if (args[0] == "remove") {
        std::filesystem::remove_all(extension, ignored);
        x4vr::linux_port::write_setting(settings_file(), "hud_factor", "0");
        std::cout << "HUD distance mod removed.\n";
        return 0;
    } else if (!parse_number(args[0], scale) || scale < 1 || scale > 6) {
        std::cerr << "HUD distance: use a factor between 1 and 6 (2.5 is a good start).\n";
        return 2;
    }
    std::string error;
    if (!install_hud(game, scale, error)) { std::cerr << "Could not build the HUD mod: " << error << '\n'; return 1; }
    if (!refresh && std::filesystem::exists(settings_file())) x4vr::linux_port::write_setting(settings_file(), "hud_factor", x4vr::launcher::format_number(scale));
    std::cout << (refresh ? "x4vr: " : "") << "HUD distance mod installed: factor " << x4vr::launcher::format_number(scale) << " (" << extension.string() << ")\n";
    if (!refresh) {
        std::cout << "X4 will report a modified game: online features are off, and saves made with the mod stay flagged.\n";
        std::cout << "X4's Protected UI Mode blocks the HUD's size factors: turn it off in X4 (Extension Settings), else the HUD only moves back and shrinks.\n";
    }
    enable_hud_in_vr();
    return 0;
}

// ---- game-grep --------------------------------------------------------------------------------
int game_grep(const std::vector<std::string_view>& args) {
    if (args.empty() || args.size() > 2) { usage(); return 2; }
    const auto game = game_dir();
    if (game.empty()) { std::cerr << "X4's game directory not found: set X4VR_GAME_DIR to the directory with 01.cat.\n"; return 1; }
    std::regex text, path_filter;
    try {
        text = std::regex(std::string(args[0]));
        path_filter = std::regex(args.size() > 1 ? std::string(args[1]) : std::string(R"(^(ui/|assets/ui/).*\.(lua|xpl|xml)$)"));
    } catch (const std::regex_error& error) { std::cerr << "Bad regex: " << error.what() << '\n'; return 2; }
    // Every catalog's index, later catalogs replacing earlier copies of a path (as read_game_files).
    std::set<std::string> paths;
    for (int index = 1; index <= 99; ++index) {
        char name[8];
        std::snprintf(name, sizeof(name), "%02d.cat", index);
        std::ifstream cat(game/name);
        for (std::string line; std::getline(cat, line);) {
            int spaces = 0; size_t cut = line.size();
            while (spaces < 3 && cut != std::string::npos && cut > 0) { cut = line.rfind(' ', cut-1); ++spaces; }
            if (spaces == 3 && cut != std::string::npos) {
                auto path = line.substr(0, cut);
                if (std::regex_search(path, path_filter)) paths.insert(std::move(path));
            }
        }
    }
    int matches = 0;
    for (const auto& [path, blob] : x4vr::launcher::read_game_files(game, paths)) {
        std::istringstream in(blob);
        int number = 0;
        for (std::string line; std::getline(in, line);) {
            ++number;
            if (std::regex_search(line, text)) { std::cout << path << ':' << number << ": " << line << '\n'; ++matches; }
        }
    }
    std::cerr << matches << " matches in " << paths.size() << " files\n";
    return 0;
}

// ---- ctl ------------------------------------------------------------------------------------
std::filesystem::path settings_file() {
    if (const char* dir = std::getenv("X4VR_DIR"); dir && *dir) return std::filesystem::path(dir)/"stereo.txt";
    if (const char* state = std::getenv("XDG_STATE_HOME"); state && *state) return std::filesystem::path(state)/"x4vr/stereo.txt";
    const char* home = std::getenv("HOME");
    return std::filesystem::path(home ? home : ".")/".local/state/x4vr/stereo.txt";
}
int ctl(const std::vector<std::string_view>& args) {
    if (args.size() != 1 || (args[0] != "recenter" && args[0] != "flat")) { usage(); return 2; }
    const auto path = settings_file();
    const int next = x4vr::linux_port::control_settings(path, args[0]);
    if (next < 0) { std::cerr << "Can't update " << path << " (start X4 once with x4vr-run)\n"; return 1; }
    std::cout << (args[0] == "recenter" ? "recentred" : next == 2 ? "flat screen on" : "flat screen automatic") << '\n';
    return 0;
}

// ---- elf-classes ----------------------------------------------------------------------------
int elf_classes(const std::vector<std::string_view>& args) {
    if (args.size() != 2) { usage(); return 2; }
    const auto image = x4vr::elf::Image::load(std::string(args[0]));
    const auto classes = x4vr::elf::find_classes(image, args[1]);
    for (const auto& c : classes) {
        std::printf("class %s  [%s]  type_info 0x%llx\n", c.name.c_str(), c.mangled.c_str(), static_cast<unsigned long long>(c.typeinfo));
        for (const auto& base : c.bases) std::printf("  base %s\n", base.c_str());
        if (c.vtables.empty()) std::puts("  no vtable found");
        for (const auto& v : c.vtables) {
            std::printf("  vtable address_point 0x%llx  offset_to_top %lld  %zu slots\n",
                        static_cast<unsigned long long>(v.address_point()), static_cast<long long>(v.offset_to_top), v.slots.size());
            for (size_t i = 0; i < v.slots.size(); ++i)
                std::printf("    [%3zu] +0x%03zx  0x%llx\n", i, i*8, static_cast<unsigned long long>(v.slots[i]));
        }
    }
    std::printf("%zu classes matching \"%.*s\"\n", classes.size(), int(args[1].size()), args[1].data());
    return classes.empty() ? 3 : 0;
}

// ---- menu (x4vr without arguments): checks, settings, launch in VR, bug report, uninstall ----
// The Steam launch option `x4vr-run %command%` stays set; x4vr-run starts VR only when a launch
// request from here is fresh (linux/x4vr-run.in), so Steam's Play button starts the normal game.
// `steam -applaunch` asks the running Steam client to start X4 with Steam's usual chain
// (reaper, the Steam Linux Runtime); the game inherits Steam's environment, not ours.
std::filesystem::path program; // this executable as started (argv[0], symlinks unresolved)
std::filesystem::path state_dir() { return settings_file().parent_path(); }
std::filesystem::path run_script() { return program.parent_path()/"x4vr-run"; }
std::filesystem::path desktop_file() {
    const char* data = std::getenv("XDG_DATA_HOME");
    const char* home = std::getenv("HOME");
    return (data && *data ? std::filesystem::path(data) : std::filesystem::path(home ? home : ".")/".local/share")/"applications/x4vr.desktop";
}
bool process_named(std::string_view name) {
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator("/proc", error)) {
        std::ifstream comm(entry.path()/"comm");
        std::string text;
        if (comm && std::getline(comm, text) && text == name) return true;
    }
    return false;
}
bool steamvr_running() { return process_named("vrserver"); }
std::string quoted_path(const std::filesystem::path& path) {
    const auto text = path.string();
    return text.find(' ') == std::string::npos ? text : "\""+text+"\"";
}
std::string wanted_launch_option() { return quoted_path(run_script())+" %command%"; }

// Starts a program detached from this terminal (its own session, output discarded).
bool spawn(const std::vector<std::string>& argv) {
    const pid_t child = fork();
    if (child < 0) return false;
    if (child == 0) {
        setsid();
        if (fork() != 0) _exit(0);
        const int null = ::open("/dev/null", O_RDWR);
        if (null >= 0) { dup2(null, 0); dup2(null, 1); dup2(null, 2); }
        std::vector<char*> args;
        for (const auto& a : argv) args.push_back(const_cast<char*>(a.c_str()));
        args.push_back(nullptr);
        execvp(args[0], args.data());
        _exit(127);
    }
    int status = 0;
    waitpid(child, &status, 0);
    return true;
}
// Runs a program and waits; its output goes to `log`. True on exit status 0.
bool run_and_wait(const std::vector<std::string>& argv, const std::filesystem::path& log) {
    const pid_t child = fork();
    if (child < 0) return false;
    if (child == 0) {
        const int out = ::open(log.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
        const int null = ::open("/dev/null", O_RDONLY);
        if (null >= 0) dup2(null, 0);
        if (out >= 0) { dup2(out, 1); dup2(out, 2); }
        std::vector<char*> args;
        for (const auto& a : argv) args.push_back(const_cast<char*>(a.c_str()));
        args.push_back(nullptr);
        execvp(args[0], args.data());
        _exit(127);
    }
    int status = 0;
    waitpid(child, &status, 0);
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

// X4's launch option in every Steam account: 0 none set, 1 ours, 2 an x4vr-run elsewhere (an
// older build), 3 something else; `accounts` = how many accounts know X4.
struct LaunchOption { int state = 0; int accounts = 0; std::string value; };
LaunchOption launch_option() {
    LaunchOption result;
    for (const auto& file : x4vr::steam::local_configs()) {
        const auto value = x4vr::steam::launch_options(x4vr::steam::read_file(file));
        if (!value) continue;
        ++result.accounts;
        const int state = value->empty() ? 0 : *value == wanted_launch_option() ? 1 : value->find("x4vr-run") != std::string::npos ? 2 : 3;
        if (state > result.state) { result.state = state; result.value = *value; }
    }
    return result;
}
// Copies text to the clipboard: wl-copy (Wayland), xclip or xsel (X11), else the terminal's
// own clipboard (OSC 52, which most terminals support). Returns how.
std::string copy_to_clipboard(const std::string& text) {
    for (const auto& tool : std::vector<std::vector<std::string>>{{"wl-copy"}, {"xclip", "-selection", "clipboard"}, {"xsel", "--clipboard", "--input"}}) {
        int fds[2];
        if (pipe(fds) != 0) break;
        const pid_t child = fork();
        if (child == 0) {
            dup2(fds[0], 0);
            close(fds[0]); close(fds[1]);
            const int null = ::open("/dev/null", O_WRONLY);
            if (null >= 0) { dup2(null, 1); dup2(null, 2); }
            std::vector<char*> args;
            for (const auto& a : tool) args.push_back(const_cast<char*>(a.c_str()));
            args.push_back(nullptr);
            execvp(args[0], args.data());
            _exit(127);
        }
        close(fds[0]);
        if (child > 0) { (void)!::write(fds[1], text.data(), text.size()); }
        close(fds[1]);
        int status = 0;
        if (child > 0) waitpid(child, &status, 0);
        if (child > 0 && WIFEXITED(status) && WEXITSTATUS(status) == 0) return tool[0];
    }
    static const char* table = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string encoded;
    for (size_t i = 0; i < text.size(); i += 3) {
        const uint32_t n = (uint32_t(uint8_t(text[i])) << 16) | (i+1 < text.size() ? uint32_t(uint8_t(text[i+1])) << 8 : 0) |
                           (i+2 < text.size() ? uint8_t(text[i+2]) : 0);
        encoded += table[(n >> 18) & 63]; encoded += table[(n >> 12) & 63];
        encoded += i+1 < text.size() ? table[(n >> 6) & 63] : '=';
        encoded += i+2 < text.size() ? table[n & 63] : '=';
    }
    const std::string sequence = "\x1b]52;c;"+encoded+"\a";
    (void)!::write(STDOUT_FILENO, sequence.data(), sequence.size());
    return "the terminal";
}
std::string launch_option_steps() { return "In Steam: X4 > Properties > General > Launch options, paste it there."; }

// X4's settings for 2D and for VR, kept apart: launching in VR saves config.xml as
// config.xml.x4vr-2d and puts the VR copy (config.xml.x4vr-vr, if any) in its place; when X4
// exits, config.xml goes back to x4vr-vr and the 2D copy returns. So Steam's Play keeps the
// player's own settings, and VR keeps its own (x4vr-run, then fix-settings on top). A marker
// (x4-settings.vr: the config path) survives a crash: the next start of either kind restores 2D.
std::filesystem::path settings_marker() { return state_dir()/"x4-settings.vr"; }
bool copy_over(const std::filesystem::path& from, const std::filesystem::path& to) {
    std::error_code error;
    const auto temporary = to.string()+".x4vr-tmp";
    std::filesystem::copy_file(from, temporary, std::filesystem::copy_options::overwrite_existing, error);
    if (!error) std::filesystem::rename(temporary, to, error);
    return !error;
}
// The HUD distance extension only in VR: X4's per-user content.xml records it as
// <extension id="x4vr_hud" enabled="..."/>; switched off for 2D (no farther HUD, no "modified"
// flag on 2D saves) and on for VR. Without an entry yet (X4 adds it once it has seen the
// extension) nothing changes.
void hud_extension_enabled(const std::filesystem::path& config, bool on) {
    const auto path = config.parent_path()/"content.xml";
    const auto text = read_text(path);
    const auto changed = std::regex_replace(text, std::regex(R"re((<extension\s+id="x4vr_hud"\s+enabled=")(true|false)")re"),
                                            std::string("$1")+(on ? "true" : "false")+"\"");
    if (changed != text) write_text(path, changed);
}
int settings_mode(const std::vector<std::string_view>& args) {
    if (args.size() != 1 || (args[0] != "vr" && args[0] != "2d" && args[0] != "status")) { usage(); return 2; }
    const auto marker = settings_marker();
    std::filesystem::path config = read_text(marker);
    while (!config.empty() && std::isspace(static_cast<unsigned char>(config.string().back()))) config = config.string().substr(0, config.string().size()-1);
    const bool in_vr = !config.empty();
    if (config.empty()) config = x4_config();
    if (args[0] == "status") { std::cout << (in_vr ? "X4 has its VR settings (" : "X4 has its 2D settings (") << config.string() << ")\n"; return 0; }
    if (config.empty()) return 0; // X4 never started: nothing to keep apart
    for (int i = 0; i < 20 && x4_running(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(500)); // X4 finishing its exit
    if (x4_running()) { std::cerr << "X4 is running: its settings are switched when it isn't.\n"; return 1; }
    const std::filesystem::path two_d = config.string()+".x4vr-2d", vr = config.string()+".x4vr-vr";
    std::error_code error;
    if (args[0] == "vr") {
        if (in_vr) return 0; // already (a launch that didn't get to restore 2D)
        if (!copy_over(config, two_d)) { std::cerr << "x4vr: can't save X4's 2D settings to " << two_d << '\n'; return 1; }
        if (std::filesystem::exists(vr) && !copy_over(vr, config)) { std::cerr << "x4vr: can't load X4's VR settings\n"; return 1; }
        std::filesystem::create_directories(marker.parent_path(), error);
        write_text(marker, config.string()+"\n");
        hud_extension_enabled(config, true);
        std::cout << "x4vr: X4's 2D settings saved (" << two_d.filename().string() << "), VR settings in place\n";
        return 0;
    }
    if (!in_vr) return 0;
    if (!copy_over(config, vr)) { std::cerr << "x4vr: can't save X4's VR settings to " << vr << '\n'; return 1; }
    if (std::filesystem::exists(two_d) && !copy_over(two_d, config)) { std::cerr << "x4vr: can't restore X4's 2D settings\n"; return 1; }
    std::filesystem::remove(marker, error);
    hud_extension_enabled(config, false);
    std::cout << "x4vr: X4's VR settings saved (" << vr.filename().string() << "), 2D settings restored\n";
    return 0;
}
std::string install_desktop() {
    const auto file = desktop_file();
    std::error_code error;
    std::filesystem::create_directories(file.parent_path(), error);
    const auto exec = quoted_path(program);
    const bool ok = write_text(file, "[Desktop Entry]\nType=Application\nName=X4 VR\n"
        "Comment=X4: Foundations in VR: checks, settings and launch\nExec="+exec+"\nTerminal=true\n"
        "Icon=steam_icon_392160\nCategories=Game;\n");
    return ok ? "Desktop entry written: "+file.string()+" (rofi -show drun: \"X4 VR\")" : "Can't write "+file.string();
}

// What the menu shows; the X4 scan (a second on the file) only once.
struct Checks {
    bool steam{}, steamvr{}, x4{}, in_vr{}; // in_vr: X4 has its VR settings (a VR session)
    LaunchOption option;
    double hud{};            // installed HUD factor, 0: none
    bool hud_off_in_x4{};    // installed but switched off in X4's Extensions menu
    int settings_to_fix = -1; // -1: X4's config.xml not found, -2: VR settings made at the first VR launch
    struct Fix { bool required; std::string label, current; };
    std::vector<Fix> to_fix;
    bool desktop{};
};
Checks current_checks() {
    Checks c;
    c.steam = x4vr::steam::steam_running();
    c.steamvr = steamvr_running();
    c.x4 = x4_running();
    c.in_vr = std::filesystem::exists(settings_marker());
    c.option = launch_option();
    if (const auto game = game_dir(); !game.empty()) {
        const auto installed = installed_hud(game/"extensions/x4vr_hud");
        c.hud = installed.count("scale") ? std::atof(installed.at("scale").c_str()) : 0;
    }
    if (const auto config = x4_config(); !config.empty()) {
        bool disabled = false;
        x4vr::launcher::enable_hud_extension(read_text(config.parent_path()/"content.xml"), disabled);
        c.hud_off_in_x4 = c.hud > 0 && disabled && c.in_vr; // off outside VR on purpose
        // config.xml holds the 2D settings except during a VR session; the VR ones are the copy.
        const auto vr = std::filesystem::path(config.string()+".x4vr-vr");
        const auto xml = read_text(c.in_vr ? config : vr);
        c.settings_to_fix = c.in_vr || std::filesystem::exists(vr) ? 0 : -2;
        if (c.settings_to_fix == 0) for (const auto& check : linux_checks(xml)) {
            std::string value;
            const bool present = std::all_of(check.fix.begin(), check.fix.end(), [&](const auto& kv) { return x4vr::launcher::xml_value(xml, kv.first, value); });
            if (check.ok || !present) continue;
            if (check.required) ++c.settings_to_fix;
            c.to_fix.push_back({check.required, check.label, check.current});
        }
    }
    c.desktop = std::filesystem::exists(desktop_file());
    return c;
}
std::string scan_summary(const x4vr::linux_port::code::X4Sites& sites, int& state) {
    const bool all = sites.backward_clamp && sites.onfoot_zeroing && sites.camera_offset && sites.frame_half_global && sites.opentrack_vtable;
    const bool core = sites.opentrack_vtable && sites.frame_half_global;
    state = all ? 0 : core ? 1 : 2;
    return all ? "all VR features supported" : core ? "some features off in this X4 (see Bug report)" : "this X4 build isn't supported";
}

// Runs `body` with std::cout and std::cerr captured (functions shared with the command line
// print); returns what they printed, one entry per line.
std::vector<std::string> captured(const std::function<void()>& body) {
    std::ostringstream text;
    auto* out = std::cout.rdbuf(text.rdbuf());
    auto* err = std::cerr.rdbuf(text.rdbuf());
    try { body(); } catch (const std::exception& e) { text << e.what() << '\n'; }
    std::cout.rdbuf(out);
    std::cerr.rdbuf(err);
    std::vector<std::string> lines;
    std::istringstream in(text.str());
    for (std::string line; std::getline(in, line);) if (!line.empty()) lines.push_back(line);
    return lines;
}

// Launch in VR: SteamVR (started if needed, waiting for it), then the request for x4vr-run and
// `steam -applaunch`. `progress` shows a line; `cancelled` is polled while waiting.
bool launch_vr(const std::function<void(const std::string&)>& progress, const std::function<bool()>& cancelled) {
    if (x4_running()) { progress("X4 is already running."); return false; }
    const auto option = launch_option();
    if (option.state != 1 && !(option.state == 2 && std::getenv("X4VR_ANY_RUN"))) {
        progress(option.accounts == 0 ? "Start X4 once from Steam first, then set its launch option."
                 : option.state == 2 ? "X4's launch option points to another x4vr-run: copy the new one (menu: Copy the launch option)."
                 : "Set X4's launch option first (menu: Copy the launch option).");
        return false;
    }
    if (!steamvr_running()) {
        progress("Starting SteamVR... (put the headset on and connect it)");
        spawn({"steam", "steam://rungameid/250820"});
        for (int i = 0; i < 180 && !steamvr_running(); ++i) {
            if (cancelled()) { progress("Cancelled."); return false; }
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
        if (!steamvr_running()) { progress("SteamVR didn't start within 90 s; start it from Steam and try again."); return false; }
        progress("SteamVR is running.");
    }
    std::error_code error;
    std::filesystem::create_directories(state_dir(), error);
    if (!write_text(state_dir()/"launch.request", std::to_string(std::time(nullptr))+"\n")) { progress("Can't write the launch request."); return false; }
    progress("Starting X4 through Steam... If the headset isn't connected yet, X4 waits up to 2 minutes for it: put it on.");
    spawn({"steam", "-applaunch", std::string(x4vr::steam::x4_app)});
    for (int i = 0; i < 240 && !x4_running(); ++i) {
        if (cancelled()) { progress("Stopped waiting; X4 may still start."); return true; }
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    progress(x4_running() ? "X4 is running in VR. Recentre: Ctrl+F12 or SteamVR's menu." : "X4 didn't start within 2 minutes; check Steam.");
    return x4_running();
}

// Bug report: logs, settings, X4's config.xml and a summary in ~/x4vr-report-<time>.tar.gz.
std::string make_report() {
    setenv("X4VR_NO_STEAMVR_QUERY", "1", 1); // the settings check: no SteamVR client from here
    char stamp[32];
    const auto now = std::time(nullptr);
    std::strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", std::localtime(&now));
    const char* home = std::getenv("HOME");
    const auto out = std::filesystem::path(home ? home : ".")/("x4vr-report-"+std::string(stamp)+".tar.gz");
    const auto staging = std::filesystem::temp_directory_path()/("x4vr-report-"+std::string(stamp));
    std::error_code error;
    std::filesystem::create_directories(staging, error);
    for (const auto* name : {"x4vr.log", "x4vr.previous.log", "stderr.log", "stderr.previous.log", "stereo.txt", "x4_resolution.txt", "pair_stats.txt"})
        std::filesystem::copy_file(state_dir()/name, staging/name, error), error.clear();
    if (const auto config = x4_config(); !config.empty()) std::filesystem::copy_file(config, staging/"x4-config.xml", error), error.clear();
    std::ostringstream summary;
    summary << "x4vr report " << stamp << "\n";
    struct utsname u{};
    if (uname(&u) == 0) summary << "system: " << u.sysname << ' ' << u.release << ' ' << u.machine << "\n";
    const auto c = current_checks();
    summary << "steam running: " << c.steam << "\nsteamvr running: " << c.steamvr << "\nx4 running: " << c.x4
            << "\nlaunch option: " << c.option.value << " (state " << c.option.state << ")\nhud factor: " << c.hud
            << (c.hud_off_in_x4 ? " (off in X4)" : "") << "\nx4 settings to fix: " << c.settings_to_fix << "\n";
    if (const auto game = game_dir(); !game.empty() && std::filesystem::exists(game/"X4")) {
        summary << "x4 scan (" << (game/"X4").string() << "):\n";
        for (const auto& note : x4vr::linux_port::code::find_x4_sites(x4vr::elf::Image::load((game/"X4").string())).notes) summary << "  " << note << "\n";
    }
    write_text(staging/"summary.txt", summary.str());
    const bool ok = run_and_wait({"tar", "czf", out.string(), "-C", staging.string(), "."}, staging.parent_path()/"x4vr-report-tar.log");
    std::filesystem::remove_all(staging, error);
    return ok ? out.string() : std::string();
}
constexpr const char* issues_url = "https://github.com/Cully-Curwen/X4_VR_Linux/issues/new";

// Uninstall: what the mod left, each as a choice.
// The installed program (cmake --install): the mod's files under the prefix this x4vr runs from
// and under ~/.local (the guide's prefix), those that exist. /nix/store is skipped: read-only,
// and a Nix install goes with the source folder's result link.
// `where`: the install folders where some were found.
std::vector<std::filesystem::path> installed_files(std::vector<std::filesystem::path>* where = nullptr) {
    const char* home = std::getenv("HOME");
    std::vector<std::filesystem::path> prefixes{program.parent_path().parent_path(), std::filesystem::path(home ? home : ".")/".local"};
    std::vector<std::filesystem::path> found;
    for (const auto& prefix : prefixes) {
        std::error_code error;
        const auto real = std::filesystem::weakly_canonical(prefix, error);
        if (error || real.string().rfind("/nix/store/", 0) == 0) continue;
        for (const std::filesystem::path& lib : {std::filesystem::path(X4VR_INSTALL_LIBDIR), std::filesystem::path("lib"), std::filesystem::path("lib64")})
            for (const auto& file : {real/"bin/x4vr", real/"bin/x4vr-run", real/"bin/x4vr-probe-run", real/lib/"libx4vr.so",
                                     real/lib/"libx4vr_probe.so", real/lib/"libVkLayer_x4vr_probe.so",
                                     real/"share/vulkan/explicit_layer.d/VkLayer_x4vr.json",
                                     real/"share/vulkan/explicit_layer.d/VkLayer_x4vr_probe.json", real/"share/x4vr"})
                if (std::filesystem::exists(std::filesystem::symlink_status(file, error)) &&
                    std::find(found.begin(), found.end(), file) == found.end()) {
                    found.push_back(file);
                    if (where && std::find(where->begin(), where->end(), real) == where->end()) where->push_back(real);
                }
    }
    return found;
}
// The source folder this was built from (the GitHub clone), if it can be found:
// - a CMake build: the folder compiled in, if still there;
// - a Nix build (compiled in /build/source, gone): the folder holding the `result` link this
//   x4vr runs through, else the one Nix records as a garbage-collector root for this build.
std::filesystem::path source_dir() {
    const auto is_source = [](const std::filesystem::path& dir) { return std::filesystem::exists(dir/"linux/x4vr-run.in"); };
    if (const std::filesystem::path dir = X4VR_SOURCE_DIR; is_source(dir)) return dir;
    for (auto dir = program.parent_path(); dir.has_relative_path(); dir = dir.parent_path())
        if (dir.filename().string().rfind("result", 0) == 0 && is_source(dir.parent_path())) return dir.parent_path();
    std::error_code error;
    const auto store = std::filesystem::canonical(program, error).parent_path().parent_path(); // /nix/store/<hash>-x4vr
    if (error || store.string().rfind("/nix/store/", 0) != 0) return {};
    for (const auto& root : std::filesystem::directory_iterator("/nix/var/nix/gcroots/auto", error)) {
        std::error_code link_error;
        const auto link = std::filesystem::read_symlink(root.path(), link_error); // .../source/result
        if (!link_error && std::filesystem::canonical(link, link_error) == store && is_source(link.parent_path())) return link.parent_path();
    }
    return {};
}
struct Removal { std::string id, label; bool on; std::string help; };
std::vector<Removal> removals() {
    std::vector<Removal> list{
        {"desktop", "Remove the desktop entry", true, desktop_file().string()},
        {"hud", "Remove the HUD distance extension", true, "X4 must be closed. Saves made with it stay flagged as modified."},
        {"restore_x4", "Restore X4's settings from before the mod", false, "config.xml.x4vr-backup, the first copy: also undoes 2D settings changed since."},
        {"x4_copies", "Delete the mod's copies of X4's settings", true, "config.xml.x4vr-2d, -vr and -backup. Your 2D settings stay in config.xml."},
        {"state", "Delete the mod's settings, profiles and logs", true, state_dir().string()},
    };
    std::vector<std::filesystem::path> places;
    const auto files = installed_files(&places);
    std::string where;
    for (const auto& p : places) where += (where.empty() ? "" : ", ")+p.string();
    list.push_back({"program", "Remove the installed program files", !files.empty(),
        files.empty() ? "None found outside /nix/store (a Nix install goes with the GitHub clone directory)."
                      : "x4vr, x4vr-run, the mod and its data ("+std::to_string(files.size())+" found) under "+where+"."});
    return list;
}
std::vector<std::string> uninstall(const std::vector<Removal>& chosen) {
    std::vector<std::string> done;
    const auto on = [&](std::string_view id) { return std::any_of(chosen.begin(), chosen.end(), [&](const auto& r) { return r.id == id && r.on; }); };
    std::error_code error;
    if (on("desktop")) { std::filesystem::remove(desktop_file(), error); done.push_back("Desktop entry removed."); }
    if ((on("hud") || on("restore_x4")) && x4_running()) done.push_back("X4 is running: close it to remove the HUD extension or restore its settings.");
    else {
        if (on("hud")) {
            if (const auto game = game_dir(); !game.empty()) std::filesystem::remove_all(game/"extensions/x4vr_hud", error);
            if (const auto config = x4_config(); !config.empty()) { // and X4's own record of it
                const auto content = config.parent_path()/"content.xml";
                const auto text = read_text(content);
                const auto cleaned = std::regex_replace(text, std::regex(R"re([ \t]*<extension\s+id="x4vr_hud"[^>]*/>[ \t]*\r?\n?)re"), "");
                if (cleaned != text) write_text(content, cleaned);
            }
            done.push_back("HUD distance extension removed.");
        }
        captured([] { settings_mode(std::vector<std::string_view>{"2d"}); }); // a VR session that didn't finish
        if (const auto config = x4_config(); !config.empty()) {
            const auto backup = config.string()+".x4vr-backup";
            if (on("restore_x4") && std::filesystem::exists(backup))
                done.push_back(copy_over(backup, config) ? "X4's settings restored from before the mod." : "Can't restore X4's settings.");
            if (on("x4_copies")) {
                for (const auto* suffix : {".x4vr-backup", ".x4vr-2d", ".x4vr-vr"}) std::filesystem::remove(config.string()+suffix, error);
                done.push_back("The mod's copies of X4's settings deleted.");
            }
        }
    }
    if (on("state")) { std::filesystem::remove_all(state_dir(), error); done.push_back("Mod settings and logs deleted."); }
    if (on("program")) {
        int removed = 0;
        for (const auto& file : installed_files()) removed += int(std::filesystem::remove_all(file, error) > 0);
        done.push_back("Program files removed: "+std::to_string(removed)+".");
    }
    done.push_back("Then clear X4's launch option in Steam and delete the GitHub clone directory (see below).");
    return done;
}

// ---- profiles: sets of VR settings ------------------------------------------------------------
// Built in: share/x4vr/profiles/<name>.txt (installed); the player's own: <state>/profiles/.
// A profile holds stereo.txt lines for the settings the menu shows; loading one writes them to
// stereo.txt. The profile last loaded or saved is named in <state>/profile.
std::filesystem::path data_dir() { return program.parent_path().parent_path()/"share/x4vr"; }
std::filesystem::path user_profiles() { return state_dir()/"profiles"; }
// The settings a profile holds, with the mod's defaults (runtime_bootstrap.hpp) for missing lines.
const std::vector<std::pair<const char*, const char*>> profile_keys{
    {"stereo", "1"}, {"shared_pose", "1"}, {"ipd_scale", "1"}, {"predict", "0.035"}, {"async_submit", "1"}, {"cursor", "1"},
    {"theater", "1"}, {"theater_distance", "2"}, {"theater_width", "2.2"}, {"x4_width", "0"}, {"x4_height", "0"}, {"hud_factor", ""}};
struct Profile { std::string name; bool built_in; std::filesystem::path file; };
std::vector<Profile> profiles() {
    std::vector<Profile> list;
    for (const bool built_in : {true, false}) {
        std::vector<Profile> found;
        std::error_code error;
        for (const auto& entry : std::filesystem::directory_iterator(built_in ? data_dir()/"profiles" : user_profiles(), error))
            if (entry.path().extension() == ".txt") found.push_back({entry.path().stem().string(), built_in, entry.path()});
        std::sort(found.begin(), found.end(), [](const auto& a, const auto& b) { return a.name < b.name; });
        for (auto& p : found)
            if (std::none_of(list.begin(), list.end(), [&](const auto& q) { return q.name == p.name; })) list.push_back(std::move(p));
    }
    return list;
}
std::string current_profile() {
    auto name = read_text(state_dir()/"profile");
    while (!name.empty() && std::isspace(static_cast<unsigned char>(name.back()))) name.pop_back();
    return name;
}
// Whether stereo.txt has what the profile sets (numbers compared as numbers).
bool profile_matches(const Profile& p) {
    for (const auto& [key, fallback] : profile_keys) {
        const auto wanted = x4vr::linux_port::read_setting(p.file, key, "");
        if (wanted.empty()) continue;
        const auto now = x4vr::linux_port::read_setting(settings_file(), key, fallback);
        if (now.empty() || std::fabs(std::atof(now.c_str())-std::atof(wanted.c_str())) > 1e-6) return false;
    }
    return true;
}
bool load_profile(const Profile& p) {
    bool ok = true;
    for (const auto& [key, fallback] : profile_keys)
        if (const auto value = x4vr::linux_port::read_setting(p.file, key, ""); !value.empty())
            ok = x4vr::linux_port::write_setting(settings_file(), key, value) && ok;
    return write_text(state_dir()/"profile", p.name+"\n") && ok;
}
std::string save_profile(const std::string& name) {
    if (name.empty() || name.find_first_of("/\\") != std::string::npos || name[0] == '.') return "Not a usable name: "+name;
    for (const auto& p : profiles()) if (p.built_in && p.name == name) return "\""+name+"\" is built in: pick another name.";
    std::error_code error;
    std::filesystem::create_directories(user_profiles(), error);
    std::string text = "# x4vr profile, saved from the menu\n";
    for (const auto& [key, fallback] : profile_keys)
        if (const auto value = x4vr::linux_port::read_setting(settings_file(), key, fallback); !value.empty()) text += std::string(key)+"="+value+"\n";
    if (!write_text(user_profiles()/(name+".txt"), text)) return "Can't write "+(user_profiles()/(name+".txt")).string();
    write_text(state_dir()/"profile", name+"\n");
    return "Saved as profile \""+name+"\".";
}

// ---- notices: known issues, tips and requests for help, shipped in share/x4vr/notices.txt -----
// One per line, "issue: text", "tip: text" or "help: text"; # starts a comment.
std::vector<std::pair<int, std::string>> notices() {
    std::vector<std::pair<int, std::string>> list;
    std::ifstream in(data_dir()/"notices.txt");
    for (std::string line; std::getline(in, line);) {
        if (line.empty() || line[0] == '#') continue;
        const int kind = line.rfind("tip:", 0) == 0 ? 0 : line.rfind("issue:", 0) == 0 ? 1 : line.rfind("help:", 0) == 0 ? 2 : -1;
        if (kind < 0) continue;
        auto text = line.substr(line.find(':')+1);
        text.erase(0, text.find_first_not_of(' '));
        list.emplace_back(kind, text);
    }
    return list;
}

// The headset's frame rate while X4 runs in VR: the mod's newest pair_stats.txt line (every ~2 s).
std::optional<x4vr::launcher::Stats> headset_stats() {
    const auto path = state_dir()/"pair_stats.txt";
    std::error_code error;
    const auto time = std::filesystem::last_write_time(path, error);
    if (error || std::filesystem::file_time_type::clock::now()-time > std::chrono::seconds(6)) return std::nullopt;
    std::ifstream in(path);
    std::string line, last;
    while (std::getline(in, line)) if (!line.empty()) last = line;
    x4vr::launcher::Stats stats{};
    if (!x4vr::launcher::parse_stats_line(last, stats)) return std::nullopt;
    return stats;
}

// ---- tiling window managers: rules that float the VR window ------------------------------------
// X4 renders at its window's size, and a tiling window manager resizes the window (and honours
// X4's fullscreen request, which shrinks it to the monitor). In VR the window's class is X4VR
// (x4vr-run sets SDL_APP_ID), so a rule can float it and leave 2D (class X4) alone.
struct TilingRule { std::string id, name, where, rule; bool tested; };
std::vector<TilingRule> tiling_rules() {
    return {
        {"hyprland_lua", "Hyprland 0.55+ (Lua config)", "~/.config/hypr/hyprland.lua",
         "hl.window_rule({ name = \"x4-vr\", match = { class = \"^(X4VR)$\" }, float = true, "
         "suppress_event = \"fullscreen maximize\", fullscreen_state = \"0 0\" })", true},
        {"hyprland_conf", "Hyprland (hyprland.conf)", "~/.config/hypr/hyprland.conf",
         "windowrule = float, class:^(X4VR)$\nwindowrule = suppressevent fullscreen maximize, class:^(X4VR)$", false},
        {"sway", "Sway", "~/.config/sway/config", "for_window [class=\"X4VR\"] floating enable, fullscreen disable", false},
        {"i3", "i3", "~/.config/i3/config", "for_window [class=\"X4VR\"] floating enable, fullscreen disable", false},
    };
}
// The tiling window manager this session runs, if one of those: "hyprland", "sway", "i3" or "".
std::string tiling_wm() {
    if (const char* h = std::getenv("HYPRLAND_INSTANCE_SIGNATURE"); h && *h) return "hyprland";
    if (const char* s = std::getenv("SWAYSOCK"); s && *s) return "sway";
    if (const char* i = std::getenv("I3SOCK"); i && *i) return "i3";
    return {};
}
void tiling_screen(x4vr::tui::Terminal& terminal) {
    using namespace x4vr::tui;
    Menu menu(terminal);
    menu.title = "X4 VR  ·  Tiling window manager rules";
    menu.keys = "↑↓ move   Enter copy   Esc back";
    menu.label_width = 60;
    const auto wm = tiling_wm();
    const auto rules = tiling_rules();
    for (;;) {
        std::vector<Item> items{
            info("X4 renders at its window's size. A tiling window manager resizes it (or makes it fullscreen at the monitor's "
                 "size), so the headset gets a smaller image. In VR the window's class is X4VR (2D keeps X4), so a rule can float "
                 "only the VR window at the size the mod sets. Add the rule for yours to its config; it reloads on save."),
        };
        for (const auto& r : rules) {
            const bool here = r.id.rfind(wm.empty() ? "-" : wm, 0) == 0;
            items.push_back(section(r.name+(here ? "  (this session)" : "")+(r.tested ? "" : "  (untested)")));
            items.push_back(info("In "+r.where+":"));
            std::istringstream lines(r.rule);
            for (std::string line; std::getline(lines, line);) items.push_back(info("  "+line));
            items.push_back(action(r.id, "Copy this rule", "Copies the rule above to the clipboard."));
        }
        items.push_back(section("Check"));
        items.push_back(info("Launch in VR, then hyprctl clients (Hyprland) or swaymsg -t get_tree (Sway): the X4VR window should be "
                             "floating, not fullscreen, at the VR resolution. Remove the rule when uninstalling."));
        items.push_back(action("back", "Back"));
        Event e;
        if (!menu.step(items, e)) continue;
        if (e.kind == Event::Back || e.id == "back") return;
        if (e.kind != Event::Activate) continue;
        for (const auto& r : rules)
            if (r.id == e.id) menu.messages = {"Copied ("+copy_to_clipboard(r.rule)+"): paste it into "+r.where+"."};
    }
}

void uninstall_screen(x4vr::tui::Terminal& terminal) {
    using namespace x4vr::tui;
    auto chosen = removals();
    Menu menu(terminal);
    menu.title = "X4 VR  ·  Uninstall";
    menu.keys = "↑↓ move   Space tick   Enter select   Esc back";
    menu.label_width = 48;
    for (;;) {
        std::vector<Item> items{section("Remove what the mod set up")};
        for (const auto& r : chosen) items.push_back(toggle(r.id, r.label, r.on, r.help, {}));
        items.push_back(action("go", "Remove the ticked items", "Your 2D settings and saves stay."));
        items.push_back(action("back", "Back"));
        items.push_back(section("Then, by hand"));
        items.push_back(info("1. In Steam: X4 > Properties > General > Launch options: clear the line."));
        const auto source = source_dir();
        items.push_back(info("2. Delete the GitHub clone directory"+(source.empty() ? std::string(" you built the mod from.")
                                                                                  : ": "+source.string())));
        if (const auto wm = tiling_wm(); !wm.empty())
            items.push_back(info("3. Remove the X4VR window rule from your "+std::string(wm == "hyprland" ? "Hyprland" : wm == "sway" ? "Sway" : "i3")
                                 +" config, if you added one "
                                 "(Setup > Tiling window manager rules shows it)."));
        Event e;
        if (!menu.step(items, e)) continue;
        if (e.kind == Event::Back || (e.kind == Event::Activate && e.id == "back")) return;
        if (e.kind == Event::Changed)
            for (auto& r : chosen) if (r.id == e.id) r.on = e.item.on;
        if (e.kind == Event::Activate && e.id == "go") menu.messages = uninstall(chosen);
    }
}

// README "Set X4's options" as a screen, by where each setting is in X4, with the value X4 has
// in its VR settings (config.xml during a VR session, else the VR copy; the head-tracking factors
// and Protected UI Mode aren't in that file: checked by eye). Linux: windowed, not fullscreen + DSR.
// HUD Scaled: chosen in the menu (stereo.txt hud_factor), else whether the extension is installed.
bool hud_scaled() {
    if (const double wanted = wanted_hud(); wanted >= 0) return wanted > 0;
    const auto game = game_dir();
    return !game.empty() && installed_hud(game/"extensions/x4vr_hud").count("scale") > 0;
}
void checklist_screen(x4vr::tui::Terminal& terminal) {
    using namespace x4vr::tui;
    Menu menu(terminal);
    menu.title = "X4 VR  ·  In-game settings checklist";
    menu.keys = "↑↓ scroll   Esc back";
    menu.timeout_ms = 2000;
    menu.label_width = 38;
    for (;;) {
        const auto config = x4_config();
        const bool in_vr = std::filesystem::exists(settings_marker());
        const auto vr_copy = std::filesystem::path(config.string()+".x4vr-vr");
        const bool have = !config.empty() && (in_vr || std::filesystem::exists(vr_copy));
        const auto xml = have ? read_text(in_vr ? config : vr_copy) : std::string();
        const auto checks = have ? linux_checks(xml) : std::vector<x4vr::launcher::Check>{};
        // One row: from X4's settings file when it has the key, else to check in the game.
        std::vector<Item> items;
        const auto row = [&](const char* label, const char* want, std::initializer_list<const char*> keys, bool required = true) {
            for (const auto& check : checks)
                for (const auto* key : keys)
                    if (!check.fix.empty() && check.fix[0].first == key) {
                        const auto wanted = *want ? std::string(want) : check.label.substr(check.label.rfind(' ')+1);
                        items.push_back(status(label, check.ok ? 0 : required ? 2 : 1,
                            wanted+(check.ok ? "" : "   now: "+check.current+(required ? "" : " (recommended)"))));
                        if (!check.ok && !required) items.back().mark = "·";
                        return;
                    }
            items.push_back(status(label, 3, std::string(*want ? want : "the VR resolution")+(have || keys.size() == 0 ? "" : "   (not known yet)")));
            items.back().mark = "-";
        };
        items.push_back(info(have ? "Key:  ✓ right   ✗ wrong (fixed at the next VR launch)   · recommended   "
                                    "- check by hand in X4 (the mod can't read it)"
                                  : "X4's VR settings are made at the first VR launch; until then check everything by hand in X4."));
        items.push_back(section("Settings > Controls > Head Tracking Support"));
        row("OpenTrack Support", "On", {"enableopentrack"});
        items.push_back(section("Settings > Controls > OpenTrack"));
        row("Head Rotation Factor", "100 %", {});
        row("Head Position Factor", "100 %", {});
        items.push_back(section("Settings > Display"));
        row("Display Mode", "Windowed", {"fullscreen", "borderless"});
        row("Resolution", "", {"res_width"}); // the size checked (menu choice or automatic)
        row("FOV", "maximum (120°)", {"fov"});
        row("Anti-Aliasing", "None or not temporal", {"antialiasing"});
        row("Upscaling (AMD FSR, DLSS)", "Off", {"upmode"});
        row("AMD FSR frame generation", "Off", {"fsr3g"});
        row("DLSS frame generation", "Off", {"dlssg"});
        row("VSync", "Off", {"presentmode"});
        row("Frame Rate Limit", "Off", {"frameratelimit"});
        items.push_back(section("Settings > Graphics (recommended, not required)"));
        row("Chromatic Aberration", "Off", {"chromaticaberration"}, false);
        row("Distortion", "Off", {"distortion"}, false);
        if (hud_scaled()) { // only matters with the HUD extension
            items.push_back(section("Settings > Extensions (HUD Scaled is on)"));
            row("Protected UI Mode", "Off", {});
        }
        items.push_back(action("back", "Back"));
        Event e;
        if (!menu.step(items, e)) continue;
        if (e.kind == Event::Back || e.id == "back") return;
    }
}

// The menu: one screen in sections. Status and the X4 settings list are rebuilt every 2 s.
// Settings tagged "live" are in stereo.txt, re-read by the mod every half second; "next launch"
// ones are applied by x4vr-run when X4 starts in VR (resolution, HUD extension).
int menu(std::string_view start = {}) {
    using namespace x4vr::tui;
    setenv("X4VR_NO_STEAMVR_QUERY", "1", 1); // the settings checks: no SteamVR client from here
    Terminal terminal;
    if (!terminal.ok()) { std::cerr << "x4vr: the menu needs a terminal (run it in one, or use the subcommands: x4vr help)\n"; return 1; }
    if (start == "uninstall") { uninstall_screen(terminal); return 0; }
    const auto path = settings_file();
    if (!std::filesystem::exists(path)) { // first use: the defaults, as x4vr-run would copy them
        std::error_code error;
        std::filesystem::create_directories(path.parent_path(), error);
        std::filesystem::copy_file(data_dir()/"stereo.txt", path, error);
        if (const auto list = profiles(); !list.empty() && list[0].built_in) load_profile(list[0]);
    }
    Menu menu(terminal);
    menu.title = "X4 VR for Linux";
    menu.keys = "↑↓ move   Enter select   Space tick   ←→ change";
    menu.timeout_ms = 2000; // live status (SteamVR, X4, headset)
    {
        std::vector<Item> waiting{info("Checking X4...")};
        Event e;
        menu.timeout_ms = 0;
        menu.step(waiting, e);
        menu.timeout_ms = 2000;
    }
    x4vr::linux_port::code::X4Sites sites;
    bool scanned = false;
    if (const auto game = game_dir(); !game.empty() && std::filesystem::exists(game/"X4")) {
        try { sites = x4vr::linux_port::code::find_x4_sites(x4vr::elf::Image::load((game/"X4").string())); scanned = true; } catch (...) {}
    }
    const auto board = notices();
    const auto get = [&](const char* key, const char* fallback) { return x4vr::linux_port::read_setting(path, key, fallback); };
    const auto num = [&](const char* key, const char* fallback) { return std::atof(get(key, fallback).c_str()); };
    static constexpr std::pair<int, int> modes[] = {{1920, 1080}, {2560, 1440}, {2880, 1620}, {3200, 1800}, {3840, 2160}};
    const std::string live = "live", next = "next launch";
    bool custom_resolution = false; // "Custom" picked: width and height shown even if they match a listed size
    for (;;) {
        const auto c = current_checks();
        int scan_state = 3;
        const auto scan_text = scanned ? scan_summary(sites, scan_state) : std::string("X4 not found (set X4VR_GAME_DIR)");
        const double wanted_hud_factor = wanted_hud();
        const double hud_factor = wanted_hud_factor >= 0 ? wanted_hud_factor : c.hud;
        std::vector<Item> items;

        items.push_back(section("Status"));
        items.push_back(status("SteamVR", c.steamvr ? 0 : 3, c.steamvr ? "running" : "not running (Launch starts it)"));
        items.push_back(status("X4 build", scan_state, scan_text));
        items.push_back(status("Steam launch option", c.option.state == 1 ? 0 : 2,
            c.option.state == 1 ? "set" : c.option.state == 2 ? "points to another x4vr-run: copy it again (Setup)"
            : c.option.state == 3 ? "set to something else: "+c.option.value : c.option.accounts ? "not set: copy it (Setup)" : "start X4 once from Steam first"));
        items.push_back(status("X4 settings for VR", c.settings_to_fix < 0 ? 3 : c.to_fix.empty() ? 0 : 1,
            c.settings_to_fix == -1 ? "X4's config.xml not found (start X4 once)" : c.settings_to_fix == -2 ? "made at the first VR launch"
            : c.to_fix.empty() ? "ready (your 2D settings are kept apart)" : std::to_string(c.to_fix.size())+" fixed at the next VR launch (list below)"));
        if (c.hud_off_in_x4) items.push_back(status("HUD extension", 1, "turned off in X4's Extensions menu: turn on \"X4 VR HUD distance\""));
        if (!c.x4) items.push_back(status("X4", 3, std::string("not running")+(c.in_vr ? " (VR settings still in place: restored at the next start)" : "")));
        else if (!c.in_vr) items.push_back(status("X4", 3, "running in 2D"));
        else if (const auto stats = headset_stats())
            items.push_back(status("X4 in VR", stats->late || stats->repeated > 2 ? 1 : 0, std::to_string(stats->fps)+" fps per eye pair, "
                                   +std::to_string(stats->late)+" late, "+std::to_string(stats->repeated)+" repeated (last 2 s)"));
        else items.push_back(status("X4 in VR", 1, "running, no frames reaching the headset yet"));

        items.push_back(section("Play"));
        if (!c.x4) items.push_back(action("launch", "Launch X4 in VR",
            "Starts SteamVR if needed, then X4 through Steam with the mod. Steam's own Play button starts the normal 2D game."));
        else if (c.in_vr) {
            items.push_back(action("recenter", "Recentre the view  (Ctrl+F12)", "Look straight ahead first. Same as Ctrl+F12 in X4, or SteamVR's recentre.", live));
            items.push_back(action("flat", "Flat screen on / automatic  (Ctrl+F11)", "Same as Ctrl+F11 in X4: the game on a flat screen in front of you.", live));
        } else items.push_back(info("X4 is running in 2D: quit it to launch in VR."));

        if (!board.empty()) {
            items.push_back(section("Notices"));
            for (const auto& [kind, text] : board) items.push_back(notice(kind, text));
        }

        items.push_back(section("Settings"));
        items.push_back(info("live: applies at once, also while X4 runs.   next launch: applied when X4 next starts in VR."));
        {
            const auto list = profiles();
            const auto name = current_profile();
            std::vector<std::string> names;
            int at = -1;
            for (size_t i = 0; i < list.size(); ++i) {
                const bool current = list[i].name == name;
                if (current) at = int(i);
                names.push_back(list[i].name+(list[i].built_in ? " (built in)" : "")+(current && !profile_matches(list[i]) ? " · changed" : ""));
            }
            if (at < 0) { names.insert(names.begin(), "(none)"); at = 0; }
            items.push_back(choice("profile", "Profile", names, at,
                "←→ loads another set of the settings below. Built in: Steam Frame. Change settings, then \"Save as profile\" to keep them.", live));
            items.push_back(action("profile_save", "Save as profile", "Saves the settings below under a name of your choice ("+user_profiles().string()+").", {}));
            if (at < int(list.size()) && name == list[size_t(at)].name && !list[size_t(at)].built_in)
                items.push_back(action("profile_delete", "Delete profile \""+name+"\"", "Your settings stay as they are.", {}));
        }
        items.push_back(status("VR runtime", 3, "SteamVR (OpenVR)"));
        items.push_back(toggle("stereo", "3D (stereo)", num("stereo", "1") != 0,
            "Each eye gets its own image. Off: both eyes see the same flat image (for comparing, or if 3D feels wrong).", live));
        items.push_back(toggle("shared_pose", "Shared pose per eye pair", num("shared_pose", "1") != 0,
            "Both eyes of a pair are submitted with one head pose. Keep on for the Steam Frame: without it the right eye ghosts.", live));
        items.push_back(number("ipd_scale", "World scale", num("ipd_scale", "1"), 0.05, 0.3, 3, 2, "",
            "The distance between your eyes in the game. Below 1 the world looks bigger, above 1 smaller. 1 is real size.", live));
        items.push_back(number("predict", "Head prediction", num("predict", "0.035")*1000, 1, 0, 100, 0, "ms",
            "How far ahead the head pose is predicted for X4. Higher: less lag when turning your head, but more wobble.", live));
        items.push_back(toggle("async_submit", "Stutter protection", num("async_submit", "1") != 0,
            "Sends an image to SteamVR every headset frame; a late game frame repeats the last image instead of a flash. Keep it on: "
            "off is for troubleshooting only, and loses the flat screen (menus) and the Steam Frame's shared pose.", next));
        items.push_back(toggle("cursor", "Mouse cursor in VR", num("cursor", "1") != 0, "Draws the mouse cursor in the headset.", live));
        {
            const int theater = int(num("theater", "1"));
            items.push_back(choice("theater", "Flat screen", {"automatic", "always", "never"}, theater == 2 ? 1 : theater == 0 ? 2 : 0,
                "Automatic: menus, the map and views without ship controls go to a flat screen in front of you (Ctrl+F11 switches).", live));
        }
        items.push_back(number("theater_distance", "Flat screen distance", num("theater_distance", "2"), 0.25, 0.5, 10, 2, "m",
            "How far in front of you the flat screen stands.", live));
        items.push_back(number("theater_width", "Flat screen width", num("theater_width", "2.2"), 0.1, 0.5, 10, 2, "m",
            "The flat screen's width; its height follows X4's image.", live));
        items.push_back(toggle("hud_on", "HUD Scaled", hud_factor > 0,
            "X4's cockpit HUD sits a hand's width from your face. On: an extension moves it back at the same apparent size, in VR "
            "only. X4 counts as modified then (saves flagged); turn Protected UI Mode off in X4's Extension Settings.", next));
        if (hud_factor > 0)
            items.push_back(number("hud_factor", "HUD Scale Ratio", hud_factor, 0.1, 1, 6, 1, "",
                "How many times farther the HUD is, from 1.0 to 6.0 (2.5 is a good start). Enter to type a value.", next));
        {
            const int w = int(num("x4_width", "0")), h = int(num("x4_height", "0"));
            std::vector<std::string> names{"automatic"};
            int at = 0;
            for (const auto& [mw, mh] : modes) { names.push_back(std::to_string(mw)+"x"+std::to_string(mh)); if (mw == w && mh == h) at = int(names.size())-1; }
            if (w <= 0 || h <= 0) custom_resolution = false;         // automatic, e.g. after loading a profile
            else if (at == 0) custom_resolution = true;               // a size not in the list
            names.push_back(custom_resolution ? "Custom: "+std::to_string(w)+"x"+std::to_string(h) : "Custom");
            if (custom_resolution) at = int(names.size())-1;
            items.push_back(choice("resolution", "X4 resolution in VR", names, at,
                "Automatic: the smallest 16:9 size that covers what SteamVR renders (no wasted pixels). Lower is faster, higher "
                "sharper. Custom: any width and height; the image keeps X4's vertical view and any aspect works.", next));
            if (custom_resolution) {
                items.push_back(number("x4_width", "Resolution Width", w, 16, 640, 16384, 0, "px",
                    "X4's image width in VR. Enter to type it.", next));
                items.push_back(number("x4_height", "Resolution Height", h, 16, 360, 16384, 0, "px",
                    "X4's image height in VR. Enter to type it.", next));
            }
        }

        items.push_back(section("X4 settings for VR"));
        items.push_back(action("checklist", "In-game settings checklist",
            "Every X4 setting VR needs, by where it is in X4's settings, with what X4 has now. For troubleshooting."));
        if (!c.to_fix.empty()) {
            items.push_back(info("VR uses its own copy of X4's settings; these are fixed in it at the next VR launch. Your 2D settings aren't touched."));
            for (const auto& fix : c.to_fix)
                items.push_back(status(fix.label, fix.required ? 2 : 1, std::string(fix.required ? "required" : "recommended")+(fix.current.empty() ? "" : ", now: "+fix.current)));
        }

        items.push_back(section("Setup"));
        items.push_back(action("option_copy", "Copy the Steam launch option",
            "Paste it in Steam: X4 > Properties > General > Launch options. Steam's Play still starts the normal game."));
        items.push_back(action("tiling", "Tiling window manager rules",
            "Hyprland, Sway, i3: a rule that floats the VR window (class X4VR) so X4 keeps the VR resolution. 2D isn't affected."));
        if (!c.desktop) items.push_back(action("desktop", "Add to the app launcher", "\"X4 VR\" in your desktop's app menu, rofi or wofi: "+desktop_file().string()));
        items.push_back(action("report", "Make a bug report", "Packs logs, settings and a summary into ~/x4vr-report-<time>.tar.gz, to attach to a GitHub issue."));
        items.push_back(action("uninstall", "Uninstall", "Removes the desktop entry, HUD extension, the mod's copies of X4's settings, and its settings."));
        items.push_back(action("quit", "Quit"));

        Event e;
        if (!menu.step(items, e)) continue;
        if (e.kind == Event::Back) continue; // Esc doesn't close the menu: only Quit does
        if (e.id == "quit") return 0;
        if (e.kind == Event::Changed) {
            const auto& item = e.item;
            std::string key = e.id, value;
            if (e.id == "profile") {
                const auto list = profiles();
                const size_t index = size_t(item.choice)-(current_profile().empty() || std::none_of(list.begin(), list.end(), [](const auto& p) { return p.name == current_profile(); }) ? 1 : 0);
                if (index < list.size()) menu.messages = {load_profile(list[index]) ? "Profile \""+list[index].name+"\" loaded." : "Can't write "+path.string()};
                continue;
            }
            if (e.id == "resolution") {
                int w = 0, h = 0;
                custom_resolution = item.choice == int(item.choices.size())-1;
                if (custom_resolution) { // start from the size in use: the choice before, else SteamVR's last
                    w = int(num("x4_width", "0")); h = int(num("x4_height", "0"));
                    if (w <= 0 || h <= 0) {
                        std::ifstream saved(state_dir()/"x4_resolution.txt");
                        if (!(saved >> w) || saved.get() != 'x' || !(saved >> h) || w <= 0 || h <= 0) { w = 2560; h = 1440; }
                    }
                } else if (item.choice > 0) std::sscanf(item.choices[size_t(item.choice)].c_str(), "%dx%d", &w, &h);
                const bool ok = x4vr::linux_port::write_setting(path, "x4_width", std::to_string(w)) && x4vr::linux_port::write_setting(path, "x4_height", std::to_string(h));
                if (!ok) menu.messages = {"Can't write "+path.string()};
                continue;
            }
            if (e.id == "hud_on") { key = "hud_factor"; value = item.on ? format(c.hud > 0 ? c.hud : 2.5, 1) : "0"; }
            else if (item.kind == Item::Toggle) value = item.on ? "1" : "0";
            else if (e.id == "theater") value = item.choice == 0 ? "1" : item.choice == 1 ? "2" : "0";
            else if (e.id == "predict") value = format(item.number/1000, 3);
            else value = format(item.number, item.decimals);
            if (!x4vr::linux_port::write_setting(path, key, value)) menu.messages = {"Can't write "+path.string()};
            continue;
        }
        if (e.kind != Event::Activate) continue;
        if (e.id == "launch") {
            std::vector<Item> progress_items{section("Launch X4 in VR"), info("Esc: stop waiting")};
            Menu progress(terminal);
            progress.title = menu.title;
            progress.keys = "Esc stop waiting";
            progress.timeout_ms = 0;
            std::vector<std::string> lines;
            const auto show = [&](const std::string& line) { lines.push_back(line); progress.messages = lines; Event ignored; progress.step(progress_items, ignored); };
            const auto stop = [&] { return terminal.key(0) == Escape; };
            launch_vr(show, stop); // x4vr-run switches X4 to its VR settings, fixes them and applies the HUD distance
            menu.messages = lines;
        } else if (e.id == "recenter" || e.id == "flat") {
            const int now = x4vr::linux_port::control_settings(path, e.id);
            menu.messages = {now < 0 ? "Can't update "+path.string() : e.id == "recenter" ? "Recentred." : now == 2 ? "Flat screen on." : "Flat screen automatic."};
        } else if (e.id == "profile_save") {
            auto name = current_profile();
            for (const auto& p : profiles()) if (p.built_in && p.name == name) name.clear();
            if (const auto typed = menu.prompt(items, "Profile name", name)) menu.messages = {save_profile(*typed)};
        } else if (e.id == "profile_delete") {
            std::error_code error;
            const auto name = current_profile();
            std::filesystem::remove(user_profiles()/(name+".txt"), error);
            std::filesystem::remove(state_dir()/"profile", error);
            menu.messages = {"Profile \""+name+"\" deleted; your settings stay as they are."};
        } else if (e.id == "option_copy") {
            const auto how = copy_to_clipboard(wanted_launch_option());
            menu.messages = {"Copied (" + how + "): " + wanted_launch_option(), launch_option_steps()};
        } else if (e.id == "desktop") menu.messages = {install_desktop()};
        else if (e.id == "report") {
            const auto file = make_report();
            menu.messages = file.empty() ? std::vector<std::string>{"Couldn't write the report (tar missing?)."}
                                         : std::vector<std::string>{"Report: "+file, "Attach it to a new issue: "+std::string(issues_url)};
        } else if (e.id == "uninstall") uninstall_screen(terminal);
        else if (e.id == "checklist") checklist_screen(terminal);
        else if (e.id == "tiling") tiling_screen(terminal);
    }
}
// The menu's actions as commands, for scripts.
int launch_command() {
    return launch_vr([](const std::string& line) { std::cout << line << '\n'; }, [] { return false; }) ? 0 : 1;
}
int launch_option_command(const std::vector<std::string_view>& args) {
    if (args.size() != 1) { usage(); return 2; }
    if (args[0] == "status") {
        const auto o = launch_option();
        std::cout << (o.accounts == 0 ? "No Steam account here has started X4 yet.\n"
                      : o.state == 1 ? "Set: "+o.value+"\n" : o.state == 0 ? "Not set.\n" : "Set to: "+o.value+" (wanted: "+wanted_launch_option()+")\n");
        return o.state == 1 ? 0 : 1;
    }
    if (args[0] != "copy") { usage(); return 2; }
    const auto how = copy_to_clipboard(wanted_launch_option());
    std::cout << "Copied (" << how << "): " << wanted_launch_option() << '\n' << launch_option_steps() << '\n';
    return 0;
}
}

// The X4 scan the mod runs at startup (code_scan.hpp), on the executable file.
int patterns(const std::vector<std::string_view>& args) {
    if (args.size() > 1) { usage(); return 2; }
    const std::filesystem::path path = args.empty() ? game_dir()/"X4" : std::filesystem::path(args[0]);
    if (path.empty() || !std::filesystem::exists(path)) { std::cerr << "X4 not found; give its path\n"; return 1; }
    const auto sites = x4vr::linux_port::code::find_x4_sites(x4vr::elf::Image::load(path.string()));
    std::cout << path.string() << ":\n";
    for (const auto& note : sites.notes) std::cout << "  " << note << '\n';
    const bool all = sites.backward_clamp && sites.onfoot_zeroing && sites.camera_offset && sites.frame_half_global && sites.opentrack_vtable;
    std::cout << (all ? "All found: the mod supports this X4.\n" : "Some not found: those features stay off in this X4.\n");
    return all ? 0 : 1;
}

int main(int argc, char** argv) {
    { // this executable as started, unresolved, so a symlinked install path stays as given
        std::error_code error;
        const std::string first = argc > 0 ? argv[0] : "";
        if (first.find('/') != std::string::npos) program = std::filesystem::absolute(first, error);
        else if (const char* path = std::getenv("PATH")) {
            std::istringstream dirs(path);
            for (std::string dir; std::getline(dirs, dir, ':');)
                if (!dir.empty() && access((std::filesystem::path(dir)/first).c_str(), X_OK) == 0) { program = std::filesystem::path(dir)/first; break; }
        }
        if (program.empty()) program = std::filesystem::read_symlink("/proc/self/exe", error);
    }
    if (argc < 2) return menu();
    const std::string_view command = argv[1];
    const std::vector<std::string_view> args(argv+2, argv+argc);
    try {
        if (command == "menu") return menu();
        if (command == "uninstall") return menu("uninstall");
        if (command == "launch" && args.empty()) return launch_command();
        if (command == "launch-option") return launch_option_command(args);
        if (command == "settings-mode") return settings_mode(args);
        if (command == "install-desktop" && args.empty()) { std::cout << install_desktop() << '\n'; return 0; }
        if (command == "report" && args.empty()) {
            const auto file = make_report();
            if (file.empty()) { std::cerr << "Couldn't write the report (tar missing?)\n"; return 1; }
            std::cout << "Report: " << file << "\nAttach it to a new issue: " << issues_url << '\n';
            return 0;
        }
        if (command == "vr-check") return vr_check(args);
        if (command == "udp-send") return udp_send(args);
        if (command == "elf-classes") return elf_classes(args);
        if (command == "ctl") return ctl(args);
        if (command == "hud") return hud(args);
        if (command == "game-grep") return game_grep(args);
        if (command == "patterns") return patterns(args);
        if (command == "check" && args.empty()) return check_settings(false, false);
        if (command == "fix-settings" && args.size() <= 1 && (args.empty() || args[0] == "--auto"))
            return check_settings(true, !args.empty());
        if (command == "help" || command == "--help" || command == "-h") { usage(); return 0; }
        usage();
        return 2;
    } catch (const std::exception& error) {
        std::cerr << "x4vr " << command << ": " << error.what() << '\n';
        return 1;
    }
}
