// X4's VR settings on Linux (tools/linux/x4_settings.hpp): after x4vr's fix, every required setting
// is in config.xml with its VR value, whether it was wrong or missing; optional ones are never
// changed. The table below is the expected VR value of each, written out here independently of
// the checks.
#include "x4_settings.hpp"
#include <cstdio>
#include <map>
#include <string>

namespace {
int failures = 0;
void check(bool ok, const std::string& what) { if (!ok) { std::printf("FAIL: %s\n", what.c_str()); ++failures; } }

using namespace x4vr;
constexpr int width = 2880, height = 1620; // the VR resolution x4vr picked
// Required: key -> VR value. Keys not confirmed in a Linux config.xml yet are fixed only when there.
const std::map<std::string, std::string> required{
    {"fullscreen", "false"}, {"borderless", "false"}, {"res_width", "2880"}, {"res_height", "1620"}, {"fov", "1.3333"},
    {"antialiasing", "none"}, {"upmode", "none"}, {"presentmode", "immediate"},
    {"enableopentrack", "true"}, {"dlssg", "off"}, {"fsr3g", "off"}, {"opentrackanglefactor", "1.00"}, {"opentrackpositionfactor", "1.00"}};
const std::map<std::string, std::string> optional{{"chromaticaberration", "false"}, {"distortion", "false"}};
// Warnings: shown with why, never written (the player may want a frame rate limit).
const std::map<std::string, std::string> warning{{"frameratelimit", "false"}};

// x4vr fix-settings, as x4vr_cli.cpp does it.
std::string fix(const std::string& xml) {
    std::vector<launcher::Check> writes;
    for (const auto& c : linux_port::linux_checks(xml, width, height)) if (linux_port::to_write(c, xml)) writes.push_back(c);
    return linux_port::write_settings(xml, writes);
}
std::string value(const std::string& xml, const std::string& key) {
    std::string v;
    return launcher::xml_value(xml, key, v) ? v : "(missing)";
}
std::string config(const std::map<std::string, std::string>& settings) {
    std::string xml = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<root>\n  <gamma>1.00</gamma>\n";
    for (const auto& [key, v] : settings) xml += "  <"+key+">"+v+"</"+key+">\n";
    return xml+"</root>\n";
}
// After the fix: every required setting with a known key (or there before) at its VR value, every
// check passing but the optional ones, nothing else touched.
void check_fixed(const std::string& before, const std::string& name) {
    const auto after = fix(before);
    for (const auto& [key, wanted] : required) {
        const bool known = linux_port::known_key(key) || value(before, key) != "(missing)";
        if (known) check(value(after, key) == wanted, name+": "+key+" is "+value(after, key)+", should be "+wanted);
        else check(value(after, key) == "(missing)", name+": "+key+" added though its name isn't confirmed");
    }
    for (const auto& [key, v] : optional) check(value(after, key) == value(before, key), name+": optional "+key+" changed");
    for (const auto& [key, v] : warning) check(value(after, key) == value(before, key), name+": warning "+key+" changed");
    check(value(after, "gamma") == "1.00", name+": an unrelated setting changed");
    for (const auto& c : linux_port::linux_checks(after, width, height))
        if (c.required) check(c.ok, name+": still wrong after the fix: "+c.label+" ("+c.current+")");
    check(fix(after) == after, name+": a second fix changes something");
}
}

