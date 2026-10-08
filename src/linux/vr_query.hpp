#pragma once
// SteamVR's Vulkan requirements, asked before X4 starts (issue #7). SteamVR can answer them by
// creating a Vulkan instance of its own (seen with NVIDIA's driver). Inside X4's vkCreateInstance or
// vkCreateDevice the Vulkan loader holds its lock, which that nested call then waits for forever: X4
// hangs with no window. So `x4vr vr-vulkan` (x4vr_cli.cpp), run by x4vr-run, asks SteamVR in its own
// process and x4vr-run passes the answers to the mod in these environment variables:
//   X4VR_VR_INSTANCE_EXTENSIONS  instance extensions, separated by spaces
//   X4VR_VR_GPU                  the headset GPU's device UUID, 32 hex digits
//   X4VR_VR_DEVICE_EXTENSIONS    device extensions for that GPU, separated by spaces
// The helper prints them one per line in this order. Shared by x4vr and the mod.
#include <cstdint>
#include <cstdio>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace x4vr::vr_query {
inline constexpr const char* instance_extensions_variable = "X4VR_VR_INSTANCE_EXTENSIONS";
inline constexpr const char* gpu_variable = "X4VR_VR_GPU";
inline constexpr const char* device_extensions_variable = "X4VR_VR_DEVICE_EXTENSIONS";
inline constexpr size_t uuid_size = 16; // VK_UUID_SIZE

inline std::vector<std::string> words(const std::string& text) {
    std::istringstream in(text);
    std::vector<std::string> result;
    for (std::string word; in >> word;) result.push_back(word);
    return result;
}
inline std::string join(const std::vector<std::string>& words) {
    std::string text;
    for (const auto& word : words) text += (text.empty() ? "" : " ")+word;
    return text;
}
inline std::string uuid_text(const uint8_t* uuid) {
    std::string text;
    char digits[3];
    for (size_t i = 0; i < uuid_size; ++i) { std::snprintf(digits, sizeof digits, "%02x", uuid[i]); text += digits; }
    return text;
}
// 32 hex digits (either case) to 16 bytes; anything else: nothing.
inline std::optional<std::vector<uint8_t>> parse_uuid(const std::string& text) {
    if (text.size() != 2*uuid_size) return std::nullopt;
    std::vector<uint8_t> uuid(uuid_size);
    const auto digit = [](char c) { return c >= '0' && c <= '9' ? c-'0' : c >= 'a' && c <= 'f' ? c-'a'+10 : c >= 'A' && c <= 'F' ? c-'A'+10 : -1; };
    for (size_t i = 0; i < uuid_size; ++i) {
        const int high = digit(text[2*i]), low = digit(text[2*i+1]);
        if (high < 0 || low < 0) return std::nullopt;
        uuid[i] = static_cast<uint8_t>(high*16+low);
    }
    return uuid;
}
}
