#pragma once
// X4's settings for 2D and for VR, kept apart: launching in VR saves config.xml as
// config.xml.x4vr-2d and puts the VR copy (config.xml.x4vr-vr, if any) in its place; when X4
// exits, config.xml goes back to x4vr-vr and the 2D copy returns. So Steam's Play keeps the
// player's own settings, and VR keeps its own (x4vr-run, then fix-settings on top). A marker
// (x4-settings.vr: the config path) survives a crash: the next start of either kind restores 2D.
// The -2d copy is only the player's 2D settings while the marker exists; afterwards it is an old one.
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>

namespace x4vr::linux_port {
// Replaces `to` with a copy of `from` atomically (a temporary, then a rename).
inline bool copy_over(const std::filesystem::path& from, const std::filesystem::path& to) {
    std::error_code error;
    const auto temporary = to.string()+".x4vr-tmp";
    std::filesystem::copy_file(from, temporary, std::filesystem::copy_options::overwrite_existing, error);
    if (!error) std::filesystem::rename(temporary, to, error);
    return !error;
}
// The config the marker names (X4 has its VR settings), else empty.
inline std::filesystem::path config_in_vr(const std::filesystem::path& marker) {
    std::ifstream in(marker);
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) text.pop_back();
    return text;
}
struct Switched { bool ok = true, changed = false; std::string message; };
inline std::string file_text(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}
// Puts X4's VR settings (to_vr) or its 2D ones in place. `config`: X4's config.xml when not in VR
// (in VR the marker names it). X4 must not be running: it rewrites config.xml when it exits.
// Ordered so that a crash between two steps never loses the 2D settings: the marker is written
// before the VR copy goes in place, and removed only once the 2D settings are back.
inline Switched switch_settings(const std::filesystem::path& marker, std::filesystem::path config, bool to_vr) {
    const auto in_vr = config_in_vr(marker);
    if (!in_vr.empty()) config = in_vr;
    if (config.empty()) return {}; // X4 never started: nothing to keep apart
    const std::filesystem::path two_d = config.string()+".x4vr-2d", vr = config.string()+".x4vr-vr";
    std::error_code error;
    if (to_vr) {
        if (!in_vr.empty()) return {}; // already (a launch that didn't get to restore 2D)
        if (!copy_over(config, two_d)) return {false, false, "can't save X4's 2D settings to "+two_d.string()};
        std::filesystem::create_directories(marker.parent_path(), error);
        if (!(std::ofstream(marker) << config.string() << '\n')) {
            std::filesystem::remove(marker, error);
            return {false, false, "can't write "+marker.string()};
        }
        if (std::filesystem::exists(vr) && !copy_over(vr, config)) {
            std::filesystem::remove(marker, error); // config.xml still has the 2D settings
            return {false, false, "can't load X4's VR settings"};
        }
        return {true, true, "X4's 2D settings saved ("+two_d.filename().string()+"), VR settings in place"};
    }
    if (in_vr.empty()) return {};
    // Already back (a run stopped before the marker went): saving config.xml as the VR copy would
    // overwrite the VR settings with the 2D ones.
    const bool restored = std::filesystem::exists(two_d) && file_text(two_d) == file_text(config);
    if (!restored && !copy_over(config, vr)) return {false, false, "can't save X4's VR settings to "+vr.string()};
    if (!restored && std::filesystem::exists(two_d) && !copy_over(two_d, config)) return {false, false, "can't restore X4's 2D settings"};
    std::filesystem::remove(marker, error);
    return {true, true, "X4's VR settings saved ("+vr.filename().string()+"), 2D settings restored"};
}

// The HUD distance extension only in VR: X4 loads every directory in extensions/, so in 2D it is
// moved next to that directory (`parked`, which X4 doesn't read) and back for VR. A rename on the
// same disk: all or nothing. X4's own per-user content.xml can't do it: X4 writes that file only
// once its Extensions menu was used. True once it is where it should be (or there is none).
inline bool place_hud(const std::filesystem::path& extension, const std::filesystem::path& parked, bool in_vr) {
    const auto& from = in_vr ? parked : extension;
    const auto& to = in_vr ? extension : parked;
    std::error_code error;
    if (!std::filesystem::exists(from, error)) return true;
    std::filesystem::remove_all(to, error); // an older copy left at the destination
    std::filesystem::rename(from, to, error);
    return !error;
}
}
