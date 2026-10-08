#pragma once
// What the bug report (x4vr_cli.cpp, make_report) reads from files: Steam's and SteamVR's versions
// from their manifests, the step the mod's log stops at, GPU vendors. Kept apart from the menu so
// the tests can feed them text.
#include "steam_config.hpp"
#include <cctype>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace x4vr::report {
inline constexpr std::string_view steamvr_app = "250820";

// The value at `path` (keys, case-insensitive) in a VDF file, e.g. {"AppState", "buildid"}.
inline std::optional<std::string> vdf_value(const std::string& vdf, const std::vector<std::string_view>& path) {
    const auto tokens = steam::tokenize(vdf);
    std::vector<std::string> stack; // keys of the open blocks
    const auto on_path = [&] {
        if (stack.size()+1 != path.size()) return false;
        for (size_t i = 0; i < stack.size(); ++i) if (!steam::same_key(stack[i], path[i])) return false;
        return true;
    };
    for (size_t i = 0; i < tokens.size(); ++i) {
        const auto& t = tokens[i];
        if (t.kind == steam::Token::Close) { if (!stack.empty()) stack.pop_back(); continue; }
        if (t.kind != steam::Token::String || i+1 >= tokens.size()) continue;
        if (tokens[i+1].kind == steam::Token::Open) { stack.push_back(t.text); ++i; continue; }
        if (tokens[i+1].kind == steam::Token::String) {
            if (on_path() && steam::same_key(t.text, path.back())) return tokens[i+1].text;
            ++i;
        }
    }
    return std::nullopt;
}

// SteamVR from its appmanifest_250820.acf: build and beta branch ("public" when none).
inline std::string steamvr_version(const std::string& manifest) {
    if (manifest.empty()) return "not found";
    const auto build = vdf_value(manifest, {"AppState", "buildid"});
    auto branch = vdf_value(manifest, {"AppState", "MountedConfig", "BetaKey"});
    if (!branch) branch = vdf_value(manifest, {"AppState", "UserConfig", "BetaKey"});
    return "build "+build.value_or("?")+", branch "+(branch && !branch->empty() ? *branch : std::string("public"));
}
// Steam's client from package/steam_client_*.manifest ("version") and package/beta (the beta's
// name; no file: the stable client).
inline std::string steam_client_version(const std::string& manifest, const std::string& beta) {
    std::string version = "?";
    const auto tokens = steam::tokenize(manifest);
    for (size_t i = 0; i+1 < tokens.size(); ++i)
        if (tokens[i].kind == steam::Token::String && tokens[i+1].kind == steam::Token::String && steam::same_key(tokens[i].text, "version")) {
            version = tokens[i+1].text;
            break;
        }
    std::string name = beta;
    while (!name.empty() && (name.back() == '\n' || name.back() == '\r' || name.back() == ' ')) name.pop_back();
    return "version "+version+", "+(name.empty() ? std::string("stable") : "beta "+name);
}

// The latest run in the mod's log (from the last "== ... x4vr-run" line): the bootstrap step it
// stopped in, if the last bootstrap line is one that hadn't finished ("begin", "still running",
// "waits"); nothing when it finished, or the run has no bootstrap lines.
struct LastRun {
    std::optional<std::string> unfinished; // the bootstrap line it stopped at
    std::optional<std::string> exit;       // "X4 exited with status N", when it did
};
inline LastRun last_run(const std::string& log) {
    std::vector<std::string> lines;
    std::istringstream in(log);
    for (std::string line; std::getline(in, line);) {
        if (line.rfind("== ", 0) == 0 && line.find(" x4vr-run") != std::string::npos) lines.clear();
        lines.push_back(line);
    }
    LastRun run;
    std::string last;
    for (const auto& line : lines) {
        if (line.rfind("X4VR bootstrap: ", 0) == 0) last = line;
        if (const auto at = line.find("X4 exited with status"); line.rfind("== ", 0) == 0 && at != std::string::npos) run.exit = line.substr(at);
    }
    const auto has = [&](std::string_view text) { return last.find(text) != std::string::npos; };
    const bool begun = last.size() >= 6 && last.compare(last.size()-6, 6, " begin") == 0; // before the thread was logged
    if (!last.empty() && (begun || has(" begin (") || has(" still running after ") || has(" waits (")))
        run.unfinished = last;
    return run;
}

