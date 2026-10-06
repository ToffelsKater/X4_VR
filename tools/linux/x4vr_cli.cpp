// x4vr: the Linux port's command-line tool. Subcommands so far are the Phase 0 measurements of
// docs/LINUX_PORT_PLAN.md; settings, HUD and report commands join them later.
#include "elf_classes.hpp"
#include "../launcher/hud_mod.hpp"
#include "../launcher/launcher_settings.hpp"
#include "md5.hpp"
#include "settings_control.hpp"
#include "opentrack.hpp"
#include <x4vr/session.hpp>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <mutex>
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
        "Usage: x4vr <command> [options]\n"
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
        "      files, as the Windows launcher does. X4 must be closed. --refresh rebuilds it after a\n"
        "      game update (x4vr-run does that before every start). Game folder: $X4VR_GAME_DIR, else\n"
        "      Steam's default library. X4 then counts as modified (no online features; saves made\n"
        "      with it stay flagged).\n"
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
// Linux X4 keeps one config per Steam account under ~/.config/EgoSoft/X4/<id>/; use the newest.
std::filesystem::path x4_config() {
    const char* home = std::getenv("HOME");
    const auto base = std::filesystem::path(home ? home : ".")/".config/EgoSoft/X4";
    std::filesystem::path best;
    std::filesystem::file_time_type newest{};
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator(base, error)) {
        const auto config = entry.path()/"config.xml";
        const auto time = std::filesystem::last_write_time(config, error);
        if (!error && (best.empty() || time > newest)) { best = config; newest = time; }
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
// doesn't render pixels SteamVR throws away. X4VR_RESOLUTION=WxH sets it, =0 leaves it alone.
std::filesystem::path settings_file();
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
    const char* home = std::getenv("HOME");
    const std::filesystem::path h = home ? home : ".";
    for (const auto& candidate : {h/".local/share/Steam/steamapps/common/X4 Foundations", h/".steam/steam/steamapps/common/X4 Foundations"})
        if (std::filesystem::exists(candidate/"01.cat")) return candidate;
    return {};
}
// Same files as the Windows launcher. Known limit (docs/LINUX_FINDINGS.md): X4 9.00 loads the
// scripts' precompiled .xpl copies, so the size factors don't apply and the HUD moves back but shrinks.
std::map<std::string, std::string> hud_originals(const std::filesystem::path& game) {
    std::set<std::string> paths(x4vr::launcher::hud_anchor_files().begin(), x4vr::launcher::hud_anchor_files().end());
    paths.insert(x4vr::launcher::hud_scripts().begin(), x4vr::launcher::hud_scripts().end());
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
    const auto files = x4vr::launcher::hud_files(originals, scale, error);
    if (files.empty()) return false;
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
int hud(const std::vector<std::string_view>& args) {
    if (args.size() != 1) { usage(); return 2; }
    const auto game = game_dir();
    const bool refresh = args[0] == "--refresh";
    if (game.empty()) {
        if (refresh) return 0;
        std::cerr << "X4's game folder not found: set X4VR_GAME_DIR to the folder with 01.cat.\n";
        return 1;
    }
    const auto extension = game/"extensions/x4vr_hud";
    const auto installed = installed_hud(extension);
    const double installed_scale = installed.count("scale") ? std::atof(installed.at("scale").c_str()) : 0;
    if (args[0] == "status") {
        if (installed_scale > 0) std::cout << "HUD distance mod installed: factor " << installed.at("scale") << " (" << extension.string() << ")\n";
        else std::cout << "HUD distance mod not installed (X4's default HUD distance).\n";
        return 0;
    }
    if (x4_running()) { std::cerr << "X4 is running: close it first (it loads extensions at startup).\n"; return refresh ? 0 : 1; }
    std::error_code ignored;
    if (refresh) { // after a game update the mod would replace new game files with old copies
        if (installed_scale <= 0 || (installed.count("source") && installed.at("source") == source_hash(hud_originals(game)))) return 0;
        std::string error;
        if (install_hud(game, installed_scale, error)) { std::cout << "x4vr: HUD distance mod rebuilt for the updated game files\n"; return 0; }
        std::filesystem::remove_all(extension, ignored);
        std::cout << "x4vr: HUD distance mod removed: it no longer matches this X4 version (" << error << ")\n";
        return 0;
    }
    if (args[0] == "remove") { std::filesystem::remove_all(extension, ignored); std::cout << "HUD distance mod removed.\n"; return 0; }
    double scale = 0;
    if (!parse_number(args[0], scale) || scale < 1 || scale > 6) { std::cerr << "HUD distance: use a factor between 1 and 6 (2.5 is a good start).\n"; return 2; }
    std::string error;
    if (!install_hud(game, scale, error)) { std::cerr << "Could not build the HUD mod: " << error << '\n'; return 1; }
    std::cout << "HUD distance mod installed: factor " << x4vr::launcher::format_number(scale) << " (" << extension.string() << ")\n";
    std::cout << "X4 will report a modified game: online features are off, and saves made with the mod stay flagged.\n";
    std::cout << "Known limit on Linux X4 9.00: the HUD moves back but also looks smaller (X4 keeps its own size factors).\n";
    // X4's per-user content.xml (next to config.xml) remembers extensions turned off in its menu.
    if (const auto config = x4_config(); !config.empty()) {
        const auto content_path = config.parent_path()/"content.xml";
        bool disabled = false;
        const auto content = x4vr::launcher::enable_hud_extension(read_text(content_path), disabled);
        if (disabled && !write_text(content_path, content))
            std::cout << "X4 has it turned off: turn on \"X4 VR HUD distance\" in X4's Extensions menu.\n";
    }
    return 0;
}

// ---- game-grep --------------------------------------------------------------------------------
int game_grep(const std::vector<std::string_view>& args) {
    if (args.empty() || args.size() > 2) { usage(); return 2; }
    const auto game = game_dir();
    if (game.empty()) { std::cerr << "X4's game folder not found: set X4VR_GAME_DIR to the folder with 01.cat.\n"; return 1; }
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
}

int main(int argc, char** argv) {
    if (argc < 2) { usage(); return 2; }
    const std::string_view command = argv[1];
    const std::vector<std::string_view> args(argv+2, argv+argc);
    try {
        if (command == "vr-check") return vr_check(args);
        if (command == "udp-send") return udp_send(args);
        if (command == "elf-classes") return elf_classes(args);
        if (command == "ctl") return ctl(args);
        if (command == "hud") return hud(args);
        if (command == "game-grep") return game_grep(args);
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
