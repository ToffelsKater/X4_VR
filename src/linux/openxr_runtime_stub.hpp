#pragma once
// The Linux build has no OpenXR backend yet (docs/LINUX_PORT_PLAN.md: SteamVR through OpenVR
// first). RuntimeBootstrap holds an OpenXRRuntime pointer; this stand-in lets the Linux copy of
// runtime_bootstrap.cpp keep the Windows code paths unchanged. It is never constructed: with
// X4VR_RUNTIME=openxr the Linux runtime logs that OpenXR isn't built and uses OpenVR.
// A Linux copy of src/openxr_runtime.cpp (XR_KHR_convert_timespec_time instead of the Win32
// counter extension, a statically built loader) replaces this file when OpenXR is added.
#include <x4vr/runtime_bootstrap.hpp>
#include <stdexcept>

namespace x4vr {
class OpenXRRuntime {
    [[noreturn]] static void unavailable() { throw std::logic_error("OpenXR is not built on Linux"); }
public:
    std::vector<std::string> instance_extensions() { unavailable(); }
    std::vector<std::string> device_extensions() { unavailable(); }
    VkPhysicalDevice_T* output_device(VkInstance_T*) { unavailable(); }
    std::string start_session(const XrVulkanContext&) { unavailable(); }
    void end_session(VkDevice_T*) {}
    FrameStatus predicted_tracking(Matrix&, float) { return FrameStatus::tracking_unavailable; }
    RuntimeBootstrap::EyeSetup eye_setup() { return {}; }
    std::string wait_frame() { unavailable(); }
    std::string submit_frame(const std::array<vr::VRVulkanTextureData_t, 2>&, const std::array<vr::VRTextureBounds_t, 2>&,
                             const std::array<Matrix, 2>&, bool) { unavailable(); }
    std::string show_theater(const vr::VRVulkanTextureData_t*, const vr::VRTextureBounds_t&, const Matrix&, float) { unavailable(); }
    void hide_theater() {}
    std::string show_cursor(const uint8_t*, uint32_t, uint32_t, bool, const Matrix&, float) { unavailable(); }
    void hide_cursor() {}
};
}