int main() {
    // The table covers every check, and every check is in the table.
    for (const auto& c : linux_port::linux_checks(config({}), width, height))
        for (const auto& [key, v] : c.fix)
            check((c.required ? required : linux_port::kind(c) == linux_port::Kind::warning ? warning : optional).count(key) == 1,
                  "check not in the test's table: "+c.label+" ("+key+")");

    // Missing is never assumed to be X4's default: every required setting missing is wrong.
    for (const auto& c : linux_port::linux_checks(config({}), width, height))
        if (c.required) check(!c.ok && c.current.find("not set") != std::string::npos, "missing but passing: "+c.label+" ("+c.current+")");

    // 1. A config.xml X4 hasn't written VR-relevant settings to: all added (the known ones).
    check_fixed(config({}), "empty");
    // 2. Everything wrong.
    const auto wrong = config({{"fullscreen", "true"}, {"borderless", "true"}, {"res_width", "3840"}, {"res_height", "2160"}, {"fov", "1.0000"},
                               {"antialiasing", "taa"}, {"upmode", "fsr"}, {"presentmode", "fifo"}, {"frameratelimit", "true"},
                               {"enableopentrack", "false"}, {"dlssg", "on"}, {"fsr3g", "on"}, {"opentrackanglefactor", "0.50"},
                               {"opentrackpositionfactor", "2.00"}, {"chromaticaberration", "true"}, {"distortion", "true"}});
    check_fixed(wrong, "all wrong");
    // 3. Issue #7's: OpenTrack Support missing, the rest right.
    const auto issue7 = config({{"fullscreen", "false"}, {"borderless", "false"}, {"res_width", "2880"}, {"res_height", "1620"},
                                {"fov", "1.3333"}, {"antialiasing", "none"}, {"upmode", "none"}, {"presentmode", "immediate"},
                                {"frameratelimit", "false"}, {"chromaticaberration", "false"}, {"distortion", "false"}});
    check_fixed(issue7, "issue #7");
    check(value(fix(issue7), "enableopentrack") == "true", "issue #7: OpenTrack Support turned on");

    // How the checks show it.
    auto checks = linux_port::linux_checks(issue7, width, height);
    for (const auto& c : checks) {
        if (c.fix[0].first == "enableopentrack") check(!c.ok && c.current == "not set", "missing OpenTrack Support: wrong, not set");
        if (c.fix[0].first == "opentrackanglefactor") check(!c.ok && c.current == "not set", "missing factor: wrong, not set");
    }
    for (const auto& c : linux_port::linux_checks(wrong, width, height)) {
        if (c.fix[0].first == "chromaticaberration") check(!c.ok && !c.required && !linux_port::to_write(c, wrong), "optional: wrong, shown, not written");
        if (c.fix[0].first == "opentrackanglefactor") check(!c.ok && c.current == "50 %", "factor shown in %");
    }

    // Exact values: FOV 1.3333 only; anti-aliasing any but temporal; the frame rate limit a warning.
    const auto between = config({{"fov", "1.5000"}, {"frameratelimit", "true"}, {"frameratetarget", "120"}, {"antialiasing", "fxaa"}});
    for (const auto& c : linux_port::linux_checks(between, width, height)) {
        if (c.fix[0].first == "fov") check(!c.ok, "FOV 1.5: wrong (only 1.3333 is right)");
        if (c.fix[0].first == "frameratelimit")
            check(!c.ok && !c.required && linux_port::kind(c) == linux_port::Kind::warning && c.current == "120 fps", "frame rate limit on: a warning, 120 fps");
        if (c.fix[0].first == "antialiasing") check(c.ok, "FXAA: fine (not temporal)");
    }
    const auto between_fixed = fix(between);
    check(value(between_fixed, "fov") == "1.3333", "FOV set exactly");
    check(value(between_fixed, "frameratelimit") == "true", "frame rate limit left as the player chose");
    for (const auto& c : linux_port::linux_checks(config({{"frameratelimit", "false"}}), width, height))
        if (c.fix[0].first == "frameratelimit") check(c.ok, "frame rate limit off: fine");
    check(value(between_fixed, "antialiasing") == "fxaa", "non-temporal anti-aliasing left as the player chose");
    for (const auto* taa : {"taa", "taa_high", "temporal"}) {
        check(!linux_port::antialiasing_ok(taa), std::string("temporal anti-aliasing wrong: ")+taa);
        check(value(fix(config({{"antialiasing", taa}})), "antialiasing") == "none", std::string("temporal anti-aliasing set to none: ")+taa);
    }
    check(linux_port::antialiasing_ok("none") && linux_port::antialiasing_ok("smaa") && !linux_port::antialiasing_ok(""), "anti-aliasing values");

    // The display mode is checked without the VR resolution too (resolution then waits for SteamVR).
    bool windowed = false, resolution = false;
    for (const auto& c : linux_port::linux_checks(config({{"fullscreen", "true"}}), 0, 0)) {
        windowed = windowed || (c.label == "Display mode: windowed" && !c.ok);
        resolution = resolution || c.fix[0].first == "res_width";
    }
    check(windowed && !resolution, "display mode checked without the VR resolution, resolution not");

    // Protected UI Mode (uisafemode): required off while HUD Scaled is on, not checked otherwise.
    const auto safe_on = config({{"uisafemode", "true"}});
    bool checked = false;
    for (const auto& c : linux_port::linux_checks(safe_on, width, height)) checked = checked || c.fix[0].first == "uisafemode";
    check(!checked, "HUD Scaled off: Protected UI Mode not checked");
    for (const auto* before : {"true", "missing"}) {
        const auto xml = std::string(before) == "missing" ? config({}) : safe_on;
        bool wrong = false;
        for (const auto& c : linux_port::linux_checks(xml, width, height, true))
            if (c.fix[0].first == "uisafemode") wrong = !c.ok && c.required && c.current == (std::string(before) == "missing" ? "not set" : "on");
        check(wrong, std::string("HUD Scaled on, Protected UI Mode ")+before+": wrong");
        std::vector<launcher::Check> writes;
        for (const auto& c : linux_port::linux_checks(xml, width, height, true)) if (linux_port::to_write(c, xml)) writes.push_back(c);
        const auto after = linux_port::write_settings(xml, writes);
        check(value(after, "uisafemode") == "false", std::string("HUD Scaled on, Protected UI Mode ")+before+": set off");
        for (const auto& c : linux_port::linux_checks(after, width, height, true)) if (c.required) check(c.ok, "HUD Scaled on, after the fix: "+c.label);
    }
    check(value(fix(safe_on), "uisafemode") == "true", "HUD Scaled off: Protected UI Mode left alone");

    if (failures) return 1;
    std::printf("x4_settings: all passed\n");
}
