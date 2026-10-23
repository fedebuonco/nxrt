// Vulkan device, window, swapchain and presentation (counterpart of the window/present half of metal.m).
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <math.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef _WIN32
#include <windows.h>
#endif

#include "paths.h"
#include "../../settings.h"
#include "menu.h"
#include "nx_ui.h"
#include "vk.h"
#include "record.h"
#include "../output_scale.h"
#include "../renderer.h"
#include "../../nxgame.h"

VkG g_vk;
static SDL_Window *window;

// The system memory heap a whole-heap import (exec.c host_import) lands in and what it can hold: the largest heap with
// a host-visible memory type that isn't device-local (all of them device-local: the largest host-visible one), the
// smaller of its size and its budget (budget 0: unknown). -1: no host-visible heap. Plain types, so
// tools/test_gmem_lowmem.py builds it without Vulkan headers.
typedef struct { uint64_t size, budget; bool device_local, host_visible; } ImportHeap;
static int import_heap_room(const ImportHeap *h, uint32_t n, uint64_t *room) {
    int best = -1;
    for (int pass = 0; pass < 2 && best < 0; pass++)
        for (uint32_t i = 0; i < n; i++)
            if (h[i].host_visible && (pass || !h[i].device_local) && (best < 0 || h[i].size > h[best].size)) best = (int)i;
    if (best >= 0) *room = h[best].budget && h[best].budget < h[best].size ? h[best].budget : h[best].size;
    return best;
}
// What the heap must hold beyond the import: the transient ring's chunks once they spill to system memory (64 MB
// each), readback, the driver's own. Issue #205 (8 GB laptop, Ryzen 5 5500U, AMD's Windows driver): system heap
// 2738 MB, budget 2601 - the 3220 MB import was accepted, then the first submission failed with out of device memory.
// An 8 GB desktop with a GPU of its own (system heap ~3950 MB, budget ~3840) stays on one import.
#define IMPORT_MARGIN (256ull << 20)
// Under this much room one import should fit but might not (a driver's own limits, other programs): the import is
// tried once at the start (exec.c whole_import_probe_oom), with buffer device addresses enabled so lazy chunks can take
// over. Machines with more (an RTX with 16 GB or more of RAM) skip the probe and keep the device as before.
#define IMPORT_PROBE_BELOW (2 * HEAP_MAX)

// Whether one import of the whole heap fits its system memory heap (import_heap_room), from the physical device;
// *probe: it fits, but not by much (IMPORT_PROBE_BELOW), or NXRT_VK_SIM_HEAP_OOM=1 (the probe then fails).
static bool whole_import_fits(bool budget_ext, bool *probe) {
    VkPhysicalDeviceMemoryBudgetPropertiesEXT bp = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT};
    VkPhysicalDeviceMemoryProperties2 mp2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2,
                                             .pNext = budget_ext ? &bp : NULL};
    vkGetPhysicalDeviceMemoryProperties2(g_vk.phys, &mp2);
    const VkPhysicalDeviceMemoryProperties *mp = &mp2.memoryProperties;
    ImportHeap h[VK_MAX_MEMORY_HEAPS] = {{0}};
    for (uint32_t i = 0; i < mp->memoryHeapCount; i++) {
        h[i].size = mp->memoryHeaps[i].size;
        h[i].budget = budget_ext ? bp.heapBudget[i] : 0;
        h[i].device_local = (mp->memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) != 0;
    }
    for (uint32_t i = 0; i < mp->memoryTypeCount; i++)
        if (mp->memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) h[mp->memoryTypes[i].heapIndex].host_visible = true;
    uint64_t room = 0;
    int heap = import_heap_room(h, mp->memoryHeapCount, &room);
    const char *sim = getenv("NXRT_VK_SIM_HEAP_OOM");
    if (heap < 0 || room >= HEAP_MAX + IMPORT_MARGIN) {
        *probe = (heap >= 0 && room < IMPORT_PROBE_BELOW) || (sim && atoi(sim));
        return true;
    }
    LOG("[vk] guest heap: memory heap %d holds %llu MB (size %llu, budget %llu), one import needs %llu MB: "
        "imported in chunks as the game binds them\n", heap, (unsigned long long)(room >> 20),
        (unsigned long long)(h[heap].size >> 20), (unsigned long long)(h[heap].budget >> 20),
        (unsigned long long)((HEAP_MAX + IMPORT_MARGIN) >> 20));
    return false;
}

// Whether the guest heap will be imported chunk by chunk (exec.c gmem_lazy): on a GPU whose storage buffers can't
// span the heap (maxStorageBufferRange < HEAP_MAX: Intel, 1 GB); where the system memory heap or its budget can't hold
// one import of the whole heap (whole_import_fits: 8 GB laptops, issue #205); and on RADV when the dma-buf heap
// (exec.c guest_heap_dmabuf) has no cached host-visible memory type that holds it - the same test, which needs only
// the physical device - or NXRT_VK_GMEM_LAZY=1 (=0: never; a GPU with a short range then gets windows of one
// import, exec.c gmem_chunk_shift). Decided before the device exists because the lazy import needs buffer device
// addresses, and on RADV enabling them puts every allocation on the driver's global BO list (amdgpu checks all of
// them each submission): a device that imports the heap another way shouldn't pay for that.
// *probe: one import is to be tried first (whole_import_fits), lazy chunks if it runs out of memory.
static bool lazy_heap_predicted(bool budget_ext, bool *probe) {
    *probe = false;
    const char *e = getenv("NXRT_VK_GMEM_LAZY");
    if (e) return atoi(e) != 0;
    if (getenv("NXRT_VK_NO_GMEM")) return false;
    uint64_t lim = g_vk.props.limits.maxStorageBufferRange;
    if (lim && lim < HEAP_MAX) return true;  // (UHD 770 main pass: lazy 8.4 ms, 512 MB windows 8.5)
    if (!whole_import_fits(budget_ext, probe)) return true;
    if (!strstr(g_vk.props.deviceName, "RADV")) return false;
    *probe = false;  // (RADV: as before - the dma-buf heap or lazy chunks, no probe)
#ifdef __linux__
    const char *d = getenv("NXRT_VK_HEAP_DMABUF");
    if ((d ? atoi(d) != 0 : true) && (g_vk.dmabuf || g_vk.map_placed)) {
        VkPhysicalDeviceMemoryProperties mp;
        vkGetPhysicalDeviceMemoryProperties(g_vk.phys, &mp);
        const VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
        for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
            if ((mp.memoryTypes[i].propertyFlags & want) == want && mp.memoryHeaps[mp.memoryTypes[i].heapIndex].size >= HEAP_MAX) return false;
    }
#endif
    return true;
}
static atomic_bool ui_ready;
static VkDebugUtilsMessengerEXT messenger;
static void exit_after_shot(void);  // (UI thread section)

void vk_memory_report(void) {
#ifdef _WIN32
    MEMORYSTATUSEX m = {.dwLength = sizeof m};
    if (GlobalMemoryStatusEx(&m)) LOG("[vk] memory: Windows commit available %llu MB, physical available %llu MB\n",
        (unsigned long long)(m.ullAvailPageFile >> 20), (unsigned long long)(m.ullAvailPhys >> 20));
#endif
    if (!g_vk.vma) return;
    VkPhysicalDeviceMemoryProperties props;
    vkGetPhysicalDeviceMemoryProperties(g_vk.phys, &props);
    VmaBudget budgets[VK_MAX_MEMORY_HEAPS];
    vmaGetHeapBudgets(g_vk.vma, budgets);
    for (uint32_t i = 0; i < props.memoryHeapCount; i++) LOG("[vk] heap %u: app allocated %llu MB, usage %llu MB, budget %llu MB\n", i,
        (unsigned long long)(budgets[i].statistics.allocationBytes >> 20),
        (unsigned long long)(budgets[i].usage >> 20), (unsigned long long)(budgets[i].budget >> 20));
}

// ================================================================== instance / device
static VKAPI_ATTR VkBool32 VKAPI_CALL debug_cb(VkDebugUtilsMessageSeverityFlagBitsEXT sev, VkDebugUtilsMessageTypeFlagsEXT type,
                                               const VkDebugUtilsMessengerCallbackDataEXT *d, void *user) {
    (void)type, (void)user;
    static atomic_uint n;
    bool stop = sev >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT && getenv("NXRT_VK_VALIDATION_STOP");
    if (sev >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT &&
        (stop || atomic_fetch_add_explicit(&n, 1, memory_order_relaxed) < 500)) LOG("[vk] %s\n", d->pMessage);
    if (stop) {
        LOG("[vk] stopping at the first validation error\n");
        fflush(stderr);
        _Exit(EXIT_FAILURE);  // do not run GPU cleanup after the validation failure
    }
    return VK_FALSE;
}

// NXRT_VK_VALIDATION=1: Khronos validation layer (MSYS2 package vulkan-validation-layers)
static void create_instance(void) {
#ifdef VK_NO_PROTOTYPES
    if (volkInitialize() != VK_SUCCESS) FATAL("no Vulkan driver\n");
#endif
    Uint32 n_sdl = 0;
    const char *const *sdl_ext = SDL_Vulkan_GetInstanceExtensions(&n_sdl);
    const char *ext[16];
    uint32_t n_ext = 0;
    for (Uint32 i = 0; i < n_sdl && n_ext < 14; i++) ext[n_ext++] = sdl_ext[i];
    VkInstanceCreateFlags flags = 0;
#ifdef __APPLE__
    // MoltenVK is a portability driver: the loader lists it only to instances that ask for those
    ext[n_ext++] = VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME;
    flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
#endif
    bool validate = getenv("NXRT_VK_VALIDATION") != NULL;
    if (validate) ext[n_ext++] = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
    const char *layers[] = {"VK_LAYER_KHRONOS_validation"};
    VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = nx_game.info.title,
                             .pEngineName = "nxrt", .apiVersion = VK_API_VERSION_1_3};
    VkInstanceCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .flags = flags, .pApplicationInfo = &app,
                               .enabledExtensionCount = n_ext, .ppEnabledExtensionNames = ext,
                               .enabledLayerCount = validate ? 1 : 0, .ppEnabledLayerNames = layers};
    VkResult r = vkCreateInstance(&ci, NULL, &g_vk.inst);
    if (r != VK_SUCCESS && validate) {
        if (strict) {
            LOG("[vk] cannot create an instance with required validation (%d)\n", r);
            fflush(stderr);
            _Exit(EXIT_FAILURE);
        }
        LOG("[vk] validation layer unavailable (%d); continuing without\n", r);
        ci.enabledLayerCount = 0;
        ci.enabledExtensionCount--;
        validate = false;
        r = vkCreateInstance(&ci, NULL, &g_vk.inst);
    }
    validating = validate;
    if (r != VK_SUCCESS) FATAL("vkCreateInstance: %d (is a Vulkan 1.3 driver installed?)\n", r);
