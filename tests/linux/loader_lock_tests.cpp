// Regression test for issue #7: X4 hung at start, no window. The mod asked SteamVR for its Vulkan
// needs inside X4's vkCreateInstance (and vkCreateDevice), where the Vulkan loader holds its lock
// while the layers run. SteamVR answered by making Vulkan calls of its own on the same thread, which
// waited for that lock forever.
//
// This program stands in for X4 and the Vulkan loader: it loads the mod (libx4vr.so) as a layer and
// calls its vkCreateInstance and vkCreateDevice holding a lock as the loader does, with a fake driver
// below. fake_vrclient.so stands in for SteamVR: connecting to it and each Vulkan question first
// "call the loader" (x4vr_test_loader_call). The mod connects to SteamVR at X4's first frame, not
// in vkCreateInstance: SteamVR may make Vulkan calls while connecting too. The lock checks for errors, so a call made while it's held is counted
// instead of hanging. No Vulkan loader or driver is needed.
// usage: loader_lock_tests <libx4vr.so> <fake SteamVR runtime folder>
#include "vr_query.hpp"
#include <vulkan/vk_layer.h>
#include <dlfcn.h>
#include <pthread.h>
#include <stdlib.h>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {
int failures = 0;
void check(bool ok, const std::string& what) { if (!ok) { std::printf("FAIL: %s\n", what.c_str()); ++failures; } }

// The loader's lock, held through vkCreateInstance and vkCreateDevice while the layers run.
pthread_mutex_t loader_lock;
int calls_under_lock = 0; // Vulkan calls SteamVR made while it was held: in X4, a hang
int connections = 0;      // VR_Init calls that reached the fake SteamVR

// Dispatchable handles start with the loader's dispatch pointer (the mod looks them up by it); an
// instance's GPUs share the instance's.
struct Handle { const void* dispatch; };
const int instance_dispatch = 0, device_dispatch = 0;
Handle instance_handle{&instance_dispatch}, gpu_handle{&instance_dispatch}, device_handle{&device_dispatch}, queue_handle{&device_dispatch};
const uint8_t gpu_uuid[VK_UUID_SIZE]{0x10, 0xde, 0x26, 0x84, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
}

// For fake_vrclient.so.
extern "C" __attribute__((visibility("default"))) void x4vr_test_loader_call() {
    if (pthread_mutex_lock(&loader_lock) == EDEADLK) { ++calls_under_lock; return; } // held by this thread
    pthread_mutex_unlock(&loader_lock);
}
extern "C" __attribute__((visibility("default"))) void x4vr_test_connected() { ++connections; }
extern "C" __attribute__((visibility("default"))) uint64_t x4vr_test_gpu() { return reinterpret_cast<uint64_t>(&gpu_handle); }

