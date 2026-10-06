// VK_LAYER_X4VR: the Linux copy of src/observe_layer.cpp at commit 62569df (docs/LINUX_PORT_PLAN.md,
// section 3). The alternate-eye presenter, the submission thread, theater mode, pacing and the stats
// files are the Windows code. Changed for Linux:
// - the layer is the whole mod (libx4vr.so): VR starts only in X4 (linux_port::is_x4_process), the
//   library pins itself, and it starts the OpenTrack pose sender (pose_sender.cpp);
// - private queue: a spare queue in the game's graphics family as on Windows; without one (AMD's
//   RADV has a single graphics queue) the layer shares X4's graphics queue and serialises every use
//   of it (SharedQueue below);
// - left out: the Windows diagnostics (shader/pipeline capture, mapped-memory and native camera
//   sampling, stack walks, the frame probe) and the mouse cursor overlay (X4 on Linux draws an X11
//   cursor; capturing it is later work). Without camera sampling, turn compensation stays off
//   (it applies on foot only, which needs the on-foot patches anyway);
// - Ctrl+F12 for the theater screen: the recenter= counter (x4vr ctl recenter) until hotkeys exist.
#include "linux_runtime.hpp"
#include "x11_cursor.hpp"
#include <x4vr/eye_targets.hpp>
#include <x4vr/runtime_bootstrap.hpp>
#include <x4vr/vulkan_extensions.hpp>
#include <vulkan/vk_layer.h>
#include <dlfcn.h>
#include <pthread.h>
#include <sched.h>
#include <sys/resource.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#define EXPORT extern "C" __attribute__((visibility("default")))

namespace {
using x4vr::linux_port::log;
// Dispatchable objects belonging to an instance/device share their dispatch key.
template<class T> void* key(T object) { return object ? *reinterpret_cast<void**>(object) : nullptr; }
uint64_t milliseconds() {
    return uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}
void pause_cpu() { __builtin_ia32_pause(); }

struct Instance {
    VkInstance instance{};
    PFN_vkGetInstanceProcAddr gipa{};
    PFN_GetPhysicalDeviceProcAddr gpdpa{};
    std::shared_ptr<x4vr::RuntimeBootstrap> runtime;
};
struct Device {
    VkDevice device{};
    PFN_vkGetDeviceProcAddr gdpa{};
    VkPhysicalDeviceMemoryProperties memory_properties{};
#define DEVICE_FUNCTIONS(F) \
    F(DestroyDevice) F(CreateSwapchainKHR) F(QueuePresentKHR) F(GetDeviceQueue) F(GetDeviceQueue2) \
    F(QueueSubmit) F(QueueSubmit2) F(QueueSubmit2KHR) F(QueueBindSparse) F(QueueWaitIdle) F(DeviceWaitIdle)
// Resolved for the VR presenter but not intercepted.
#define PRESENTER_FUNCTIONS(F) \
    F(CreateCommandPool) F(AllocateCommandBuffers) F(BeginCommandBuffer) F(EndCommandBuffer) \
    F(CmdPipelineBarrier) F(CmdCopyImage) F(CmdBlitImage) F(CreateSemaphore) F(CreateFence) \
    F(WaitForFences) F(ResetFences) F(GetSwapchainImagesKHR) F(CmdClearColorImage) \
    F(CmdCopyImageToBuffer) F(CmdCopyBufferToImage) F(GetBufferMemoryRequirements) F(GetFenceStatus) F(DestroyFence) F(DestroyCommandPool) \
    F(CreateBuffer) F(AllocateMemory) F(BindBufferMemory) F(MapMemory) F(UnmapMemory)
#define MEMBER(name) PFN_vk##name name{};
    DEVICE_FUNCTIONS(MEMBER)
    PRESENTER_FUNCTIONS(MEMBER)
#undef MEMBER
    VkInstance instance{};
    VkPhysicalDevice physical{};
    PFN_vkSetDeviceLoaderData set_loader_data{};
    PFN_vkGetInstanceProcAddr gipa{};
    std::shared_ptr<x4vr::RuntimeBootstrap> runtime;
    VkQueue vr_queue{}; // queue for VR submission (null if unavailable)
    uint32_t vr_family = UINT32_MAX, vr_index{};
    bool vr_queue_shared{}; // vr_queue is X4's own queue (SharedQueue)
};
std::mutex state_mutex;
// Explicit vkDestroyInstance performs runtime shutdown. The registry is never destroyed (as on
// Windows): no VR shutdown from static destruction if X4 exits without destroying its instances.
auto& instances = *new std::unordered_map<void*, Instance>;
auto& devices = *new std::unordered_map<void*, Device>;
template<class Object> std::optional<Instance> instance_for(Object o) {
    std::lock_guard lock(state_mutex);
    auto found = instances.find(key(o));
    return found == instances.end() ? std::nullopt : std::optional(found->second);
}
template<class Object> std::optional<Device> device_for(Object o) {
    std::lock_guard lock(state_mutex);
    auto found = devices.find(key(o));
    return found == devices.end() ? std::nullopt : std::optional(found->second);
}
template<class Chain> Chain* link_info(const void* next, VkStructureType type) {
    while (next) {
        const auto* base = static_cast<const VkBaseInStructure*>(next);
        if (base->sType == type) {
            auto* chain = const_cast<Chain*>(static_cast<const Chain*>(next));
            if (chain->function == VK_LAYER_LINK_INFO) return chain;
        }
        next = base->pNext;
    }
    return nullptr;
}
std::string join(uint32_t count, const char* const* names) {
    std::string out;
    for (uint32_t i = 0; i < count; ++i) out += std::string(i ? " " : "")+names[i];
    return out;
}

// Vulkan queues need external synchronisation. When the VR queue is X4's own (no spare graphics
// queue), every use of it goes through this lock: X4's submits and presents (intercepted below), the
// layer's own submissions, and the runtime's (SteamVR reaches the queue through the loader, so
// through these intercepts; the submission thread also holds the lock around the runtime calls
// that use the queue). Recursive: a runtime call made under the lock submits through the intercept.
std::recursive_mutex shared_queue;
std::atomic<VkQueue> shared_queue_handle{};
std::unique_lock<std::recursive_mutex> lock_if_shared(VkQueue queue) {
    return queue && queue == shared_queue_handle.load() ? std::unique_lock(shared_queue) : std::unique_lock<std::recursive_mutex>();
}
std::unique_lock<std::recursive_mutex> lock_vr_queue(const Device& d) {
    return d.vr_queue_shared ? std::unique_lock(shared_queue) : std::unique_lock<std::recursive_mutex>();
}

// The layer's code runs on its own threads until X4 exits, but the loader unloads a layer with
// the last instance, and X4 creates and destroys instances while starting up.
void pin_library() {
    static std::once_flag once;
    std::call_once(once, [] {
        Dl_info info{};
        if (dladdr(reinterpret_cast<void*>(&pin_library), &info) && info.dli_fname &&
            dlopen(info.dli_fname, RTLD_NOW | RTLD_NOLOAD | RTLD_NODELETE))
            log("X4VR layer: library pinned for the process lifetime");
        else
            log("X4VR layer: could not pin the library");
    });
}
// Windows: THREAD_PRIORITY_TIME_CRITICAL. Linux: real-time FIFO if the system allows it (rtkit or
// an RLIMIT_RTPRIO allowance), else a higher nice value; neither is required.
void raise_thread_priority() {
    sched_param param{};
    param.sched_priority = std::min(10, sched_get_priority_max(SCHED_FIFO));
    if (!pthread_setschedparam(pthread_self(), SCHED_FIFO, &param)) { log("X4VR presenter: submission thread at real-time priority (SCHED_FIFO)"); return; }
    if (!setpriority(PRIO_PROCESS, 0, -10)) { log("X4VR presenter: submission thread at nice -10"); return; }
    log("X4VR presenter: submission thread at normal priority (no real-time or nice permission)");
}
}

EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL x4vr_GetInstanceProcAddr(VkInstance, const char*);
EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL x4vr_GetDeviceProcAddr(VkDevice, const char*);