#ifdef VK_NO_PROTOTYPES
    volkLoadInstance(g_vk.inst);
#endif
    if (validate) {
        PFN_vkCreateDebugUtilsMessengerEXT f =
            (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(g_vk.inst, "vkCreateDebugUtilsMessengerEXT");
        VkDebugUtilsMessengerCreateInfoEXT mi = {
            .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
            .messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
            .messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                           VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT,
            .pfnUserCallback = debug_cb};
        VkResult result = f ? f(g_vk.inst, &mi, NULL, &messenger) : VK_ERROR_EXTENSION_NOT_PRESENT;
        if (result != VK_SUCCESS && strict) {
            LOG("[vk] required validation callback unavailable (%d)\n", result);
            fflush(stderr);
            _Exit(EXIT_FAILURE);
        }
        if (result == VK_SUCCESS) LOG("[vk] validation enabled%s\n", strict ? " (stop on error)" : "");
    }
}

static bool has_ext(VkExtensionProperties *e, uint32_t n, const char *name) {
    for (uint32_t i = 0; i < n; i++) if (!strcmp(e[i].extensionName, name)) return true;
    return false;
}

// Discrete GPU first (NXRT_GPU=<index> picks one), needs Vulkan 1.3 and a queue that can present.
static void pick_device(void) {
    uint32_t n = 0;
    vkEnumeratePhysicalDevices(g_vk.inst, &n, NULL);
    if (!n) FATAL("no Vulkan device\n");
    VkPhysicalDevice *pd = calloc(n, sizeof *pd);
    vkEnumeratePhysicalDevices(g_vk.inst, &n, pd);
    int want = getenv("NXRT_GPU") ? atoi(getenv("NXRT_GPU")) : -1, best = -1, best_score = -1;
    for (uint32_t i = 0; i < n; i++) {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(pd[i], &p);
        LOG("[vk] gpu %u: %s (api %u.%u.%u)\n", i, p.deviceName, VK_API_VERSION_MAJOR(p.apiVersion),
            VK_API_VERSION_MINOR(p.apiVersion), VK_API_VERSION_PATCH(p.apiVersion));
        if (p.apiVersion < VK_API_VERSION_1_3) continue;
        int score = p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 3 : p.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 2 : 1;
        if ((int)i == want) score = 100;
        if (score > best_score) { best_score = score; best = (int)i; }
    }
    if (best < 0) FATAL("no Vulkan 1.3 device\n");
    g_vk.phys = pd[best];
    free(pd);
    vkGetPhysicalDeviceProperties(g_vk.phys, &g_vk.props);
    uint32_t nq = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(g_vk.phys, &nq, NULL);
    VkQueueFamilyProperties *qp = calloc(nq, sizeof *qp);
    vkGetPhysicalDeviceQueueFamilyProperties(g_vk.phys, &nq, qp);
    g_vk.qfam = UINT32_MAX;
    for (uint32_t i = 0; i < nq; i++) {
        VkBool32 present = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(g_vk.phys, i, g_vk.surface, &present);
        if ((qp[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && (qp[i].queueFlags & VK_QUEUE_COMPUTE_BIT) && present) {
            g_vk.qfam = i, g_vk.ts_bits = qp[i].timestampValidBits;
            break;
        }
    }
    free(qp);
    if (g_vk.qfam == UINT32_MAX) FATAL("no graphics+present queue\n");
    LOG("[vk] using %s\n", g_vk.props.deviceName);
    settings_resolve_defaults(g_vk.props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU);
    // On by default on integrated GPUs only: lean usage and the SMAA stencil measured faster on an Intel UHD 770 and
    // slightly slower on an RTX 3080 Ti; skipping the per-frame target clears saved 3.7 ms on the UHD 770 (2x) with
    // identical images. NXRT_VK_LEAN_USAGE / NXRT_VK_SMAA_STENCIL / NXRT_NO_FRAME_CLEAR =0 / =1 override.
    bool igpu = g_vk.props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU;
    const char *e = getenv("NXRT_VK_LEAN_USAGE");
    g_vk.lean_usage = e && *e ? atoi(e) != 0 : igpu;
    e = getenv("NXRT_VK_SMAA_STENCIL");
    g_vk.smaa_stencil = e && *e ? atoi(e) != 0 : igpu;
    e = getenv("NXRT_NO_FRAME_CLEAR");
    g_vk.no_frame_clear = e && *e ? atoi(e) != 0 : igpu;
    LOG("[vk] lean usage %s, SMAA stencil %s, per-frame target clears %s (%s GPU)\n", g_vk.lean_usage ? "on" : "off",
        g_vk.smaa_stencil ? "on" : "off", g_vk.no_frame_clear ? "off" : "on", igpu ? "integrated" : "not an integrated");
}

static bool g_fault_ext;
// After VK_ERROR_DEVICE_LOST: what the driver says faulted (VK_EXT_device_fault), into the log.
void vk_device_fault_report(void) {
    static int once;
    if (once++ || !g_fault_ext) return;
    PFN_vkGetDeviceFaultInfoEXT get = (PFN_vkGetDeviceFaultInfoEXT)vkGetDeviceProcAddr(g_vk.dev, "vkGetDeviceFaultInfoEXT");
    if (!get) return;
    VkDeviceFaultCountsEXT c = {.sType = VK_STRUCTURE_TYPE_DEVICE_FAULT_COUNTS_EXT};
    if (get(g_vk.dev, &c, NULL) != VK_SUCCESS) { LOG("[vk] device fault: no info\n"); return; }
    VkDeviceFaultAddressInfoEXT addr[16];
    VkDeviceFaultVendorInfoEXT vend[16];
    if (c.addressInfoCount > 16) c.addressInfoCount = 16;
    if (c.vendorInfoCount > 16) c.vendorInfoCount = 16;
    c.vendorBinarySize = 0;
    VkDeviceFaultInfoEXT info = {.sType = VK_STRUCTURE_TYPE_DEVICE_FAULT_INFO_EXT, .pAddressInfos = addr, .pVendorInfos = vend};
    if (get(g_vk.dev, &c, &info) != VK_SUCCESS && 0) return;
    LOG("[vk] device fault: %s\n", info.description);
    static const char *const kind[] = {"none", "read invalid", "write invalid", "execute invalid", "ip unknown", "ip invalid", "ip valid"};
    for (uint32_t i = 0; i < c.addressInfoCount; i++)
        LOG("[vk]   address %s %#llx (precision %#llx)\n", addr[i].addressType < 7 ? kind[addr[i].addressType] : "?",
            (unsigned long long)addr[i].reportedAddress, (unsigned long long)addr[i].addressPrecision);
    for (uint32_t i = 0; i < c.vendorInfoCount; i++)
        LOG("[vk]   vendor: %s (code %#llx data %#llx)\n", vend[i].description, (unsigned long long)vend[i].vendorFaultCode,
            (unsigned long long)vend[i].vendorFaultData);
}

// Each memory heap: its size, the driver's budget for this process (smaller when other programs hold that memory),
// this process's use and the part LAS Recomp allocated through VMA. Logged at start and when an allocation fails
// (issue #45: out of device memory on a 6 GB GTX 1060 - our own use, or other programs'?).
void vk_log_memory(const char *why) {
    if (!g_vk.vma) return;
    const VkPhysicalDeviceMemoryProperties *mp;
    vmaGetMemoryProperties(g_vk.vma, &mp);
    VmaBudget b[VK_MAX_MEMORY_HEAPS];
    vmaGetHeapBudgets(g_vk.vma, b);
    for (uint32_t i = 0; i < mp->memoryHeapCount; i++)
        LOG("[vk] memory (%s) heap %u %s: %llu MB, budget %llu MB, in use %llu MB, ours %llu MB in %u allocations\n", why, i,
            mp->memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT ? "device-local" : "system",
            (unsigned long long)(mp->memoryHeaps[i].size >> 20), (unsigned long long)(b[i].budget >> 20),
            (unsigned long long)(b[i].usage >> 20), (unsigned long long)(b[i].statistics.allocationBytes >> 20),
            b[i].statistics.allocationCount);
}

// ---- pipeline cache file
#define PCACHE_FILE "cache/vk_pipelines.bin"
#define PCACHE_TAG "cache/vk_pipelines.bin.tag"
static size_t pcache_saved;  // bytes of the last save (or load)
void vk_pipeline_cache_save(void);

static void pipeline_cache_tag(char *out, size_t n) {
    char gpu[400], uuid[2 * VK_UUID_SIZE + 1];
    for (int i = 0; i < VK_UUID_SIZE; i++) snprintf(uuid + 2 * i, 3, "%02x", g_vk.props.pipelineCacheUUID[i]);
    snprintf(gpu, sizeof gpu, "%s, vendor %x device %x, driver %u, cache %s", g_vk.props.deviceName, g_vk.props.vendorID,
             g_vk.props.deviceID, g_vk.props.driverVersion, uuid);
    shader_cache_tag(out, n, gpu);
}

static void *pipeline_cache_saver(void *a) {  // every 3 minutes, when it grew
    (void)a;
    for (;;) {
        SDL_Delay(180000);
        if (rq_stopping()) break;  // (quitting: backend_quit saves it)
        vk_pipeline_cache_save();
    }
    return NULL;
}

static void create_device(void) {
    uint32_t ne = 0;
    vkEnumerateDeviceExtensionProperties(g_vk.phys, NULL, &ne, NULL);
    VkExtensionProperties *ep = calloc(ne, sizeof *ep);
    vkEnumerateDeviceExtensionProperties(g_vk.phys, NULL, &ne, ep);
    const char *ext[12];
    uint32_t n_ext = 0;
    ext[n_ext++] = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
    g_vk.custom_border = has_ext(ep, ne, VK_EXT_CUSTOM_BORDER_COLOR_EXTENSION_NAME);
    if (g_vk.custom_border) ext[n_ext++] = VK_EXT_CUSTOM_BORDER_COLOR_EXTENSION_NAME;
    g_vk.divisor = has_ext(ep, ne, VK_EXT_VERTEX_ATTRIBUTE_DIVISOR_EXTENSION_NAME);
    if (g_vk.divisor) ext[n_ext++] = VK_EXT_VERTEX_ATTRIBUTE_DIVISOR_EXTENSION_NAME;
    g_vk.host_import = has_ext(ep, ne, VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME);  // guest heap as a GPU buffer (exec.c)
    if (g_vk.host_import) ext[n_ext++] = VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME;
    // a portability implementation (MoltenVK) must have its subset extension enabled when it lists it
    if (has_ext(ep, ne, "VK_KHR_portability_subset")) ext[n_ext++] = "VK_KHR_portability_subset";
    bool fault = has_ext(ep, ne, VK_EXT_DEVICE_FAULT_EXTENSION_NAME);  // device-lost diagnostics (vk_device_fault_report)
    if (fault) ext[n_ext++] = VK_EXT_DEVICE_FAULT_EXTENSION_NAME;
    g_fault_ext = fault;
    bool budget = has_ext(ep, ne, VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);  // (real per-heap budgets: vk_log_memory)
    if (budget) ext[n_ext++] = VK_EXT_MEMORY_BUDGET_EXTENSION_NAME;
    // pipeline-building features: logged below (issue #92); dynamic vertex input used, the library one only measured
    bool vids_ext = has_ext(ep, ne, VK_EXT_VERTEX_INPUT_DYNAMIC_STATE_EXTENSION_NAME);
#ifdef VK_EXT_graphics_pipeline_library
    bool gpl_ext = has_ext(ep, ne, VK_EXT_GRAPHICS_PIPELINE_LIBRARY_EXTENSION_NAME) &&
                   has_ext(ep, ne, VK_KHR_PIPELINE_LIBRARY_EXTENSION_NAME);
#endif
#ifdef VK_EXT_extended_dynamic_state3
    bool eds3_ext = has_ext(ep, ne, VK_EXT_EXTENDED_DYNAMIC_STATE_3_EXTENSION_NAME);
#endif
#ifdef __linux__
    // the guest heap as driver memory mapped at HEAP_BASE (exec.c backend_guest_heap_prepare)
    g_vk.dmabuf = has_ext(ep, ne, VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME) &&
                  has_ext(ep, ne, VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME);
    if (g_vk.dmabuf) {
        ext[n_ext++] = VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME;
        ext[n_ext++] = VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME;
    }
    // ... mapped there by the driver itself (its own DRM file, as every vkMapMemory), not through the dma-buf (issue #92)
    // (older Vulkan headers, e.g. a distro's, lack it: then the dma-buf mapping only)
#ifdef VK_EXT_map_memory_placed
    bool placed_ext = has_ext(ep, ne, VK_EXT_MAP_MEMORY_PLACED_EXTENSION_NAME) && has_ext(ep, ne, VK_KHR_MAP_MEMORY_2_EXTENSION_NAME);
#endif
#endif
    free(ep);

    // features the backend relies on (all present on current desktop drivers)
    VkPhysicalDeviceVulkan12Features have12 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceFeatures2 have = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &have12};
#if defined(__linux__) && defined(VK_EXT_map_memory_placed)
    VkPhysicalDeviceMapMemoryPlacedFeaturesEXT have_placed = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAP_MEMORY_PLACED_FEATURES_EXT};
    if (placed_ext) have12.pNext = &have_placed;
#endif
    VkPhysicalDeviceVertexInputDynamicStateFeaturesEXT have_vids = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_INPUT_DYNAMIC_STATE_FEATURES_EXT};
    if (vids_ext) have_vids.pNext = have.pNext, have.pNext = &have_vids;