// Lines in SteamVR's logs that point at the Steam Frame's streaming link (vrlink) rather than the
// mod: Valve's "Error 17" (shown as "vrlink: incompatible protocol settings on host (17)"; its fix
// needs SteamOS 0.4.3+ on the Frame and SteamVR 2.18.2+), the link timing out, the Wi-Fi dongle
// dropping, the Frame's driver not loading, VR_Init errors. The last `limit` matches. Left out:
// what every SteamVR start logs before the headset connects, and other headsets' drivers missing.
inline std::vector<std::string> link_problems(const std::string& log, size_t limit = 8) {
    static constexpr std::string_view signs[]{"incompatible protocol", "error 17", "(17)", "Connection inactive", "DEAUTH",
                                              "Unable to load driver", "VRInitError_", "WaitVRConnectionReady"};
    std::vector<std::string> found;
    std::istringstream in(log);
    for (std::string line; std::getline(in, line);) {
        // SteamVR's start before the headset connects, and drivers for other headsets: normal.
        if (line.find("Allowing driver load failure") != std::string::npos || line.find("No connected devices found") != std::string::npos ||
            ((line.find("Unable to load driver") != std::string::npos || line.find("Unable to init watchdog") != std::string::npos ||
              line.find("when initing driver") != std::string::npos) && line.find("vrlink") == std::string::npos))
            continue;
        std::string lower = line;
        for (auto& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        for (const auto sign : signs) {
            std::string wanted(sign);
            for (auto& c : wanted) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            const bool code = sign == "(17)"; // only with the link's name: "(17)" alone is too common
            if (lower.find(wanted) != std::string::npos && (!code || lower.find("vrlink") != std::string::npos)) {
                found.push_back(line.size() > 300 ? line.substr(0, 300)+"..." : line);
                break;
            }
        }
    }
    if (found.size() > limit) found.erase(found.begin(), found.end()-static_cast<std::ptrdiff_t>(limit));
    return found;
}
// From SteamVR's server log (logs/vrserver.txt): its version ("vrserver 2.17.10 startup ...") and
// the headset, from the driver's lines (Steam Frame: "vrlink: ManufacturerName Valve", "vrlink:
// ModelNumber Deckard MP"; others log "Model Number: ..."). The last of each: the newest session.
// The serial number SteamVR logs alongside is left out.
struct SteamVrLog { std::string version, headset; };
inline SteamVrLog steamvr_log(const std::string& log) {
    SteamVrLog result;
    std::string maker, model;
    std::istringstream in(log);
    const auto after = [](const std::string& line, std::string_view key) -> std::optional<std::string> {
        const auto at = line.find(key);
        if (at == std::string::npos) return std::nullopt;
        auto value = line.substr(at+key.size());
        if (const auto end = value.find(", Serial"); end != std::string::npos) value.resize(end);
        while (!value.empty() && (value.back() == '\r' || value.back() == ' ')) value.pop_back();
        return value;
    };
    for (std::string line; std::getline(in, line);) {
        if (const auto v = after(line, " - vrserver "); v && v->find(" startup") != std::string::npos) result.version = v->substr(0, v->find(" startup"));
        if (const auto v = after(line, "ManufacturerName "); v && !v->empty()) maker = *v;
        if (const auto v = after(line, "Model Number: "); v && !v->empty()) model = *v;
        else if (const auto v2 = after(line, "ModelNumber "); v2 && !v2->empty()) model = *v2;
    }
    result.headset = maker.empty() || model.rfind(maker, 0) == 0 ? model : model.empty() ? std::string() : maker+" "+model;
    return result;
}
// PRETTY_NAME from /etc/os-release (quotes removed), else NAME, else "".
inline std::string os_name(const std::string& os_release) {
    std::istringstream in(os_release);
    std::string pretty, name;
    for (std::string line; std::getline(in, line);) {
        const auto value = [&](size_t from) {
            auto v = line.substr(from);
            if (v.size() >= 2 && (v.front() == '"' || v.front() == '\'') && v.back() == v.front()) v = v.substr(1, v.size()-2);
            return v;
        };
        if (line.rfind("PRETTY_NAME=", 0) == 0) pretty = value(12);
        else if (line.rfind("NAME=", 0) == 0) name = value(5);
    }
    return pretty.empty() ? name : pretty;
}

// /proc/cpuinfo: the CPU's model and how many threads ("13th Gen Intel(R) Core(TM) i7-13700K, 24 threads").
inline std::string cpu(const std::string& cpuinfo) {
    std::istringstream in(cpuinfo);
    std::string model;
    int threads = 0;
    for (std::string line; std::getline(in, line);) {
        if (line.rfind("processor", 0) == 0) ++threads;
        if (model.empty() && line.rfind("model name", 0) == 0) {
            const auto colon = line.find(':');
            if (colon != std::string::npos) model = line.substr(line.find_first_not_of(" \t", colon+1));
        }
    }
    if (model.empty()) return threads ? std::to_string(threads)+" threads" : std::string();
    return model+(threads ? ", "+std::to_string(threads)+" threads" : std::string());
}
// /proc/meminfo's MemTotal in GiB ("62.5 GiB").
inline std::string memory(const std::string& meminfo) {
    std::istringstream in(meminfo);
    for (std::string line; std::getline(in, line);)
        if (line.rfind("MemTotal:", 0) == 0) {
            char text[32];
            std::snprintf(text, sizeof text, "%.1f GiB", std::atof(line.c_str()+9)/(1024.0*1024.0));
            return text;
        }
    return {};
}
// The desktop's compositor or window manager among the running processes (their names), and
// gamescope and Xwayland when they run: X4's window goes through Xwayland on a Wayland desktop.
inline std::string compositor(const std::vector<std::string>& processes) {
    static constexpr std::string_view known[]{"kwin_wayland", "kwin_x11", "gnome-shell", "Hyprland", "sway", "niri", "river",
                                              "wayfire", "labwc", "weston", "cosmic-comp", "xfwm4", "marco", "muffin", "i3",
                                              "bspwm", "openbox", "awesome", "picom", "gamescope", "Xwayland"};
    std::string found;
    for (const auto name : known)
        for (const auto& p : processes)
            if (p == name) { found += (found.empty() ? "" : ", ")+std::string(name); break; }
    return found;
}
// X4's compatibility tool from Steam's config/config.vdf (InstallConfigStore > Software > Valve >
// Steam > CompatToolMapping > 392160 > name), empty when none: X4 then runs natively. A Proton
// one runs the Windows X4, where the Linux mod doesn't apply.
inline std::string x4_compat_tool(const std::string& config_vdf) {
    return vdf_value(config_vdf, {"InstallConfigStore", "Software", "Valve", "Steam", "CompatToolMapping", steam::x4_app, "name"}).value_or("");
}

// A PCI vendor id from sysfs ("0x10de") as a name.
inline std::string gpu_vendor(std::string id) {
    while (!id.empty() && (id.back() == '\n' || id.back() == ' ')) id.pop_back();
    if (id == "0x10de") return "NVIDIA";
    if (id == "0x1002") return "AMD";
    if (id == "0x8086") return "Intel";
    return id.empty() ? "?" : id;
}
}
