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
// Positions of HUD anchor connections (tags uianchor_*, except the menu anchors) times k.
inline std::string scale_anchor_positions(const std::string& xml, double k, int& count) {
    static const std::regex connection(R"re(<connection [^>]*tags="([^"]*)"[^>]*>[\s\S]*?</connection>)re");
    static const std::regex position(R"re(<position x="([^"]+)" y="([^"]+)" z="([^"]+)")re");
    static const std::regex hud_tag(R"re((^|\s)uianchor_)re"), menu_tag(R"re((^|\s)uianchor_(front|left|right)(\s|$))re");
    int connections = 0;
    return rewrite(xml, connection, [&](const std::smatch& m) {
        const std::string block = m[0], tags = m[1];
        if (!std::regex_search(tags, hud_tag) || std::regex_search(tags, menu_tag)) return block;
        return rewrite(block, position, [&](const std::smatch& p) {
            return "<position x=\""+format_number(std::stod(p[1])*k)+"\" y=\""+format_number(std::stod(p[2])*k)+
                   "\" z=\""+format_number(std::stod(p[3])*k)+"\"";
        }, count);
    }, connections);
}
// World-space presentation scale factors (config.scalingFactor = 0.0004 etc.) times k.
inline std::string scale_presentation_factors(const std::string& lua, double k, int& count) {
    static const std::regex factor(R"re(\b(scalingFactor|targetMonitorScaleFactor|radarScaleFactor|messageTickerScaleFactor|missionBarScaleFactor)(\s*=\s*)([0-9]+\.[0-9]+))re");
    return rewrite(lua, factor, [&](const std::smatch& m) { return m[1].str()+m[2].str()+format_number(std::stod(m[3])*k); }, count);
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
inline std::map<std::string, std::string> hud_files(const std::map<std::string, std::string>& originals, double k, std::string& error) {
    std::map<std::string, std::string> out;
    for (const auto& path : hud_anchor_files()) {
        const auto source = originals.find(path);
        int count = 0;
        if (source != originals.end()) out[path] = scale_anchor_positions(source->second, k, count);
        if (count < 3) { error = "unexpected "+path; return {}; }
    }
    for (const auto& path : hud_scripts()) {
        const auto source = originals.find(path);
        int count = 0;
        if (source != originals.end()) out[path] = scale_presentation_factors(source->second, k, count);
        if (count < 1) { error = "unexpected "+path; return {}; }
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