#ifdef VK_EXT_graphics_pipeline_library
    VkPhysicalDeviceGraphicsPipelineLibraryFeaturesEXT have_gpl = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GRAPHICS_PIPELINE_LIBRARY_FEATURES_EXT};
    if (gpl_ext) have_gpl.pNext = have.pNext, have.pNext = &have_gpl;
#endif
#ifdef VK_EXT_extended_dynamic_state3
    VkPhysicalDeviceExtendedDynamicState3FeaturesEXT have_eds3 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_3_FEATURES_EXT};
    if (eds3_ext) have_eds3.pNext = have.pNext, have.pNext = &have_eds3;
#endif
    vkGetPhysicalDeviceFeatures2(g_vk.phys, &have);
    {   // what the driver offers for building pipelines (one line, for #92 reports)
        VkPhysicalDeviceDriverProperties drv = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
        VkPhysicalDeviceProperties2 pr = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &drv};
        char gpl[96] = "no extension", eds3[160] = "no extension";
#ifdef VK_EXT_graphics_pipeline_library
        VkPhysicalDeviceGraphicsPipelineLibraryPropertiesEXT gplp = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GRAPHICS_PIPELINE_LIBRARY_PROPERTIES_EXT};
        if (gpl_ext) gplp.pNext = pr.pNext, pr.pNext = &gplp;
#else
        snprintf(gpl, sizeof gpl, "not in this build's headers");
#endif
#ifndef VK_EXT_extended_dynamic_state3
        snprintf(eds3, sizeof eds3, "not in this build's headers");
#endif
        vkGetPhysicalDeviceProperties2(g_vk.phys, &pr);
#ifdef VK_EXT_graphics_pipeline_library
        if (gpl_ext)
            snprintf(gpl, sizeof gpl, "%s (fast linking %s, independent interpolation decoration %s)",
                     have_gpl.graphicsPipelineLibrary ? "yes" : "no", gplp.graphicsPipelineLibraryFastLinking ? "yes" : "no",
                     gplp.graphicsPipelineLibraryIndependentInterpolationDecoration ? "yes" : "no");
#endif
#ifdef VK_EXT_extended_dynamic_state3
        if (eds3_ext)
            snprintf(eds3, sizeof eds3, "polygon mode %d, samples %d, alpha to coverage %d, blend enable %d, "
                     "blend equation %d, write mask %d", have_eds3.extendedDynamicState3PolygonMode,
                     have_eds3.extendedDynamicState3RasterizationSamples, have_eds3.extendedDynamicState3AlphaToCoverageEnable,
                     have_eds3.extendedDynamicState3ColorBlendEnable, have_eds3.extendedDynamicState3ColorBlendEquation,
                     have_eds3.extendedDynamicState3ColorWriteMask);
#endif
        LOG("[vk] driver %s %s; graphics pipeline library: %s; vertex input dynamic state: %s; extended dynamic state 3: %s\n",
            drv.driverName, drv.driverInfo, gpl, vids_ext ? (have_vids.vertexInputDynamicState ? "yes" : "no") : "no extension",
            eds3);
    }
    {   // dynamic vertex input (exec.c): fewer pipelines; NXRT_VK_DYN_VERTEX=0 keeps the layout in each pipeline
        const char *e = getenv("NXRT_VK_DYN_VERTEX");
        bool off = e && !atoi(e);
        g_vk.vids = vids_ext && have_vids.vertexInputDynamicState && !off;
        if (g_vk.vids) ext[n_ext++] = VK_EXT_VERTEX_INPUT_DYNAMIC_STATE_EXTENSION_NAME;
        else LOG("[vk] vertex input: in each pipeline (%s)\n", off ? "NXRT_VK_DYN_VERTEX=0" :
                 !vids_ext ? "no VK_EXT_vertex_input_dynamic_state" : "vertexInputDynamicState not supported");
    }
#ifdef VK_EXT_graphics_pipeline_library
    {   // NXRT_VK_GPL_PROBE=1: library builds timed after shader preparation (exec.c gpl_probe); nothing else uses it
        const char *e = getenv("NXRT_VK_GPL_PROBE");
        if (e && atoi(e)) {
            g_vk.gpl_probe = gpl_ext && have_gpl.graphicsPipelineLibrary;
            if (g_vk.gpl_probe) {
                ext[n_ext++] = VK_KHR_PIPELINE_LIBRARY_EXTENSION_NAME;
                ext[n_ext++] = VK_EXT_GRAPHICS_PIPELINE_LIBRARY_EXTENSION_NAME;
            } else LOG("[gpl] probe: the driver has no graphics pipeline library; skipped\n");
        }
    }
#else
    if (getenv("NXRT_VK_GPL_PROBE") && atoi(getenv("NXRT_VK_GPL_PROBE"))) LOG("[gpl] probe: this build's Vulkan headers have no graphics pipeline library; skipped\n");
#endif
#if defined(__linux__) && defined(VK_EXT_map_memory_placed)
    have12.pNext = NULL;
    g_vk.map_placed = placed_ext && have_placed.memoryMapPlaced;
    g_vk.unmap_reserve = g_vk.map_placed && have_placed.memoryUnmapReserve;
    if (g_vk.map_placed) {
        ext[n_ext++] = VK_KHR_MAP_MEMORY_2_EXTENSION_NAME;
        ext[n_ext++] = VK_EXT_MAP_MEMORY_PLACED_EXTENSION_NAME;
    }
#endif
    bool probe;
    g_vk.lazy_heap = lazy_heap_predicted(budget, &probe);
    g_vk.heap_probe = !g_vk.lazy_heap && probe && g_vk.host_import && have12.bufferDeviceAddress;
    g_vk.bda = (g_vk.lazy_heap || g_vk.heap_probe) && have12.bufferDeviceAddress;
    VkPhysicalDeviceVertexAttributeDivisorFeaturesEXT div = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_FEATURES_EXT,
        .vertexAttributeInstanceRateDivisor = VK_TRUE, .vertexAttributeInstanceRateZeroDivisor = VK_FALSE};
    VkPhysicalDeviceCustomBorderColorFeaturesEXT cbc = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_FEATURES_EXT, .customBorderColors = VK_TRUE,
        .customBorderColorWithoutFormat = VK_TRUE, .pNext = g_vk.divisor ? &div : NULL};
    VkPhysicalDeviceVulkan13Features f13 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
                                            .dynamicRendering = VK_TRUE, .synchronization2 = VK_TRUE,
                                            .shaderDemoteToHelperInvocation = VK_TRUE, .shaderTerminateInvocation = VK_TRUE,
                                            .maintenance4 = VK_TRUE,
                                            .pNext = g_vk.custom_border ? (void *)&cbc : g_vk.divisor ? (void *)&div : NULL};
    VkPhysicalDeviceVulkan12Features f12 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
                                            .timelineSemaphore = VK_TRUE, .shaderInt8 = VK_TRUE, .samplerMirrorClampToEdge = VK_TRUE,
                                            .uniformAndStorageBuffer8BitAccess = VK_TRUE, .storageBuffer8BitAccess = VK_TRUE,
                                            .scalarBlockLayout = VK_TRUE, .bufferDeviceAddress = g_vk.bda, .pNext = &f13};
    VkPhysicalDeviceVulkan11Features f11 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES,
                                            .storageBuffer16BitAccess = VK_TRUE, .pNext = &f12};
    VkPhysicalDeviceFaultFeaturesEXT ff = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FAULT_FEATURES_EXT, .deviceFault = VK_TRUE,
                                           .pNext = &f11};
    VkPhysicalDeviceFeatures2 f = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = fault ? (void *)&ff : (void *)&f11};
#if defined(__linux__) && defined(VK_EXT_map_memory_placed)
    VkPhysicalDeviceMapMemoryPlacedFeaturesEXT placed = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAP_MEMORY_PLACED_FEATURES_EXT,
                                                         .memoryMapPlaced = VK_TRUE, .memoryUnmapReserve = g_vk.unmap_reserve,
                                                         .pNext = f.pNext};
    if (g_vk.map_placed) f.pNext = &placed;
