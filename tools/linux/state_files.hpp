#pragma once
// The files the mod keeps in its state directory (~/.local/state/x4vr, or $X4VR_DIR), by name, for
// uninstall. Only these are removed, then the directory if that left it empty: $X4VR_DIR may point
// anywhere (even $HOME), so the directory itself is never removed with what else is in it.
#include <filesystem>
#include <string_view>
#include <system_error>
#include <vector>

namespace x4vr::linux_port {
// Written by the mod, x4vr-run or the menu. Profiles are the *.txt files in profiles/.
inline constexpr std::string_view state_file_names[]{
    "stereo.txt", "x4vr.log", "x4vr.previous.log", "stderr.log", "stderr.previous.log", "x4_resolution.txt",
    "pair_stats.txt", "submit_trace.txt", "trace.txt", "eye-0.raw", "eye-1.raw", "eye-dump.txt", "profile",
    "launch.request", "trace.request", "submit.request", "dump.txt", "x4-settings.vr"};

// Removes the mod's files from `dir` (and its stereo.txt.tmp* leftovers and profiles), then `dir`
// and profiles/ if they are empty. Returns how many files went.
inline int remove_state_files(const std::filesystem::path& dir) {
    std::error_code error;
    if (dir.empty() || !std::filesystem::is_directory(dir, error)) return 0;
    int removed = 0;
    const auto remove = [&](const std::filesystem::path& file) {
        const auto status = std::filesystem::symlink_status(file, error);
        if (!error && (std::filesystem::is_regular_file(status) || std::filesystem::is_symlink(status)))
            removed += std::filesystem::remove(file, error);
    };
    const auto matching = [&](const std::filesystem::path& in, auto&& wanted) { // listed first, removed after
        std::vector<std::filesystem::path> found;
        for (const auto& entry : std::filesystem::directory_iterator(in, error)) if (wanted(entry.path())) found.push_back(entry.path());
        return found;
    };
    for (const auto name : state_file_names) remove(dir/name);
    for (const auto& file : matching(dir, [](const auto& p) { return p.filename().string().rfind("stereo.txt.tmp", 0) == 0; })) remove(file);
    const auto profiles = dir/"profiles";
    if (std::filesystem::is_directory(std::filesystem::symlink_status(profiles, error))) {
        for (const auto& file : matching(profiles, [](const auto& p) { return p.extension() == ".txt"; })) remove(file);
        std::filesystem::remove(profiles, error); // only if empty
    }
    if (std::filesystem::is_directory(std::filesystem::symlink_status(dir, error))) std::filesystem::remove(dir, error); // only if empty
    return removed;
}
}
