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
    const auto temporary = path.string()+".tmp";
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
    std::ifstream in(path);
    if (!in) return -1;
    std::vector<std::string> lines;
    for (std::string line; std::getline(in, line);) lines.push_back(line);
    in.close();
    const bool recenter = action == "recenter";
    const std::string key = recenter ? "recenter=" : "theater=";
    auto found = std::find_if(lines.begin(), lines.end(), [&](const std::string& l) { return l.rfind(key, 0) == 0; });
    const int current = found == lines.end() ? (recenter ? 0 : 1) : std::atoi(found->c_str()+key.size());
    const int next = recenter ? current+1 : (current == 2 ? 1 : 2);
    if (found == lines.end()) lines.push_back(key+std::to_string(next));
    else *found = key+std::to_string(next);
    const auto temporary = path.string()+".tmp"; // replaced atomically: the mod reads it concurrently
    {
        std::ofstream out(temporary, std::ios::trunc);
        for (const auto& line : lines) out << line << '\n';
        if (!out) return -1;
    }
    std::error_code error;
    std::filesystem::rename(temporary, path, error);
    return error ? -1 : next;
}
}