#endif
    VkPhysicalDeviceVertexInputDynamicStateFeaturesEXT en_vids = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_INPUT_DYNAMIC_STATE_FEATURES_EXT, .vertexInputDynamicState = VK_TRUE};
    if (g_vk.vids) en_vids.pNext = f.pNext, f.pNext = &en_vids;
#ifdef VK_EXT_graphics_pipeline_library
    VkPhysicalDeviceGraphicsPipelineLibraryFeaturesEXT en_gpl = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GRAPHICS_PIPELINE_LIBRARY_FEATURES_EXT, .graphicsPipelineLibrary = VK_TRUE};
    if (g_vk.gpl_probe) en_gpl.pNext = f.pNext, f.pNext = &en_gpl;
#endif
    VkPhysicalDeviceFeatures *b = &f.features, *h = &have.features;
    b->independentBlend = h->independentBlend;
    b->dualSrcBlend = h->dualSrcBlend;
    g_vk.logic_op = b->logicOp = h->logicOp;
    b->fillModeNonSolid = h->fillModeNonSolid;
    b->samplerAnisotropy = h->samplerAnisotropy;
    b->textureCompressionBC = h->textureCompressionBC;
    b->shaderInt64 = h->shaderInt64;
    b->depthBiasClamp = h->depthBiasClamp;
    b->depthClamp = h->depthClamp;
    b->largePoints = h->largePoints;
    b->wideLines = h->wideLines;
    b->imageCubeArray = h->imageCubeArray;
    b->sampleRateShading = h->sampleRateShading;
    b->fragmentStoresAndAtomics = h->fragmentStoresAndAtomics;
    b->vertexPipelineStoresAndAtomics = h->vertexPipelineStoresAndAtomics;
    b->robustBufferAccess = h->robustBufferAccess;  // guest shaders index cbufs freely: keep OOB reads harmless
    g_vk.ssbo_chunks = h->fragmentStoresAndAtomics && h->vertexPipelineStoresAndAtomics && h->robustBufferAccess;
    b->shaderClipDistance = h->shaderClipDistance;
    if (!h->shaderInt64) LOG("[vk] warning: no shaderInt64 (64-bit address math in shaders)\n");
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qi = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = g_vk.qfam,
                                  .queueCount = 1, .pQueuePriorities = &prio};
    VkDeviceCreateInfo di = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = &f, .queueCreateInfoCount = 1,
                             .pQueueCreateInfos = &qi, .enabledExtensionCount = n_ext, .ppEnabledExtensionNames = ext};
    VK_CHECK(vkCreateDevice(g_vk.phys, &di, NULL, &g_vk.dev));
#ifdef VK_NO_PROTOTYPES
    volkLoadDevice(g_vk.dev);
#endif
    vkGetDeviceQueue(g_vk.dev, g_vk.qfam, 0, &g_vk.queue);
    if (g_vk.vids) {
        g_vk.set_vertex_input = (PFN_vkCmdSetVertexInputEXT)vkGetDeviceProcAddr(g_vk.dev, "vkCmdSetVertexInputEXT");
        if (!g_vk.set_vertex_input) g_vk.vids = false;
        LOG("[vk] vertex input: %s\n", g_vk.vids ? "dynamic (VK_EXT_vertex_input_dynamic_state)" :
            "in each pipeline (vkCmdSetVertexInputEXT missing)");
    }

    VmaAllocatorCreateInfo ai = {.physicalDevice = g_vk.phys, .device = g_vk.dev, .instance = g_vk.inst,
                                 .vulkanApiVersion = VK_API_VERSION_1_3};
#ifdef VK_NO_PROTOTYPES
    VmaVulkanFunctions vf = {.vkGetInstanceProcAddr = vkGetInstanceProcAddr, .vkGetDeviceProcAddr = vkGetDeviceProcAddr};
    ai.pVulkanFunctions = &vf;
#endif
    VK_CHECK(vmaCreateAllocator(&ai, &g_vk.vma));
    vk_log_memory("start");

    VkSemaphoreTypeCreateInfo st = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
                                    .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE, .initialValue = 0};
    VkSemaphoreCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, .pNext = &st};
    VK_CHECK(vkCreateSemaphore(g_vk.dev, &si, NULL, &g_vk.timeline));
    VkCommandPoolCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                  .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = g_vk.qfam};
    VK_CHECK(vkCreateCommandPool(g_vk.dev, &pi, NULL, &g_vk.pool));

    // pipeline cache: cache/vk_pipelines.bin (written by vk_pipeline_cache_save), used only with its tag
    // (vk_pipelines.bin.tag: this build, the translator, the GPU and its driver - another one starts it empty)
    char tag[512];
    pipeline_cache_tag(tag, sizeof tag);
    size_t sz = 0;
    void *data = NULL;
    FILE *fp = shader_cache_tag_ok(PCACHE_TAG, tag) ? fopen(PCACHE_FILE, "rb") : NULL;
    if (!fp && !access(PCACHE_FILE, F_OK)) LOG("[vk] pipeline cache from another build / GPU / driver: starting a new one\n");
    if (fp) {
        fseek(fp, 0, SEEK_END);
        sz = (size_t)ftell(fp);
        fseek(fp, 0, SEEK_SET);
        data = malloc(sz);
        if (fread(data, 1, sz, fp) != sz) sz = 0;
        fclose(fp);
    }
    VkPipelineCacheCreateInfo pc = {.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO, .initialDataSize = sz,
                                    .pInitialData = data};
    if (vkCreatePipelineCache(g_vk.dev, &pc, NULL, &g_vk.pcache) != VK_SUCCESS) {
        pc.initialDataSize = 0;
        VK_CHECK(vkCreatePipelineCache(g_vk.dev, &pc, NULL, &g_vk.pcache));
    }
    free(data);
    if (pc.initialDataSize) LOG("[vk] pipeline cache: loaded %zu bytes\n", pc.initialDataSize);
    pcache_saved = pc.initialDataSize;
    pthread_t t;  // (kept every few minutes while it grows, not only at a clean exit)
    pthread_create(&t, NULL, pipeline_cache_saver, NULL);
    pthread_detach(t);
}

// Saved atomically (temp file + rename: a crash mid-write leaves the previous one), with its tag; one save at a time
void vk_pipeline_cache_save(void) {
    static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&lock);
    size_t sz = 0;
    if (vkGetPipelineCacheData(g_vk.dev, g_vk.pcache, &sz, NULL) == VK_SUCCESS && sz && sz != pcache_saved) {
        void *data = malloc(sz);
        if (vkGetPipelineCacheData(g_vk.dev, g_vk.pcache, &sz, data) == VK_SUCCESS && shader_cache_write(PCACHE_FILE, data, sz)) {
            char tag[512];
            pipeline_cache_tag(tag, sizeof tag);
            if (!shader_cache_tag_ok(PCACHE_TAG, tag)) shader_cache_tag_write(PCACHE_TAG, tag);
            pcache_saved = sz;
            LOG("[vk] pipeline cache: saved %zu bytes\n", sz);
        }
        free(data);
    }
    pthread_mutex_unlock(&lock);
}

// ================================================================== swapchain
static bool g_sc_turned;  // the compositor turns our images (IDENTITY on a rotated display): presents say "suboptimal"
static VkPresentModeKHR present_mode(void) {
    // MAILBOX by default: the game paces itself (vi.c's emulated 60 Hz vsync), and with FIFO every submission
    // queued behind the one waiting for a window image, so the game's fence waits followed the presentation engine:
    // a throttled / captured / busy compositor slowed the whole game (4-21 fps with the GPU mostly idle; mailbox:
    // 60). No tearing. NXRT_VK_PRESENT=fifo|immediate|mailbox to choose.
    const char *e = getenv("NXRT_VK_PRESENT");
    VkPresentModeKHR want = !e || !strcmp(e, "mailbox") ? VK_PRESENT_MODE_MAILBOX_KHR
                          : !strcmp(e, "immediate") ? VK_PRESENT_MODE_IMMEDIATE_KHR : VK_PRESENT_MODE_FIFO_KHR;
    uint32_t n = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(g_vk.phys, g_vk.surface, &n, NULL);
    VkPresentModeKHR m[16];
    if (n > 16) n = 16;
    vkGetPhysicalDeviceSurfacePresentModesKHR(g_vk.phys, g_vk.surface, &n, m);
    for (uint32_t i = 0; i < n; i++) if (m[i] == want) return want;
    return VK_PRESENT_MODE_FIFO_KHR;
}

static void create_swapchain(void) {
    VkSurfaceCapabilitiesKHR caps;
    VK_CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(g_vk.phys, g_vk.surface, &caps));
    int w, h;
    SDL_GetWindowSizeInPixels(window, &w, &h);
    VkExtent2D ext = caps.currentExtent.width != UINT32_MAX ? caps.currentExtent : (VkExtent2D){(uint32_t)w, (uint32_t)h};
    // A rotated display (a phone held in landscape: currentTransform 90 / 270): the images are made in the window's
    // own (landscape) shape - the extent it reports - and the compositor turns them (IDENTITY). (preTransform =
    // currentTransform would say they were turned already: the picture came out sideways. LASRecomp-Android-Plus
    // pre-rotates everything it draws instead, which saves the compositor's pass: a later optimisation.)
    VkSurfaceTransformFlagBitsKHR xf = caps.currentTransform;
    if (caps.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR) xf = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    g_vk.sc_quarter = 0;
    bool turned_by_system = xf != caps.currentTransform;
#ifdef __ANDROID__
    // Pre-rotation: the images in the panel's own (portrait) orientation with its transform, everything drawn into
    // them turned (vk_sc_rect, the present and UI vertex shaders). Otherwise the compositor turns the whole landscape
    // layer with the GPU at every present: ~13% of the GPU's time on a Snapdragon 865 (LASRecomp-Android-Plus).
    // NXRT_PRETRANSFORM=0: the system turns them.
    static int pre = -1;
    if (pre < 0) pre = getenv("NXRT_PRETRANSFORM") ? atoi(getenv("NXRT_PRETRANSFORM")) != 0 : 1;
    if (pre && (caps.currentTransform == VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR ||
                caps.currentTransform == VK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR)) {
        xf = caps.currentTransform;
        g_vk.sc_quarter = xf == VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR ? 1 : 3;
        turned_by_system = false;
    }
