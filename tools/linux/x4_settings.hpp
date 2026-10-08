#pragma once
// X4's VR settings on Linux: the checks (launcher_settings.hpp's check_x4, adapted), and which of
// them x4vr writes. Used by the menu's status and in-game checklist, and `x4vr fix-settings`, which
// x4vr-run runs before each VR launch.
// - Required settings are written with their VR value at every VR launch, also when config.xml
//   doesn't have them yet: X4 only writes a setting once it's changed from its default (issue #7:
//   OpenTrack Support was missing, so off, and X4 never read the headset's pose).
// - Optional ones (Check::required false) are only shown: the player decides.
// - A missing setting is only added under a key name seen in a Linux X4 config.xml (known_keys).
//   One not confirmed yet is still fixed where config.xml has it, as then its name is certain,
//   but never added: a wrong guess would only clutter config.xml and look fixed when it isn't.
#include "../launcher/launcher_settings.hpp"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iterator>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace x4vr::linux_port {
inline constexpr std::string_view known_keys[]{
    "fullscreen", "borderless", "res_width", "res_height", "fov", "antialiasing", "upmode", "presentmode",
    "frameratelimit", "enableopentrack", "chromaticaberration", "distortion", "dlssg", "fsr3g",
    "opentrackanglefactor", "opentrackpositionfactor", "uisafemode"};
inline bool known_key(std::string_view key) {
    return std::find(std::begin(known_keys), std::end(known_keys), key) != std::end(known_keys);
}

// OpenTrack's Head Rotation and Head Position Factors at 100 %: the mod sends the headset's pose
// 1:1. X4 saves them once changed in the game, as opentrack<anglefactor|positionfactor> (its
// "%sanglefactor" with the tracker's prefix, as tobiianglefactor).
inline std::vector<launcher::Check> opentrack_factor_checks(const std::string& xml) {
    std::vector<launcher::Check> checks;
    for (const auto& [label, key] : {std::pair{"OpenTrack head rotation factor 100 %", "opentrackanglefactor"},
                                     std::pair{"OpenTrack head position factor 100 %", "opentrackpositionfactor"}}) {
        std::string value;
        const bool set = launcher::xml_value(xml, key, value);
        const double factor = std::atof(value.c_str());
        checks.push_back({label, true, set && std::fabs(factor-1.0) < 0.005,
                          set ? std::to_string(static_cast<int>(std::lround(factor*100)))+" %" : "(missing)", {{key, "1.00"}}});
    }
    return checks;
}

// The checks on Linux: Windows' minus its fullscreen rule (Linux X4 renders fullscreen and
// borderless windows at the desktop size whatever its resolution setting says; only a window keeps
// it), plus a windowed display (always) and OpenTrack's factors. Each required setting must have
// exactly the value VR needs (FOV 1.3333), anti-aliasing any but temporal; the frame rate limit is
// a warning. Anything missing from config.xml is "not set": wrong, or for optional ones and the
// warning, shown. `width`, `height`: the VR resolution, 0
// when it isn't known. A required setting missing from config.xml is wrong, whatever X4's default
// would be: "not set", and written at the next VR launch.
// Three kinds of setting: required (written at each VR launch), optional (shown, the player
// decides) and warning (shown with why it matters, never changed: some players want it anyway).
enum class Kind { required, optional, warning };
// The frame rate limit: the mod paces X4 to the headset, so a limit adds nothing, and one under
// 180 fps holds back pair mode.
inline constexpr std::string_view frame_limit_reason = "VR is paced to the headset; a limit under 180 fps holds back pair mode";
inline Kind kind(const launcher::Check& check) {
    if (check.required) return Kind::required;
    return !check.fix.empty() && check.fix[0].first == "frameratelimit" ? Kind::warning : Kind::optional;
}

// Anti-aliasing: any kind but temporal (TAA), which blends in frames from the other eye; X4's
// "none" (off) when it is.
inline bool antialiasing_ok(const std::string& value) {
    return !value.empty() && value.find("taa") == std::string::npos && value.find("temporal") == std::string::npos;
}

// `hud_scaled`: the HUD distance extension is on, which needs X4's Protected UI Mode (uisafemode)
// off; otherwise that isn't checked.
inline std::vector<launcher::Check> linux_checks(const std::string& xml, int width, int height, bool hud_scaled = false) {
    auto checks = launcher::check_x4(xml, width, height);
    std::erase_if(checks, [](const auto& c) { return c.label.rfind("Display mode", 0) == 0; });
    std::string fullscreen = "(missing)", borderless = "(missing)";
    launcher::xml_value(xml, "fullscreen", fullscreen);
    launcher::xml_value(xml, "borderless", borderless);
    checks.push_back({"Display mode: windowed", true, fullscreen == "false" && borderless == "false",
                      "fullscreen "+fullscreen+", borderless "+borderless, {{"fullscreen", "false"}, {"borderless", "false"}}});
    for (auto& check : opentrack_factor_checks(xml)) checks.push_back(std::move(check));
    if (hud_scaled) {
        std::string mode = "(missing)";
        launcher::xml_value(xml, "uisafemode", mode);
        checks.push_back({"Protected UI Mode off (HUD Scaled)", true, mode == "false", mode == "true" ? "on" : mode, {{"uisafemode", "false"}}});
    }
    for (auto& check : checks) {
        std::string value;
        const bool missing = std::any_of(check.fix.begin(), check.fix.end(), [&](const auto& kv) { return !launcher::xml_value(xml, kv.first, value); });
        // Exactly the value VR needs, where check_x4 (shared with Windows) accepts a range.
        if (!missing && check.fix.size() == 1) {
            launcher::xml_value(xml, check.fix[0].first, value);
            if (check.fix[0].first == "fov") check.ok = std::fabs(std::atof(value.c_str())-1.3333) < 0.00005;
            else if (check.fix[0].first == "antialiasing") check.ok = antialiasing_ok(value);
        }
        if (!check.fix.empty() && check.fix[0].first == "frameratelimit") { // a warning: off is best, on is the player's call
            check.required = false;
            if (!missing) check.ok = value == "false";
        }
        if (!missing) continue;
        check.ok = false;
        for (size_t at; (at = check.current.find("(missing)")) != std::string::npos;) check.current.replace(at, 9, "not set");
    }
    return checks;
}

// Written at a VR launch: required, wrong (missing included), and every key it needs is in
// config.xml or known.
inline bool to_write(const launcher::Check& check, const std::string& xml) {
    if (!check.required || check.ok) return false;
    std::string value;
    return std::all_of(check.fix.begin(), check.fix.end(),
                       [&](const auto& kv) { return launcher::xml_value(xml, kv.first, value) || known_key(kv.first); });
}
// Writes the checks' VR values into config.xml (the to_write ones): also those that pass but are
// missing (launcher_settings.hpp's fix_x4, for Windows, writes only failing ones).
inline std::string write_settings(std::string xml, const std::vector<launcher::Check>& checks) {
    for (const auto& check : checks)
        for (const auto& [key, value] : check.fix) xml = launcher::xml_set(xml, key, value);
    return xml;
}
}
