// Checks the mod's X4 code signatures against an X4.exe (default: the game next to this repo;
// pass another version's X4.exe to see what the mod supports there). Each feature must match
// exactly one known version's code, once, and the frame-half readers must agree.
#include <x4vr/code_scan.hpp>
#include <cstdio>
#include <string>

namespace {
using namespace x4vr::code;
int failures = 0;
const unsigned char* base{};
// The only match of `signature`, printed; nullptr if none or several.
const unsigned char* unique(const char* name, std::string_view signature) {
    const auto found = find_all(base, parse(signature));
    std::printf("  %-17s %s", name, found.size() == 1 ? "found" : found.empty() ? "MISSING" : "AMBIGUOUS");
    for (const auto* hit : found) std::printf(" %#llx", static_cast<unsigned long long>(hit-base));
    std::printf("\n");
    return found.size() == 1 ? found.front() : nullptr;
}
// The rows (X4 versions) whose code all matches; a feature fails unless exactly one does.
template<class Row, size_t N, class Check> void versions(const char* feature, const Row (&rows)[N], Check check) {
    int matched = 0;
    for (const auto& row : rows) {
        std::printf("%s, X4 %s code:\n", feature, row.version);
        matched += check(row);
    }
    std::printf("%s: %s\n", feature, matched == 1 ? "supported" : matched ? "AMBIGUOUS (several versions match)" : "NOT SUPPORTED");
    failures += matched != 1;
}
}

int main(int argc, char** argv) {
    const std::string path = argc > 1 ? argv[1] : X4VR_DEFAULT_EXE;
    const auto module = LoadLibraryExA(path.c_str(), nullptr, LOAD_LIBRARY_AS_IMAGE_RESOURCE);
    if (!module) { std::printf("skipped: cannot map %s\n", path.c_str()); return 0; }
    base = reinterpret_cast<const unsigned char*>(reinterpret_cast<uintptr_t>(module) & ~uintptr_t(3));

    // Scanner: a wildcard pattern finds itself in a buffer, and not a near miss.
    const unsigned char sample[] = {0x90, 0x48, 0x8b, 0x0d, 1, 2, 3, 4, 0x48, 0x85, 0xc9, 0x90};
    if (!matches(sample+1, parse("48 8b 0d ?? ?? ?? ?? 48 85 c9")) || matches(sample+1, parse("48 8b 0d ?? ?? ?? ?? 48 85 c8")) ||
        reinterpret_cast<uintptr_t>(rip_target(sample+1, 3, 7)) != reinterpret_cast<uintptr_t>(sample+8)+0x04030201) { std::printf("scanner self-check FAILED\n"); return 1; }

    std::printf("leaning back:\n");
    failures += !unique("backward_clamp", x4::backward_clamp);
    versions("on-foot tracking", x4::on_foot, [](const x4::OnFoot& row) {
        const auto zeroing = unique("zeroing", row.zeroing), offset = unique("offset", row.offset);
        if (!zeroing || !offset) return false;
        if (rip_target(zeroing, 3, 7) != rip_target(offset+11, 3, 7)) { std::printf("  signatures read different camera managers\n"); return false; }
        return true;
    });
    versions("eye at use", x4::trackers, [](const x4::Tracker& row) {
        const auto update = unique("update", row.update);
        const bool accessors = unique("position", row.position) && unique("still", row.still);
        if (update) {
            float gain;
            std::memcpy(&gain, rip_target(update, 4, 8), sizeof(gain));
            std::printf("  tracker gain      %g (8.00, 9.00: 0.2)\n", gain);
        }
        return update && accessors;
    });
    versions("rival trackers off", x4::rival_trackers, [](const x4::RivalTrackers& row) {
        const bool trackir = unique("trackir", row.trackir);
        return unique("tobii", row.tobii) && trackir;
    });
    versions("HUD fixed to the ship", x4::hud_cameras, [](const x4::HudCamera& row) { return unique("update", row.update) != nullptr; });
    const auto half = find_all(base, parse(x4::frame_half));
    bool agree = !half.empty();
    for (const auto* reader : half) agree = agree && rip_target(reader, 3, 7) == rip_target(half[0], 3, 7);
    std::printf("frame half:\n  %s, %zu readers", agree ? "found" : "MISSING OR DISAGREEING", half.size());
    if (!half.empty()) std::printf(", global %#llx", static_cast<unsigned long long>(rip_target(half[0], 3, 7)-base));
    std::printf("\n");
    failures += !agree;
    std::printf(failures ? "%d feature(s) failed: they stay off on this X4 version\n" : "all features supported\n", failures);
    return failures ? 1 : 0;
}