#endif
    g_sc_turned = turned_by_system;
    if (!ext.width || !ext.height) return;  // minimised
    uint32_t nf = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(g_vk.phys, g_vk.surface, &nf, NULL);
    VkSurfaceFormatKHR fmts[64];
    if (nf > 64) nf = 64;
    vkGetPhysicalDeviceSurfaceFormatsKHR(g_vk.phys, g_vk.surface, &nf, fmts);
    // scanout shows the bytes the game wrote (see the raw views in the present pass): a UNORM swapchain like Metal's
    VkSurfaceFormatKHR sf = fmts[0];
    for (uint32_t i = 0; i < nf; i++)
        if (fmts[i].format == VK_FORMAT_B8G8R8A8_UNORM && fmts[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) { sf = fmts[i]; break; }
    uint32_t count = caps.minImageCount + 1;
    if (caps.maxImageCount && count > caps.maxImageCount) count = caps.maxImageCount;
    if (count > 8) count = 8;
    VkSwapchainKHR old = g_vk.swapchain;
    // (Android's compositors often take INHERIT only)
    VkCompositeAlphaFlagBitsKHR alpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    if (!(caps.supportedCompositeAlpha & alpha))
        for (uint32_t b = 1; b <= VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR; b <<= 1)
            if (caps.supportedCompositeAlpha & b) { alpha = (VkCompositeAlphaFlagBitsKHR)b; break; }
    VkExtent2D phys = g_vk.sc_quarter ? (VkExtent2D){ext.height, ext.width} : ext;
    VkSwapchainCreateInfoKHR ci = {
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR, .surface = g_vk.surface, .minImageCount = count,
        .imageFormat = sf.format, .imageColorSpace = sf.colorSpace, .imageExtent = phys, .imageArrayLayers = 1,
        .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE, .preTransform = xf,
        .compositeAlpha = alpha, .presentMode = present_mode(), .clipped = VK_TRUE,
        .oldSwapchain = old};
    VkSwapchainKHR next = VK_NULL_HANDLE;
    VkResult result = vkCreateSwapchainKHR(g_vk.dev, &ci, NULL, &next);
    // A fullscreen transition can invalidate the surface between querying it and creating the swapchain.
    // Passing oldSwapchain retires it even when creation fails, so retry with a fresh chain next frame.
    if (result != VK_SUCCESS && result != VK_ERROR_OUT_OF_DATE_KHR && result != VK_ERROR_INITIALIZATION_FAILED)
        VK_CHECK(result);
    for (uint32_t i = 0; i < g_vk.sc_count; i++) vkDestroyImageView(g_vk.dev, g_vk.sc_views[i], NULL);
    if (old) vkDestroySwapchainKHR(g_vk.dev, old, NULL);
    g_vk.swapchain = result == VK_SUCCESS ? next : VK_NULL_HANDLE;
    g_vk.sc_count = 0;
    if (result != VK_SUCCESS) {
        LOG("[vk] surface changing (%d); retrying swapchain next frame\n", result);
        return;
    }
    g_vk.sc_format = sf.format;
    g_vk.sc_extent = ext;
    g_vk.sc_phys = phys;
    if (g_vk.sc_quarter) LOG("[vk] swapchain pre-rotated %d degrees (images %ux%u)\n", g_vk.sc_quarter * 90, phys.width, phys.height);
    g_vk.sc_count = 8;
    vkGetSwapchainImagesKHR(g_vk.dev, g_vk.swapchain, &g_vk.sc_count, g_vk.sc_images);
    for (uint32_t i = 0; i < g_vk.sc_count; i++) {
        VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = g_vk.sc_images[i],
                                    .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = sf.format,
                                    .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
        VK_CHECK(vkCreateImageView(g_vk.dev, &vi, NULL, &g_vk.sc_views[i]));
    }
}

VkRect2D vk_sc_rect(int x, int y, int w, int h) {
    int W = (int)g_vk.sc_extent.width, H = (int)g_vk.sc_extent.height;
    if (g_vk.sc_quarter == 1) return (VkRect2D){{H - y - h, x}, {(uint32_t)h, (uint32_t)w}};
    if (g_vk.sc_quarter == 3) return (VkRect2D){{y, W - x - w}, {(uint32_t)h, (uint32_t)w}};
    return (VkRect2D){{x, y}, {(uint32_t)w, (uint32_t)h}};
}

static void recreate_swapchain(void) {
    rec_drain();  // (every queued present and submission made: the queue is idle for the wait)
    vkDeviceWaitIdle(g_vk.dev);
    create_swapchain();
}

// Phones: an app in the background has no window - its surface is gone, and drawing waits until it's in front again,
// then the window gets a new surface and swapchain (after LASRecomp-Android-Plus). Desktop windows never get these.
static atomic_bool g_background, g_surface_stale;

static bool SDLCALL lifecycle_watch(void *user, SDL_Event *e) {
    (void)user;
    if (e->type == SDL_EVENT_WILL_ENTER_BACKGROUND || e->type == SDL_EVENT_DID_ENTER_BACKGROUND) {
        if (!atomic_exchange(&g_background, true)) {
            LOG("[vk] in the background: drawing paused\n");
            if (g_vk.dev) vk_pipeline_cache_save();  // (phones end apps in the background without exit())
        }
    } else if (e->type == SDL_EVENT_DID_ENTER_FOREGROUND && atomic_load(&g_background)) {
        LOG("[vk] in front again\n");
        atomic_store(&g_surface_stale, true);
        atomic_store(&g_background, false);
    }
    return true;
}

static void new_surface(void) {
    vkDeviceWaitIdle(g_vk.dev);
    for (uint32_t i = 0; i < g_vk.sc_count; i++) vkDestroyImageView(g_vk.dev, g_vk.sc_views[i], NULL);
    g_vk.sc_count = 0;
    if (g_vk.swapchain) vkDestroySwapchainKHR(g_vk.dev, g_vk.swapchain, NULL);
    g_vk.swapchain = VK_NULL_HANDLE;
    if (g_vk.surface) vkDestroySurfaceKHR(g_vk.inst, g_vk.surface, NULL);
    g_vk.surface = VK_NULL_HANDLE;
    if (!SDL_Vulkan_CreateSurface(window, g_vk.inst, NULL, &g_vk.surface)) {
        LOG("[vk] new surface: %s\n", SDL_GetError());
        atomic_store(&g_surface_stale, true);  // (the window isn't back yet: next frame)
        return;
    }
    create_swapchain();
}

// ================================================================== frames (start screen and game)
// Acquire semaphores rotate per frame in flight; a slot is reused once the submission that waited on it is done.
#define FRAMES 3
static VkSemaphore acquire_sem[FRAMES], done_sem[8];
static uint64_t slot_seq[FRAMES];
static int slot;

static VkSemaphore binary_semaphore(void) {
    VkSemaphoreCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VkSemaphore s;
    VK_CHECK(vkCreateSemaphore(g_vk.dev, &si, NULL, &s));
    return s;
}

static void image_barrier(VkCommandBuffer cb, VkImage img, VkImageLayout from, VkImageLayout to) {
    VkImageMemoryBarrier2 b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                               .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT,
                               .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                               .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                               .oldLayout = from, .newLayout = to, .image = img,
                               .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
                               .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &b};
    rec_CmdPipelineBarrier2(cb, &di);
}

typedef struct { uint32_t image; VkSemaphore acquired; bool ok; } Frame;
static Frame cur_frame;

// Acquire the next window image and begin rendering into it (cleared to `clear`). Returns false when there is
// nothing to draw into (minimised window).
bool vk_frame_begin(const float clear[4]) {
    cur_frame.ok = false;
    if (rq_stopping()) return false;  // quitting: no window image after the window closed (#40)
    int w, h;
    SDL_GetWindowSizeInPixels(window, &w, &h);
    if (!w || !h) return false;  // minimised
    if (atomic_load(&g_background)) return false;
    if (atomic_exchange(&g_surface_stale, false)) new_surface();
    if (!g_vk.surface) return false;
    if (!g_vk.swapchain || (uint32_t)w != g_vk.sc_extent.width || (uint32_t)h != g_vk.sc_extent.height) recreate_swapchain();
    if (!g_vk.swapchain) return false;
    static int trace = -1;
    if (trace < 0) trace = getenv("NXRT_VK_TRACE") != NULL;
    if (trace) LOG("[vk] frame begin %dx%d (swapchain %ux%u)\n", w, h, g_vk.sc_extent.width, g_vk.sc_extent.height);
    slot = (slot + 1) % FRAMES;
    if (!acquire_sem[slot]) acquire_sem[slot] = binary_semaphore();
    WAITING_AT(0, vk_wait_seq(slot_seq[slot]));
    VkResult r;
    WAITING_AT(1, r = vkAcquireNextImageKHR(g_vk.dev, g_vk.swapchain, UINT64_MAX, acquire_sem[slot], VK_NULL_HANDLE, &cur_frame.image));
    if (r == VK_ERROR_OUT_OF_DATE_KHR) {
        recreate_swapchain();
        if (!g_vk.swapchain) return false;
        r = vkAcquireNextImageKHR(g_vk.dev, g_vk.swapchain, UINT64_MAX, acquire_sem[slot], VK_NULL_HANDLE, &cur_frame.image);
    }
    if (r == VK_ERROR_SURFACE_LOST_KHR) { atomic_store(&g_surface_stale, true); return false; }
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) { LOG("[vk] acquire failed: %d\n", r); return false; }
    cur_frame.acquired = acquire_sem[slot];
    slot_seq[slot] = vk_pending_seq();
    VkCommandBuffer cb = vk_cmd();
    image_barrier(cb, g_vk.sc_images[cur_frame.image], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    vk_gpu_timer_window_start();  // (the GPU timer: this submission's work starts after that barrier, which waits for the image)
    VkRenderingAttachmentInfo ca = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = g_vk.sc_views[cur_frame.image],
                                    .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                    .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
                                    .clearValue.color.float32 = {clear[0], clear[1], clear[2], clear[3]}};
    VkRenderingInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = {{0, 0}, g_vk.sc_phys}, .layerCount = 1,
                          .colorAttachmentCount = 1, .pColorAttachments = &ca};
    vkCmdBeginRendering(cb, &ri);
    VkViewport vp = {0, 0, (float)g_vk.sc_phys.width, (float)g_vk.sc_phys.height, 0, 1};
    VkRect2D sc = {{0, 0}, g_vk.sc_phys};
    vkCmdSetViewport(cb, 0, 1, &vp);
    vkCmdSetScissor(cb, 0, 1, &sc);
    extern void vk_imgui_prerotation(int quarter);  // (the UI drawn into this image turned like it: imgui_glue.cpp)
    vk_imgui_prerotation(g_vk.sc_quarter);
    cur_frame.ok = true;
    return true;
}

// Passes of their own between vk_frame_begin and vk_frame_end (exec.c post passes): end the window image's
// rendering, and begin it again keeping what was drawn.
void vk_frame_suspend(void) {
    if (cur_frame.ok) rec_CmdEndRendering(vk_cmd());
}
void vk_frame_resume(void) {
    if (!cur_frame.ok) return;
    VkCommandBuffer cb = vk_cmd();
    VkRenderingAttachmentInfo ca = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = g_vk.sc_views[cur_frame.image],
                                    .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                    .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD, .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
    VkRenderingInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = {{0, 0}, g_vk.sc_phys}, .layerCount = 1,
                          .colorAttachmentCount = 1, .pColorAttachments = &ca};
    vkCmdBeginRendering(cb, &ri);
    VkViewport vp = {0, 0, (float)g_vk.sc_phys.width, (float)g_vk.sc_phys.height, 0, 1};
    VkRect2D sc = {{0, 0}, g_vk.sc_phys};
    vkCmdSetViewport(cb, 0, 1, &vp);
    vkCmdSetScissor(cb, 0, 1, &sc);
}