namespace {
VKAPI_ATTR VkResult VKAPI_CALL CreateInstance(const VkInstanceCreateInfo* ci, const VkAllocationCallbacks* alloc, VkInstance* output) {
    auto* chain = link_info<VkLayerInstanceCreateInfo>(ci->pNext, VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO);
    if (!chain || !chain->u.pLayerInfo) return VK_ERROR_INITIALIZATION_FAILED;
    const auto gipa = chain->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    const auto gpdpa = chain->u.pLayerInfo->pfnNextGetPhysicalDeviceProcAddr;
    const auto create = reinterpret_cast<PFN_vkCreateInstance>(gipa(nullptr, "vkCreateInstance"));
    if (!create) return VK_ERROR_INITIALIZATION_FAILED;
    std::shared_ptr<x4vr::RuntimeBootstrap> runtime;
    std::optional<x4vr::VulkanExtensions> extensions;
    std::vector<const char*> names;
    auto augmented = *ci;
    try {
        if (x4vr::linux_port::is_x4_process() && !x4vr::is_runtime_bootstrap_thread()) {
            pin_library();
            runtime = x4vr::acquire_runtime_bootstrap();
            extensions.emplace(ci->enabledExtensionCount, ci->ppEnabledExtensionNames, runtime->instance_extensions());
            names = extensions->names();
            augmented.enabledExtensionCount = static_cast<uint32_t>(names.size());
            augmented.ppEnabledExtensionNames = names.data();
            x4vr::linux_port::start_pose_sender();
        }
    } catch (const std::exception& error) {
        // No headset or no SteamVR: X4 runs flat, as without the mod.
        log(std::string("X4VR layer: VR runtime unavailable, X4 runs without VR: ")+error.what());
        runtime.reset();
        augmented = *ci;
    }
    chain->u.pLayerInfo = chain->u.pLayerInfo->pNext;
    const auto result = create(&augmented, alloc, output);
    if (result != VK_SUCCESS) return result;
    try {
        std::lock_guard lock(state_mutex);
        instances.emplace(key(*output), Instance{*output, gipa, gpdpa, runtime});
    } catch (...) {
        reinterpret_cast<PFN_vkDestroyInstance>(gipa(*output, "vkDestroyInstance"))(*output, alloc);
        *output = VK_NULL_HANDLE; return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    if (runtime) log("X4VR layer: instance created with extensions: "+join(augmented.enabledExtensionCount, augmented.ppEnabledExtensionNames));
    return result;
}
VKAPI_ATTR void VKAPI_CALL DestroyInstance(VkInstance instance, const VkAllocationCallbacks* alloc) {
    auto data = instance_for(instance); if (!data) return;
    const auto destroy = reinterpret_cast<PFN_vkDestroyInstance>(data->gipa(instance, "vkDestroyInstance"));
    { std::lock_guard lock(state_mutex); instances.erase(key(instance)); }
    data->runtime.reset(); // the runtime itself stays pinned (acquire_runtime_bootstrap)
    destroy(instance, alloc);
}
// The loader's own vkGetPhysicalDeviceProperties2 (for OpenVR's loader-facing physical device).
PFN_vkGetPhysicalDeviceProperties2 public_properties2() {
    void* loader = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_NOLOAD);
    return loader ? reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(dlsym(loader, "vkGetPhysicalDeviceProperties2")) : nullptr;
}
VKAPI_ATTR VkResult VKAPI_CALL CreateDevice(VkPhysicalDevice physical, const VkDeviceCreateInfo* ci,
                                            const VkAllocationCallbacks* alloc, VkDevice* output) {
    const auto parent = instance_for(physical); if (!parent) return VK_ERROR_INITIALIZATION_FAILED;
    auto* chain = link_info<VkLayerDeviceCreateInfo>(ci->pNext, VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO);
    if (!chain || !chain->u.pLayerInfo) return VK_ERROR_INITIALIZATION_FAILED;
    const auto gipa = chain->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    const auto gdpa = chain->u.pLayerInfo->pfnNextGetDeviceProcAddr;
    const auto create = reinterpret_cast<PFN_vkCreateDevice>(gipa(parent->instance, "vkCreateDevice"));
    if (!create) return VK_ERROR_INITIALIZATION_FAILED;
    auto runtime = parent->runtime;
    std::optional<x4vr::VulkanExtensions> extensions;
    std::vector<const char*> names;
    auto augmented = *ci;
    // One extra queue in the game's graphics family for the submission thread, which must not
    // share the game's queues (Vulkan queues need external synchronization). Without a spare one,
    // the game's first graphics queue is shared (SharedQueue).
    std::vector<VkDeviceQueueCreateInfo> queues(ci->pQueueCreateInfos, ci->pQueueCreateInfos+ci->queueCreateInfoCount);
    std::vector<float> priorities;
    uint32_t vr_family = UINT32_MAX, vr_index = 0;
    bool shared = false;
    if (runtime) {
        const auto family_properties = reinterpret_cast<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(
            parent->gipa(parent->instance, "vkGetPhysicalDeviceQueueFamilyProperties"));
        uint32_t count{};
        family_properties(physical, &count, nullptr);
        std::vector<VkQueueFamilyProperties> families(count);
        family_properties(physical, &count, families.data());
        for (auto& q : queues) {
            if (q.queueFamilyIndex >= count || !(families[q.queueFamilyIndex].queueFlags & VK_QUEUE_GRAPHICS_BIT) ||
                q.flags || q.queueCount >= families[q.queueFamilyIndex].queueCount) continue;
            priorities.assign(q.pQueuePriorities, q.pQueuePriorities+q.queueCount);
            priorities.push_back(1.f);
            vr_family = q.queueFamilyIndex; vr_index = q.queueCount;
            q.queueCount += 1; q.pQueuePriorities = priorities.data();
            augmented.pQueueCreateInfos = queues.data();
            break;
        }
        if (vr_family == UINT32_MAX)
            for (const auto& q : queues)
                if (q.queueFamilyIndex < count && (families[q.queueFamilyIndex].queueFlags & VK_QUEUE_GRAPHICS_BIT) && !q.flags && q.queueCount) {
                    vr_family = q.queueFamilyIndex; vr_index = 0; shared = true;
                    break;
                }
    }
    try {
        if (runtime) {
            // GetOutputDevice returns a loader-facing physical handle; this hook
            // receives the next layer's handle. Compare GPU UUIDs, not wrappers.
            const auto headset_physical = runtime->output_device(parent->instance);
            const auto loader_properties = public_properties2();
            const auto next_properties = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(
                parent->gipa(parent->instance, "vkGetPhysicalDeviceProperties2"));
            if (!loader_properties || !next_properties) throw std::runtime_error("GPU identity query unavailable");
            VkPhysicalDeviceIDProperties headset_id{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
            VkPhysicalDeviceIDProperties game_id{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
            VkPhysicalDeviceProperties2 headset_properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
            VkPhysicalDeviceProperties2 game_properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
            headset_properties.pNext = &headset_id; game_properties.pNext = &game_id;
            loader_properties(headset_physical, &headset_properties);
            next_properties(physical, &game_properties);
            const uint8_t empty_uuid[VK_UUID_SIZE]{};
            if (!std::memcmp(game_id.deviceUUID, empty_uuid, VK_UUID_SIZE) ||
                std::memcmp(game_id.deviceUUID, headset_id.deviceUUID, VK_UUID_SIZE))
                throw std::runtime_error("X4 selected a GPU different from the headset's GPU ("+
                                         std::string(game_properties.properties.deviceName)+" vs "+headset_properties.properties.deviceName+")");
            extensions.emplace(ci->enabledExtensionCount, ci->ppEnabledExtensionNames, runtime->device_extensions(headset_physical));
            names = extensions->names();
            augmented.enabledExtensionCount = static_cast<uint32_t>(names.size());
            augmented.ppEnabledExtensionNames = names.data();
        }
    } catch (const std::exception& error) {
        log(std::string("X4VR layer: VR disabled for this device: ")+error.what());
        runtime.reset();
        augmented = *ci;
        vr_family = UINT32_MAX; shared = false;
    }
    chain->u.pLayerInfo = chain->u.pLayerInfo->pNext;
    const auto result = create(physical, &augmented, alloc, output);
    if (result != VK_SUCCESS) return result;
    Device data; data.device = *output; data.gdpa = gdpa;
    data.instance = parent->instance; data.physical = physical; data.gipa = parent->gipa; data.runtime = runtime;
    for (auto* next = static_cast<const VkBaseInStructure*>(ci->pNext); next; next = next->pNext)
        if (next->sType == VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO &&
            reinterpret_cast<const VkLayerDeviceCreateInfo*>(next)->function == VK_LOADER_DATA_CALLBACK)
            data.set_loader_data = reinterpret_cast<const VkLayerDeviceCreateInfo*>(next)->u.pfnSetDeviceLoaderData;
    reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(parent->gipa(parent->instance, "vkGetPhysicalDeviceMemoryProperties"))(physical, &data.memory_properties);
#define RESOLVE(name) data.name = reinterpret_cast<PFN_vk##name>(gdpa(*output, "vk" #name));
    DEVICE_FUNCTIONS(RESOLVE)
    PRESENTER_FUNCTIONS(RESOLVE)
#undef RESOLVE
    if (vr_family != UINT32_MAX && data.GetDeviceQueue) {
        data.GetDeviceQueue(*output, vr_family, vr_index, &data.vr_queue);
        // A queue the layer added needs the loader's dispatch pointer; X4's own already has it.
        if (data.vr_queue && (shared || (data.set_loader_data && data.set_loader_data(*output, data.vr_queue) == VK_SUCCESS))) {
            data.vr_family = vr_family; data.vr_index = vr_index; data.vr_queue_shared = shared;
            if (shared) shared_queue_handle = data.vr_queue;
        }
        else data.vr_queue = VK_NULL_HANDLE;
    }
    try { std::lock_guard lock(state_mutex); devices.emplace(key(*output), data); }
    catch (...) { data.DestroyDevice(*output, alloc); *output = VK_NULL_HANDLE; return VK_ERROR_OUT_OF_HOST_MEMORY; }
    if (runtime) {
        VkPhysicalDeviceProperties properties{};
        reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(parent->gipa(parent->instance, "vkGetPhysicalDeviceProperties"))(physical, &properties);
        log(std::string("X4VR layer: device created on ")+properties.deviceName+", VR queue: "+
            (!data.vr_queue ? "none" : shared ? "shared with X4 (family "+std::to_string(vr_family)+")"
                                              : "private (family "+std::to_string(vr_family)+" index "+std::to_string(vr_index)+")")+
            "; extensions: "+join(augmented.enabledExtensionCount, augmented.ppEnabledExtensionNames));
    }
    return result;
}
}
void stop_submission(VkDevice device);
namespace {
VKAPI_ATTR void VKAPI_CALL DestroyDevice(VkDevice device, const VkAllocationCallbacks* alloc) {
    const auto data = device_for(device); if (!data) return;
    stop_submission(device); // it submits on this device's queue: X4 crashed on exit without this
    if (data->runtime) data->runtime->end_session(device);
    { std::lock_guard lock(state_mutex); devices.erase(key(device)); }
    if (data->vr_queue_shared) shared_queue_handle = VK_NULL_HANDLE;
    data->DestroyDevice(device, alloc);
}
}

// Alternate-eye VR presenter. Each presented game frame rendered one eye (the pose
// source advances the shared eye counter once per game frame); copy it into that
// eye's texture, then submit both eye textures to OpenVR with frustum bounds.
struct Presenter {
    std::timed_mutex mutex; // the submission thread waits for it with a deadline
    // Own lock, not `mutex`: an OpenXR runtime fetches its queue through our vkGetDeviceQueue
    // hook inside xrCreateSession, which runs while the present holds `mutex`.
    std::mutex families_mutex;
    std::unordered_map<VkQueue, uint32_t> families;
    uint32_t family_of(VkQueue queue) {
        std::lock_guard lock(families_mutex);
        const auto found = families.find(queue);
        return found == families.end() ? UINT32_MAX : found->second;
    }
    VkSwapchainKHR swapchain{};
    std::vector<VkImage> images;
    VkExtent2D extent{};
    VkFormat format{};
    // Eye textures are padded so they span each eye's whole frustum; the game image sits
    // centred (scaled to `placed`), the rest stays black. Linux: at the pixels per tangent SteamVR
    // recommends (its render resolution setting), at most the game's own (no upscaling).
    VkExtent2D eye_extent{}, placed{};
    VkOffset3D offset{};
    float span_x{}, span_y{};
    // Each eye rotates through a ring of texture objects, so the submission thread can keep
    // showing one image while the game writes the next.
    static constexpr uint32_t ring_size = 3;
    std::array<std::unique_ptr<x4vr::EyeTargets>, ring_size> targets;
    std::array<uint32_t, 2> current{}; // ring slot holding each eye's latest image
    std::array<std::array<bool, ring_size>, 2> filled{}; // slot written at least once
    VkImage image(uint32_t eye, uint32_t slot) const { return targets[slot]->eyes()[eye].color.image; }
    bool has_image(uint32_t eye) const { return filled[eye][current[eye]]; }
    // Asynchronous submission (compositor_loop): per ring image, a fence signalled once the
    // copy into it completed and the pose it was rendered with. Images the submission thread
    // holds (submitted last, or being submitted) are never overwritten.
    std::array<std::array<VkFence, ring_size>, 2> written{};
    std::array<std::array<x4vr::Matrix, ring_size>, 2> slot_pose{};
    std::array<std::array<bool, ring_size>, 2> held{};
    std::array<std::array<uint64_t, ring_size>, 2> slot_seq{}; // copy order, to find the newest finished image
    uint64_t copies{};
    uint64_t generation{}; // bumped when the ring images are rebuilt
    bool async_frame{}; // the frame just copied is submitted by the thread, not inline
    // Theater mode: the flat game image goes to eye 0's ring and is shown on a virtual screen.
    bool theater{};
    uint32_t flat_run{}, stereo_run{}; // consecutive frames wanting the other mode
    // The submission thread; vkDestroyDevice stops it before the device goes away.
    std::thread thread;
    VkDevice thread_device{};
    std::atomic_bool stopping{};
    // Replaced on swapchain resize but never freed: the runtime may still read the last
    // submitted textures and in-flight copies may reference them.
    std::vector<std::unique_ptr<x4vr::EyeTargets>> retired;
    VkCommandPool pool{};
    std::array<VkCommandBuffer, 3> commands{};
    std::array<VkFence, 3> fences{};
    std::array<VkSemaphore, 3> copied{};
    std::array<bool, 2> fresh{}; // eye texture rewritten since the last submission
    std::array<x4vr::Matrix, 2> poses{}; // head pose each eye texture was rendered with
    VkPhysicalDevice output_physical{};
    x4vr::RuntimeBootstrap::EyeSetup eyes{};
    uint64_t frame{}, last_number{};
    uint32_t last_eye{};
    bool failed{}, ready{};
    // X4's mouse cursor (x11_cursor.hpp), copied into the eye images: one upload buffer per
    // command slot, reused once that slot's fence signalled.
    std::array<VkBuffer, 3> cursor_buffer{};
    std::array<VkDeviceMemory, 3> cursor_memory{};
    std::array<void*, 3> cursor_mapped{};
    // Diagnostic readback of both eye textures, requested by creating dump.txt.
    VkBuffer dump_buffer{};
    VkDeviceMemory dump_memory{};
    bool dump_pending{};
};
namespace {
std::atomic_uint64_t present_count{};
std::atomic_uint64_t late_frames{}; // game frames released at once because a compositor tick had passed
void check(VkResult result, const char* what) {
    if (result != VK_SUCCESS) throw std::runtime_error(std::string(what)+" failed: "+std::to_string(result));
}
std::filesystem::path capture_root() {
    const char* root = std::getenv("X4VR_CAPTURE_DIR");
    return root ? std::filesystem::path(root) : std::filesystem::path();
}
void readback_prepare(const Device& d, VkDeviceSize size, VkBuffer& buffer, VkDeviceMemory& memory,
                      VkBufferUsageFlags usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT) {
    VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    info.size = size; info.usage = usage;
    check(d.CreateBuffer(d.device, &info, nullptr, &buffer), "readback buffer");
    VkMemoryRequirements need{};
    d.GetBufferMemoryRequirements(d.device, buffer, &need);
    VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocate.allocationSize = need.size;
    const auto wanted = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    for (uint32_t i = 0; i < d.memory_properties.memoryTypeCount; ++i)
        if ((need.memoryTypeBits & (1u << i)) && (d.memory_properties.memoryTypes[i].propertyFlags & wanted) == wanted) { allocate.memoryTypeIndex = i; break; }
    check(d.AllocateMemory(d.device, &allocate, nullptr, &memory), "readback memory");
    check(d.BindBufferMemory(d.device, buffer, memory, 0), "readback bind");
}
// Writes eye-0.raw / eye-1.raw (BGRA8, eye_extent) after the slot's fence signalled.
void dump_write(const Device& d, Presenter& p, uint32_t slot) {
    check(d.WaitForFences(d.device, 1, &p.fences[slot], VK_TRUE, UINT64_MAX), "dump wait");
    void* data{};
    const auto bytes = size_t(p.eye_extent.width)*p.eye_extent.height*4;
    check(d.MapMemory(d.device, p.dump_memory, 0, VK_WHOLE_SIZE, 0, &data), "dump map");
    const auto root = capture_root();
    for (int i = 0; i < 2; ++i) {
        std::ofstream out(root/("eye-"+std::to_string(i)+".raw"), std::ios::binary);
        out.write(static_cast<const char*>(data)+i*bytes, std::streamsize(bytes));
    }
    d.UnmapMemory(d.device, p.dump_memory);
    std::ofstream info(root/"eye-dump.txt");
    info << p.eye_extent.width << ' ' << p.eye_extent.height << ' ' << p.format << ' ' << p.offset.x << ' ' << p.offset.y
         << ' ' << p.span_x << ' ' << p.span_y << '\n';
    for (const auto& pose : p.poses) { // submitted (rendered) head pose per eye, row-major 3x4
        for (int r = 0; r < 3; ++r) for (int c = 0; c < 4; ++c) info << pose.m[r][c] << ' ';
        info << '\n';
    }
    log("X4VR presenter: eye textures dumped");
}
Presenter& presenter() { static auto* value = new Presenter; return *value; }
}
void compositor_loop(Device d);
namespace {
void presenter_initialize(const Device& d, uint32_t family) {
    auto& p = presenter();
    const auto memory = reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(d.gipa(d.instance, "vkGetPhysicalDeviceMemoryProperties"));
    const auto image_properties = reinterpret_cast<PFN_vkGetPhysicalDeviceImageFormatProperties>(d.gipa(d.instance, "vkGetPhysicalDeviceImageFormatProperties"));
    p.eyes = d.runtime->eye_setup();
    const float tan_y = x4vr::stereo_settings().game_tan_y;
    const float tan_x = tan_y*float(p.extent.width)/float(p.extent.height);
    p.span_x = tan_x; p.span_y = tan_y;
    for (const auto& t : p.eyes.tangents) {
        p.span_x = std::fmax(p.span_x, std::fmax(std::fabs(t[0]), std::fabs(t[1])));
        p.span_y = std::fmax(p.span_y, std::fmax(std::fabs(t[2]), std::fabs(t[3])));
    }
    // Pixels per tangent: the game's, or SteamVR's recommendation over each eye's frustum if lower.
    const float game_x = float(p.extent.width)/(2*tan_x), game_y = float(p.extent.height)/(2*tan_y);
    float want_x = 0, want_y = 0;
    uint32_t recommended_w = 0, recommended_h = 0;
    if (const char* native = std::getenv("X4VR_NATIVE_SIZE"); native && *native == '1')
        log("X4VR presenter: X4VR_NATIVE_SIZE=1: eye images at the game's own pixel density");
    else if (auto* system = d.runtime->openxr() ? nullptr : vr::VRSystem()) system->GetRecommendedRenderTargetSize(&recommended_w, &recommended_h);
    else log("X4VR presenter: SteamVR's recommended size unavailable; eye images at the game's own pixel density");
    for (const auto& t : p.eyes.tangents) {
        if (t[1] > t[0]) want_x = std::fmax(want_x, float(recommended_w)/(t[1]-t[0]));
        if (std::fabs(t[3]-t[2]) > 0) want_y = std::fmax(want_y, float(recommended_h)/std::fabs(t[3]-t[2]));
    }
    const float density_x = want_x > 0 ? std::fmin(want_x, game_x) : game_x, density_y = want_y > 0 ? std::fmin(want_y, game_y) : game_y;
    p.placed = {uint32_t(std::lround(2*tan_x*density_x)), uint32_t(std::lround(2*tan_y*density_y))};
    p.eye_extent = {uint32_t(std::ceil(2*p.span_x*density_x)), uint32_t(std::ceil(2*p.span_y*density_y))};
    p.eye_extent.width = std::max(p.eye_extent.width, p.placed.width); p.eye_extent.height = std::max(p.eye_extent.height, p.placed.height);
    p.offset = {int32_t(p.eye_extent.width-p.placed.width)/2, int32_t(p.eye_extent.height-p.placed.height)/2, 0};
    if (recommended_w) {
        std::ostringstream size;
        size << "X4VR presenter: SteamVR recommends " << recommended_w << 'x' << recommended_h << " per eye; game image "
             << p.extent.width << 'x' << p.extent.height << " placed at " << p.placed.width << 'x' << p.placed.height;
        const uint32_t ideal_w = uint32_t(std::lround(2*tan_x*want_x)), ideal_h = uint32_t(std::lround(2*tan_y*want_y));
        if (ideal_w < p.extent.width) size << " (X4 renders more than SteamVR uses: about " << ideal_w << 'x' << ideal_h << " would be enough)";
        else size << " (SteamVR would use up to " << ideal_w << 'x' << ideal_h << ")";
        log(size.str());
        // For the next start: x4vr-run (fix-settings) sets X4's resolution from this while X4 is closed.
        if (const char* dir = std::getenv("X4VR_CAPTURE_DIR"); dir && *dir) {
            std::ofstream out(std::string(dir)+"/x4_resolution.txt", std::ios::trunc);
            out << ideal_w << 'x' << ideal_h << '\n';
        }
    }
    // ponytail: EyeTargets also allocates an unused depth image per eye; fine for 3 slots
    for (auto& set : p.targets) set = x4vr::EyeTargets::create({d.device, d.physical, d.gdpa, memory, image_properties}, p.eye_extent, p.format);
    p.filled = {}; p.current = {}; p.held = {}; ++p.generation;
    for (auto& eye : p.written) for (auto& fence : eye) if (!fence) {
        VkFenceCreateInfo signalled{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO}; signalled.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        check(d.CreateFence(d.device, &signalled, nullptr, &fence), "vkCreateFence");
    }
    if (!p.pool) {
    VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT; pool.queueFamilyIndex = family;
    check(d.CreateCommandPool(d.device, &pool, nullptr, &p.pool), "vkCreateCommandPool");
    VkCommandBufferAllocateInfo allocate{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    allocate.commandPool = p.pool; allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; allocate.commandBufferCount = 3;
    check(d.AllocateCommandBuffers(d.device, &allocate, p.commands.data()), "vkAllocateCommandBuffers");
    for (int i = 0; i < 3; ++i) {
        // Layer-created dispatchable objects need the loader's dispatch pointer.
        if (d.set_loader_data) check(d.set_loader_data(d.device, p.commands[i]), "vkSetDeviceLoaderData");
        VkFenceCreateInfo fence{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO}; fence.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        check(d.CreateFence(d.device, &fence, nullptr, &p.fences[i]), "vkCreateFence");
        VkSemaphoreCreateInfo semaphore{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        check(d.CreateSemaphore(d.device, &semaphore, nullptr, &p.copied[i]), "vkCreateSemaphore");
    }
    p.output_physical = d.runtime->output_device(d.instance);
    for (uint32_t i = 0; i < 3; ++i) { // cursor upload buffers: 256x256 BGRA, X11's cursor size limit here
        readback_prepare(d, 256*256*4, p.cursor_buffer[i], p.cursor_memory[i], VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
        check(d.MapMemory(d.device, p.cursor_memory[i], 0, VK_WHOLE_SIZE, 0, &p.cursor_mapped[i]), "cursor buffer map");
    }
    x4vr::linux_port::start_cursor_reader();
    }
    std::ostringstream s;
    s << "X4VR presenter: eye targets ready (" << p.extent.width << 'x' << p.extent.height << " game image, "
      << p.eye_extent.width << 'x' << p.eye_extent.height << " per eye, format " << p.format << ", eyes at x="
      << p.eyes.head_from_eye[0].m[0][3] << ',' << p.eyes.head_from_eye[1].m[0][3] << "); alternate-eye submission active";
    log(s.str());
    for (int e = 0; e < 2; ++e) { // per-eye optics: frustum tangents and head-from-eye (rotation = display cant)
        std::ostringstream o;
        const auto& t = p.eyes.tangents[e];
        const auto& m = p.eyes.head_from_eye[e].m;
        o << "X4VR presenter: eye " << e << " tangents l,r,t,b " << t[0] << ',' << t[1] << ',' << t[2] << ',' << t[3]
          << "; head_from_eye rows";
        for (int r = 0; r < 3; ++r) o << " [" << m[r][0] << ' ' << m[r][1] << ' ' << m[r][2] << ' ' << m[r][3] << ']';
        log(o.str());
    }
    p.ready = true;
    static std::once_flag started;
    if (d.vr_queue) std::call_once(started, [&] {
        p.thread_device = d.device;
        p.thread = std::thread(compositor_loop, d);
        log("X4VR presenter: asynchronous submission thread started");
    });
}
void barrier(const Device& d, VkCommandBuffer command, VkImage image, VkImageLayout from, VkImageLayout to,
             VkAccessFlags src, VkAccessFlags dst) {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.oldLayout = from; b.newLayout = to; b.srcAccessMask = src; b.dstAccessMask = dst;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image; b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    d.CmdPipelineBarrier(command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0,
                         0, nullptr, 0, nullptr, 1, &b);
}
// Theater mode: fullscreen menus and frames without a head pose (main menu, loading, modes that
// don't poll the head tracker) go to a virtual screen. Switching takes 30 flat (~0.25 s; Windows:
// 10, too few for X4's pop-up hints on Linux) or 3 stereo frames in a row, so one missing pose or
// a brief pop-up doesn't flash the screen. Returns whether this frame is shown;
// frames that don't match the current mode are skipped.
bool update_theater(Presenter& p, const x4vr::StereoSettings& s, bool posed, bool flat) {
    const bool want = s.theater == 2 || (s.theater == 1 && (!posed || flat));
    if (want == p.theater) p.flat_run = p.stereo_run = 0;
    else if (++(want ? p.flat_run : p.stereo_run) < (want ? 30u : 3u)) return false;
    else {
        p.theater = want; p.flat_run = p.stereo_run = 0;
        log(!want ? "X4VR theater: off" : !posed ? "X4VR theater: on (no head pose)"
            : flat ? "X4VR theater: on (fullscreen menu)" : "X4VR theater: on (forced)");
    }
    return p.theater || posed;
}
// Copies the cursor's pixels (alpha >= 128; a transfer can't blend) into a game image at the
// pointer's place, row runs as buffer-to-image regions. `image` is in TRANSFER_DST layout.
// The cursor keeps its own pixel size; the game image may be scaled to `placed`.
void draw_cursor(const Device& d, const Presenter& p, VkCommandBuffer command, VkImage image, uint32_t slot,
                 const x4vr::linux_port::CursorState& c) {
    const int x0 = p.offset.x+int(std::lround(double(c.x)*p.placed.width/c.window_w))-int(c.xhot);
    const int y0 = p.offset.y+int(std::lround(double(c.y)*p.placed.height/c.window_h))-int(c.yhot);
    std::vector<VkBufferImageCopy> regions;
    for (uint32_t row = 0; row < c.height; ++row) {
        const int y = y0+int(row);
        if (y < 0 || y >= int(p.eye_extent.height)) continue;
        for (uint32_t col = 0; col < c.width;) {
            if ((c.bgra[size_t(row)*c.width+col] >> 24) < 128) { ++col; continue; }
            uint32_t end = col;
            while (end < c.width && (c.bgra[size_t(row)*c.width+end] >> 24) >= 128) ++end;
            const int from = std::max(x0+int(col), 0), to = std::min(x0+int(end), int(p.eye_extent.width));
            if (from < to) {
                VkBufferImageCopy r{};
                r.bufferOffset = (VkDeviceSize(row)*c.width+uint32_t(from-x0))*4;
                r.bufferRowLength = c.width;
                r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                r.imageOffset = {from, y, 0};
                r.imageExtent = {uint32_t(to-from), 1, 1};
                regions.push_back(r);
            }
            col = end;
        }
    }
    if (regions.empty()) return;
    barrier(d, command, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT); // after the game image's copy
    d.CmdCopyBufferToImage(command, p.cursor_buffer[slot], image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, uint32_t(regions.size()), regions.data());
}
// Returns the semaphore the real present must wait on, or null to present unchanged.
// Runs on the present queue, under the shared-queue lock if that queue is shared.
VkSemaphore presenter_copy(const Device& d, VkQueue queue, const VkPresentInfoKHR* info) {
    auto& p = presenter();
    if (p.failed || !d.runtime || info->swapchainCount != 1 || info->pSwapchains[0] != p.swapchain) return VK_NULL_HANDLE;
    const auto family = p.family_of(queue);
    if (family == UINT32_MAX || info->pImageIndices[0] >= p.images.size()) return VK_NULL_HANDLE;
    if (!p.ready) presenter_initialize(d, family);
    const auto settings = x4vr::stereo_settings();
    const auto number = x4vr::next_present();
    {
        // Stage C check: X4's frame half should flip on every present (logged after 1000, then every 20000).
        static int last_half = -1;
        static uint64_t seen{}, flips{};
        const int half = x4vr::frame_half();
        if (half >= 0) {
            if (last_half >= 0) { ++seen; flips += half != last_half; }
            last_half = half;
            if (seen == 1000 || (seen && seen % 20000 == 0))
                log("X4VR presenter: frame half flipped on "+std::to_string(flips)+" of "+std::to_string(seen)+" presents (stage C check)");
        }
    }
    p.last_number = number;
    uint32_t eye{};
    auto rendered = x4vr::Matrix::identity();
    bool flat{}, walking{};
    const bool posed = x4vr::presented_frame(number, eye, rendered, flat, walking);
    if (posed) x4vr::trace_event('S', number*2+eye, rendered.m[0][2], rendered.m[1][2], rendered.m[2][2]); // submitted head z axis
    if (!update_theater(p, settings, posed, flat)) return VK_NULL_HANDLE; // skip frame
    if (!settings.stereo || p.theater) eye = 0;
    p.last_eye = eye;
    p.async_frame = settings.async_submit && settings.pace && d.vr_queue && d.vr_family == family;
    constexpr auto ring = Presenter::ring_size;
    std::array<uint32_t, 2> slot_of{UINT32_MAX, UINT32_MAX}; // ring image each written eye goes to
    for (uint32_t target = 0; target < 2; ++target) {
        if (settings.stereo && target != eye) continue; // mono mode fills both eyes
        for (uint32_t step = 1; step <= ring && slot_of[target] == UINT32_MAX; ++step) {
            const auto s = (p.current[target]+step) % ring; // never the newest image or one the thread holds
            if (s != p.current[target] && !p.held[target][s]) slot_of[target] = s;
        }
        if (slot_of[target] == UINT32_MAX) return VK_NULL_HANDLE; // every image in use: drop this frame
    }
    for (uint32_t target = 0; target < 2; ++target) if (slot_of[target] != UINT32_MAX) {
        check(d.WaitForFences(d.device, 1, &p.written[target][slot_of[target]], VK_TRUE, UINT64_MAX), "vkWaitForFences");
        check(d.ResetFences(d.device, 1, &p.written[target][slot_of[target]]), "vkResetFences");
    }

    const auto slot = p.frame++ % 3;
    check(d.WaitForFences(d.device, 1, &p.fences[slot], VK_TRUE, UINT64_MAX), "vkWaitForFences");
    check(d.ResetFences(d.device, 1, &p.fences[slot]), "vkResetFences");
    const auto command = p.commands[slot];
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    check(d.BeginCommandBuffer(command, &begin), "vkBeginCommandBuffer");
    const auto source = p.images[info->pImageIndices[0]];
    barrier(d, command, source, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    // The cursor for this frame, staged in this slot's buffer (its last use finished: fence above).
    auto cursor = settings.cursor ? x4vr::linux_port::cursor_state() : x4vr::linux_port::CursorState{};
    if (cursor.visible && cursor.window_w && cursor.window_h && p.cursor_mapped[slot]) {
        const bool rgba = p.format == VK_FORMAT_R8G8B8A8_UNORM || p.format == VK_FORMAT_R8G8B8A8_SRGB;
        auto* out = static_cast<uint32_t*>(p.cursor_mapped[slot]);
        for (size_t i = 0; i < cursor.bgra.size(); ++i) {
            const uint32_t v = cursor.bgra[i];
            out[i] = rgba ? (v & 0xff00ff00u) | ((v >> 16) & 0xffu) | ((v & 0xffu) << 16) : v;
        }
    } else cursor.visible = false;
    for (uint32_t target = 0; target < 2; ++target) {
        if (slot_of[target] == UINT32_MAX) continue;
        const auto next = slot_of[target];
        const auto image = p.image(target, next);
        barrier(d, command, image, p.filled[target][next] ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
        if (!p.filled[target][next]) {
            const VkClearColorValue black{};
            const VkImageSubresourceRange all{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            d.CmdClearColorImage(command, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &all);
            barrier(d, command, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
        }
        if (p.placed.width == p.extent.width && p.placed.height == p.extent.height) {
            VkImageCopy copy{};
            copy.srcSubresource = copy.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            copy.dstOffset = p.offset;
            copy.extent = {p.extent.width, p.extent.height, 1};
            d.CmdCopyImage(command, source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        } else { // scaled to SteamVR's pixel density
            VkImageBlit blit{};
            blit.srcSubresource = blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            blit.srcOffsets[1] = {int32_t(p.extent.width), int32_t(p.extent.height), 1};
            blit.dstOffsets[0] = p.offset;
            blit.dstOffsets[1] = {p.offset.x+int32_t(p.placed.width), p.offset.y+int32_t(p.placed.height), 1};
            d.CmdBlitImage(command, source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
        }
        if (cursor.visible) draw_cursor(d, p, command, image, slot, cursor);
        barrier(d, command, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
        p.filled[target][next] = p.fresh[target] = true;
        p.current[target] = next;
        p.slot_seq[target][next] = ++p.copies;
        p.poses[target] = p.slot_pose[target][next] = rendered;
    }
    barrier(d, command, source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
            VK_ACCESS_TRANSFER_READ_BIT, 0);
    p.dump_pending = false;
    if (p.has_image(0) && p.has_image(1) && x4vr::take_request("dump.txt")) {
        if (!p.dump_buffer) readback_prepare(d, VkDeviceSize(p.eye_extent.width)*p.eye_extent.height*4*2, p.dump_buffer, p.dump_memory);
        for (uint32_t i = 0; i < 2; ++i) {
            VkBufferImageCopy region{};
            region.bufferOffset = VkDeviceSize(i)*p.eye_extent.width*p.eye_extent.height*4;
            region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            region.imageExtent = {p.eye_extent.width, p.eye_extent.height, 1};
            d.CmdCopyImageToBuffer(command, p.image(i, p.current[i]), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, p.dump_buffer, 1, &region);
        }
        p.dump_pending = true;
    }
    check(d.EndCommandBuffer(command), "vkEndCommandBuffer");
    std::vector<VkPipelineStageFlags> stages(info->waitSemaphoreCount, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.waitSemaphoreCount = info->waitSemaphoreCount; submit.pWaitSemaphores = info->pWaitSemaphores;
    submit.pWaitDstStageMask = stages.data();
    submit.commandBufferCount = 1; submit.pCommandBuffers = &command;
    submit.signalSemaphoreCount = 1; submit.pSignalSemaphores = &p.copied[slot];
    check(d.QueueSubmit(queue, 1, &submit, p.fences[slot]), "vkQueueSubmit");
    for (uint32_t target = 0; target < 2; ++target) // signals once the copy above completed
        if (slot_of[target] != UINT32_MAX) check(d.QueueSubmit(queue, 0, nullptr, p.written[target][slot_of[target]]), "vkQueueSubmit");
    if (p.dump_pending) dump_write(d, p, static_cast<uint32_t>(slot));
    return p.copied[slot];
}
// Diagnostics, once per submission: every ~2 s one line is appended to pair_stats.txt.
// stale = an eye submitted again without a new image; late = more than ~1.2 headset frames
// since the previous submission (90 Hz); fallback = the newest image's copy missed the
// submit budget, so the previous image was submitted; waited = time from WaitGetPoses to
// Submit (lock and copies); x4_late = game frames released at once because they ended after a
// tick; resubmit = the present hook held the lock past the budget, so the last frame was sent again.
void pair_stats(const x4vr::StereoSettings& s, std::array<bool, 2> fresh, double blocked, double interval, bool failed,
                uint32_t fallbacks = 0, double waited = 0, double submitting = 0, bool resubmit = false) {
    struct Totals { uint32_t submits, stale[2], late, failed, fallbacks, resubmits; double blocked, blocked_max, interval_max, waited_max, submit_max; };
    static Totals t{};
    static auto start = std::chrono::steady_clock::now();
    static bool header{};
    static uint64_t late_seen{};
    ++t.submits; t.failed += failed; t.fallbacks += fallbacks; t.resubmits += resubmit;
    for (int i = 0; i < 2; ++i) t.stale[i] += !fresh[i];
    t.late += interval > 0.0135;
    t.blocked += blocked; t.blocked_max = std::fmax(t.blocked_max, blocked);
    t.interval_max = std::fmax(t.interval_max, interval); t.waited_max = std::fmax(t.waited_max, waited);
    t.submit_max = std::fmax(t.submit_max, submitting);
    const auto now = std::chrono::steady_clock::now();
    if (now-start < std::chrono::seconds(2)) return;
    std::ostringstream out;
    if (!header) { header = true; out << "# tick_ms submits stale_L stale_R late failed fallback blocked_avg_ms blocked_max_ms interval_max_ms waited_max_ms submit_max_ms x4_late resubmit | async pair pair_wait half_xor_render half_xor_present handoff release_late\n"; }
    const auto late = late_frames.load();
    out << milliseconds() << ' ' << t.submits << ' ' << t.stale[0] << ' ' << t.stale[1] << ' ' << t.late << ' ' << t.failed << ' '
        << t.fallbacks << ' ' << std::fixed << std::setprecision(2) << 1000*t.blocked/t.submits << ' ' << 1000*t.blocked_max << ' '
        << 1000*t.interval_max << ' ' << 1000*t.waited_max << ' ' << 1000*t.submit_max << ' ' << late-late_seen << ' ' << t.resubmits
        << " | " << s.async_submit << ' ' << s.pair << ' ' << s.pair_wait << ' ' << s.half_xor_render << ' ' << s.half_xor_present
        << ' ' << s.handoff << ' ' << s.release_late << '\n';
    x4vr::write_file_later(capture_root()/"pair_stats.txt", out.str(), true);
    t = {}; start = now; late_seen = late;
}
// The padded texture spans [-span, span] tangents symmetrically; crop each eye's asymmetric
// frustum out of it. Tangent convention: OpenVR top is negative. GetProjectionRaw's top/bottom
// are flipped relative to texture v (Valve's plugin: vMin = 0.5-0.5*bottom/tanY, vMax = 0.5-0.5*top/tanY).
std::array<vr::VRTextureBounds_t, 2> eye_bounds(const Presenter& p, bool valve) {
    std::array<vr::VRTextureBounds_t, 2> bounds{};
    for (int i = 0; i < 2; ++i) {
        const auto& t = p.eyes.tangents[i];
        bounds[i] = valve ? vr::VRTextureBounds_t{0.5f+0.5f*t[0]/p.span_x, 0.5f-0.5f*t[3]/p.span_y, 0.5f+0.5f*t[1]/p.span_x, 0.5f-0.5f*t[2]/p.span_y}
                          : vr::VRTextureBounds_t{0.5f+0.5f*t[0]/p.span_x, 0.5f+0.5f*t[2]/p.span_y, 0.5f+0.5f*t[1]/p.span_x, 0.5f+0.5f*t[3]/p.span_y};
    }
    return bounds;
}
vr::VRVulkanTextureData_t eye_texture(const Device& d, const Presenter& p, VkImage image, VkQueue queue, uint32_t family) {
    vr::VRVulkanTextureData_t v{};
    v.m_nImage = reinterpret_cast<uint64_t>(image);
    v.m_pDevice = d.device; v.m_pPhysicalDevice = p.output_physical; v.m_pInstance = d.instance;
    v.m_pQueue = queue; v.m_nQueueFamilyIndex = family;
    v.m_nWidth = p.eye_extent.width; v.m_nHeight = p.eye_extent.height; v.m_nFormat = p.format; v.m_nSampleCount = 1;
    return v;
}
void report_submit(const std::string& error) {
    static std::atomic_uint reported{};
    if (!error.empty() && reported++ < 8) log("X4VR presenter: "+error);
    static std::atomic_bool first{};
    if (error.empty() && !first.exchange(true)) log("X4VR presenter: first stereo pair submitted to the VR runtime");
}
// Compositor frame clock, published by whichever path calls WaitGetPoses; paces the game.
struct Ticks {
    std::mutex mutex;
    std::condition_variable changed;
    uint64_t count{};
    uint64_t released{}; // count when the game's render thread was last released (pace_to_compositor)
    std::chrono::steady_clock::time_point start{};
    double period = 1.0/90;
    double tick() { // WaitGetPoses just returned; returns the measured frame interval
        const auto now = std::chrono::steady_clock::now();
        double measured = 0;
        {
            std::lock_guard lock(mutex);
            if (count) {
                measured = std::chrono::duration<double>(now-start).count();
                if (measured > 0.004 && measured < 0.2) period = 0.9*period+0.1*measured;
            }
            start = now; ++count;
        }
        changed.notify_all();
        return measured;
    }
};
Ticks& ticks() { static auto* value = new Ticks; return *value; }
// Pair mode: both eyes are rendered back to back, then submitted together once per
// compositor frame (90 Hz per eye when the game reaches 2x the headset rate). The first eye
// of a pair is held until mid-way through the compositor frame, keeping presents evenly paced.
void wait_mid_frame() {
    auto& t = ticks();
    std::chrono::steady_clock::time_point target;
    {
        std::lock_guard lock(t.mutex);
        if (!t.count) return;
        target = t.start + std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(t.period/2));
    }
    // ponytail: sleep for the bulk, spin the last ~1.5 ms
    while (std::chrono::steady_clock::now() < target-std::chrono::microseconds(1500)) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    while (std::chrono::steady_clock::now() < target) pause_cpu();
}
// The scene both eyes see behind the theater screen: black images, cleared once on the
// submission queue. Same size as the eye textures: switching SteamVR to a tiny texture
// lost the Vulkan device and made its compositor free-run (Windows).
std::unique_ptr<x4vr::EyeTargets> make_black(const Device& d, VkExtent2D extent, VkFormat format) {
    const auto memory = reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(d.gipa(d.instance, "vkGetPhysicalDeviceMemoryProperties"));
    const auto image_properties = reinterpret_cast<PFN_vkGetPhysicalDeviceImageFormatProperties>(d.gipa(d.instance, "vkGetPhysicalDeviceImageFormatProperties"));
    auto black = x4vr::EyeTargets::create({d.device, d.physical, d.gdpa, memory, image_properties}, extent, format);
    VkCommandPoolCreateInfo info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    info.queueFamilyIndex = d.vr_family;
    VkCommandPool pool{};
    check(d.CreateCommandPool(d.device, &info, nullptr, &pool), "vkCreateCommandPool");
    VkFence done{};
    try {
        VkCommandBufferAllocateInfo allocate{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        allocate.commandPool = pool; allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; allocate.commandBufferCount = 1;
        VkCommandBuffer command{};
        check(d.AllocateCommandBuffers(d.device, &allocate, &command), "vkAllocateCommandBuffers");
        if (d.set_loader_data) check(d.set_loader_data(d.device, command), "vkSetDeviceLoaderData");
        VkFenceCreateInfo fence{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        check(d.CreateFence(d.device, &fence, nullptr, &done), "vkCreateFence");
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        check(d.BeginCommandBuffer(command, &begin), "vkBeginCommandBuffer");
        const VkClearColorValue color{};
        const VkImageSubresourceRange all{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        for (const auto& eye : black->eyes()) {
            barrier(d, command, eye.color.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
            d.CmdClearColorImage(command, eye.color.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &color, 1, &all);
            barrier(d, command, eye.color.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
        }
        check(d.EndCommandBuffer(command), "vkEndCommandBuffer");
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.commandBufferCount = 1; submit.pCommandBuffers = &command;
        {
            const auto queue_lock = lock_vr_queue(d);
            check(d.QueueSubmit(d.vr_queue, 1, &submit, done), "vkQueueSubmit");
        }
        check(d.WaitForFences(d.device, 1, &done, VK_TRUE, UINT64_MAX), "vkWaitForFences");
    } catch (...) {
        if (done) d.DestroyFence(d.device, done, nullptr);
        d.DestroyCommandPool(d.device, pool, nullptr);
        throw;
    }
    d.DestroyFence(d.device, done, nullptr);
    d.DestroyCommandPool(d.device, pool, nullptr);
    return black;
}
// Linux: SteamVR accepts the theater overlay but never shows it (black), so the virtual screen is
// drawn into the eye textures instead: the flat image, theater_width wide and theater_distance
// ahead, submitted with the head pose of when the screen was placed. SteamVR's reprojection then
// keeps it fixed in space while the head moves. X4VR_THEATER_OVERLAY=1 uses the overlay again.
// The part of the screen inside one eye's frustum, as blit rectangles (source in the game image,
// target in the eye texture); false when out of view. Vertical tangents as in eye_bounds (valve):
// texture v=0 at raw tangent `bottom`, v=1 at `top`.
bool screen_rects(const std::array<float, 4>& t, float eye_x, float half_w, float half_h, float distance,
                  VkExtent2D target, VkOffset3D game_offset, VkExtent2D game, std::array<VkOffset3D, 2>& from,
                  std::array<VkOffset3D, 2>& to) {
    const float u0 = ((-half_w-eye_x)/distance-t[0])/(t[1]-t[0]), u1 = ((half_w-eye_x)/distance-t[0])/(t[1]-t[0]);
    const float v0 = (half_h/distance-t[3])/(t[2]-t[3]), v1 = (-half_h/distance-t[3])/(t[2]-t[3]); // screen top, bottom
    const float cu0 = std::max(u0, 0.f), cu1 = std::min(u1, 1.f), cv0 = std::max(v0, 0.f), cv1 = std::min(v1, 1.f);
    if (!(u1 > u0 && v1 > v0 && cu1 > cu0 && cv1 > cv0)) return false;
    to = {VkOffset3D{int32_t(cu0*target.width), int32_t(cv0*target.height), 0}, VkOffset3D{int32_t(cu1*target.width), int32_t(cv1*target.height), 1}};
    from = {VkOffset3D{game_offset.x+int32_t((cu0-u0)/(u1-u0)*game.width), game_offset.y+int32_t((cv0-v0)/(v1-v0)*game.height), 0},
            VkOffset3D{game_offset.x+int32_t((cu1-u0)/(u1-u0)*game.width), game_offset.y+int32_t((cv1-v0)/(v1-v0)*game.height), 1}};
    return to[1].x > to[0].x && to[1].y > to[0].y && from[1].x > from[0].x && from[1].y > from[0].y;
}
// Per-frame timeline of the submission thread (diagnostics): creating submit.request in the
// capture folder writes the last 2048 frames to submit_trace.txt. Times in microseconds on the
// steady clock. SteamVR's timing (OpenVR only) is of the compositor frame before.
struct SubmitRecord {
    std::chrono::steady_clock::time_point called, returned, locked, ready; // WaitGetPoses called/returned, lock taken, copies done
    x4vr::RuntimeBootstrap::SubmitMarks marks{}; // Submit(left), Submit(right), handoff returned
    uint8_t fresh{}, fallbacks{}; // fresh: bit per eye
    bool resubmit{}, theater{};
    uint64_t x4_late{}; // running count of late game frames
    vr::Compositor_FrameTiming timing{};
};
void write_timeline(std::vector<SubmitRecord> records, uint64_t next) {
    const auto us = [](std::chrono::steady_clock::time_point t) {
        return std::chrono::duration_cast<std::chrono::microseconds>(t.time_since_epoch()).count();
    };
    std::ostringstream out;
    out << "# called returned locked ready left right handoff fresh fallbacks resubmit theater x4_late"
           " | frame presents mispresented dropped reprojection system_s wait_called_ms poses_ready_ms frame_ready_ms"
           " update_start_ms render_start_ms submit_ms idle_ms interval_ms\n" << std::fixed << std::setprecision(3);
    for (auto i = next > records.size() ? next-records.size() : 0; i < next; ++i) {
        const auto& r = records[i % records.size()];
        const auto& t = r.timing;
        out << us(r.called) << ' ' << us(r.returned) << ' ' << us(r.locked) << ' ' << us(r.ready) << ' ' << us(r.marks[0]) << ' '
            << us(r.marks[1]) << ' ' << us(r.marks[2]) << ' ' << int(r.fresh) << ' ' << int(r.fallbacks) << ' ' << r.resubmit << ' '
            << r.theater << ' ' << r.x4_late << " | " << t.m_nFrameIndex << ' ' << t.m_nNumFramePresents << ' ' << t.m_nNumMisPresented
            << ' ' << t.m_nNumDroppedFrames << ' ' << t.m_nReprojectionFlags << ' ' << t.m_flSystemTimeInSeconds << ' '
            << t.m_flWaitGetPosesCalledMs << ' ' << t.m_flNewPosesReadyMs << ' ' << t.m_flNewFrameReadyMs << ' '
            << t.m_flCompositorUpdateStartMs << ' ' << t.m_flCompositorRenderStartMs << ' ' << t.m_flSubmitFrameMs << ' '
            << t.m_flCompositorIdleCpuMs << ' ' << t.m_flClientFrameIntervalMs << '\n';
    }
    x4vr::write_file_later(capture_root()/"submit_trace.txt", out.str(), false);
}
}
// Asynchronous submission thread: every compositor frame, submit each eye's newest image
// on the VR queue, waiting at most submit_budget_ms for its copy to finish, else the previous
// image. The game never makes the compositor miss a frame: on Windows with SteamVR's own
// reprojection off, every late frame showed as a dark/grey flash.
// In theater mode it shows eye 0's newest (flat) image on the virtual screen instead and
// submits black eyes.
void compositor_loop(Device d) {
    auto& p = presenter();
    // Mostly blocked in the runtime; once it returns, Submit has ~3 ms before the frame latches.
    raise_thread_priority();
    // Per recent submission: a fence signalled once the runtime's copies of its images completed,
    // and those images, held until then. Nothing waits for these between WaitGetPoses and Submit.
    struct Read { VkFence fence{}; std::array<uint32_t, 2> slots{UINT32_MAX, UINT32_MAX}; uint64_t generation{}; };
    std::array<Read, 4> reads{};
    uint64_t submissions{};
    VkFenceCreateInfo signalled{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO}; signalled.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    for (auto& r : reads) if (d.CreateFence(d.device, &signalled, nullptr, &r.fence) != VK_SUCCESS) return;
    const auto hold_unread = [&] { // under p.mutex
        for (const auto& r : reads)
            if (r.generation == p.generation && d.GetFenceStatus(d.device, r.fence) != VK_SUCCESS)
                for (uint32_t e = 0; e < 2; ++e) if (r.slots[e] != UINT32_MAX) p.held[e][r.slots[e]] = true;
    };
    const auto wait_reads = [&] { for (const auto& r : reads) d.WaitForFences(d.device, 1, &r.fence, VK_TRUE, UINT64_MAX); };
    // The last submission. Sent again when the present hook holds the lock past the budget: its
    // images stay held until the next successful selection.
    struct Frame {
        std::array<vr::VRVulkanTextureData_t, 2> textures{};
        std::array<vr::VRTextureBounds_t, 2> bounds{};
        std::array<x4vr::Matrix, 2> poses{};
        bool with_pose{}, valid{};
        std::array<uint32_t, 2> slots{UINT32_MAX, UINT32_MAX};
        uint64_t generation{};
    } last;
    std::array<uint32_t, 2> shown{UINT32_MAX, UINT32_MAX}; // slot submitted last per eye
    std::array<uint64_t, 2> shown_seq{}; // and the copy it held then
    uint64_t generation{};
    std::unique_ptr<x4vr::EyeTargets> black;
    bool screen_visible{}, ready{}, theater{};
    // The screen drawn into the eye textures (see screen_rects): its own command buffer and fence.
    const char* overlay_env = std::getenv("X4VR_THEATER_OVERLAY");
    const bool scene_screen = !(overlay_env && *overlay_env == '1');
    bool placed{}; // scene_screen: screen_origin set for this theater period
    bool drawn{}; // scene_screen: `black` holds the screen with game image drawn_seq
    uint64_t drawn_seq{};
    VkCommandPool screen_pool{};
    VkCommandBuffer screen_command{};
    VkFence screen_done{};
    if (scene_screen) {
        VkCommandPoolCreateInfo info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT; info.queueFamilyIndex = d.vr_family;
        VkCommandBufferAllocateInfo allocate{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; allocate.commandBufferCount = 1;
        VkFenceCreateInfo fence{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        if (d.CreateCommandPool(d.device, &info, nullptr, &screen_pool) != VK_SUCCESS) return;
        allocate.commandPool = screen_pool;
        if (d.AllocateCommandBuffers(d.device, &allocate, &screen_command) != VK_SUCCESS ||
            (d.set_loader_data && d.set_loader_data(d.device, screen_command) != VK_SUCCESS) ||
            d.CreateFence(d.device, &fence, nullptr, &screen_done) != VK_SUCCESS) return;
        log("X4VR theater: virtual screen drawn into the eye images (X4VR_THEATER_OVERLAY=1: SteamVR overlay)");
    }
    // Draws the game image (`source`, the flat image in eye 0's ring) onto the screen in both
    // black eye textures, on the VR queue; waits for it (menus aren't latency-critical).
    const auto draw_screen = [&](VkImage source, VkOffset3D game_offset, VkExtent2D game, VkExtent2D target,
                                 const x4vr::RuntimeBootstrap::EyeSetup& eyes, const x4vr::StereoSettings& s) {
        check(d.ResetFences(d.device, 1, &screen_done), "vkResetFences");
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        check(d.BeginCommandBuffer(screen_command, &begin), "vkBeginCommandBuffer");
        const VkClearColorValue color{};
        const VkImageSubresourceRange all{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        const float half_w = s.theater_width/2, half_h = half_w*float(game.height)/float(game.width);
        for (uint32_t e = 0; e < 2; ++e) {
            const auto image = black->eyes()[e].color.image;
            // After the runtime's copy of the last submission (earlier on this queue).
            barrier(d, screen_command, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
            d.CmdClearColorImage(screen_command, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &color, 1, &all);
            barrier(d, screen_command, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
            std::array<VkOffset3D, 2> from{}, to{};
            if (screen_rects(eyes.tangents[e], eyes.head_from_eye[e].m[0][3], half_w, half_h, s.theater_distance,
                             target, game_offset, game, from, to)) {
                VkImageBlit blit{};
                blit.srcSubresource = blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                blit.srcOffsets[0] = from[0]; blit.srcOffsets[1] = from[1];
                blit.dstOffsets[0] = to[0]; blit.dstOffsets[1] = to[1];
                d.CmdBlitImage(screen_command, source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                               1, &blit, VK_FILTER_LINEAR);
            }
            barrier(d, screen_command, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
        }
        check(d.EndCommandBuffer(screen_command), "vkEndCommandBuffer");
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.commandBufferCount = 1; submit.pCommandBuffers = &screen_command;
        {
            const auto queue_lock = lock_vr_queue(d);
            check(d.QueueSubmit(d.vr_queue, 1, &submit, screen_done), "vkQueueSubmit");
        }
        check(d.WaitForFences(d.device, 1, &screen_done, VK_TRUE, UINT64_MAX), "vkWaitForFences");
    };
    int screen_recenter = x4vr::stereo_settings().recenter;
    auto screen_origin = x4vr::Matrix::identity(), screen = screen_origin; // seated_from_screen
    std::vector<SubmitRecord> timeline(2048);
    uint64_t frames{};
    uint64_t pairs_matched{}, pairs_differing{}, pairs_total{}; // shared pose statistics, logged every 4000 stereo submits
    while (!p.stopping) try {
        const auto s = x4vr::stereo_settings();
        if (!s.async_submit || !s.pace) { std::this_thread::sleep_for(std::chrono::milliseconds(5)); continue; }
        if (std::unique_lock lock(p.mutex, std::chrono::milliseconds(1)); lock) { // busy: keep the last state
            theater = p.theater;
            ready = p.ready && p.async_frame && p.has_image(0) && (theater || p.has_image(1));
        }
        if (!ready) { std::this_thread::sleep_for(std::chrono::milliseconds(1)); continue; }
        if (screen_visible && !theater) { d.runtime->hide_theater(); screen_visible = false; }
        if (!theater) placed = false;
        auto& record = timeline[frames++ % timeline.size()];
        record = {};
        record.theater = theater;
        record.called = std::chrono::steady_clock::now();
        auto error = d.runtime->wait_frame();
        const auto interval = ticks().tick();
        const auto frame_start = record.returned = std::chrono::steady_clock::now();
        const auto blocked = std::chrono::duration<double>(frame_start-record.called).count();
        const auto budget = frame_start + std::chrono::microseconds(int64_t(s.submit_budget_ms*1000));
        // Theater mode never waits for copies: its screen isn't latency-critical, and a prompt submit keeps pacing.
        const auto deadline = theater ? frame_start : budget;
        if (!error.empty()) { report_submit(error); std::this_thread::sleep_for(std::chrono::milliseconds(5)); continue; }
        std::array<uint32_t, 2> newest{}, older{};
        std::array<uint64_t, 2> newest_seq{}, older_seq{};
        std::array<VkImage, 2> images{}, fallback{};
        std::array<VkFence, 2> fences{};
        std::array<x4vr::Matrix, 2> poses{}, fallback_poses{};
        std::array<vr::VRTextureBounds_t, 2> bounds{};
        std::array<vr::VRVulkanTextureData_t, 2> textures{};
        const uint32_t eyes = theater ? 1 : 2; // theater: eye 0's ring holds the flat image
        vr::VRTextureBounds_t crop{}; // the game image inside the padded texture
        VkFormat format{};
        VkExtent2D eye_extent{}, game_extent{};
        VkOffset3D game_offset{};
        x4vr::RuntimeBootstrap::EyeSetup eye_setup{};
        bool resubmit{};
        {
            std::unique_lock lock(p.mutex, budget);
            resubmit = !lock;
            if (lock) {
                if (!p.ready) continue; // swapchain resized meanwhile
                if (p.generation != generation) { generation = p.generation; shown = {UINT32_MAX, UINT32_MAX}; }
                const auto width = float(p.eye_extent.width), height = float(p.eye_extent.height);
                crop = {p.offset.x/width, p.offset.y/height, (p.offset.x+p.placed.width)/width, (p.offset.y+p.placed.height)/height};
                format = p.format; eye_extent = p.eye_extent;
                game_extent = p.placed; game_offset = p.offset; eye_setup = p.eyes;
                // Shared pose (linux_runtime.hpp): submit a pair built from one head pose. When the
                // newest images differ, the eye that is ahead steps back to its image with the other's pose.
                std::array<uint32_t, 2> pick{p.current[0], p.current[1]};
                const auto same_pose = [](const x4vr::Matrix& a, const x4vr::Matrix& b) { return !std::memcmp(&a, &b, sizeof a); };
                if (eyes == 2 && s.stereo && x4vr::linux_port::shared_pose() && p.filled[0][pick[0]] && p.filled[1][pick[1]] &&
                    !same_pose(p.slot_pose[0][pick[0]], p.slot_pose[1][pick[1]])) {
                    const uint32_t ahead = p.slot_seq[0][pick[0]] > p.slot_seq[1][pick[1]] ? 0 : 1, behind = 1-ahead;
                    for (uint32_t k = 0; k < Presenter::ring_size; ++k)
                        if (k != pick[ahead] && p.filled[ahead][k] && same_pose(p.slot_pose[ahead][k], p.slot_pose[behind][pick[behind]])) {
                            pick[ahead] = k; ++pairs_matched; break;
                        }
                }
                for (uint32_t e = 0; e < eyes; ++e) {
                    for (auto& h : p.held[e]) h = false;
                    newest[e] = pick[e];
                    p.held[e][newest[e]] = true;
                    images[e] = p.image(e, newest[e]); fences[e] = p.written[e][newest[e]]; poses[e] = p.slot_pose[e][newest[e]];
                    newest_seq[e] = p.slot_seq[e][newest[e]];
                    // Fallback: the newest finished image other than the newest one. When the GPU runs
                    // behind, every newest image is still in flight at submit time; the one that was late
                    // last tick is shown now instead of repeating the same image forever.
                    older[e] = UINT32_MAX;
                    for (uint32_t k = 0; k < Presenter::ring_size; ++k)
                        if (k != newest[e] && p.filled[e][k] && (older[e] == UINT32_MAX || p.slot_seq[e][k] > p.slot_seq[e][older[e]]) &&
                            d.GetFenceStatus(d.device, p.written[e][k]) == VK_SUCCESS) older[e] = k;
                    if (older[e] != UINT32_MAX) {
                        p.held[e][older[e]] = true;
                        fallback[e] = p.image(e, older[e]); fallback_poses[e] = p.slot_pose[e][older[e]];
                        older_seq[e] = p.slot_seq[e][older[e]];
                    }
                    textures[e] = eye_texture(d, p, VK_NULL_HANDLE, d.vr_queue, d.vr_family);
                }
                hold_unread();
                bounds = eye_bounds(p, s.valve_bounds && !d.runtime->openxr());
            }
        }
        if (resubmit && !last.valid) continue;
        record.locked = std::chrono::steady_clock::now();
        uint32_t fallbacks = 0;
        std::array<bool, 2> fresh{};
        if (!resubmit) {
            for (uint32_t e = 0; e < eyes; ++e) {
                // Unbounded only without a fallback, i.e. before anything was shown since start or a resize.
                const auto left = std::chrono::duration_cast<std::chrono::nanoseconds>(deadline-std::chrono::steady_clock::now()).count();
                const bool done = d.WaitForFences(d.device, 1, &fences[e], VK_TRUE, fallback[e] ? uint64_t(std::max<int64_t>(left, 0)) : UINT64_MAX) == VK_SUCCESS;
                const bool use_newest = done || !fallback[e];
                fallbacks += !use_newest;
                const auto seq = use_newest ? newest_seq[e] : older_seq[e];
                fresh[e] = seq != shown_seq[e];
                shown_seq[e] = seq;
                shown[e] = use_newest ? newest[e] : older[e];
                textures[e].m_nImage = reinterpret_cast<uint64_t>(use_newest ? images[e] : fallback[e]);
                if (!use_newest) poses[e] = fallback_poses[e];
            }
        }
        record.ready = std::chrono::steady_clock::now();
        const auto waited = std::chrono::duration<double>(record.ready-frame_start).count();
        if (!resubmit) {
            if (theater) {
                if (!black || black->extent().width != eye_extent.width || black->extent().height != eye_extent.height ||
                    black->color_format() != format) {
                    wait_reads(); // the runtime is done with the old images
                    black.reset();
                    black = make_black(d, eye_extent, format);
                    drawn = false;
                }
                // The screen stands theater_distance ahead of the recentred origin (else the current
                // head), placed when theater mode starts and again on each recenter (Windows: Ctrl+F12).
                const bool recentred = s.recenter != screen_recenter;
                screen_recenter = s.recenter;
                if (scene_screen ? !placed || recentred : !screen_visible || recentred) {
                    x4vr::Matrix head;
                    if (recentred || !x4vr::view_origin(screen_origin))
                        screen_origin = d.runtime->predicted_tracking(head, 0) == x4vr::FrameStatus::ready ? x4vr::seated_origin(head) : x4vr::Matrix::identity();
                }
                auto ahead = x4vr::Matrix::identity();
                ahead.m[2][3] = -s.theater_distance;
                screen = x4vr::multiply(screen_origin, ahead);
                if (scene_screen) {
                    if (!placed || recentred) drawn = false;
                    placed = true;
                    if (!drawn || drawn_seq != shown_seq[0]) {
                        draw_screen(reinterpret_cast<VkImage>(textures[0].m_nImage), game_offset, game_extent, eye_extent, eye_setup, s);
                        drawn = true; drawn_seq = shown_seq[0];
                    }
                } else {
                    const auto queue_lock = lock_vr_queue(d); // SteamVR copies the overlay texture on the VR queue
                    error = d.runtime->show_theater(fresh[0] || !screen_visible ? &textures[0] : nullptr, crop, screen, s.theater_width);
                    screen_visible = true;
                }
            }
            if (theater) {
                auto dark = textures;
                for (uint32_t e = 0; e < 2; ++e) {
                    dark[e] = textures[0];
                    dark[e].m_nImage = reinterpret_cast<uint64_t>(black->eyes()[e].color.image);
                }
                // The drawn screen is seen from the head pose it was placed for; SteamVR reprojects it.
                if (scene_screen) last = {dark, {{{0, 0, 1, 1}, {0, 0, 1, 1}}}, {screen_origin, screen_origin}, true, true, {UINT32_MAX, UINT32_MAX}, generation};
                else last = {dark, {{{0, 0, 1, 1}, {0, 0, 1, 1}}}, poses, false, true, {shown[0], UINT32_MAX}, generation};
            } else {
                last = {textures, bounds, poses, s.submit_pose != 0, true, shown, generation};
                if (s.stereo && x4vr::linux_port::shared_pose()) {
                    pairs_differing += std::memcmp(&poses[0], &poses[1], sizeof poses[0]) != 0;
                    if (++pairs_total == 4000) {
                        log("X4VR presenter: shared pose: "+std::to_string(pairs_matched)+" of 4000 pairs matched by stepping back, "+
                            std::to_string(pairs_differing)+" submitted with different poses");
                        pairs_total = pairs_matched = pairs_differing = 0;
                    }
                }
            }
        }
        const auto submitted_at = std::chrono::steady_clock::now();
        auto& pending = reads[submissions++ % reads.size()];
        {
            // The runtime's Submit records its copies on the VR queue: hold it if shared.
            const auto queue_lock = lock_vr_queue(d);
            if (error.empty()) error = d.runtime->submit_frame(last.textures, last.bounds, last.poses, last.with_pose, s.handoff, &record.marks);
            // ponytail: reuses a fence 4 submissions (~44 ms) old; waits only if the GPU is that far behind
            check(d.WaitForFences(d.device, 1, &pending.fence, VK_TRUE, UINT64_MAX), "vkWaitForFences");
            check(d.ResetFences(d.device, 1, &pending.fence), "vkResetFences");
            check(d.QueueSubmit(d.vr_queue, 0, nullptr, pending.fence), "vkQueueSubmit");
        }
        const auto submitting = std::chrono::duration<double>(std::chrono::steady_clock::now()-submitted_at).count();
        pending.slots = last.slots; pending.generation = last.generation;
        if (!resubmit) {
            // Keep only the images now on screen or still being read. Busy: they stay held until next tick.
            if (std::unique_lock lock(p.mutex, std::chrono::milliseconds(2)); lock) {
                if (p.generation == generation) for (uint32_t e = 0; e < eyes; ++e) {
                    for (auto& h : p.held[e]) h = false;
                    if (shown[e] != UINT32_MAX) p.held[e][shown[e]] = true;
                }
                hold_unread();
            }
        }
        pair_stats(s, fresh, blocked, interval, !error.empty(), fallbacks, waited, submitting, resubmit);
        report_submit(error);
        record.fresh = uint8_t(int(fresh[0]) | int(fresh[1]) << 1); record.fallbacks = uint8_t(fallbacks); record.resubmit = resubmit;
        record.x4_late = late_frames.load();
        d.runtime->frame_timing(record.timing, 1);
        if (x4vr::take_request("submit.request")) std::thread(write_timeline, timeline, frames).detach();
    } catch (const std::exception& error) {
        log("X4VR submission thread: "+std::string(error.what()));
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    if (screen_visible) d.runtime->hide_theater();
    wait_reads(); // the runtime's last copies
    if (screen_done) d.DestroyFence(d.device, screen_done, nullptr);
    if (screen_pool) d.DestroyCommandPool(d.device, screen_pool, nullptr);
    for (const auto& r : reads) d.DestroyFence(d.device, r.fence, nullptr);
}
void stop_submission(VkDevice device) {
    auto& p = presenter();
    if (device != p.thread_device || !p.thread.joinable()) return;
    p.stopping = true;
    p.thread.join();
    log("X4VR presenter: submission thread stopped");
}
namespace {
// After the game's present: hold the render thread to the compositor clock (what WaitGetPoses
// did when submission was inline). Pair mode releases the first eye mid-frame.
// A frame that ends after a tick is late: it goes on at once (the thread submits it next tick
// anyway). Waiting for the next tick turned every frame slightly over 11.1 ms into 22.2 ms.
void pace_to_compositor(const x4vr::StereoSettings& s, uint32_t eye) {
    if (s.pair && s.stereo && eye == 0) { if (s.pair_wait) wait_mid_frame(); return; }
    auto& t = ticks();
    std::unique_lock lock(t.mutex);
    if (s.release_late && t.count != t.released) ++late_frames;
    else {
        const auto seen = t.count;
        t.changed.wait_for(lock, std::chrono::milliseconds(100), [&] { return t.count != seen; });
    }
    t.released = t.count;
}
// Inline submission (async_submit=0): WaitGetPoses and Submit on the game's present queue.
void presenter_submit(const Device& d, VkQueue queue) {
    auto& p = presenter();
    if (!p.has_image(0) || !p.has_image(1)) return;
    const auto settings = x4vr::stereo_settings();
    if (!settings.pace) return; // calibration: no compositor pacing/submission
    if (settings.pair && settings.stereo && p.last_eye == 0) { if (settings.pair_wait) wait_mid_frame(); return; }
    std::array<vr::VRVulkanTextureData_t, 2> textures{};
    for (uint32_t i = 0; i < 2; ++i) textures[i] = eye_texture(d, p, p.image(i, p.current[i]), queue, p.family_of(queue));
    auto poses = p.poses;
    if (settings.submit_pose == 2) poses[0] = poses[1] = p.poses[p.last_eye]; // the eye just copied is newest
    const auto called = std::chrono::steady_clock::now();
    const auto error = d.runtime->submit_stereo(textures, eye_bounds(p, settings.valve_bounds && !d.runtime->openxr()), poses,
                                                settings.submit_pose != 0);
    const auto interval = ticks().tick();
    pair_stats(settings, p.fresh, std::chrono::duration<double>(std::chrono::steady_clock::now()-called).count(), interval, !error.empty());
    p.fresh = {};
    report_submit(error);
}
VKAPI_ATTR VkResult VKAPI_CALL CreateSwapchain(VkDevice device, const VkSwapchainCreateInfoKHR* ci,
                                               const VkAllocationCallbacks* alloc, VkSwapchainKHR* output) {
    const auto data = device_for(device); if (!data || !data->CreateSwapchainKHR) return VK_ERROR_INITIALIZATION_FAILED;
    const auto result = data->CreateSwapchainKHR(device, ci, alloc, output);
    if (result != VK_SUCCESS || !data->runtime) return result;
    if (!(ci->imageUsage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT)) {
        log("X4VR presenter: swapchain without transfer-source usage; the game image can't be copied");
        return result;
    }
    try {
        auto& p = presenter();
        std::lock_guard lock(p.mutex);
        if (p.ready && (ci->imageExtent.width != p.extent.width || ci->imageExtent.height != p.extent.height || ci->imageFormat != p.format)) {
            // ponytail: old targets are retired (leaked) per resize; fine for occasional resolution changes
            for (auto& set : p.targets) p.retired.push_back(std::move(set));
            p.dump_buffer = VK_NULL_HANDLE; p.dump_memory = VK_NULL_HANDLE;
            p.ready = false;
            log("X4VR presenter: swapchain size changed; eye targets will be rebuilt");
        }
        uint32_t count{};
        data->GetSwapchainImagesKHR(device, *output, &count, nullptr);
        p.images.resize(count);
        data->GetSwapchainImagesKHR(device, *output, &count, p.images.data());
        p.swapchain = *output; p.extent = ci->imageExtent; p.format = ci->imageFormat;
    } catch (...) {}
    return result;
}
VKAPI_ATTR void VKAPI_CALL GetDeviceQueue(VkDevice device, uint32_t family, uint32_t index, VkQueue* queue) {
    const auto data = device_for(device); if (!data || !data->GetDeviceQueue) return;
    data->GetDeviceQueue(device, family, index, queue);
    if (*queue) { auto& p = presenter(); std::lock_guard lock(p.families_mutex); p.families[*queue] = family; }
}
VKAPI_ATTR void VKAPI_CALL GetDeviceQueue2(VkDevice device, const VkDeviceQueueInfo2* info, VkQueue* queue) {
    const auto data = device_for(device); if (!data || !data->GetDeviceQueue2) return;
    data->GetDeviceQueue2(device, info, queue);
    if (*queue) { auto& p = presenter(); std::lock_guard lock(p.families_mutex); p.families[*queue] = info->queueFamilyIndex; }
}
VKAPI_ATTR VkResult VKAPI_CALL QueuePresent(VkQueue queue, const VkPresentInfoKHR* info) {
    const auto data = device_for(queue); if (!data || !data->QueuePresentKHR) return VK_ERROR_INITIALIZATION_FAILED;
    auto queue_lock = lock_if_shared(queue); // before the presenter lock, as everywhere
    if (data->runtime) x4vr::linux_port::sample_game_state(); // X4's main thread
    auto& p = presenter();
    std::unique_lock lock(p.mutex);
    VkSemaphore copied{};
    try { copied = presenter_copy(*data, queue, info); }
    catch (const std::exception& error) {
        p.failed = true;
        log("X4VR presenter disabled: "+std::string(error.what()));
    }
    auto forwarded = *info;
    if (copied) { forwarded.waitSemaphoreCount = 1; forwarded.pWaitSemaphores = &copied; }
    const bool async = copied && p.async_frame;
    const auto eye = p.last_eye;
    // The driver's present can block while the GPU is behind; the submission thread must not
    // wait for it. Inline submission (after the present) keeps the lock.
    if (!copied || async) lock.unlock();
    const auto result = data->QueuePresentKHR(queue, &forwarded);
    if (lock.owns_lock()) {
        try { presenter_submit(*data, queue); } catch (...) {}
        lock.unlock();
    }
    if (queue_lock.owns_lock()) queue_lock.unlock(); // pacing below must not hold X4's queue
    const auto settings = x4vr::stereo_settings();
    if (async) pace_to_compositor(settings, eye);
    static uint64_t presents{};
    if (settings.hitch_ms > 0 && settings.hitch_every > 0 && ++presents % uint64_t(settings.hitch_every) == 0) {
        // diagnostics: simulated game hitch, or CPU load with hitch_every=1. Busy-wait like game work.
        const auto until = std::chrono::steady_clock::now()+std::chrono::duration_cast<std::chrono::steady_clock::duration>(
            std::chrono::duration<double, std::milli>(settings.hitch_ms));
        while (std::chrono::steady_clock::now() < until) pause_cpu();
    }
    ++present_count;
    return result;
}
// The shared queue: X4's other uses of it, serialised with the layer's and the runtime's.
VKAPI_ATTR VkResult VKAPI_CALL QueueSubmit(VkQueue queue, uint32_t count, const VkSubmitInfo* submits, VkFence fence) {
    const auto data = device_for(queue); if (!data || !data->QueueSubmit) return VK_ERROR_INITIALIZATION_FAILED;
    const auto lock = lock_if_shared(queue);
    return data->QueueSubmit(queue, count, submits, fence);
}
VKAPI_ATTR VkResult VKAPI_CALL QueueSubmit2(VkQueue queue, uint32_t count, const VkSubmitInfo2* submits, VkFence fence) {
    const auto data = device_for(queue); if (!data || !data->QueueSubmit2) return VK_ERROR_INITIALIZATION_FAILED;
    const auto lock = lock_if_shared(queue);
    return data->QueueSubmit2(queue, count, submits, fence);
}
VKAPI_ATTR VkResult VKAPI_CALL QueueSubmit2KHR(VkQueue queue, uint32_t count, const VkSubmitInfo2* submits, VkFence fence) {
    const auto data = device_for(queue); if (!data || !data->QueueSubmit2KHR) return VK_ERROR_INITIALIZATION_FAILED;
    const auto lock = lock_if_shared(queue);
    return data->QueueSubmit2KHR(queue, count, submits, fence);
}
VKAPI_ATTR VkResult VKAPI_CALL QueueBindSparse(VkQueue queue, uint32_t count, const VkBindSparseInfo* info, VkFence fence) {
    const auto data = device_for(queue); if (!data || !data->QueueBindSparse) return VK_ERROR_INITIALIZATION_FAILED;
    const auto lock = lock_if_shared(queue);
    return data->QueueBindSparse(queue, count, info, fence);
}
VKAPI_ATTR VkResult VKAPI_CALL QueueWaitIdle(VkQueue queue) {
    const auto data = device_for(queue); if (!data || !data->QueueWaitIdle) return VK_ERROR_INITIALIZATION_FAILED;
    const auto lock = lock_if_shared(queue);
    return data->QueueWaitIdle(queue);
}
VKAPI_ATTR VkResult VKAPI_CALL DeviceWaitIdle(VkDevice device) {
    const auto data = device_for(device); if (!data || !data->DeviceWaitIdle) return VK_ERROR_INITIALIZATION_FAILED;
    const auto lock = data->vr_queue_shared ? std::unique_lock(shared_queue) : std::unique_lock<std::recursive_mutex>();
    return data->DeviceWaitIdle(device);
}
PFN_vkVoidFunction device_intercept(const char* name) {
#define MATCH(fn, impl) if (!std::strcmp(name, "vk" #fn)) return reinterpret_cast<PFN_vkVoidFunction>(impl);
    MATCH(DestroyDevice, DestroyDevice) MATCH(CreateSwapchainKHR, CreateSwapchain) MATCH(QueuePresentKHR, QueuePresent)
    MATCH(GetDeviceQueue, GetDeviceQueue) MATCH(GetDeviceQueue2, GetDeviceQueue2)
    MATCH(QueueSubmit, QueueSubmit) MATCH(QueueSubmit2, QueueSubmit2) MATCH(QueueSubmit2KHR, QueueSubmit2KHR)
    MATCH(QueueBindSparse, QueueBindSparse) MATCH(QueueWaitIdle, QueueWaitIdle) MATCH(DeviceWaitIdle, DeviceWaitIdle)
    return nullptr;
}
}
EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL x4vr_GetInstanceProcAddr(VkInstance instance, const char* name) {
    if (!name) return nullptr;
    if (!std::strcmp(name, "vkGetInstanceProcAddr")) return reinterpret_cast<PFN_vkVoidFunction>(x4vr_GetInstanceProcAddr);
    if (!std::strcmp(name, "vkCreateInstance")) return reinterpret_cast<PFN_vkVoidFunction>(CreateInstance);
    if (!instance) return nullptr;
    if (!std::strcmp(name, "vkGetDeviceProcAddr")) return reinterpret_cast<PFN_vkVoidFunction>(x4vr_GetDeviceProcAddr);
    if (!std::strcmp(name, "vkDestroyInstance")) return reinterpret_cast<PFN_vkVoidFunction>(DestroyInstance);
    if (!std::strcmp(name, "vkCreateDevice")) return reinterpret_cast<PFN_vkVoidFunction>(CreateDevice);
    const auto data = instance_for(instance); if (!data) return nullptr;
    const auto next = data->gipa(instance, name);
    if (next) if (const auto intercepted = device_intercept(name)) return intercepted;
    return next;
}
EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL x4vr_GetDeviceProcAddr(VkDevice device, const char* name) {
    if (!name || !device) return nullptr;
    if (!std::strcmp(name, "vkGetDeviceProcAddr")) return reinterpret_cast<PFN_vkVoidFunction>(x4vr_GetDeviceProcAddr);
    const auto data = device_for(device); if (!data) return nullptr;
    const auto next = data->gdpa(device, name);
    if (next) if (const auto intercepted = device_intercept(name)) return intercepted;
    return next;
}
namespace {
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL GetPhysicalDeviceProcAddr(VkInstance instance, const char* name) {
    if (!name || !instance) return nullptr;
    const auto data = instance_for(instance);
    return data && data->gpdpa ? data->gpdpa(instance, name) : nullptr;
}
}
EXPORT VKAPI_ATTR VkResult VKAPI_CALL x4vrNegotiateLoaderLayerInterfaceVersion(VkNegotiateLayerInterface* info) {
    if (!info || info->sType != LAYER_NEGOTIATE_INTERFACE_STRUCT || info->loaderLayerInterfaceVersion < 2)
        return VK_ERROR_INITIALIZATION_FAILED;
    info->loaderLayerInterfaceVersion = 2;
    info->pfnGetInstanceProcAddr = x4vr_GetInstanceProcAddr;
    info->pfnGetDeviceProcAddr = x4vr_GetDeviceProcAddr;
    info->pfnGetPhysicalDeviceProcAddr = GetPhysicalDeviceProcAddr;
    return VK_SUCCESS;
}
