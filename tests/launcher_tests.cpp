// Launcher logic: profile merge into stereo.txt, X4 config.xml checks and fixes, stats parse,
// HUD distance mod rewriting.
#include "../tools/launcher/launcher_settings.hpp"
#include "../tools/launcher/hud_mod.hpp"
#include <cstdio>
#include <cstdlib>

using namespace x4vr::launcher;

#define CHECK(condition) do { if (!(condition)) { std::printf("FAILED %s:%d %s\n", __FILE__, __LINE__, #condition); std::exit(1); } } while (0)

int main() {
    // Profiles overlay only their keys; calibration and the live recenter counter survive.
    const auto defaults = parse_settings("stereo=1\r\ndelay=2\n# comment\nrecenter=0\npos_scale=3.6\n");
    const auto profile = parse_settings("pair=1\nipd_scale=1.2\nx4_width=2560\nx4_height=1440\n");
    const auto live = compose_live(defaults, profile, "7");
    CHECK(get(live, "delay") == "2" && get(live, "pos_scale") == "3.6");
    CHECK(get(live, "pair") == "1" && get(live, "ipd_scale") == "1.2");
    CHECK(get(live, "recenter") == "7");
    CHECK(get(live, "x4_width").empty()); // launcher-only keys stay out of stereo.txt
    CHECK(parse_settings(format_settings(live)) == live);
    CHECK(get(profile_of(live), "delay").empty() && get(profile_of(live), "pair") == "1");
    // The launcher's opt-in checkboxes are profile keys and reach stereo.txt.
    const auto options = compose_live(defaults, parse_settings("external_vr=1\nshared_pose=1\n"), "0");
    CHECK(get(options, "external_vr") == "1" && get(profile_of(options), "shared_pose") == "1");
    // VR runtime: OpenVR unless the profile says openxr; kept in profiles (and stereo.txt, for reports).
    CHECK(!uses_openxr(profile) && uses_openxr(parse_settings("runtime=openxr\n")));
    CHECK(get(profile_of(compose_live(defaults, parse_settings("runtime=openxr\n"), "0")), "runtime") == "openxr");

    // X4 config: flag what VR needs, fix exactly that, keep everything else.
    const std::string xml = "<?xml version=\"1.0\"?>\n<root>\n  <fullscreen>false</fullscreen>\n  <borderless>false</borderless>\n"
        "  <antialiasing>taa_high</antialiasing>\n  <fov>1.0000</fov>\n  <dlss>false</dlss>\n  <dlssmode>off</dlssmode>\n"
        "  <dlssg>off</dlssg>\n  <fsr3g>off</fsr3g>\n  <upmode>none</upmode>\n  <presentmode>immediate</presentmode>\n"
        "  <frameratelimit>true</frameratelimit>\n  <frameratetarget>60</frameratetarget>\n  <enableopentrack>true</enableopentrack>\n"
        "  <chromaticaberration>false</chromaticaberration>\n  <distortion>false</distortion>\n  <res_width>1920</res_width>\n"
        "  <res_height>1080</res_height>\n  <gamma>1.00</gamma>\n</root>\n";
    auto failing = [](const std::vector<Check>& checks) { int n = 0; for (const auto& c : checks) n += !c.ok; return n; };
    const auto checks = check_x4(xml, 2560, 1440);
    CHECK(failing(checks) == 5); // fullscreen, AA, FOV, frame limit, resolution
    const auto fixed = fix_x4(xml, checks);
    CHECK(failing(check_x4(fixed, 2560, 1440)) == 0);
    std::string v;
    CHECK(xml_value(fixed, "gamma", v) && v == "1.00");
    CHECK(xml_value(fixed, "res_width", v) && v == "2560");
    CHECK(failing(check_x4(fixed, 0, 0)) == 0); // resolution unchecked when the profile has none
    CHECK(failing(check_x4(xml_set(xml_set(fixed, "frameratelimit", "true"), "frameratetarget", "180"), 2560, 1440)) == 1); // any cap: the mod paces, and under 180 fps a cap defeats pair mode
    CHECK(failing(check_x4(xml_set(xml_set(fixed, "dlss", "true"), "dlssmode", "ultra_performance"), 2560, 1440)) == 0); // DLSS allowed
    std::string without_fsr = fixed;
    without_fsr.erase(without_fsr.find("  <fsr3g>"), std::string("  <fsr3g>off</fsr3g>\n").size());
    CHECK(failing(check_x4(without_fsr, 2560, 1440)) == 0); // X4 8.00 has no FSR frame generation setting
    const auto inserted = xml_set("<root>\n</root>\n", "enableopentrack", "true");
    CHECK(xml_value(inserted, "enableopentrack", v) && v == "true");

    Stats stats{};
    CHECK(parse_stats_line("29994546 180 94 93 2 0 5 0.21 0.29 11.64 0.00 11.41 | 1 0 1 0 1", stats));
    CHECK(stats.fps == 90 && stats.late == 2 && stats.repeated == 5);
    CHECK(!parse_stats_line("# tick_ms submits", stats));

    // HUD mod: HUD anchors move k times out, menu anchors and non-anchor connections stay.
    const std::string anchors =
        "<connection name=\"con_crosshair\" tags=\"uianchor_crosshair\">\n<offset>\n<position x=\"0\" y=\"-0.1\" z=\"0.2906321\"/>\n"
        "<quaternion qx=\"0\" qy=\"0\" qz=\"0\" qw=\"-1\"/>\n</offset>\n</connection>\n"
        "<connection name=\"con_menu_front\" tags=\"uianchor_front \">\n<offset>\n<position x=\"0\" y=\"0\" z=\"0.3469819\"/>\n</offset>\n</connection>\n"
        "<connection name=\"con_ticker\" tags=\"uianchor_messageticker_plain \">\n<offset>\n<position x=\"-0.2\" y=\"-0.166\" z=\"4.587704E-02\"/>\n</offset>\n</connection>\n"
        "<connection name=\"part\" tags=\"part nocollision\">\n<offset>\n<position x=\"1\" y=\"1\" z=\"1\"/>\n</offset>\n</connection>\n";
    int moved = 0;
    const auto scaled = scale_anchor_positions(anchors, 2.5, moved);
    CHECK(moved == 2);
    CHECK(scaled.find("<position x=\"0\" y=\"-0.25\" z=\"0.726580") != std::string::npos);
    CHECK(scaled.find("<position x=\"0\" y=\"0\" z=\"0.3469819\"") != std::string::npos); // menu anchor unchanged
    CHECK(scaled.find("<position x=\"-0.5\" y=\"-0.415\" z=\"0.1146926\"") != std::string::npos);
    CHECK(scaled.find("<position x=\"1\" y=\"1\" z=\"1\"") != std::string::npos); // not a UI anchor
    CHECK(scaled.find("qw=\"-1\"") != std::string::npos);
    int factors = 0;
    const auto lua = scale_presentation_factors("local config = {\n\tscalingFactor = 0.0004, -- note\n\tradarScaleFactor = 0.0002,\n}\n"
        "if config.scalingFactor == 0.5 then end\nprivate.scalingFactor = private.scalingFactor * 2\n", 2.5, factors);
    CHECK(factors == 2);
    CHECK(lua.find("scalingFactor = 0.001, -- note") != std::string::npos && lua.find("radarScaleFactor = 0.0005,") != std::string::npos);
    CHECK(lua.find("== 0.5") != std::string::npos);
    std::string error;
    CHECK(hud_files({}, 2.5, error).empty() && !error.empty()); // missing game files: nothing half-built
    // X4's user content.xml: only our entry is turned back on.
    bool was_disabled = false;
    const auto content = enable_extension("<content>\n  <extension id=\"ws_1\" enabled=\"false\"/>\n"
                                          "  <extension id=\"x4vr_hud\" enabled=\"false\"/>\n</content>\n", "x4vr_hud", was_disabled);
    CHECK(was_disabled);
    CHECK(content.find("id=\"x4vr_hud\" enabled=\"true\"/>") != std::string::npos);
    CHECK(content.find("id=\"ws_1\" enabled=\"false\"/>") != std::string::npos);
    enable_extension(content, "x4vr_hud", was_disabled);
    CHECK(!was_disabled);

    // Seat position mod: one diff per ship under its DLC, which loads first.
    const auto seat = seat_files();
    const auto& kukri = seat.at("extensions/ego_dlc_terran/assets/units/size_s/ship_ter_s_fighter_01.xml");
    CHECK(kukri.find("<replace sel=\"//connection[@name='con_cockpit']/offset/position/@z\">2.253249</replace>") != std::string::npos);
    CHECK(kukri.find("@name='con_uianchor']/offset/position/@z\">2.491607<") != std::string::npos);
    CHECK(seat.at("content.xml").find("<dependency id=\"ego_dlc_terran\" optional=\"true\"/>") != std::string::npos);
    CHECK(seat.at("x4vr_seat.txt") == "ships=Kukri\n");
    std::printf("launcher logic ok\n");
}
