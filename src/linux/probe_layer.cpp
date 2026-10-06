// VK_LAYER_X4VR_probe: Phase 0 Vulkan probe (docs/LINUX_PORT_PLAN.md, 0.7).
//
// A pass-through layer that only logs: that it loaded at all inside Steam's container, the
// instance and device X4 creates (GPU, queue families and the queues X4 asks for, extensions),
// the window system (Wayland or X11), the swapchain (format, size, present mode) and which
// thread presents, how often. Nothing is changed.
//
// Log: $X4VR_PROBE_DIR/layer-<pid>.log (x4vr-probe-run sets it).
#include <vulkan/vk_layer.h>
#include <vulkan/vulkan.h>
#include <fcntl.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#define EXPORT extern "C" __attribute__((visibility("default")))

namespace {
// ---- log --------------------------------------------------------------------------------------
int log_fd = -1;
const auto start = std::chrono::steady_clock::now();
void open_log() {
    std::string dir;
    if (const char* d = std::getenv("X4VR_PROBE_DIR"); d && *d) dir = d;
    else if (const char* s = std::getenv("XDG_STATE_HOME"); s && *s) dir = std::string(s)+"/x4vr/probe";
    else dir = std::string(std::getenv("HOME") ? std::getenv("HOME") : "/tmp")+"/.local/state/x4vr/probe";
    for (size_t at = 1; (at = dir.find('/', at)) != std::string::npos; ++at) mkdir(dir.substr(0, at).c_str(), 0755);
    mkdir(dir.c_str(), 0755);
    log_fd = open((dir+"/layer-"+std::to_string(getpid())+".log").c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
}
__attribute__((format(printf, 1, 2))) void log_line(const char* format, ...) {
    static std::once_flag once;
    std::call_once(once, open_log);
    if (log_fd < 0) return;
    char line[4096], name[17]{};
    prctl(PR_GET_NAME, name);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now()-start).count();
    int n = std::snprintf(line, sizeof(line), "%10.3f tid=%ld [%s] ", ms, long(syscall(SYS_gettid)), name);
    va_list args;
    va_start(args, format);
    n += std::vsnprintf(line+n, sizeof(line)-size_t(n), format, args);
    va_end(args);
    if (n > int(sizeof(line))-2) n = int(sizeof(line))-2;
    line[n++] = '\n';
    if (write(log_fd, line, size_t(n)) < 0) {}
}
std::string join(const char* const* names, uint32_t count) {
    std::string out;
    for (uint32_t i = 0; i < count; ++i) out += std::string(i ? " " : "")+names[i];
    return out.empty() ? "(none)" : out;
}
const char* format_name(VkFormat f) {
    switch (f) {
    case VK_FORMAT_B8G8R8A8_UNORM: return "B8G8R8A8_UNORM";
    case VK_FORMAT_B8G8R8A8_SRGB: return "B8G8R8A8_SRGB";
    case VK_FORMAT_R8G8B8A8_UNORM: return "R8G8B8A8_UNORM";
    case VK_FORMAT_R8G8B8A8_SRGB: return "R8G8B8A8_SRGB";
    case VK_FORMAT_A2B10G10R10_UNORM_PACK32: return "A2B10G10R10_UNORM";
    case VK_FORMAT_A2R10G10B10_UNORM_PACK32: return "A2R10G10B10_UNORM";
    case VK_FORMAT_R16G16B16A16_SFLOAT: return "R16G16B16A16_SFLOAT";
    default: return "other";
    }
}
const char* present_mode_name(VkPresentModeKHR m) {
    switch (m) {
    case VK_PRESENT_MODE_IMMEDIATE_KHR: return "IMMEDIATE";
    case VK_PRESENT_MODE_MAILBOX_KHR: return "MAILBOX";
    case VK_PRESENT_MODE_FIFO_KHR: return "FIFO";
    case VK_PRESENT_MODE_FIFO_RELAXED_KHR: return "FIFO_RELAXED";
    default: return "other";
    }
}

// ---- dispatch ---------------------------------------------------------------------------------
// Dispatchable handles start with the loader's dispatch table pointer; it identifies the instance
// or device (queues share their device's).
void* key(const void* handle) { return *static_cast<void* const*>(handle); }

struct InstanceData {
    VkInstance instance{};
    PFN_vkGetInstanceProcAddr gipa{};
    PFN_vkGetPhysicalDeviceProperties properties{};
    PFN_vkGetPhysicalDeviceQueueFamilyProperties families{};
};
struct DeviceData {
    VkDevice device{};
    PFN_vkGetDeviceProcAddr gdpa{};
    PFN_vkQueuePresentKHR present{};
    PFN_vkCreateSwapchainKHR create_swapchain{};
    PFN_vkGetDeviceQueue get_queue{};
    PFN_vkGetDeviceQueue2 get_queue2{};
    std::unordered_map<VkQueue, std::string> queues; // "family F index I"
    std::set<long> present_threads;
    uint64_t presents = 0, window_presents = 0;
    std::chrono::steady_clock::time_point window_start = std::chrono::steady_clock::now();
};
std::mutex mutex;
std::unordered_map<void*, InstanceData> instances;
std::unordered_map<void*, DeviceData> devices;

InstanceData* instance_of(const void* handle) {
    std::lock_guard lock(mutex);
    const auto found = instances.find(key(handle));
    return found == instances.end() ? nullptr : &found->second;
}
DeviceData* device_of(const void* handle) {
    std::lock_guard lock(mutex);
    const auto found = devices.find(key(handle));
    return found == devices.end() ? nullptr : &found->second;
}

// ---- instance ---------------------------------------------------------------------------------
VKAPI_ATTR VkResult VKAPI_CALL CreateInstance(const VkInstanceCreateInfo* info, const VkAllocationCallbacks* allocator, VkInstance* instance) {
    auto* chain = static_cast<VkLayerInstanceCreateInfo*>(const_cast<void*>(info->pNext));
    while (chain && !(chain->sType == VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO && chain->function == VK_LAYER_LINK_INFO))
        chain = static_cast<VkLayerInstanceCreateInfo*>(const_cast<void*>(chain->pNext));
    if (!chain) return VK_ERROR_INITIALIZATION_FAILED;
    const auto gipa = chain->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    chain->u.pLayerInfo = chain->u.pLayerInfo->pNext;
    const auto create = reinterpret_cast<PFN_vkCreateInstance>(gipa(VK_NULL_HANDLE, "vkCreateInstance"));
    const VkResult result = create(info, allocator, instance);
    const auto* app = info->pApplicationInfo;
    char exe[512]{};
    if (readlink("/proc/self/exe", exe, sizeof(exe)-1) < 0) std::strcpy(exe, "?");
    log_line("vkCreateInstance -> %d  exe=%s app=\"%s\" engine=\"%s\" api=%u.%u.%u layers=[%s] extensions=[%s]", result, exe,
             app && app->pApplicationName ? app->pApplicationName : "", app && app->pEngineName ? app->pEngineName : "",
             app ? VK_API_VERSION_MAJOR(app->apiVersion) : 0, app ? VK_API_VERSION_MINOR(app->apiVersion) : 0,
             app ? VK_API_VERSION_PATCH(app->apiVersion) : 0, join(info->ppEnabledLayerNames, info->enabledLayerCount).c_str(),
             join(info->ppEnabledExtensionNames, info->enabledExtensionCount).c_str());
    if (result != VK_SUCCESS) return result;
    InstanceData data;
    data.instance = *instance;
    data.gipa = gipa;
    data.properties = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(gipa(*instance, "vkGetPhysicalDeviceProperties"));
    data.families = reinterpret_cast<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(gipa(*instance, "vkGetPhysicalDeviceQueueFamilyProperties"));
    std::lock_guard lock(mutex);
    instances[key(*instance)] = data;
    return result;
}
VKAPI_ATTR void VKAPI_CALL DestroyInstance(VkInstance instance, const VkAllocationCallbacks* allocator) {
    const auto* data = instance_of(instance);
    if (!data) return;
    const auto destroy = reinterpret_cast<PFN_vkDestroyInstance>(data->gipa(instance, "vkDestroyInstance"));
    log_line("vkDestroyInstance");
    { std::lock_guard lock(mutex); instances.erase(key(instance)); }
    destroy(instance, allocator);
}
// Every surface-creation function has the same shape; only the name tells the window system.
template<const char* Name> VKAPI_ATTR VkResult VKAPI_CALL CreateSurface(VkInstance instance, const void* info, const VkAllocationCallbacks* allocator, VkSurfaceKHR* surface) {
    using Fn = VkResult (VKAPI_PTR*)(VkInstance, const void*, const VkAllocationCallbacks*, VkSurfaceKHR*);
    const auto* data = instance_of(instance);
    const auto next = data ? reinterpret_cast<Fn>(data->gipa(instance, Name)) : nullptr;
    if (!next) return VK_ERROR_EXTENSION_NOT_PRESENT;
    const VkResult result = next(instance, info, allocator, surface);
    log_line("%s -> %d (window system: %s)", Name, result, std::strstr(Name, "Wayland") ? "Wayland" : "X11 (Xwayland on a Wayland desktop)");
    return result;
}
constexpr char wayland_surface[] = "vkCreateWaylandSurfaceKHR";
constexpr char xlib_surface[] = "vkCreateXlibSurfaceKHR";
constexpr char xcb_surface[] = "vkCreateXcbSurfaceKHR";

// ---- device -----------------------------------------------------------------------------------
VKAPI_ATTR VkResult VKAPI_CALL CreateDevice(VkPhysicalDevice physical, const VkDeviceCreateInfo* info, const VkAllocationCallbacks* allocator, VkDevice* device) {
    auto* chain = static_cast<VkLayerDeviceCreateInfo*>(const_cast<void*>(info->pNext));
    while (chain && !(chain->sType == VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO && chain->function == VK_LAYER_LINK_INFO))
        chain = static_cast<VkLayerDeviceCreateInfo*>(const_cast<void*>(chain->pNext));
    if (!chain) return VK_ERROR_INITIALIZATION_FAILED;
    const auto gipa = chain->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    const auto gdpa = chain->u.pLayerInfo->pfnNextGetDeviceProcAddr;
    chain->u.pLayerInfo = chain->u.pLayerInfo->pNext;
    // Physical devices share their instance's dispatch key.
    const auto* inst = instance_of(physical);
    const auto create = reinterpret_cast<PFN_vkCreateDevice>(gipa(inst ? inst->instance : VK_NULL_HANDLE, "vkCreateDevice"));
    const VkResult result = create(physical, info, allocator, device);

    if (inst && inst->properties && inst->families) {
        VkPhysicalDeviceProperties p{};
        inst->properties(physical, &p);
        log_line("vkCreateDevice -> %d  gpu=\"%s\" vendor=0x%04x device=0x%04x driver=0x%x api=%u.%u.%u", result, p.deviceName,
                 p.vendorID, p.deviceID, p.driverVersion, VK_API_VERSION_MAJOR(p.apiVersion), VK_API_VERSION_MINOR(p.apiVersion),
                 VK_API_VERSION_PATCH(p.apiVersion));
        uint32_t count = 0;
        inst->families(physical, &count, nullptr);
        std::vector<VkQueueFamilyProperties> families(count);
        inst->families(physical, &count, families.data());
        for (uint32_t f = 0; f < count; ++f)
            log_line("  queue family %u: %u queues, flags 0x%x%s%s%s", f, families[f].queueCount, families[f].queueFlags,
                     families[f].queueFlags & VK_QUEUE_GRAPHICS_BIT ? " graphics" : "", families[f].queueFlags & VK_QUEUE_COMPUTE_BIT ? " compute" : "",
                     families[f].queueFlags & VK_QUEUE_TRANSFER_BIT ? " transfer" : "");
    } else {
        log_line("vkCreateDevice -> %d", result);
    }
    for (uint32_t i = 0; i < info->queueCreateInfoCount; ++i) {
        const auto& q = info->pQueueCreateInfos[i];
        log_line("  requests %u queue(s) in family %u (flags 0x%x)", q.queueCount, q.queueFamilyIndex, q.flags);
    }
    log_line("  device extensions=[%s]", join(info->ppEnabledExtensionNames, info->enabledExtensionCount).c_str());
    if (result != VK_SUCCESS) return result;
    DeviceData data;
    data.device = *device;
    data.gdpa = gdpa;
    data.present = reinterpret_cast<PFN_vkQueuePresentKHR>(gdpa(*device, "vkQueuePresentKHR"));
    data.create_swapchain = reinterpret_cast<PFN_vkCreateSwapchainKHR>(gdpa(*device, "vkCreateSwapchainKHR"));
    data.get_queue = reinterpret_cast<PFN_vkGetDeviceQueue>(gdpa(*device, "vkGetDeviceQueue"));
    data.get_queue2 = reinterpret_cast<PFN_vkGetDeviceQueue2>(gdpa(*device, "vkGetDeviceQueue2"));
    std::lock_guard lock(mutex);
    devices[key(*device)] = std::move(data);
    return result;
}
VKAPI_ATTR void VKAPI_CALL DestroyDevice(VkDevice device, const VkAllocationCallbacks* allocator) {
    auto* data = device_of(device);
    if (!data) return;
    const auto destroy = reinterpret_cast<PFN_vkDestroyDevice>(data->gdpa(device, "vkDestroyDevice"));
    log_line("vkDestroyDevice after %llu presents", static_cast<unsigned long long>(data->presents));
    { std::lock_guard lock(mutex); devices.erase(key(device)); }
    destroy(device, allocator);
}
VKAPI_ATTR void VKAPI_CALL GetDeviceQueue(VkDevice device, uint32_t family, uint32_t index, VkQueue* queue) {
    auto* data = device_of(device);
    if (!data || !data->get_queue) return;
    data->get_queue(device, family, index, queue);
    std::lock_guard lock(mutex);
    if (*queue && data->queues.emplace(*queue, "family "+std::to_string(family)+" index "+std::to_string(index)).second)
        log_line("vkGetDeviceQueue family %u index %u", family, index);
}
VKAPI_ATTR void VKAPI_CALL GetDeviceQueue2(VkDevice device, const VkDeviceQueueInfo2* info, VkQueue* queue) {
    auto* data = device_of(device);
    if (!data || !data->get_queue2) return;
    data->get_queue2(device, info, queue);
    std::lock_guard lock(mutex);
    if (*queue && data->queues.emplace(*queue, "family "+std::to_string(info->queueFamilyIndex)+" index "+std::to_string(info->queueIndex)).second)
        log_line("vkGetDeviceQueue2 family %u index %u", info->queueFamilyIndex, info->queueIndex);
}
VKAPI_ATTR VkResult VKAPI_CALL CreateSwapchain(VkDevice device, const VkSwapchainCreateInfoKHR* info, const VkAllocationCallbacks* allocator, VkSwapchainKHR* swapchain) {
    auto* data = device_of(device);
    if (!data || !data->create_swapchain) return VK_ERROR_INITIALIZATION_FAILED;
    const VkResult result = data->create_swapchain(device, info, allocator, swapchain);
    log_line("vkCreateSwapchainKHR -> %d  %ux%u format=%d (%s) colorspace=%d images>=%u present_mode=%s usage=0x%x old_swapchain=%s",
             result, info->imageExtent.width, info->imageExtent.height, info->imageFormat, format_name(info->imageFormat), info->imageColorSpace,
             info->minImageCount, present_mode_name(info->presentMode), info->imageUsage, info->oldSwapchain ? "yes" : "no");
    return result;
}
VKAPI_ATTR VkResult VKAPI_CALL QueuePresent(VkQueue queue, const VkPresentInfoKHR* info) {
    auto* data = device_of(queue);
    if (!data || !data->present) return VK_ERROR_DEVICE_LOST;
    const long thread = long(syscall(SYS_gettid));
    {
        std::lock_guard lock(mutex);
        ++data->presents; ++data->window_presents;
        if (data->present_threads.insert(thread).second) {
            const auto q = data->queues.find(queue);
            log_line("first present from this thread (present #%llu) on queue %s", static_cast<unsigned long long>(data->presents),
                     q == data->queues.end() ? "(unknown)" : q->second.c_str());
        }
        const auto now = std::chrono::steady_clock::now();
        const double seconds = std::chrono::duration<double>(now-data->window_start).count();
        if (seconds >= 5) {
            log_line("presents: %llu total, %.1f per second over the last %.0f s", static_cast<unsigned long long>(data->presents),
                     double(data->window_presents)/seconds, seconds);
            data->window_presents = 0;
            data->window_start = now;
        }
    }
    return data->present(queue, info);
}

// ---- lookup -----------------------------------------------------------------------------------
PFN_vkVoidFunction device_function(const char* name) {
#define HOOK(fn, impl) if (!std::strcmp(name, fn)) return reinterpret_cast<PFN_vkVoidFunction>(&impl)
    HOOK("vkDestroyDevice", DestroyDevice);
    HOOK("vkGetDeviceQueue", GetDeviceQueue);
    HOOK("vkGetDeviceQueue2", GetDeviceQueue2);
    HOOK("vkCreateSwapchainKHR", CreateSwapchain);
    HOOK("vkQueuePresentKHR", QueuePresent);
    return nullptr;
}
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL GetDeviceProcAddr(VkDevice device, const char* name);
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL GetInstanceProcAddr(VkInstance instance, const char* name) {
    HOOK("vkGetInstanceProcAddr", GetInstanceProcAddr);
    HOOK("vkCreateInstance", CreateInstance);
    HOOK("vkDestroyInstance", DestroyInstance);
    HOOK("vkCreateDevice", CreateDevice);
    HOOK("vkGetDeviceProcAddr", GetDeviceProcAddr);
    const auto* data = instance ? instance_of(instance) : nullptr;
    const auto next = data ? data->gipa(instance, name) : nullptr;
    if (next) { // surface types only where the driver offers them
        HOOK(wayland_surface, CreateSurface<wayland_surface>);
        HOOK(xlib_surface, CreateSurface<xlib_surface>);
        HOOK(xcb_surface, CreateSurface<xcb_surface>);
    }
    if (const auto fn = device_function(name)) return fn;
    return next;
}
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL GetDeviceProcAddr(VkDevice device, const char* name) {
    HOOK("vkGetDeviceProcAddr", GetDeviceProcAddr);
    if (const auto fn = device_function(name)) return fn;
    const auto* data = device ? device_of(device) : nullptr;
    return data ? data->gdpa(device, name) : nullptr;
#undef HOOK
}
}

EXPORT VKAPI_ATTR VkResult VKAPI_CALL x4vrProbeNegotiateLoaderLayerInterfaceVersion(VkNegotiateLayerInterface* version) {
    if (!version || version->sType != LAYER_NEGOTIATE_INTERFACE_STRUCT) return VK_ERROR_INITIALIZATION_FAILED;
    if (version->loaderLayerInterfaceVersion > 2) version->loaderLayerInterfaceVersion = 2;
    version->pfnGetInstanceProcAddr = GetInstanceProcAddr;
    version->pfnGetDeviceProcAddr = GetDeviceProcAddr;
    version->pfnGetPhysicalDeviceProcAddr = nullptr;
    log_line("x4vr probe layer loaded (loader interface version %u)", version->loaderLayerInterfaceVersion);
    return VK_SUCCESS;
}
