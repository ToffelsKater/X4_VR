#pragma once
// The two live controls (Windows: Ctrl+F12 / Ctrl+F11), as edits of stereo.txt, which the mod
// re-reads every half second: "recenter" bumps the recenter counter; "flat" toggles theater
// between 2 (always the virtual screen) and 1 (automatic). Used by `x4vr ctl`, the mod's hotkeys
// and SteamVR's recentre.
#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>
#include <unistd.h>

namespace x4vr::linux_port {
// One key of stereo.txt (`key=value` lines), or `fallback` if it isn't set.
inline std::string read_setting(const std::filesystem::path& path, std::string_view key, std::string fallback) {
    std::ifstream in(path);
    const std::string prefix = std::string(key)+"=";
    for (std::string line; std::getline(in, line);)
        if (line.rfind(prefix, 0) == 0) return line.substr(prefix.size());
    return fallback;
}
// Sets one key (added at the end if missing), replacing the file atomically: the mod reads it
// concurrently. False if it can't be written.
inline bool write_setting(const std::filesystem::path& path, std::string_view key, const std::string& value) {
    std::vector<std::string> lines;
    {
        std::ifstream in(path);
        for (std::string line; std::getline(in, line);) lines.push_back(line);
    }
    const std::string prefix = std::string(key)+"=";
    auto found = std::find_if(lines.begin(), lines.end(), [&](const std::string& l) { return l.rfind(prefix, 0) == 0; });
    if (found == lines.end()) lines.push_back(prefix+value);
    else *found = prefix+value;
    // Own temporary per thread: the menu and the mod (hotkeys, SteamVR's recentre) may write at once.
    const auto temporary = path.string()+".tmp"+std::to_string(getpid())+"-"+std::to_string(gettid());
    {
        std::ofstream out(temporary, std::ios::trunc);
        for (const auto& line : lines) out << line << '\n';
        if (!out) return false;
    }
    std::error_code error;
    std::filesystem::rename(temporary, path, error);
    return !error;
}
// Returns the new value, or -1 if the file can't be read or written.
inline int control_settings(const std::filesystem::path& path, std::string_view action) {
    if (!std::filesystem::exists(path)) return -1;
    const bool recenter = action == "recenter";
    const char* key = recenter ? "recenter" : "theater";
    const int current = std::atoi(read_setting(path, key, recenter ? "0" : "1").c_str());
    const int next = recenter ? current+1 : (current == 2 ? 1 : 2);
    return write_setting(path, key, std::to_string(next)) ? next : -1;
}
}
