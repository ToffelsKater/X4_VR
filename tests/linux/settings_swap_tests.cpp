// X4's 2D and VR settings kept apart (tools/linux/settings_swap.hpp), in a temporary directory:
// VR launch and exit, a second VR session, a crash in VR, and a switch back with no session.
#include "settings_swap.hpp"
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>

namespace {
int failures = 0;
void check(bool ok, const char* what) { if (!ok) { std::printf("FAIL: %s\n", what); ++failures; } }
void write(const std::filesystem::path& file, const std::string& text) { std::ofstream(file) << text; }
std::string read(const std::filesystem::path& file) {
    std::ifstream in(file);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}
}

int main() {
    namespace fs = std::filesystem;
    using x4vr::linux_port::switch_settings;
    using x4vr::linux_port::config_in_vr;
    char pattern[] = "/tmp/x4vr-swap-test-XXXXXX";
    const fs::path root = mkdtemp(pattern);
    const auto config = root/"x4/config.xml", marker = root/"state/x4-settings.vr";
    const fs::path two_d = config.string()+".x4vr-2d", vr = config.string()+".x4vr-vr";
    fs::create_directories(config.parent_path());

    check(switch_settings(marker, {}, true).ok && !fs::exists(marker), "X4 never started: nothing to do");
    write(config, "2D");
    check(switch_settings(marker, config, false).ok && read(config) == "2D", "back to 2D without a session: nothing to do");

    // First VR launch: no VR copy yet, so X4 starts from the 2D settings (fix-settings then fixes them).
    auto done = switch_settings(marker, config, true);
    check(done.ok && done.changed, "first VR launch");
    check(read(two_d) == "2D" && read(config) == "2D" && config_in_vr(marker) == config, "2D saved, marker names the config");
    write(config, "VR"); // fix-settings, and X4 writing its settings when it exits
    done = switch_settings(marker, config, false);
    check(done.ok && done.changed, "X4 exits");
    check(read(config) == "2D" && read(vr) == "VR" && !fs::exists(marker), "2D back, VR kept, marker gone");

    // The player changes 2D settings in between; the next VR session keeps them.
    write(config, "2D changed");
    check(switch_settings(marker, config, true).ok && read(config) == "VR" && read(two_d) == "2D changed", "second VR launch");
    write(config, "VR changed");
    check(switch_settings(marker, config, false).ok && read(config) == "2D changed" && read(vr) == "VR changed", "second exit");

    // A crash in VR: the marker stays. A second VR launch doesn't save the VR settings as 2D...
    check(switch_settings(marker, config, true).ok && fs::exists(marker), "VR launch");
    check(switch_settings(marker, config, true).ok && read(two_d) == "2D changed", "VR launch after a crash: 2D copy untouched");
    // ...and the next start of either kind puts 2D back, even if X4's config path is unknown now.
    check(switch_settings(marker, {}, false).ok && read(config) == "2D changed" && !fs::exists(marker), "2D back after the crash");

    // The 2D copy is old once no session runs: switching back must not copy it over newer settings.
    write(config, "2D newest");
    check(switch_settings(marker, config, false).ok && read(config) == "2D newest", "an old 2D copy isn't restored");

    // A 2D copy that can't be written: VR isn't put in place, and no marker.
    fs::remove(two_d);
    fs::create_directory(two_d); // a directory where the copy goes
    done = switch_settings(marker, config, true);
    check(!done.ok && read(config) == "2D newest" && !fs::exists(marker), "2D copy failed: nothing switched");

    // A run stopped after the 2D settings were back but before the marker went: the next switch to
    // 2D only removes the marker, and doesn't overwrite the VR copy with the 2D settings.
    fs::remove_all(two_d);
    write(config, "2D");
    check(switch_settings(marker, config, true).ok, "VR launch");
    write(config, "VR 3");
    write(vr, "VR 3");
    write(config, "2D"); // as switching back does, then the stop
    check(switch_settings(marker, config, false).ok && read(config) == "2D" && read(vr) == "VR 3" && !fs::exists(marker),
          "stopped before the marker went: VR copy kept");

    // VR settings that can't be loaded: the marker goes again, X4 keeps its 2D settings.
    fs::remove(vr);
    fs::create_directory(vr);
    done = switch_settings(marker, config, true);
    check(!done.ok && read(config) == "2D" && !fs::exists(marker), "VR copy unreadable: back to 2D, no marker");

    // The HUD extension: aside for 2D, back for VR, an older copy replaced, nothing to move.
    using x4vr::linux_port::place_hud;
    const auto extension = root/"game/extensions/x4vr_hud", parked = root/"game/x4vr_hud.off";
    check(place_hud(extension, parked, false) && place_hud(extension, parked, true), "no extension: nothing to do");
    fs::create_directories(extension);
    write(extension/"x4vr_hud.txt", "scale=2.5");
    check(place_hud(extension, parked, false) && !fs::exists(extension) && read(parked/"x4vr_hud.txt") == "scale=2.5", "2D: moved aside");
    check(place_hud(extension, parked, false) && !fs::exists(extension), "2D again: stays aside");
    check(place_hud(extension, parked, true) && !fs::exists(parked) && read(extension/"x4vr_hud.txt") == "scale=2.5", "VR: back in place");
    fs::create_directories(parked);
    write(parked/"x4vr_hud.txt", "scale=1.5"); // an older copy aside
    check(place_hud(extension, parked, false) && read(parked/"x4vr_hud.txt") == "scale=2.5", "2D: the older copy replaced");

    fs::remove_all(root);
    if (failures) return 1;
    std::printf("settings swap: all checks passed\n");
    return 0;
}
