#pragma once
// The GPU of the last VR session, from the mod's log (x4vr.log): its name and the mod's queue (its
// own, as on Windows, when the driver has a spare graphics queue, e.g. NVIDIA; else shared with X4,
// e.g. AMD's RADV), or why VR was off on it.
#include <optional>
#include <sstream>
#include <string>
#include <string_view>

namespace x4vr::linux_port {
struct GpuStatus { int state = 3; std::string text; }; // state as the menu's status rows: 0 ok, 2 problem
inline std::optional<GpuStatus> gpu_from_log(const std::string& log) {
    constexpr std::string_view created = "X4VR layer: device created on ", disabled = "X4VR layer: VR disabled for this device";
    std::string found, line;
    std::istringstream lines(log);
    while (std::getline(lines, line)) if (line.rfind(created, 0) == 0 || line.rfind(disabled, 0) == 0) found = line;
    if (found.empty()) return std::nullopt;
    if (found.rfind(disabled, 0) == 0) {
        const auto colon = found.find(": ", disabled.size());
        return GpuStatus{2, "VR was off: "+(colon == std::string::npos ? std::string("see Bug report") : found.substr(colon+2))};
    }
    const auto end = found.find(", VR queue: ");
    const auto gpu = found.substr(created.size(), end == std::string::npos ? std::string::npos : end-created.size());
    const auto queue = end == std::string::npos ? std::string() : found.substr(end+12, found.find(';', end)-end-12);
    return GpuStatus{queue.rfind("none", 0) == 0 ? 2 : 0,
                     gpu+(queue.rfind("shared", 0) == 0 ? " (shared queue)" : queue.rfind("private", 0) == 0 ? " (own queue)" : " (no VR queue)")};
}
}
