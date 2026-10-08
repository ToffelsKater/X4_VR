// A stand-in for SteamVR's vrclient.so, for loader_lock_tests (issue #7). On the reporter's machine
// SteamVR answered the mod's Vulkan questions by first making Vulkan calls of its own; inside X4's
// vkCreateInstance / vkCreateDevice those wait for the Vulkan loader's lock, held by the same thread:
// X4 hung. Here each Vulkan question first "calls the loader" (loader_lock_tests' loader_call),
// which reports instead of hanging when its lock is already held. So does connecting (VR_Init),
// which then succeeds and is counted (x4vr_test_connected). Any other call returns 0.
#include <openvr.h>
#include "ivrclientcore.h"
#include <dlfcn.h>
#include <cstdint>
#include <cstring>

namespace {
// The vtable slot of a virtual member function (Itanium C++ ABI: its pointer holds 1 + the slot's
// byte offset).
template<class Member> size_t slot(Member member) {
    struct { uintptr_t pointer; ptrdiff_t adjust; } raw;
    static_assert(sizeof(member) == sizeof(raw));
    std::memcpy(&raw, &member, sizeof raw);
    return (raw.pointer-1)/sizeof(void*);
}
// The test program's stand-ins for the Vulkan loader (exported by loader_lock_tests).
void loader_call() {
    if (auto* call = reinterpret_cast<void (*)()>(dlsym(RTLD_DEFAULT, "x4vr_test_loader_call"))) call();
}
void connected() {
    if (auto* count = reinterpret_cast<void (*)()>(dlsym(RTLD_DEFAULT, "x4vr_test_connected"))) count();
}
uint64_t gpu() {
    auto* gpu = reinterpret_cast<uint64_t (*)()>(dlsym(RTLD_DEFAULT, "x4vr_test_gpu"));
    return gpu ? gpu() : 0;
}
uint32_t answer(const char* text, char* value, uint32_t size) {
    const auto needed = static_cast<uint32_t>(std::strlen(text)+1);
    if (value && size >= needed) std::memcpy(value, text, needed);
    return needed;
}
uint32_t instance_extensions(void*, char* value, uint32_t size) {
    loader_call();
    return answer("VK_KHR_get_physical_device_properties2", value, size);
}
uint32_t device_extensions(void*, VkPhysicalDevice_T*, char* value, uint32_t size) {
    loader_call();
    return answer("VK_KHR_maintenance1", value, size);
}
void output_device(void*, uint64_t* device, vr::ETextureType, VkInstance_T*) {
    loader_call();
    *device = gpu();
}
uintptr_t nothing(void*) { return 0; }

struct Fake { void** vtable; };
void* compositor_table[256];
void* system_table[256];
Fake compositor{compositor_table}, system_object{system_table};
struct Core : vr::IVRClientCore {
    vr::EVRInitError Init(vr::EVRApplicationType, const char*) override {
        loader_call();
        connected();
        return vr::VRInitError_None;
    }
    void Cleanup() override {}
    vr::EVRInitError IsInterfaceVersionValid(const char*) override { return vr::VRInitError_None; }
    void* GetGenericInterface(const char* name, vr::EVRInitError* error) override {
        *error = vr::VRInitError_None;
        if (!std::strncmp(name, "IVRCompositor_", 14)) return &compositor;
        if (!std::strncmp(name, "IVRSystem_", 10)) return &system_object;
        *error = vr::VRInitError_Init_InterfaceNotFound;
        return nullptr;
    }
    bool BIsHmdPresent() override { return true; }
    const char* GetEnglishStringForHmdError(vr::EVRInitError) override { return "fake SteamVR"; }
    const char* GetIDForVRInitError(vr::EVRInitError) override { return "fake SteamVR"; }
} core;
}

extern "C" __attribute__((visibility("default"))) void* VRClientCoreFactory(const char*, int* code) {
    for (auto& f : compositor_table) f = reinterpret_cast<void*>(&nothing);
    for (auto& f : system_table) f = reinterpret_cast<void*>(&nothing);
    compositor_table[slot(&vr::IVRCompositor::GetVulkanInstanceExtensionsRequired)] = reinterpret_cast<void*>(&instance_extensions);
    compositor_table[slot(&vr::IVRCompositor::GetVulkanDeviceExtensionsRequired)] = reinterpret_cast<void*>(&device_extensions);
    system_table[slot(&vr::IVRSystem::GetOutputDevice)] = reinterpret_cast<void*>(&output_device);
    *code = 0;
    return &core;
}