namespace {
// The driver below the mod: what it was asked for.
std::vector<std::string> instance_extensions, device_extensions;
uint32_t queues = 0;
std::vector<std::string> names(uint32_t count, const char* const* list) { return std::vector<std::string>(list, list+count); }
bool has(const std::vector<std::string>& list, const char* name) { for (const auto& n : list) if (n == name) return true; return false; }

VKAPI_ATTR VkResult VKAPI_CALL create_instance(const VkInstanceCreateInfo* ci, const VkAllocationCallbacks*, VkInstance* out) {
    instance_extensions = names(ci->enabledExtensionCount, ci->ppEnabledExtensionNames);
    *out = reinterpret_cast<VkInstance>(&instance_handle);
    return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL destroy_instance(VkInstance, const VkAllocationCallbacks*) {}
VKAPI_ATTR VkResult VKAPI_CALL create_device(VkPhysicalDevice, const VkDeviceCreateInfo* ci, const VkAllocationCallbacks*, VkDevice* out) {
    device_extensions = names(ci->enabledExtensionCount, ci->ppEnabledExtensionNames);
    queues = 0;
    for (uint32_t i = 0; i < ci->queueCreateInfoCount; ++i) queues += ci->pQueueCreateInfos[i].queueCount;
    *out = reinterpret_cast<VkDevice>(&device_handle);
    return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL queue_families(VkPhysicalDevice, uint32_t* count, VkQueueFamilyProperties* families) {
    if (!families) { *count = 1; return; }
    families[0] = {};
    families[0].queueFlags = VK_QUEUE_GRAPHICS_BIT; families[0].queueCount = 4;
}
VKAPI_ATTR void VKAPI_CALL properties(VkPhysicalDevice, VkPhysicalDeviceProperties* p) {
    *p = {};
    std::strcpy(p->deviceName, "Test GPU");
}
VKAPI_ATTR void VKAPI_CALL properties2(VkPhysicalDevice gpu, VkPhysicalDeviceProperties2* p) {
    properties(gpu, &p->properties);
    for (auto* next = static_cast<VkBaseOutStructure*>(p->pNext); next; next = next->pNext)
        if (next->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES)
            std::memcpy(reinterpret_cast<VkPhysicalDeviceIDProperties*>(next)->deviceUUID, gpu_uuid, VK_UUID_SIZE);
}
VKAPI_ATTR void VKAPI_CALL memory(VkPhysicalDevice, VkPhysicalDeviceMemoryProperties* p) { *p = {}; }
VKAPI_ATTR void VKAPI_CALL device_queue(VkDevice, uint32_t, uint32_t, VkQueue* queue) { *queue = reinterpret_cast<VkQueue>(&queue_handle); }
VKAPI_ATTR VkResult VKAPI_CALL loader_data(VkDevice, void*) { return VK_SUCCESS; }

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL driver_instance(VkInstance, const char* name) {
    const std::string n = name;
    if (n == "vkCreateInstance") return reinterpret_cast<PFN_vkVoidFunction>(create_instance);
    if (n == "vkDestroyInstance") return reinterpret_cast<PFN_vkVoidFunction>(destroy_instance);
    if (n == "vkCreateDevice") return reinterpret_cast<PFN_vkVoidFunction>(create_device);
    if (n == "vkGetPhysicalDeviceQueueFamilyProperties") return reinterpret_cast<PFN_vkVoidFunction>(queue_families);
    if (n == "vkGetPhysicalDeviceProperties") return reinterpret_cast<PFN_vkVoidFunction>(properties);
    if (n == "vkGetPhysicalDeviceProperties2") return reinterpret_cast<PFN_vkVoidFunction>(properties2);
    if (n == "vkGetPhysicalDeviceMemoryProperties") return reinterpret_cast<PFN_vkVoidFunction>(memory);
    return nullptr;
}
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL driver_device(VkDevice, const char* name) {
    return std::string(name) == "vkGetDeviceQueue" ? reinterpret_cast<PFN_vkVoidFunction>(device_queue) : nullptr;
}

// The loader's part: the layer chain, and the lock held while the mod runs.
PFN_vkGetInstanceProcAddr layer;
VkResult loader_create_instance() {
    VkLayerInstanceLink link{nullptr, driver_instance, nullptr};
    VkLayerInstanceCreateInfo chain{VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO};
    chain.function = VK_LAYER_LINK_INFO; chain.u.pLayerInfo = &link;
    VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ci.pNext = &chain;
    VkInstance instance;
    pthread_mutex_lock(&loader_lock);
    const auto result = reinterpret_cast<PFN_vkCreateInstance>(layer(nullptr, "vkCreateInstance"))(&ci, nullptr, &instance);
    pthread_mutex_unlock(&loader_lock);
    return result;
}
VkResult loader_create_device() {
    VkLayerDeviceLink link{nullptr, driver_instance, driver_device};
    VkLayerDeviceCreateInfo data{VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO};
    data.function = VK_LOADER_DATA_CALLBACK; data.u.pfnSetDeviceLoaderData = loader_data;
    VkLayerDeviceCreateInfo chain{VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO, &data};
    chain.function = VK_LAYER_LINK_INFO; chain.u.pLayerInfo = &link;
    const float priority = 1.f;
    VkDeviceQueueCreateInfo queue{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    queue.queueFamilyIndex = 0; queue.queueCount = 1; queue.pQueuePriorities = &priority;
    VkDeviceCreateInfo ci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &chain};
    ci.queueCreateInfoCount = 1; ci.pQueueCreateInfos = &queue;
    VkDevice device;
    const auto instance = reinterpret_cast<VkInstance>(&instance_handle);
    pthread_mutex_lock(&loader_lock);
    const auto result = reinterpret_cast<PFN_vkCreateDevice>(layer(instance, "vkCreateDevice"))(
        reinterpret_cast<VkPhysicalDevice>(&gpu_handle), &ci, nullptr, &device);
    pthread_mutex_unlock(&loader_lock);
    return result;
}

std::string log_path;
std::string take_log() { // the mod's log since the last call
    std::ifstream in(log_path);
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::ofstream(log_path, std::ios::trunc);
    return text;
}
bool contains(const std::string& text, const char* part) { return text.find(part) != std::string::npos; }
}

int main(int argc, char** argv) {
    if (argc != 3) { std::fprintf(stderr, "usage: loader_lock_tests <libx4vr.so> <fake SteamVR runtime folder>\n"); return 2; }
    pthread_mutexattr_t attributes;
    pthread_mutexattr_init(&attributes);
    pthread_mutexattr_settype(&attributes, PTHREAD_MUTEX_ERRORCHECK);
    pthread_mutex_init(&loader_lock, &attributes);

    char pattern[] = "/tmp/x4vr-loader-lock-XXXXXX";
    const std::string work = mkdtemp(pattern);
    std::ofstream(work+"/vrpath") << "{\"runtime\":[\""+std::string(argv[2])+"\"],\"config\":[\""+work+"\"],\"log\":[\""+work+
                                     "\"],\"version\":1,\"jsonid\":\"vrpathreg\"}\n";
    log_path = work+"/x4vr.log";
    setenv("VR_PATHREG_OVERRIDE", (work+"/vrpath").c_str(), 1);
    setenv("X4VR_CAPTURE_DIR", work.c_str(), 1);
    setenv("X4VR_DIR", work.c_str(), 1);
    setenv("X4VR_ANY_PROCESS", "1", 1); // the mod acts as in X4
    setenv("X4VR_HEADSET_WAIT", "0", 1);
    using namespace x4vr::vr_query;
    for (const auto* name : {instance_extensions_variable, gpu_variable, device_extensions_variable}) unsetenv(name);

    void* mod = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!mod) { std::printf("FAIL: %s\n", dlerror()); return 1; }
    VkNegotiateLayerInterface negotiate{LAYER_NEGOTIATE_INTERFACE_STRUCT, nullptr, 2};
    const auto negotiate_function = reinterpret_cast<PFN_vkNegotiateLoaderLayerInterfaceVersion>(dlsym(mod, "x4vrNegotiateLoaderLayerInterfaceVersion"));
    if (!negotiate_function || negotiate_function(&negotiate) != VK_SUCCESS) { std::printf("FAIL: the mod's layer interface\n"); return 1; }
    layer = negotiate.pfnGetInstanceProcAddr;

    // 1. Without x4vr-run's answers: X4 flat, and nothing asked of SteamVR under the lock.
    check(loader_create_instance() == VK_SUCCESS, "vkCreateInstance without x4vr-run's answers");
    auto log = take_log();
    check(calls_under_lock == 0, "without x4vr-run's answers, SteamVR made Vulkan calls inside vkCreateInstance (X4 would hang)");
    check(contains(log, "runs without VR"), "without x4vr-run's answers, the mod should leave X4 flat");
    const auto instance = reinterpret_cast<VkInstance>(&instance_handle);
    reinterpret_cast<PFN_vkDestroyInstance>(layer(instance, "vkDestroyInstance"))(instance, nullptr); // as X4 does

    // 2. With them (as `x4vr vr-vulkan` gives them): VR on X4's instance and device, nothing asked
    // of SteamVR under the lock.
    setenv(instance_extensions_variable, "VK_KHR_get_physical_device_properties2", 1);
    setenv(gpu_variable, uuid_text(gpu_uuid).c_str(), 1);
    setenv(device_extensions_variable, "VK_KHR_maintenance1", 1);
    check(loader_create_instance() == VK_SUCCESS, "vkCreateInstance with x4vr-run's answers");
    log = take_log();
    check(calls_under_lock == 0, "SteamVR made Vulkan calls inside vkCreateInstance (X4 would hang)");
    check(has(instance_extensions, "VK_KHR_get_physical_device_properties2"), "SteamVR's instance extension enabled");
    check(!contains(log, "runs without VR"), "VR on with x4vr-run's answers");
    check(loader_create_device() == VK_SUCCESS, "vkCreateDevice with x4vr-run's answers");
    log = take_log();
    check(calls_under_lock == 0, "SteamVR made Vulkan calls inside vkCreateDevice (X4 would hang)");
    check(has(device_extensions, "VK_KHR_maintenance1"), "SteamVR's device extension enabled");
    check(queues == 2, "the mod's own queue added");
    check(contains(log, "X4VR layer: device created on Test GPU"), "VR on X4's device");

    // 3. X4 on another GPU than the headset's: that device without VR, still nothing under the lock.
    setenv(gpu_variable, "00000000000000000000000000000001", 1);
    check(loader_create_device() == VK_SUCCESS, "vkCreateDevice on another GPU");
    log = take_log();
    check(calls_under_lock == 0, "SteamVR made Vulkan calls inside vkCreateDevice (X4 would hang)");
    check(contains(log, "VR disabled for this device: X4 selected a GPU different from the headset's"), "VR off on another GPU");
    check(!has(device_extensions, "VK_KHR_maintenance1") && queues == 1, "that device as X4 asked for it");

    // SteamVR not connected to yet: that waits for X4's first frame, outside the loader's lock.
    check(connections == 0, "the mod connected to SteamVR inside vkCreateInstance or vkCreateDevice");

    if (failures) { std::printf("--- the mod's log\n%s", log.c_str()); return 1; }
    std::printf("loader_lock: all passed\n");
    std::fflush(stdout);
    _Exit(0); // the mod's threads (OpenTrack sender) run on, as in X4
}
