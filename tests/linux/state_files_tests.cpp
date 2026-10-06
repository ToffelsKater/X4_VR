// Uninstall's removal of the mod's state directory (tools/linux/state_files.hpp): only the mod's
// own files go, whatever directory X4VR_DIR names (here a fake home directory).
#include "state_files.hpp"
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>

namespace {
int failures = 0;
void check(bool ok, const char* what) { if (!ok) { std::printf("FAIL: %s\n", what); ++failures; } }
void touch(const std::filesystem::path& file) { std::filesystem::create_directories(file.parent_path()); std::ofstream(file) << "x\n"; }
bool present(const std::filesystem::path& p) { return std::filesystem::exists(std::filesystem::symlink_status(p)); }
}

int main() {
    namespace fs = std::filesystem;
    char pattern[] = "/tmp/x4vr-state-test-XXXXXX";
    const fs::path root = mkdtemp(pattern);
    using x4vr::linux_port::remove_state_files;

    // X4VR_DIR=$HOME: a home directory with the mod's files among the player's own.
    const auto home = root/"home";
    for (const auto* own : {"stereo.txt", "x4vr.log", "stderr.log", "x4-settings.vr", "launch.request", "stereo.txt.tmp123-4",
                            "profile", "profiles/Mine.txt"}) touch(home/own);
    for (const auto* theirs : {"notes.txt", "Documents/letter.txt", ".bashrc", "profiles/readme.md", "stereo.txt.bak", "x4vr.log.old"})
        touch(home/theirs);
    fs::create_directories(root/"elsewhere");
    touch(root/"elsewhere/kept.txt");
    fs::create_symlink(root/"elsewhere", home/"link");
    check(remove_state_files(home) == 8, "the mod's 8 files removed");
    check(!present(home/"stereo.txt") && !present(home/"profiles/Mine.txt") && !present(home/"stereo.txt.tmp123-4"), "the mod's files gone");
    for (const auto* theirs : {"notes.txt", "Documents/letter.txt", ".bashrc", "profiles/readme.md", "stereo.txt.bak", "x4vr.log.old"})
        check(present(home/theirs), "the player's file kept");
    check(present(home/"profiles"), "profiles/ kept while it holds other files");
    check(present(home) && present(home/"link") && present(root/"elsewhere/kept.txt"), "the directory and links kept");

    // The usual state directory: empty afterwards, so it goes.
    const auto state = root/"state/x4vr";
    for (const auto* own : {"stereo.txt", "x4vr.log", "pair_stats.txt", "profiles/A.txt"}) touch(state/own);
    check(remove_state_files(state) == 4, "state files removed");
    check(!present(state), "the empty state directory removed");
    check(present(root/"state"), "its parent kept");

    // Nothing to do: empty path, a missing directory, a file.
    check(remove_state_files({}) == 0, "empty path");
    check(remove_state_files(root/"missing") == 0, "missing directory");
    touch(root/"file");
    check(remove_state_files(root/"file") == 0 && present(root/"file"), "a file, not a directory");

    fs::remove_all(root);
    if (failures) return 1;
    std::printf("state files: all checks passed\n");
    return 0;
}
