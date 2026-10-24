#pragma once
// HUD distance mod, built from the player's own game files (nothing of Egosoft's ships with
// this project). X4 places each HUD element at a UI anchor 0.29-0.44 m in front of the pilot
// (assets/ui/ui_screen_positions.xml and ui_panels.xml; cutscenes and the main menu have their
// own anchors), and each HUD script scales its presentation by a world-space factor
// (ui/core/lua/*.lua). Multiplying both by k scales the HUD about the eye point: same apparent
// size, k times farther away. In stereo the unmodified HUD sits a hand's width from the face.
// Menus (widget system) stay untouched, anchors included: they go to the theater screen, and
// their scale is shared with cutscene anchors; moving their anchors shrank them, scaling the
// widgets too pushed the main menu off-screen.
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <regex>
#include <set>
#include <string>
#include <vector>

namespace x4vr::launcher {

inline const std::vector<std::string>& hud_anchor_files() {
    static const std::vector<std::string> files{"assets/ui/ui_screen_positions.xml", "assets/ui/ui_panels.xml",
        "assets/ui/ui_screen_positions_cutscene.xml", "assets/cutscenecore/startmenu.xml"};
    return files;
}
inline const std::vector<std::string>& hud_scripts() {
    static const std::vector<std::string> files{"ui/core/lua/monitors.lua", "ui/core/lua/infobar.lua", "ui/core/lua/infobar2.lua",
        "ui/core/lua/infobar3.lua", "ui/core/lua/infobar4.lua", "ui/core/lua/compass.lua", "ui/core/lua/dialogmenu.lua",
        "ui/core/lua/subchannelbar.lua", "ui/core/lua/overlay.lua"};
    return files;
}
// X4 9.00 ships each UI script as .lua and as .xpl (precompiled) and loads the .xpl first, on Linux
// (2026-10-04) and on Windows (2026-10-24: the order flag at 0xb22613; the Windows mod's patched
// .lua scales never applied). The patched Lua source goes to the .xpl path too: Lua's loader takes
// source or bytecode.
inline std::string xpl_of(const std::string& lua) { return lua.substr(0, lua.size()-4)+".xpl"; }
// Every game file the mod is built from: anchors, scripts and their precompiled copies.
inline std::set<std::string> hud_sources() {
    std::set<std::string> paths(hud_anchor_files().begin(), hud_anchor_files().end());
    for (const auto& script : hud_scripts()) { paths.insert(script); paths.insert(xpl_of(script)); }
    return paths;
}
inline std::string format_number(double value) {
    char text[32];
    std::snprintf(text, sizeof(text), "%.7g", value);
    return text;
}
// Rewrites every match of `pattern` through `replace`, counting matches.
template<class Replace> std::string rewrite(const std::string& text, const std::regex& pattern, Replace replace, int& count) {
    std::string out;
    auto last = text.cbegin();
    for (std::sregex_iterator it(text.cbegin(), text.cend(), pattern), end; it != end; ++it) {
        out.append(last, (*it)[0].first);
        out += replace(*it);
        last = (*it)[0].second;
        ++count;
    }
    return out.append(last, text.cend());
}
// Conversation menu (anchor uianchor_dialogmenu, script dialogmenu.lua): X4 puts it 19.5 degrees
// below the view centre at the HUD's depth, too low and too close in a headset. It goes
// dialog_distance times farther than the rest of the HUD (its script scale follows, so the same
// apparent size), and its height shrinks by dialog_raise: about 5 degrees below the centre.
// Headset check 2026-10-24: 1.5 and 0.5 (10 degrees) were too far and still too low; 1.2 and
// 0.25 better.
inline constexpr double dialog_distance = 1.2, dialog_raise = 0.25;
// Lower-corner panels (message ticker bottom left, target panel bottom right) sit about 30 degrees
// to the side and 19 down. Their sideways and vertical offsets shrink by a pull factor, one for the
// cockpit and one on foot (the launcher's "Side panels" fields). On foot the HUD follows the camera,
// and the headset's round lens view cut the panels off: never in view, 0.4 right while the panels
// were too small (2026-10-24), but at their real size 0.4 overlapped the conversation options, so
// 0.6. In the cockpit the HUD stays on the ship, and 0.4 put the target panel next to the
// crosshair: X4's place there ("perfect", 2026-10-24).
inline constexpr double corner_pull = 1, foot_pull = 0.6;
// Part of the installed mod's source hash: change it when hud_files changes its output, so
// installed mods rebuild at the next start.
inline constexpr const char* hud_layout = "layout-6";
// Positions of HUD anchor connections (tags uianchor_*, except the menu anchors) times k.
inline std::string scale_anchor_positions(const std::string& xml, double k, int& count, double pull = corner_pull) {
    static const std::regex connection(R"re(<connection [^>]*tags="([^"]*)"[^>]*>[\s\S]*?</connection>)re");
    static const std::regex position(R"re(<position x="([^"]+)" y="([^"]+)" z="([^"]+)")re");
    static const std::regex hud_tag(R"re((^|\s)uianchor_)re"), menu_tag(R"re((^|\s)uianchor_(front|left|right)(\s|$))re");
    static const std::regex dialog_tag(R"re((^|\s)uianchor_dialogmenu(\s|$))re");
    static const std::regex corner_tag(R"re((^|\s)uianchor_(messageticker|messageticker_plain|eventmonitor)(\s|$))re");
    int connections = 0;
    return rewrite(xml, connection, [&](const std::smatch& m) {
        const std::string block = m[0], tags = m[1];
        if (!std::regex_search(tags, hud_tag) || std::regex_search(tags, menu_tag)) return block;
        const bool dialog = std::regex_search(tags, dialog_tag), corner = std::regex_search(tags, corner_tag);
        const double kz = dialog ? k*dialog_distance : k, kx = corner ? kz*pull : kz, ky = dialog ? kz*dialog_raise : kx;
        return rewrite(block, position, [&](const std::smatch& p) {
            return "<position x=\""+format_number(std::stod(p[1])*kx)+"\" y=\""+format_number(std::stod(p[2])*ky)+
                   "\" z=\""+format_number(std::stod(p[3])*kz)+"\"";
        }, count);
    }, connections);
}
// World-space presentation scale factors (config.scalingFactor = 0.0004 etc.) times k.
inline std::string scale_presentation_factors(const std::string& lua, double k, int& count) {
    static const std::regex factor(R"re(\b(scalingFactor|targetMonitorScaleFactor|radarScaleFactor|messageTickerScaleFactor|missionBarScaleFactor)(\s*=\s*)([0-9]+\.[0-9]+))re");
    return rewrite(lua, factor, [&](const std::smatch& m) { return m[1].str()+m[2].str()+format_number(std::stod(m[3])*k); }, count);
}

// On foot the corner panels move from the cockpit pull to the on-foot pull. The anchors (XML) are
// shared, so monitors.lua shifts the panel inside its anchor while X4's game plan is "firstperson".
// The shift is the anchor's move between the two pulls (ui_screen_positions.xml, metres), in the
// anchor's own x and y axes (its quaternion), since the panel element lives in the anchor's frame.
// The panel moves within its own tilted plane: the target panel (anchor turned ~31 degrees) ends
// about 3 degrees farther out and a little farther away than the exact pull would put it.
struct PanelShift { double x = 0, y = 0; };
inline bool foot_shift(const std::string& xml, const std::string& tag, double k, double pull, double foot, PanelShift& out) {
    const std::regex connection("<connection [^>]*tags=\""+tag+"\"[^>]*>[\\s\\S]*?</connection>");
    static const std::regex position(R"re(<position x="([^"]+)" y="([^"]+)")re");
    static const std::regex rotation(R"re(<quaternion qx="([^"]+)" qy="([^"]+)" qz="([^"]+)" qw="([^"]+)")re");
    std::smatch c, p, q;
    if (!std::regex_search(xml, c, connection)) return false;
    const std::string block = c[0];
    if (!std::regex_search(block, p, position)) return false;
    double x = 0, y = 0, z = 0, w = 1;
    if (std::regex_search(block, q, rotation)) x = std::stod(q[1]), y = std::stod(q[2]), z = std::stod(q[3]), w = std::stod(q[4]);
    const double dx = k*(foot-pull)*std::stod(p[1]), dy = k*(foot-pull)*std::stod(p[2]);
    out = {dx*(1-2*(y*y+z*z))+dy*2*(x*y+z*w), dx*2*(x*y-z*w)+dy*(1-2*(x*x+z*z))}; // onto the anchor's x and y axes
    return true;
}
// monitors.lua with the on-foot shift of the target panel ("Monitor") and the ticker; `count`
// gets the number of places patched (3 when the script is as expected).
inline std::string shift_panels_on_foot(std::string lua, PanelShift monitor, PanelShift ticker, int& count) {
    const auto insert = [&](const std::regex& at, const std::string& text, bool after) {
        std::smatch m;
        if (!std::regex_search(lua, m, at)) return;
        const auto spot = size_t(m.position(0)+(after ? m.length(0) : 0));
        lua.insert(spot, text);
        ++count;
    };
    static const std::regex init(R"re(function self:onInitialize\(\))re");
    static const std::regex place(R"re(\t\tsetAttribute\(mainelement, "position\.y", mainelementoffset\.y\)\r?\n)re");
    static const std::regex plan(R"re(function onGamePlanChange\(_, gameplan\)\r?\n)re");
    insert(init, "-- X4 VR (X4VRLauncher, side panels on foot): shift in metres along each anchor's x and y\n"
        "local x4vrFoot = { Monitor = { "+format_number(monitor.x)+", "+format_number(monitor.y)+" }, ticker = { "+
        format_number(ticker.x)+", "+format_number(ticker.y)+" } }\n"
        "local x4vrPanels = {}\n"
        "local function x4vrPlace(name, onfoot)\n"
        "\tlocal panel, shift = x4vrPanels[name], x4vrFoot[name]\n"
        "\tif not panel or not shift then return end\n"
        "\tlocal foot = onfoot and 1 or 0\n"
        "\tsetAttribute(panel.element, \"position.x\", panel.x + foot * shift[1] / panel.scale)\n"
        "\tsetAttribute(panel.element, \"position.y\", panel.y + foot * shift[2] / panel.scale)\n"
        "end\n\n", false);
    insert(place, "\t\tx4vrPanels[childElement] = { element = mainelement, x = mainelementoffset.x, y = mainelementoffset.y, scale = appliedscale }\n"
        "\t\tx4vrPlace(childElement, IsFirstPerson())\n", true);
    insert(plan, "\tx4vrPlace(\"Monitor\", gameplan == \"firstperson\")\n\tx4vrPlace(\"ticker\", gameplan == \"firstperson\")\n", true);
    return lua;
}

// X4's per-user content.xml (next to config.xml) remembers extensions turned off in its Extensions
// menu, e.g. by "disable all" when switching game versions; X4 then skips the mod. Returns the
// text with extension `id` turned back on, and whether it was off.
inline std::string enable_extension(const std::string& content, const std::string& id, bool& was_disabled) {
    const std::regex disabled("(<extension\\s+id=\""+id+"\"\\s+enabled=\")false\"");
    was_disabled = std::regex_search(content, disabled);
    return std::regex_replace(content, disabled, "$1true\"");
}

// The base game's copy of each path (later catalogs win). X4's .cat lines are "path size time md5",
// the .dat holds the files back to back.
inline std::map<std::string, std::string> read_game_files(const std::filesystem::path& game, const std::set<std::string>& paths) {
    std::map<std::string, std::string> found;
    for (int index = 1; index <= 99; ++index) {
        char name[8];
        std::snprintf(name, sizeof(name), "%02d", index);
        std::ifstream cat(game/(std::string(name)+".cat")), dat(game/(std::string(name)+".dat"), std::ios::binary);
        if (!cat || !dat) continue;
        unsigned long long offset = 0;
        for (std::string line; std::getline(cat, line);) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            size_t cut = line.size();
            std::string fields[3];
            for (auto& field : fields) { // size, time, md5 from the right; the path may contain spaces
                const auto space = line.rfind(' ', cut-1);
                if (space == std::string::npos || !cut) { cut = std::string::npos; break; }
                field = line.substr(space+1, cut-space-1);
                cut = space;
            }
            if (cut == std::string::npos) continue;
            const auto path = line.substr(0, cut);
            const auto size = std::stoull(fields[2]);
            if (paths.count(path)) {
                std::string blob(size_t(size), '\0');
                dat.seekg(std::streamoff(offset));
                dat.read(blob.data(), std::streamsize(size));
                if (dat) found[path] = std::move(blob);
                dat.clear();
            }
            offset += size;
        }
    }
    return found;
}
// Patched copies of every file the mod replaces; empty with `error` set if a file is missing or
// no longer looks as expected (a game update).
inline std::map<std::string, std::string> hud_files(const std::map<std::string, std::string>& originals, double k, std::string& error,
                                                    double pull = corner_pull, double foot = foot_pull) {
    std::map<std::string, std::string> out;
    for (const auto& path : hud_anchor_files()) {
        const auto source = originals.find(path);
        int count = 0;
        if (source != originals.end()) out[path] = scale_anchor_positions(source->second, k, count, pull);
        if (count < 3) { error = "unexpected "+path; return {}; }
    }
    PanelShift monitor, ticker;
    const auto anchors = originals.find("assets/ui/ui_screen_positions.xml");
    if (anchors == originals.end() || !foot_shift(anchors->second, "uianchor_eventmonitor", k, pull, foot, monitor) ||
        !foot_shift(anchors->second, "uianchor_messageticker", k, pull, foot, ticker)) { error = "unexpected corner anchors"; return {}; }
    for (const auto& path : hud_scripts()) {
        const auto source = originals.find(path);
        int count = 0;
        const double factor = path == "ui/core/lua/dialogmenu.lua" ? k*dialog_distance : k;
        if (source != originals.end()) out[path] = scale_presentation_factors(source->second, factor, count);
        if (count < 1) { error = "unexpected "+path; return {}; }
        if (path == "ui/core/lua/monitors.lua") {
            int places = 0;
            out[path] = shift_panels_on_foot(out[path], monitor, ticker, places);
            if (places != 3) { error = "unexpected "+path; return {}; }
        }
        if (originals.count(xpl_of(path))) out[xpl_of(path)] = out[path];
    }
    return out;
}

// Seat position mod (issue #23, ship list in #22): in some ships the pilot camera sits too far
// forward for VR. One XML diff per ship moves the cockpit camera (con_cockpit) and the HUD anchor
// (con_uianchor) back by the same distance. A diff holds only our values, nothing of Egosoft's.
// A DLC's ship is patched under extensions/<dlc>/, with the DLC loaded first.
struct SeatFix { const char* ship; const char* dlc; const char* file; const char* cockpit_z; const char* uianchor_z; };
inline constexpr SeatFix seat_fixes[] = {
    // 0.75 m back: X4 has 3.003249 and 3.241607 (8.00 and 9.00)
    {"Kukri", "ego_dlc_terran", "assets/units/size_s/ship_ter_s_fighter_01.xml", "2.253249", "2.491607"},
};
inline std::string seat_ships() {
    std::string ships;
    for (const auto& fix : seat_fixes) ships += (ships.empty() ? "" : ", ")+std::string(fix.ship);
    return ships;
}
// Every file of extensions\x4vr_seat, by its path in there.
inline std::map<std::string, std::string> seat_files() {
    std::map<std::string, std::string> files;
    // Loaded after VRO (ws_1696862840) as well: its diffs rewrite the same ships' connections.
    std::set<std::string> before{"ws_1696862840"};
    for (const auto& fix : seat_fixes) {
        const std::string dlc = fix.dlc;
        if (!dlc.empty()) before.insert(dlc);
        files[(dlc.empty() ? "" : "extensions/"+dlc+"/")+fix.file] = "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<diff>\n"
            "  <replace sel=\"//connection[@name='con_cockpit']/offset/position/@z\">"+std::string(fix.cockpit_z)+"</replace>\n"
            "  <replace sel=\"//connection[@name='con_uianchor']/offset/position/@z\">"+std::string(fix.uianchor_z)+"</replace>\n</diff>\n";
    }
    auto& content = files["content.xml"];
    content = "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
        "<content id=\"x4vr_seat\" name=\"X4 VR seat position\" version=\"100\" save=\"0\" enabled=\"1\"\n"
        "  description=\"Generated by X4VRLauncher: pilot camera further back in ships where it sits too far forward for VR ("+seat_ships()+").\">\n";
    for (const auto& id : before) content += "  <dependency id=\""+id+"\" optional=\"true\"/>\n";
    content += "</content>\n";
    files["x4vr_seat.txt"] = "ships="+seat_ships()+"\n"; // marks the folder as the launcher's
    return files;
}
}
