#pragma once
// Launcher logic without Win32: stereo.txt profiles and X4's config.xml checks.
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

namespace x4vr::launcher {

// Ordered key=value lines (stereo.txt and profile files share the format).
using Settings = std::vector<std::pair<std::string, std::string>>;

inline Settings parse_settings(const std::string& text) {
    Settings out;
    size_t start = 0;
    while (start < text.size()) {
        auto end = text.find('\n', start);
        if (end == std::string::npos) end = text.size();
        auto line = text.substr(start, end-start);
        start = end+1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const auto split = line.find('=');
        if (line.empty() || line[0] == '#' || split == std::string::npos) continue;
        out.emplace_back(line.substr(0, split), line.substr(split+1));
    }
    return out;
}
inline std::string format_settings(const Settings& settings) {
    std::string out;
    for (const auto& [key, value] : settings) out += key+'='+value+'\n';
    return out;
}
inline std::string get(const Settings& settings, const std::string& key, const std::string& fallback = {}) {
    for (const auto& [k, v] : settings) if (k == key) return v;
    return fallback;
}
inline void set(Settings& settings, const std::string& key, const std::string& value) {
    for (auto& [k, v] : settings) if (k == key) { v = value; return; }
    settings.emplace_back(key, value);
}

// What a profile holds. Everything else in stereo.txt is X4 calibration from config/stereo.txt.
// x4_width/x4_height are the launcher's own (the resolution the X4 check expects). runtime (openvr
// or openxr) is read by the game at startup from X4VR_RUNTIME; stereo.txt keeps it for bug reports.
inline const std::vector<std::string>& profile_keys() {
    static const std::vector<std::string> keys{"stereo", "pair", "ipd_scale", "predict", "async_submit", "external_vr", "shared_pose", "runtime", "x4_width", "x4_height"};
    return keys;
}
inline Settings profile_of(const Settings& settings) {
    Settings out;
    for (const auto& key : profile_keys()) {
        const auto value = get(settings, key);
        if (!value.empty()) out.emplace_back(key, value);
    }
    return out;
}
// Live stereo.txt: calibrated defaults, the profile's VR keys on top, recenter counter kept.
inline Settings compose_live(const Settings& defaults, const Settings& profile, const std::string& recenter) {
    auto out = defaults;
    for (const auto& [key, value] : profile) if (key.rfind("x4_", 0) != 0) set(out, key, value);
    if (!recenter.empty()) set(out, "recenter", recenter);
    return out;
}

inline bool uses_openxr(const Settings& profile) { return get(profile, "runtime") == "openxr"; }

// X4's config.xml is flat <key>value</key> lines under <root>.
inline bool xml_value(const std::string& xml, const std::string& key, std::string& value) {
    const auto open = "<"+key+">", close = "</"+key+">";
    const auto begin = xml.find(open);
    if (begin == std::string::npos) return false;
    const auto end = xml.find(close, begin);
    if (end == std::string::npos) return false;
    value = xml.substr(begin+open.size(), end-begin-open.size());
    return true;
}
inline std::string xml_set(std::string xml, const std::string& key, const std::string& value) {
    const auto open = "<"+key+">", close = "</"+key+">";
    const auto begin = xml.find(open);
    const auto end = begin == std::string::npos ? std::string::npos : xml.find(close, begin);
    if (end != std::string::npos) return xml.replace(begin+open.size(), end-begin-open.size(), value);
    const auto root = xml.rfind("</root>");
    if (root == std::string::npos) return xml;
    return xml.insert(root, "  "+open+value+close+"\n");
}

struct Check {
    std::string label;
    bool required; // false: recommended only
    bool ok;
    std::string current;
    std::vector<std::pair<std::string, std::string>> fix; // key/value pairs that make it pass
};
// VR requirements from README "In-game settings". width/height 0 = resolution not checked.
inline std::vector<Check> check_x4(const std::string& xml, int width, int height) {
    std::vector<Check> checks;
    const auto value = [&](const std::string& key) { std::string v; return xml_value(xml, key, v) ? v : std::string("(missing)"); };
    // absent_ok: the setting doesn't exist in older X4 versions, so it can't be on there.
    const auto equals = [&](const char* label, const char* key, const char* want, bool required, bool absent_ok = false) {
        const auto v = value(key);
        checks.push_back({label, required, v == want || (absent_ok && v == "(missing)"), v, {{key, want}}});
    };
    equals("Display mode: Fullscreen", "fullscreen", "true", true);
    equals("Display mode: not borderless", "borderless", "false", true);
    const auto fov = value("fov");
    checks.push_back({"FOV at maximum (120\xC2\xB0)", true, std::atof(fov.c_str()) >= 1.333, fov, {{"fov", "1.3333"}}});
    const auto aa = value("antialiasing");
    checks.push_back({"Anti-aliasing not temporal", true, aa.find("taa") == std::string::npos && aa.find("temporal") == std::string::npos,
                      aa, {{"antialiasing", "none"}}});
    // DLSS upscaling is allowed: tested up to Ultra Performance (2026-09-27) without the eyes
    // mixing. Frame generation inserts frames between two different eyes, so it stays off.
    equals("DLSS frame generation off", "dlssg", "off", true, true);
    equals("AMD FSR frame generation off", "fsr3g", "off", true, true); // new in 9.00
    equals("Upscaling off", "upmode", "none", true);
    equals("VSync off", "presentmode", "immediate", true);
    // The mod paces X4 to the headset, so a cap at or above 90 does nothing in alternate mode
    // (measured 2026-09-29: cap 90 vs off, same frame times), and in pair mode any cap under
    // 180 fps defeats the mode. Off is the one value right for both.
    const auto limited = value("frameratelimit");
    checks.push_back({"Frame rate limit off", true, limited != "true",
                      limited == "true" ? value("frameratetarget")+" fps" : limited, {{"frameratelimit", "false"}}});
    equals("OpenTrack (FreeTrack) support on", "enableopentrack", "true", true);
    equals("Chromatic aberration off", "chromaticaberration", "false", false);
    equals("Distortion off", "distortion", "false", false);
    if (width > 0 && height > 0) {
        const auto w = value("res_width"), h = value("res_height");
        checks.push_back({"Resolution "+std::to_string(width)+"x"+std::to_string(height), true,
                          std::atoi(w.c_str()) == width && std::atoi(h.c_str()) == height, w+"x"+h,
                          {{"res_width", std::to_string(width)}, {"res_height", std::to_string(height)}}});
    }
    return checks;
}
inline std::string fix_x4(std::string xml, const std::vector<Check>& checks) {
    for (const auto& check : checks)
        if (!check.ok) for (const auto& [key, value] : check.fix) xml = xml_set(xml, key, value);
    return xml;
}

// Last line of pair_stats.txt: "tick submits stale_L stale_R late failed fallback ...".
struct Stats { int fps, late, repeated; };
inline bool parse_stats_line(const std::string& line, Stats& stats) {
    std::vector<long> fields;
    const char* p = line.c_str();
    for (int i = 0; i < 7; ++i) {
        char* end{};
        const long v = std::strtol(p, &end, 10);
        if (end == p) return false;
        fields.push_back(v);
        p = end;
    }
    stats = {int(fields[1]/2), int(fields[4]), int(fields[6])}; // lines cover ~2 s
    return true;
}
}
