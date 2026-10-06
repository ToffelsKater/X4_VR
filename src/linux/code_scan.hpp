#pragma once
// Linux counterpart of include/x4vr/code_scan.hpp: finds X4 code by its bytes instead of by
// address, so the mod keeps working on an X4 build whose code at these spots is unchanged,
// wherever the linker placed it. "??" is a wildcard byte, used for RIP-relative and call
// displacements, which change with every relink. Struct offsets and short jumps stay literal: if
// they differ, the code around them changed and the mod leaves it alone. The OpenTrack tracker's
// vtable is found through its RTTI name. Same sites in the running game (opentrack_client.cpp,
// runtime_bootstrap.cpp) and in a file (`x4vr patterns`, which reports what it finds).
// Adding an X4 build: run `x4vr patterns`; for a site it doesn't find, look up the same code in
// that build (docs/linux/ARCHITECTURE.md, "Linux addresses") and adjust the pattern, then check it in the headset.
#include "elf_classes.hpp"
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace x4vr::linux_port::code {
using Pattern = std::vector<int>; // byte value, or -1 for a wildcard

// parse and matches are the same as in include/x4vr/code_scan.hpp, which can't be included here
// (it includes <windows.h>).
inline Pattern parse(std::string_view text) {
    Pattern bytes;
    for (size_t i = 0; i+1 < text.size(); i += 3) {
        const auto nibble = [](char c) { return c <= '9' ? c-'0' : (c|0x20)-'a'+10; };
        bytes.push_back(text[i] == '?' ? -1 : nibble(text[i])*16+nibble(text[i+1]));
    }
    return bytes;
}
inline bool matches(const unsigned char* code, const Pattern& pattern) {
    for (size_t i = 0; i < pattern.size(); ++i) if (pattern[i] >= 0 && code[i] != pattern[i]) return false;
    return true;
}
// Every match (load address) in the executable segments.
inline std::vector<uint64_t> find_all(const elf::Image& image, const Pattern& pattern) {
    // Search for the longest literal run, then check the whole pattern around each hit.
    size_t anchor = 0, length = 0;
    for (size_t i = 0; i < pattern.size();) {
        size_t j = i;
        while (j < pattern.size() && pattern[j] >= 0) ++j;
        if (j-i > length) anchor = i, length = j-i;
        i = j+1;
    }
    std::vector<unsigned char> literal;
    for (size_t i = anchor; i < anchor+length; ++i) literal.push_back(static_cast<unsigned char>(pattern[i]));
    const std::boyer_moore_horspool_searcher searcher(literal.begin(), literal.end());
    std::vector<uint64_t> found;
    for (const auto& segment : image.segments()) {
        if (!segment.exec) continue;
        const auto* begin = segment.bytes.data();
        const auto* end = begin+segment.bytes.size();
        for (auto hit = begin; (hit = std::search(hit, end, searcher)) != end; ++hit)
            if (size_t(hit-begin) >= anchor && size_t(end-hit)+anchor >= pattern.size() && matches(hit-anchor, pattern))
                found.push_back(segment.vaddr+uint64_t(hit-anchor-begin));
    }
    return found;
}
inline bool matches_at(const elf::Image& image, uint64_t address, const Pattern& pattern) {
    const auto* code = image.at(address, pattern.size());
    return code && matches(code, pattern);
}
// Address a RIP-relative operand refers to: the instruction at `address` is `length` bytes long
// with its 32-bit displacement at `disp_at`.
inline uint64_t rip_target(const elf::Image& image, uint64_t address, size_t disp_at, size_t length) {
    const auto* code = image.at(address, length);
    if (!code) return 0;
    int32_t disp;
    std::memcpy(&disp, code+disp_at, sizeof(disp));
    return address+length+uint64_t(int64_t(disp));
}

// X4 code the Linux mod relies on (X4 9.00 addresses in the comments; docs/linux/ARCHITECTURE.md).
namespace x4 {
// Camera input (0xfeb72b): `if (tracker type != 7 && z > 0) z = 0`; its `jbe` (+20) past the
// zeroing becomes `jmp`. cmp $7,%eax; je; movss -0xb0(%rbp),%xmm1; pxor; comiss; jbe; movss.
inline constexpr std::string_view backward_clamp =
    "83 f8 07 74 19 f3 0f 10 8d 50 ff ff ff 66 0f ef c0 0f 2f c8 76 08 f3 0f 11 85 50 ff ff ff";
inline constexpr size_t backward_clamp_at = 20;
// Camera input (0xfec070): with no ship, a zero pose unless the tracker is an eye tracker;
// the `je` after the call (+7) becomes `jno` (byte +8, 0x84 -> 0x81). call; test %al,%al; je;
// test %r13,%r13; je; mov 0x148(%r13),%rdi (active tracker); test; je; mov (%rdi),%rax; call *0x40(%rax).
inline constexpr std::string_view onfoot_zeroing =
    "e8 ?? ?? ?? ?? 84 c0 0f 84 ?? ?? ?? ?? 4d 85 ed 74 2a 49 8b bd 48 01 00 00 48 85 ff 74 1e 48 8b 07 ff 50 40";
inline constexpr size_t onfoot_zeroing_at = 8;
// Camera::GetOffset (0x1929ff6): without a movement controller (camera +0x18) it returns
// (je, displacement 0x213) before composing the head offset; the displacement byte (+9) becomes
// 0x81, the block that composes it without one (offset_block). Then the player global (+13, RIP)
// and the camera entity +0x780 against [player global]+0x238.
inline constexpr std::string_view camera_offset =
    "4c 8b 63 18 4d 85 e4 0f 84 13 02 00 00 48 8b 05 ?? ?? ?? ?? 48 8b 93 80 07 00 00 48 85 c0 "
    "0f 84 b6 02 00 00 48 39 90 38 02 00 00";
inline constexpr size_t camera_offset_at = 9, camera_offset_end = 13, camera_offset_jump = 0x281;
// Camera input (0xfeb6c5): player global, head-tracking manager, then the rendered camera
// [player+0x3e8] and its mode +0x880; mode 0 jumps (+49) to the on-foot zeroing site. Pins the
// offsets walking detection reads, as Windows' on-foot signature pins +0x3d0 and the mode; it must
// name the camera offset's player global and reach the zeroing site, else on foot stays off.
// mov (rip),%r15; mov (rip),%r13; test; je; mov 0x3e8(%r15),%rax; test; je; mov 0x880(%rax),%r11d;
// test; je.
inline constexpr std::string_view walking_check =
    "4c 8b 3d ?? ?? ?? ?? 4c 8b 2d ?? ?? ?? ?? 4d 85 ff 0f 84 ?? ?? ?? ?? 49 8b 87 e8 03 00 00 48 85 c0 "
    "0f 84 ?? ?? ?? ?? 44 8b 98 80 08 00 00 45 85 db 0f 84 ?? ?? ?? ??";
inline constexpr size_t walking_check_jump = 49;
// 0x192a284: movaps 0x10/0x20/0x30/0x0(%rbp) into %xmm6/7/5/8; jmp to the composition.
inline constexpr std::string_view offset_block = "0f 28 75 10 0f 28 7d 20 0f 28 6d 30 44 0f 28 45 00 e9";
// The frame half's reader (0x218e220): mov half(%rip),%eax; xor $1,%eax; ret. All matches must
// name the same global.
inline constexpr std::string_view frame_half = "8b 05 ?? ?? ?? ?? 83 f0 01 c3";
// VR::OpenTrack's position accessor (vtable slot 34): movss 0xd0(%rdi),%xmm0.
inline constexpr std::string_view opentrack_type = "N2VR9OpenTrackE";
inline constexpr size_t opentrack_position_slot = 34;
inline constexpr std::string_view opentrack_position = "f3 0f 10 87 d0 00 00 00";
// VR::OpenTrack's update (vtable slot 2, 0x1a1b720; this part at +0x70): pins the tracker fields
// the eye-at-use hook uses, as Windows' tracker signatures do. A new packet is copied to
// +0x78..+0xa7 (roll at +0xa0), the fresh flag +0xa8 set, then the roll read and the position
// scaled by +0x114. movdqu 0x20(%rbx); movb $1,0x50; movb $1,0xa8; movups ..0x78/0x88/0x98;
// call; test; jne; movss (rip),%xmm4; pxor x3; cvtsd2ss 0x98/0xa0; pxor; cvtsd2ss 0x90; mulss;
// movss 0x118; cmpb 0x110; mulss x2; movss 0x114(%rbx),%xmm4.
inline constexpr size_t opentrack_update_slot = 2, opentrack_update_within = 0x100;
inline constexpr std::string_view opentrack_update =
    "f3 0f 6f 43 20 c6 43 50 01 c6 83 a8 00 00 00 01 0f 11 43 78 f3 0f 6f 43 30 0f 11 83 88 00 00 00 "
    "f3 0f 6f 43 40 0f 11 83 98 00 00 00 e8 ?? ?? ?? ?? 84 c0 0f 85 ?? ?? ?? ?? f3 0f 10 25 ?? ?? ?? ?? "
    "66 0f ef db 66 0f ef c9 66 0f ef c0 f2 0f 5a 9b 98 00 00 00 f2 0f 5a 8b a0 00 00 00 66 0f ef ed "
    "f2 0f 5a 83 90 00 00 00 f3 0f 59 dc f3 0f 10 93 18 01 00 00 80 bb 10 01 00 00 00 f3 0f 59 cc "
    "f3 0f 59 c4 f3 0f 10 a3 14 01 00 00";
}

// Where each site is in one X4 build; 0 where it isn't found, with the reason in `notes`.
struct X4Sites {
    uint64_t backward_clamp{}, onfoot_zeroing{}, camera_offset{}; // patch sites (pattern start)
    uint64_t player_global{};      // X4 9.00: 0x3db6948 (rendered camera at +0x3e8, mode +0x880)
    uint64_t frame_half_global{};  // X4 9.00: 0x72a0fa0
    uint64_t opentrack_vtable{}, opentrack_typeinfo{}, opentrack_position{}; // address point, type_info, slot 34 code
    std::vector<std::string> notes;
};
inline X4Sites find_x4_sites(const elf::Image& image) {
    X4Sites s;
    const auto hex = [](uint64_t v) { char t[24]; std::snprintf(t, sizeof t, "0x%llx", static_cast<unsigned long long>(v)); return std::string(t); };
    const auto unique = [&](const char* what, std::string_view pattern) -> uint64_t {
        const auto found = find_all(image, parse(pattern));
        if (found.size() == 1) { s.notes.push_back(std::string(what)+": "+hex(found[0])); return found[0]; }
        s.notes.push_back(std::string(what)+": "+(found.empty() ? "not found" : std::to_string(found.size())+" matches"));
        return 0;
    };
    s.backward_clamp = unique("backward head-position clamp", x4::backward_clamp);
    s.onfoot_zeroing = unique("on-foot head-pose zeroing", x4::onfoot_zeroing);
    if (const auto site = unique("on-foot camera offset", x4::camera_offset)) {
        const auto block = site+x4::camera_offset_end+x4::camera_offset_jump;
        if (matches_at(image, block, parse(x4::offset_block))) {
            s.camera_offset = site;
            s.player_global = rip_target(image, site+x4::camera_offset_end, 3, 7);
            s.notes.push_back("player global: "+hex(s.player_global));
        } else s.notes.push_back("on-foot camera offset: the offset block isn't at "+hex(block));
    }
    { // walking detection's offsets; on foot needs all three sites, or none is patched
        const auto site = s.onfoot_zeroing && s.player_global ? unique("walking check", x4::walking_check) : 0;
        const bool same = site && rip_target(image, site, 3, 7) == s.player_global &&
                          rip_target(image, site+x4::walking_check_jump, 2, 6) == s.onfoot_zeroing;
        if (site && !same) s.notes.push_back("walking check: doesn't read the player global or reach the zeroing site");
        if (!same) {
            if (s.onfoot_zeroing || s.camera_offset) s.notes.push_back("on foot: off in this X4");
            s.onfoot_zeroing = s.camera_offset = s.player_global = 0;
        }
    }
    {
        const auto found = find_all(image, parse(x4::frame_half));
        uint64_t global = 0;
        for (const auto hit : found) {
            const auto target = rip_target(image, hit, 2, 6);
            global = !global || global == target ? target : ~uint64_t(0);
        }
        if (global && global != ~uint64_t(0)) { s.frame_half_global = global; s.notes.push_back("frame half global: "+hex(global)); }
        else s.notes.push_back(found.empty() ? "frame half global: not found" : "frame half global: readers name different globals");
    }
    const auto update_hits = find_all(image, parse(x4::opentrack_update));
    bool update_checked = false;
    for (const auto& info : elf::find_classes(image, x4::opentrack_type)) {
        if (info.mangled != x4::opentrack_type) continue;
        for (const auto& table : info.vtables) {
            if (table.offset_to_top != 0 || table.slots.size() <= x4::opentrack_position_slot) continue;
            const auto code = table.slots[x4::opentrack_position_slot];
            if (!matches_at(image, code, parse(x4::opentrack_position))) continue;
            const auto update = table.slots[x4::opentrack_update_slot];
            update_checked = std::any_of(update_hits.begin(), update_hits.end(),
                                         [&](uint64_t hit) { return hit >= update && hit < update+x4::opentrack_update_within; });
            if (!update_checked) continue; // the tracker fields moved: the hook would read the wrong ones
            s.opentrack_vtable = table.address_point();
            s.opentrack_typeinfo = info.typeinfo;
            s.opentrack_position = code;
        }
    }
    s.notes.push_back(s.opentrack_vtable ? "VR::OpenTrack vtable: "+hex(s.opentrack_vtable)+", position accessor "+hex(s.opentrack_position)
                                         : update_hits.empty() || !update_checked ? "VR::OpenTrack vtable: not found, or its update doesn't match (tracker fields moved)"
                                                                                  : "VR::OpenTrack vtable: not found");
    return s;
}
}