VkExtent2D vk_frame_extent(void) { return g_vk.sc_extent; }
// The window image's command buffer for direct recording (the start screen's ImGui): the recorder has recorded
// everything queued before it, and records what is queued after it.
void *vk_frame_cmd(void) {
    VkCommandBuffer cb = vk_cmd();
    rec_drain();
    return (void *)cb;
}

static void save_window_image(VkCommandBuffer cb, VkSpan *out) {
    uint32_t w = g_vk.sc_phys.width, h = g_vk.sc_phys.height;
    *out = vk_readback((uint64_t)w * h * 4);
    image_barrier(cb, g_vk.sc_images[cur_frame.image], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    VkBufferImageCopy rg = {.bufferOffset = out->off, .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                            .imageExtent = {w, h, 1}};
    rec_CmdCopyImageToBuffer(cb, g_vk.sc_images[cur_frame.image], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, out->buf, 1, &rg);
    image_barrier(cb, g_vk.sc_images[cur_frame.image], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
}

// End rendering into the window image and present it. shot_path: also save the window image as PNG.
void vk_frame_end(const char *shot_path) {
    if (!cur_frame.ok) return;
    VkCommandBuffer cb = vk_cmd();
    rec_CmdEndRendering(cb);
    VkSpan shot = {0};
    if (shot_path) save_window_image(cb, &shot);
    image_barrier(cb, g_vk.sc_images[cur_frame.image], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    uint32_t img = cur_frame.image;
    if (!done_sem[img]) done_sem[img] = binary_semaphore();
    uint64_t seq = vk_pending_seq();
    vk_submit_extra(cur_frame.acquired, done_sem[img], VK_NULL_HANDLE);
    VkPresentInfoKHR pi = {.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, .waitSemaphoreCount = 1, .pWaitSemaphores = &done_sem[img],
                           .swapchainCount = 1, .pSwapchains = &g_vk.swapchain, .pImageIndices = &img};
    VkResult r = vkQueuePresentKHR(g_vk.queue, &pi);
    if (r == VK_ERROR_SURFACE_LOST_KHR) atomic_store(&g_surface_stale, true);
    // (a swapchain the system turns, IDENTITY on a rotated display, is "suboptimal" at every present: that's this,
    // not a new size - recreating it each frame would cost the frame)
    else if (r == VK_ERROR_OUT_OF_DATE_KHR || (r == VK_SUBOPTIMAL_KHR && !g_sc_turned)) recreate_swapchain();
    cur_frame.ok = false;
    if (shot_path) {
        vk_wait_seq(seq);
        uint32_t w = g_vk.sc_phys.width, h = g_vk.sc_phys.height;
        uint8_t *px = shot.ptr;
        bool bgr = g_vk.sc_format == VK_FORMAT_B8G8R8A8_UNORM || g_vk.sc_format == VK_FORMAT_B8G8R8A8_SRGB;
        for (uint64_t i = 0; bgr && i < (uint64_t)w * h; i++) { uint8_t t = px[i * 4]; px[i * 4] = px[i * 4 + 2]; px[i * 4 + 2] = t; }
        for (uint64_t i = 0; i < (uint64_t)w * h; i++) px[i * 4 + 3] = 255;
        if (g_vk.sc_quarter) {  // (the pre-rotated image turned back into the window's shape)
            uint32_t W = h, H = w;  // landscape
            uint8_t *t = malloc((size_t)W * H * 4);
            for (uint32_t y = 0; y < H; y++)
                for (uint32_t x = 0; x < W; x++) {
                    uint32_t qx = g_vk.sc_quarter == 1 ? H - 1 - y : y, qy = g_vk.sc_quarter == 1 ? x : W - 1 - x;
                    memcpy(t + ((size_t)y * W + x) * 4, px + ((size_t)qy * w + qx) * 4, 4);
                }
            vk_save_png(shot_path, t, W, H);
            free(t);
        } else {
            vk_save_png(shot_path, px, w, h);
        }
    }
}

void vk_save_png(const char *path, const uint8_t *rgba, uint32_t w, uint32_t h) {
    SDL_Surface *s = SDL_CreateSurfaceFrom((int)w, (int)h, SDL_PIXELFORMAT_ABGR8888, (void *)rgba, (int)w * 4);
    if (!s || !SDL_SavePNG(s, path)) LOG("[vk] saving %s failed: %s\n", path, SDL_GetError());
    else LOG("[vk] saved %s (%ux%u)\n", path, w, h);
    SDL_DestroySurface(s);
}

// ================================================================== game presentation (metal.m backend_present)
// Draws the game's image into the window with the widescreen crop, then the overlay menu.
extern void vk_present_draw(VkCommandBuffer cb, TexBackend *b, float vx, float vy, float vw, float vh, const float pv[8]);
extern bool g_debug_frame;  // (exec.c; set in present_hooks, read in the render thread's present)
void renderer_submit(void);
void renderer_set_scale(float sx, float sy);
uint64_t renderer_present_count(void);
void renderer_save_texture_tex(TexBackend *b, const char *path);

static uint64_t frames, total_frames, g_frame_counter;
uint64_t renderer_present_count(void) { return total_frames; }  // (the game thread's frame: the HLE hooks')
// The frame whose work is being translated (rqueue.h rq_frame): "once per frame" state of the translation. Without the
// render thread, total_frames.
uint64_t renderer_exec_frame(void) { return rq_current_item() ? rq_frame() : total_frames; }
uint64_t vk_frame_counter(void) { return renderer_exec_frame(); }  // (the translation's: g_frame_counter == total_frames)
static float g_shot_crop[4] = {0, 0, 1, 1};
static double last_fps_time;
static NvnTexture *g_last_presented;

const float *vk_shot_crop(void) { return g_shot_crop; }

void backend_present(NvnTexture *t);
void *backend_pool_push(void) { return NULL; }
void backend_pool_pop(void *p) { (void)p; }
void backend_represent(void) {
    if (!g_last_presented) { SDL_Delay(16); return; }
    if (rq_on()) rq_present(g_last_presented, total_frames, false);
    else backend_present_now(g_last_presented, total_frames);
}

void backend_present(NvnTexture *t) {
    static uint64_t pres_count;
    if (++pres_count % 60 == 1) LOG("[vk] present frame %llu\n", (unsigned long long)pres_count);
    g_last_presented = t;
    TexBackend *b = vk_tex(t, true);
    b->presented = true;
    vk_end_pass();
    // the frame's work goes to the GPU now, in a submission of its own: it starts while we wait for the window's image.
    // Its GPU time is counted, and the window image's submission's from after its wait (exec.c gpu_timer_add)
    renderer_submit();
    pp_mark(PP_SUBMIT);
    static const float black[4] = {0, 0, 0, 1};
    uint64_t t0 = SDL_GetTicksNS();
    bool have = vk_frame_begin(black);
    pp_mark(PP_ACQUIRE);
    uint64_t nd = SDL_GetTicksNS() - t0;
    if (nd > 30000000) {
        static int warned;
        if (warned++ < 20) LOG("[vk] acquire blocked %llums\n", (unsigned long long)(nd / 1000000));
    }
    if (have) {
        double dw = g_vk.sc_extent.width, dh = g_vk.sc_extent.height;
        if (NX_HOOK(present_frame_end)) NX_HOOK(present_frame_end)(total_frames);
        // render scale + placement in the window: runtime/nvn/output_scale.c (Video tab "Internal resolution");
        // GPU time since the last frame (exec.c's GPU timer; -1 without one: dynamic resolution goes by frame times):
        // the game's submissions and the window image's (NXRT_DYNRES_COUNT_PRESENT=0: the game's only, as before)
        static int count_present = -1;
        if (count_present < 0) count_present = !getenv("NXRT_DYNRES_COUNT_PRESENT") || atoi(getenv("NXRT_DYNRES_COUNT_PRESENT"));
        static uint64_t sp_prev[4], wt_prev[4], cw_prev;
        static bool sp_have;
        uint64_t sp[4], wt[4];
        for (int i = 0; i < 4; i++) wt[i] = __atomic_load_n(&g_vk_frame_waits[i], __ATOMIC_RELAXED);  // ([2], [3]: the recorder's)
        double game_ms = -1, win_ms = -1;
        if (vk_gpu_split_ns(sp)) {
            uint64_t cw = vk_cum_wait_ns();
            if (sp_have) {
                double game = (sp[0] - sp_prev[0]) / 1e6, win = (sp[1] - sp_prev[1]) / 1e6;
                game_ms = game, win_ms = count_present ? win : -1;
                double slotw = (wt[0] - wt_prev[0]) / 1e6;
                DynSplit ds = {game, win, (sp[2] - sp_prev[2]) / 1e6, (double)(sp[3] - sp_prev[3]), slotw,
                               (wt[1] - wt_prev[1]) / 1e6, (wt[2] - wt_prev[2]) / 1e6,
                               fmax(0, (cw - cw_prev) / 1e6 - slotw), count_present != 0};
                output_scale_note_split(&ds);
            }
            memcpy(sp_prev, sp, sizeof sp);
            memcpy(wt_prev, wt, sizeof wt_prev);
            cw_prev = cw;
            sp_have = true;
        }
        OutScale os;
        output_scale(dw, dh, NX_HOOK(present_wide) && NX_HOOK(present_wide)(), SDL_GetTicksNS() / 1e9, -1, &os);
        double sx = os.sx, sy = os.sy, vx = os.vx, vy = os.vy, vw = os.vw, vh = os.vh, shape = os.shape;
        output_scale_log((unsigned long long)frame, dw, dh, &os);  // (diagnostics: [scale] on a change)
        // interiors / dungeons: their 16:9 centre, or the part inside a room wider than one screen
        float crop[4] = {0, 0, 1, 1};  // uv origin and size (the game may show part of the image: present_crop)
        if (NX_HOOK(present_crop)) NX_HOOK(present_crop)(total_frames, shape, crop, &vx, &vy, &vw, &vh);
        if (NX_HOOK(present_shape)) NX_HOOK(present_shape)((float)shape);
        renderer_set_scale((float)sx, (float)sy);
        pp_mark(PP_SCALE);
        VkCommandBuffer cb = vk_cmd();
        float pv[8] = {renderer_origin_lower_left() ? 1.0f : 0.0f, 0, 0, 0, crop[0], crop[1], crop[2], crop[3]};
        memcpy(g_shot_crop, crop, sizeof crop);
        vk_present_draw(cb, b, (float)vx, (float)vy, (float)vw, (float)vh, pv);
        pp_mark(PP_BLIT);
        static long menu_at = -1;
        if (menu_at < 0) menu_at = getenv("NXRT_MENU_AT") ? atol(getenv("NXRT_MENU_AT")) : 0;
        if (menu_at && (long)total_frames + 1 == menu_at) { extern void menu_test_open(void); menu_test_open(); }
        menu_render((void *)cb, g_vk.sc_extent.width, g_vk.sc_extent.height);  // the menu and overlays (nx-ui)
        static long win_shot = -1;
        if (win_shot < 0) win_shot = getenv("NXRT_WINDOW_SHOT") ? atol(getenv("NXRT_WINDOW_SHOT")) : 0;
        bool shot = win_shot && (long)total_frames + 1 == win_shot;
        extern char g_window_shot_request[256];
        vk_frame_end(g_window_shot_request[0] ? g_window_shot_request : shot ? "build/window.png" : NULL);
        g_window_shot_request[0] = 0;
        if (shot && getenv("NXRT_EXIT_AFTER_SHOT")) { fflush(stderr); _exit(0); }
    } else {
        renderer_submit();
    }
    pp_mark(PP_END);
    pp_frames++;
    extern void vk_prof_frame(uint64_t frame, uint64_t present_ns);
    vk_prof_frame(frame, SDL_GetTicksNS() - t0);
    frames++;
    total_frames++;
    g_frame_counter++;
    extern void vk_pace_frame(void);
    vk_pace_frame();  // (exec.c: the game's 30 fps mode when frames don't fit 60)
    extern void input_frame(uint64_t frame);
    input_frame(total_frames);
    static long slow_ms = -1;
    if (slow_ms < 0) slow_ms = getenv("NXRT_SLOW_FRAME_MS") ? atol(getenv("NXRT_SLOW_FRAME_MS")) : 0;
    static long slow_until = -1;  // NXRT_SLOW_FRAME_UNTIL=<frame>: the stall ends there (frame pacing recovery tests)
    if (slow_until < 0) slow_until = getenv("NXRT_SLOW_FRAME_UNTIL") ? atol(getenv("NXRT_SLOW_FRAME_UNTIL")) : 0;
    if (slow_ms > 0 && total_frames > 2600 && (!slow_until || (long)total_frames < slow_until)) SDL_Delay((Uint32)slow_ms);
    extern void fnprof_frame(uint64_t frame);
    fnprof_frame(total_frames);
    if (NX_HOOK(present_tick)) NX_HOOK(present_tick)(total_frames);
    extern void reboot_test_frame(uint64_t frame);  // runtime/main.c (NXRT_REBOOT_AT)
    reboot_test_frame(total_frames);
    extern void vk_level_change_check(void);
    vk_level_change_check();
    static long shot_every = -1;
    if (shot_every < 0) { const char *e = getenv("NXRT_SCREENSHOT_EVERY"); shot_every = e ? atol(e) : 0; }
    extern bool g_debug_frame;
    static long debug_at = -1;
    if (debug_at < 0) { const char *e = getenv("NXRT_DEBUG_FRAME"); debug_at = e ? atol(e) : 0; }
    extern volatile int g_debug_frame_request;
    if (g_debug_frame_request) { g_debug_frame_request = 0; debug_at = (long)total_frames + 1; }
    // NXRT_DEBUG_FRAMES=<n> (with NXRT_DEBUG_FRAME / F9): dump n consecutive frames (build/pass_<frame>_NNN.png)
    static long debug_n = -1;
    if (debug_n < 0) { const char *e = getenv("NXRT_DEBUG_FRAMES"); debug_n = e && atol(e) > 1 ? atol(e) : 1; }
    g_debug_frame = debug_at && total_frames >= (uint64_t)debug_at && total_frames < (uint64_t)debug_at + (uint64_t)debug_n;
    {   // NXRT_GPUPROF_AT=<frame>[,<frame>...]: GPU time per pass of those frames ("[gpuprof]" in the log)
        extern bool g_gpuprof_frame;
        extern void vk_gpuprof_report(void);
        if (g_gpuprof_frame) { g_gpuprof_frame = false; vk_gpuprof_report(); }
        const char *e = getenv("NXRT_GPUPROF_AT");
        for (const char *p = e; p && *p;) {
            long at = strtol(p, (char **)&p, 10);
            if (at == (long)frame) { g_gpuprof_frame = true; break; }
            if (*p == ',') p++; else break;
        }
    }
    if (g_debug_frame) LOG("===== debug frame %llu =====\n", (unsigned long long)total_frames);
    {   // NXRT_SHOT_AT=<frame>,<frame>,... (ascending): the game image at those frames -> build/frames/frame_N.png
        static const char *next;
        static int init;
        if (!init) { init = 1; next = getenv("NXRT_SHOT_AT"); if (next) mkdir("build/frames", 0755); }
        while (next && *next) {
            long at = strtol(next, (char **)&next, 10);
            if ((long)total_frames < at) { while (next > getenv("NXRT_SHOT_AT") && next[-1] != ',') next--; break; }
            if ((long)total_frames == at) {
                char path[256];
                snprintf(path, sizeof path, "build/frames/frame_%05llu.png", (unsigned long long)frame);
                renderer_save_texture_tex(b, path);
            }
            if (*next == ',') next++;
        }
    }
    static long shot_from = -1;
    if (shot_from < 0) { const char *e = getenv("NXRT_SCREENSHOT_FROM"); shot_from = e ? atol(e) : 0; }
    if (shot_every && total_frames % shot_every == 0 && (long)total_frames >= shot_from) {
        char path[256];
        if (shot_from) mkdir("build/frames", 0755);
        snprintf(path, sizeof path, shot_from ? "build/frames/frame_%05llu.png" : "build/frame_%05llu.png",
                 (unsigned long long)frame);
        renderer_save_texture_tex(b, path);
        // a series (SCREENSHOT_FROM) runs on until NXRT_WINDOW_SHOT exits
        if (getenv("NXRT_EXIT_AFTER_SHOT") && !shot_from) { fflush(stderr); _exit(0); }
    }
    double now = SDL_GetTicks() / 1000.0;
    static double fps_iv = -1, prev_present, max_gap;
    if (fps_iv < 0) fps_iv = getenv("NXRT_FPS_INTERVAL") ? atof(getenv("NXRT_FPS_INTERVAL")) : 2.0;
    if (prev_present > 0 && now - prev_present > max_gap) max_gap = now - prev_present;
    static double hitch_ms = -1;  // NXRT_LOG_HITCH_MS=<ms>: every present gap over that (tests: hitches per run)
    if (hitch_ms < 0) hitch_ms = getenv("NXRT_LOG_HITCH_MS") ? atof(getenv("NXRT_LOG_HITCH_MS")) : 0;
    if (hitch_ms > 0 && prev_present > 0 && (now - prev_present) * 1000 > hitch_ms)
        LOG("[hitch] t=%.2f frame %llu gap %.0f ms\n", now, (unsigned long long)frame, (now - prev_present) * 1000);
    prev_present = now;
    pp_mark(PP_REST);  // (the fps line itself is not counted)
    if (now - last_fps_time >= fps_iv) {
        if (last_fps_time > 0)
            LOG("[vk] t=%.1f frame %llu %.1f fps max %.0fms level %s (%s)\n", now, (unsigned long long)total_frames,
                frames / (now - last_fps_time), max_gap * 1000.0, NX_HOOK(scene_name) ? NX_HOOK(scene_name)() : "-", renderer_stats());
        max_gap = 0;
        frames = 0;
        last_fps_time = now;
    }
    static double mem_at = 60.0;  // (memory in steady state: a minute in, then every 10 minutes)
    if (now >= mem_at) { vk_log_memory("playing"); mem_at = now + 600.0; }
}

// The game thread's side of a present: the frame counters and the per-frame hooks (input, pacing, test hooks).
static void present_hooks(void) {
    total_frames++;
    g_frame_counter++;
    pace_frame();  // (pace.c: the game's 30 fps mode when frames don't fit 60)
    extern void input_frame(uint64_t frame);
    input_frame(total_frames);
    static long slow_ms = -1;  // NXRT_SIM_SLOW_FRAME_MS=<ms>: sleep per presented frame (simulate a slow machine)
    if (slow_ms < 0) slow_ms = getenv("NXRT_SIM_SLOW_FRAME_MS") ? atol(getenv("NXRT_SIM_SLOW_FRAME_MS")) : 0;
    static long slow_until = -1;  // NXRT_SLOW_FRAME_UNTIL=<frame>: the stall ends there (frame pacing recovery tests)
    if (slow_until < 0) slow_until = getenv("NXRT_SLOW_FRAME_UNTIL") ? atol(getenv("NXRT_SLOW_FRAME_UNTIL")) : 0;
    if (slow_ms > 0 && total_frames > 2600 && (!slow_until || (long)total_frames < slow_until)) SDL_Delay((Uint32)slow_ms);
    extern void fnprof_frame(uint64_t frame);
    fnprof_frame(total_frames);
    extern void warp_frame(uint64_t frame);
    warp_frame(total_frames);
    extern void reboot_test_frame(uint64_t frame);  // runtime/main.c (NXRT_REBOOT_AT)
    reboot_test_frame(total_frames);
    extern void save_editor_frame(uint64_t frame);
    save_editor_frame(total_frames);
    extern void vk_level_change_check(void);
    vk_level_change_check();
    static long debug_at = -1;
    if (debug_at < 0) { const char *e = getenv("NXRT_DEBUG_FRAME"); debug_at = e ? atol(e) : 0; }
    extern volatile int g_debug_frame_request;
    if (g_debug_frame_request) { g_debug_frame_request = 0; debug_at = (long)total_frames + 1; }
    // NXRT_DEBUG_FRAMES=<n> (with NXRT_DEBUG_FRAME / F9): dump n consecutive frames (build/pass_<frame>_NNN.png)
    static long debug_n = -1;
    if (debug_n < 0) { const char *e = getenv("NXRT_DEBUG_FRAMES"); debug_n = e && atol(e) > 1 ? atol(e) : 1; }
    g_debug_frame = debug_at && total_frames >= (uint64_t)debug_at && total_frames < (uint64_t)debug_at + (uint64_t)debug_n;
}

void backend_present(NvnTexture *t) {
    g_last_presented = t;
    uint64_t frame = total_frames, t0 = plat_now_ns();
    if (rq_on()) rq_present(t, frame, true);
    else backend_present_now(t, frame);
    uint64_t t1 = plat_now_ns();
    g_present_ns += t1 - t0;  // (NXRT_LOG_PACE, pace.c)
    present_hooks();
    g_hooks_ns += plat_now_ns() - t1;
}

// ================================================================== UI thread
void vk_exec_init(void);
void vk_shaders_init(void);

// The end of a validation run (NXRT_VK_VALIDATION, after backend_quit: the GPU idle, the pipeline cache kept):
// the debug messenger goes, then the process ends at once - no vkDestroyDevice / vkDestroyInstance, no atexit
// handlers, no DLL teardown. _exit() alone isn't enough on Windows: it ends in ExitProcess, which runs the layer's
// and the driver's DLL teardown after every other thread is gone, and with the Khronos layer on NVIDIA that left the
// process hung there and unkillable (one thread waiting, gigabytes of the layer's state).
static _Noreturn void validation_exit(void) {
    if (messenger) {
        PFN_vkDestroyDebugUtilsMessengerEXT f =
            (PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr(g_vk.inst, "vkDestroyDebugUtilsMessengerEXT");
        VkDebugUtilsMessengerEXT m = messenger;
        messenger = VK_NULL_HANDLE;
        if (f) f(g_vk.inst, m, NULL);
    }
    LOG("[vk] validation run: ending without GPU teardown\n");
    fflush(stdout);
    fflush(stderr);
#if defined(_WIN32) || defined(__linux__)
    plat_terminate();
#endif
    _Exit(0);
}

// NXRT_EXIT_AFTER_SHOT: the test run ends with its shot. As it always did (_exit: no cache save), except under
// validation: backend_quit, then validation_exit.
static void exit_after_shot(void) {
    fflush(stderr);
    if (validating) {
        backend_quit();
        validation_exit();
    }
    _exit(0);
}

// Quitting, before exit() (the window closed; also from exit() itself, quick_exit_handler): the render thread stops
// between items, so no image is acquired or presented any more; the recorder thread stops making driver calls (the
// packets it still had are dropped, record.c rec_quit) and is idle; then the GPU finishes and the pipeline cache is
// kept. All of it before exit() runs the atexit handlers and the libraries' destructors: a Vulkan layer loaded with
// the instance (the Steam overlay on the Steam Deck, #40) tears its swapchain state down there, and a present from
// the render thread after that crashed in vkQueuePresentKHR. The swapchain, surface and window are never destroyed
// explicitly: the process ends with them.
void backend_quit(void) {
    static atomic_flag once = ATOMIC_FLAG_INIT;
    if (atomic_flag_test_and_set(&once)) return;
    static int trace = -1;
    if (trace < 0) trace = getenv("NXRT_VK_TRACE") != NULL;
    if (trace) LOG("[vk] exit: waiting for the GPU\n");
    if (g_vk.dev) {
        vkDeviceWaitIdle(g_vk.dev);
        if (trace) LOG("[vk] exit: saving the pipeline cache\n");
        vk_pipeline_cache_save();
    }
    if (trace) LOG("[vk] exit: terminating\n");
    fflush(stdout);
    fflush(stderr);
#if defined(_WIN32) || defined(__linux__)
    plat_terminate();
#endif
}

// macOS: the .app's own MoltenVK (Contents/Resources/vulkan/icd.d, tools/bundle_macos.sh), not another one installed
// on the machine (Homebrew's: a different version, and the device listed twice); a source build has none. Before
// SDL_Init: SDL's Vulkan loading scans the drivers too.
static void use_bundled_driver(void) {
#ifdef __APPLE__
    char icd[1100];
    if (!getenv("VK_DRIVER_FILES") && !getenv("VK_ICD_FILENAMES") &&
        rt_resource("vulkan/icd.d/MoltenVK_icd.json", NULL, icd, sizeof icd))
        setenv("VK_DRIVER_FILES", icd, 1);
#endif
}

void ui_window_init(void) {
    if (window) return;
    LOG("[vk] initializing window\n");
    atexit(quick_exit_handler);
#ifdef _WIN32
    char manifest[1100], module[1100];
    settings_load();
    const char *reshade = getenv("NXRT_RESHADE");
    bool use_reshade = g_settings.reshade && (!reshade || strcmp(reshade, "0"));
    if (!use_reshade) {
        setenv("DISABLE_VK_LAYER_reshade_1", "1", 1);
        setenv("VK_LOADER_LAYERS_DISABLE", "VK_LAYER_reshade", 1);
        LOG("[reshade] disabled by settings\n");
    }
    if (use_reshade &&
        rt_resource("reshade/ReShade64.json", "reshade/ReShade64.json", manifest, sizeof manifest) &&
        rt_resource("reshade/ReShade64.dll", "reshade/ReShade64.dll", module, sizeof module)) {
        char *end = strrchr(manifest, '/'), *back = strrchr(manifest, '\\');
        if (back && (!end || back > end)) end = back;
        if (end) {
            *end = 0;
            char paths[4096];
            const char *key = getenv("VK_LAYER_PATH") ? "VK_LAYER_PATH" : "VK_ADD_LAYER_PATH";
            const char *prior = getenv(key);
            if (snprintf(paths, sizeof paths, "%s%s%s", manifest, prior && *prior ? ";" : "", prior ? prior : "") < sizeof paths) {
                setenv(key, paths, 1);
                const char *layers = getenv("VK_INSTANCE_LAYERS");
                if (!layers || !strstr(layers, "VK_LAYER_reshade")) {
                    if (snprintf(paths, sizeof paths, "%s%sVK_LAYER_reshade", layers ? layers : "", layers && *layers ? ";" : "") < sizeof paths)
                        setenv("VK_INSTANCE_LAYERS", paths, 1);
                }
                LOG("[vk] local ReShade layer enabled\n");
            }
        }
    }
#endif
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
#ifdef __ANDROID__
    SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight");
    SDL_SetHint(SDL_HINT_ANDROID_TRAP_BACK_BUTTON, "1");  // (Back opens the menu, not "close the app")
#endif
    settings_load();
    LOG("[vk] initializing SDL video and input\n");
    use_bundled_driver();
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) FATAL("SDL_Init: %s\n", SDL_GetError());
    LOG("[vk] SDL ready\n");
    int win_w = 1280, win_h = 720;
    if (getenv("NXRT_WINDOW")) sscanf(getenv("NXRT_WINDOW"), "%dx%d", &win_w, &win_h);
    SDL_WindowFlags wflags = SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
    if (getenv("NXRT_HIDDEN")) wflags |= SDL_WINDOW_HIDDEN;
#ifdef __ANDROID__
    wflags |= SDL_WINDOW_FULLSCREEN;  // (immersive: no status / navigation bars over the game)
#endif
    {   // scripted test runs (user input ignored: runtime/hle/input.c input_user_blocked) open without taking the
        // keyboard focus from whatever the player is doing
        const char *v = getenv("NXRT_NO_USER_INPUT");
        bool scripted = v ? atoi(v) != 0 : (getenv("NXRT_INPUT") || getenv("NXRT_DOORJUMP_TEST") || getenv("NXRT_PLACE") ||
                                            getenv("NXRT_HOST_INPUT") || getenv("NXRT_BIND_TEST"));
        if (scripted) SDL_SetHint(SDL_HINT_WINDOW_ACTIVATE_WHEN_SHOWN, "0");
    }
    window = SDL_CreateWindow(nx_game.info.title, win_w, win_h, wflags);
    if (!window) FATAL("SDL_CreateWindow: %s\n", SDL_GetError());
    LOG("[vk] SDL window created at %llums\n", (unsigned long long)SDL_GetTicks());
    SDL_AddEventWatch(lifecycle_watch, NULL);
    rt_window_icon(window);
    LOG("[vk] window icon ready at %llums\n", (unsigned long long)SDL_GetTicks());
    // Create Vulkan before requesting fullscreen; finish the transition before the game starts presenting.
    create_instance();
    extern void vk_reshade_init(void);
    vk_reshade_init();
    if (!SDL_Vulkan_CreateSurface(window, g_vk.inst, NULL, &g_vk.surface)) FATAL("SDL_Vulkan_CreateSurface: %s\n", SDL_GetError());
    pick_device();
    create_device();
    LOG("[vk] device ready\n");
    create_swapchain();
    LOG("[vk] swapchain ready\n");
    vk_exec_init();
    LOG("[vk] execution ready\n");
    vk_shaders_init();
    LOG("[vk] shaders ready\n");
    if (g_ui_settings.fullscreen && !getenv("NXRT_WINDOW") && !getenv("NXRT_HIDDEN")) {
        if (!SDL_SetWindowFullscreen(window, true) || !SDL_SyncWindow(window))
            LOG("[vk] fullscreen transition: %s\n", SDL_GetError());
    }
}

SDL_Window *ui_window(void) { return window; }

void backend_init(void) {
    while (!atomic_load(&ui_ready)) SDL_Delay(1);
    LOG("[vk] %s, swapchain %ux%u\n", g_vk.props.deviceName, g_vk.sc_extent.width, g_vk.sc_extent.height);
}

// ---- in-place reboot (runtime/main.c teardown; main thread, guest threads stopped)
void backend_reset(void) {
    if (!g_vk.dev) return;
    rq_drain();  // (the render thread: everything queued ran; it idles while the game is down)
    rec_drain();  // (the recorder: everything the render thread queued is recorded and submitted)
    vkDeviceWaitIdle(g_vk.dev);
    extern void vk_textures_reset(void);
    extern void vk_exec_reset(void);
    vk_textures_reset();
    vk_exec_reset();
    g_last_presented = NULL;
}
void backend_guest_memory_reset(void) {}  // (exec.c re-imports the heap on first use)
void backend_clear_window(void) {
    if (rq_stopping()) return;
    rq_drain();
    static const float black[4] = {0, 0, 0, 1};
    if (g_vk.dev && vk_frame_begin(black)) vk_frame_end(NULL);
}

int ui_run(void) {
    ui_window_init();
    atomic_store(&ui_ready, true);
    uint64_t last_dump = SDL_GetTicksNS();
    for (;;) {
        if (rt_reboot_requested()) { input_mouse_update(window, false); return rt_reboot_requested(); }
        if (getenv("NXRT_DUMP_THREADS")) {
            uint64_t now = SDL_GetTicksNS();
            if (now - last_dump >= 3000000000ull) {
                last_dump = now;
                extern void kernel_dump_threads(void);
                kernel_dump_threads();
            }
        }
        SDL_Event e;
        while (SDL_WaitEventTimeout(&e, 16)) {
            if (rt_reboot_requested()) break;
            if (e.type == SDL_EVENT_QUIT) {
                input_rumble_stop();
                input_led_stop();
                LOG("window closed\n");
                backend_quit();  // (before exit(): see there)
                exit(0);
            }
            extern bool input_user_blocked(const SDL_Event *e);
            if (input_user_blocked(&e)) continue;  // scripted test runs: the real keyboard / mouse / pads do nothing (as metal.m)
            bool consumed = menu_handle_event(&e);
            input_mouse_update(window, true);
            if (consumed) continue;
            input_handle_event(&e);
            if (NX_HOOK(input_event)) NX_HOOK(input_event)(&e);  // (the game's: e.g. mouse clicks as actions)
        }
        menu_apply_window_requests(window);
        input_rumble_poll();
        input_led_poll();
    }
}
