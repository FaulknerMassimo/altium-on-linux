/*
 * Vulkan layer (Linux side) that makes Wine show the frame just presented in a child
 * window, not the one before it.
 *
 * Wine renders Vulkan into a child window (Altium's PCB and schematic views) off-screen:
 * the swapchain targets a hidden X window, and right after the host vkQueuePresentKHR
 * returns, win32u copies that window into the top-level window (client_surface_present
 * -> X11DRV_client_surface_present). Mesa's X11 WSI only queues the image there: its own
 * thread waits for rendering to finish before it sends PresentPixmap, and in FIFO mode
 * the X server copies the image into the window only at its next tick. Wine's copy
 * usually runs first, so the screen shows the previous frame: a click selects a
 * component, but the highlight appears only with the next redraw.
 *
 * This layer sits between Wine and the driver. After vkQueuePresentKHR it waits with
 * vkWaitForPresentKHR until the X server has completed the present, so the frame is in
 * the window when Wine copies it. It uses the present IDs the application already
 * passes (DXVK does when the device has VK_KHR_present_id and VK_KHR_present_wait);
 * presents without one, and devices without present wait, are passed through
 * untouched. A wait gives up after 100 ms.
 *
 * Loaded as an implicit layer: scripts/install.sh points the bottle's
 * VK_ADD_IMPLICIT_LAYER_PATH at a folder holding this library and its manifest
 * (VkLayer_altium_present_wait.json). DISABLE_ALTIUM_PRESENT_WAIT=1 turns it off;
 * ALTIUM_PRESENT_WAIT_DEBUG=1 logs how long every wait took.
 *
 * Build: scripts/build.sh. No Vulkan headers needed: the few types used are declared
 * below, from vulkan_core.h and vk_layer.h.
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

#define EXPORT __attribute__((visibility("default")))

typedef int32_t VkResult;
typedef uint32_t VkBool32;
typedef int32_t VkStructureType;
typedef struct VkInstance_T *VkInstance;
typedef struct VkPhysicalDevice_T *VkPhysicalDevice;
typedef struct VkDevice_T *VkDevice;
typedef struct VkQueue_T *VkQueue;
typedef uint64_t VkSwapchainKHR;
typedef void (*PFN_vkVoidFunction)(void);
typedef PFN_vkVoidFunction (*PFN_vkGetInstanceProcAddr)(VkInstance, const char *);
typedef PFN_vkVoidFunction (*PFN_vkGetDeviceProcAddr)(VkDevice, const char *);

#define VK_SUCCESS 0
#define VK_TIMEOUT 2
#define VK_ERROR_INITIALIZATION_FAILED (-3)
#define VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO 47
#define VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO 48
#define VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_WAIT_FEATURES_KHR 1000248000
#define VK_STRUCTURE_TYPE_PRESENT_ID_KHR 1000294000
#define VK_LAYER_LINK_INFO 0

typedef struct VkBaseInStructure {
    VkStructureType sType;
    const struct VkBaseInStructure *pNext;
} VkBaseInStructure;

typedef struct {
    VkStructureType sType;
    const void *pNext;
    uint32_t flags;
    const void *pApplicationInfo;
    uint32_t enabledLayerCount;
    const char *const *ppEnabledLayerNames;
    uint32_t enabledExtensionCount;
    const char *const *ppEnabledExtensionNames;
} VkInstanceCreateInfo;

typedef struct {
    VkStructureType sType;
    const void *pNext;
    uint32_t flags;
    uint32_t queueCreateInfoCount;
    const void *pQueueCreateInfos;
    uint32_t enabledLayerCount;
    const char *const *ppEnabledLayerNames;
    uint32_t enabledExtensionCount;
    const char *const *ppEnabledExtensionNames;
    const void *pEnabledFeatures;
} VkDeviceCreateInfo;

typedef struct {
    VkStructureType sType;
    void *pNext;
    VkBool32 presentWait;
} VkPhysicalDevicePresentWaitFeaturesKHR;

typedef struct {
    VkStructureType sType;
    const void *pNext;
    uint32_t waitSemaphoreCount;
    const void *pWaitSemaphores;
    uint32_t swapchainCount;
    const VkSwapchainKHR *pSwapchains;
    const uint32_t *pImageIndices;
    VkResult *pResults;
} VkPresentInfoKHR;

typedef struct {
    VkStructureType sType;
    const void *pNext;
    uint32_t swapchainCount;
    const uint64_t *pPresentIds;
} VkPresentIdKHR;

/* The loader's link information (vk_layer.h). Only pLayerInfo is used; the union's other
 * members are function pointers, at most two. */
typedef struct VkLayerInstanceLink {
    struct VkLayerInstanceLink *pNext;
    PFN_vkGetInstanceProcAddr pfnNextGetInstanceProcAddr;
    PFN_vkVoidFunction pfnNextGetPhysicalDeviceProcAddr;
} VkLayerInstanceLink;

typedef struct {
    VkStructureType sType;
    const void *pNext;
    int32_t function;
    union { VkLayerInstanceLink *pLayerInfo; void *other[2]; } u;
} VkLayerInstanceCreateInfo;

typedef struct VkLayerDeviceLink {
    struct VkLayerDeviceLink *pNext;
    PFN_vkGetInstanceProcAddr pfnNextGetInstanceProcAddr;
    PFN_vkGetDeviceProcAddr pfnNextGetDeviceProcAddr;
} VkLayerDeviceLink;

typedef struct {
    VkStructureType sType;
    const void *pNext;
    int32_t function;
    union { VkLayerDeviceLink *pLayerInfo; void *other[2]; } u;
} VkLayerDeviceCreateInfo;

typedef struct {
    int32_t sType; /* LAYER_NEGOTIATE_INTERFACE_STRUCT = 1 */
    void *pNext;
    uint32_t loaderLayerInterfaceVersion;
    PFN_vkGetInstanceProcAddr pfnGetInstanceProcAddr;
    PFN_vkGetDeviceProcAddr pfnGetDeviceProcAddr;
    PFN_vkVoidFunction pfnGetPhysicalDeviceProcAddr;
} VkNegotiateLayerInterface;

typedef VkResult (*PFN_vkCreateInstance)(const VkInstanceCreateInfo *, const void *, VkInstance *);
typedef void (*PFN_vkDestroyInstance)(VkInstance, const void *);
typedef VkResult (*PFN_vkCreateDevice)(VkPhysicalDevice, const VkDeviceCreateInfo *, const void *, VkDevice *);
typedef void (*PFN_vkDestroyDevice)(VkDevice, const void *);
typedef VkResult (*PFN_vkQueuePresentKHR)(VkQueue, const VkPresentInfoKHR *);
typedef VkResult (*PFN_vkWaitForPresentKHR)(VkDevice, VkSwapchainKHR, uint64_t, uint64_t);

#define WAIT_TIMEOUT_NS 100000000ull /* 100 ms */

/* Every dispatchable handle starts with the loader's dispatch table pointer, shared by an
 * instance and its physical devices, and by a device and its queues. */
static void *dispatch_key(const void *handle) { return *(void *const *)handle; }

struct instance {
    void *key;
    VkInstance handle;
    PFN_vkGetInstanceProcAddr gipa;
    PFN_vkDestroyInstance destroy;
};

struct device {
    void *key;
    VkDevice handle;
    PFN_vkGetDeviceProcAddr gdpa;
    PFN_vkDestroyDevice destroy;
    PFN_vkQueuePresentKHR present;
    PFN_vkWaitForPresentKHR wait; /* NULL: present wait not enabled, pass presents through */
};

#define MAX_OBJECTS 64
static struct instance instances[MAX_OBJECTS];
static struct device devices[MAX_OBJECTS];
static volatile int table_lock;

static void lock(void) { while (__sync_lock_test_and_set(&table_lock, 1)) ; }
static void unlock(void) { __sync_lock_release(&table_lock); }

static struct instance *find_instance(void *key)
{
    for (int i = 0; i < MAX_OBJECTS; i++) if (instances[i].key == key) return &instances[i];
    return NULL;
}

static struct device *find_device(void *key)
{
    for (int i = 0; i < MAX_OBJECTS; i++) if (devices[i].key == key) return &devices[i];
    return NULL;
}

static int debug(void)
{
    static int cached = -1;
    if (cached < 0) { const char *e = getenv("ALTIUM_PRESENT_WAIT_DEBUG"); cached = e && *e == '1'; }
    return cached;
}

static void log_line(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void log_line(const char *fmt, ...)
{
    char buf[256];
    int n = snprintf(buf, sizeof(buf), "altium-present-wait: ");
    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);
    n += vsnprintf(buf + n, sizeof(buf) - n, fmt, ap);
    __builtin_va_end(ap);
    if (n > (int)sizeof(buf) - 2) n = sizeof(buf) - 2;
    buf[n++] = '\n';
    if (write(2, buf, n) < 0) return;
}

static const void *find_struct(const void *chain, VkStructureType type)
{
    for (const VkBaseInStructure *s = chain; s; s = s->pNext) if (s->sType == type) return s;
    return NULL;
}

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec;
}

/* ---- Instance ------------------------------------------------------------ */

static VkResult layer_CreateInstance(const VkInstanceCreateInfo *info, const void *alloc, VkInstance *out)
{
    VkLayerInstanceCreateInfo *link = (VkLayerInstanceCreateInfo *)info->pNext;
    while (link && !(link->sType == VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO && link->function == VK_LAYER_LINK_INFO))
        link = (VkLayerInstanceCreateInfo *)link->pNext;
    if (!link) return VK_ERROR_INITIALIZATION_FAILED;

    PFN_vkGetInstanceProcAddr gipa = link->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    PFN_vkCreateInstance create = (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance");
    if (!create) return VK_ERROR_INITIALIZATION_FAILED;
    link->u.pLayerInfo = link->u.pLayerInfo->pNext; /* the next layer reads its own link */

    VkResult res = create(info, alloc, out);
    if (res != VK_SUCCESS) return res;

    struct instance new_inst = {
        .key = dispatch_key(*out),
        .handle = *out,
        .gipa = gipa,
        .destroy = (PFN_vkDestroyInstance)gipa(*out, "vkDestroyInstance"),
    };
    lock();
    struct instance *inst = find_instance(NULL);
    if (inst) *inst = new_inst;
    unlock();
    if (!inst) log_line("too many instances, not tracking this one");
    return res;
}

static void layer_DestroyInstance(VkInstance handle, const void *alloc)
{
    PFN_vkDestroyInstance destroy = NULL;
    lock();
    struct instance *inst = find_instance(dispatch_key(handle));
    if (inst) { destroy = inst->destroy; inst->key = NULL; }
    unlock();
    if (destroy) destroy(handle, alloc);
}

/* ---- Device -------------------------------------------------------------- */

static int has_extension(const VkDeviceCreateInfo *info, const char *name)
{
    for (uint32_t i = 0; i < info->enabledExtensionCount; i++)
        if (!strcmp(info->ppEnabledExtensionNames[i], name)) return 1;
    return 0;
}

static VkResult layer_CreateDevice(VkPhysicalDevice physical, const VkDeviceCreateInfo *info, const void *alloc,
                                   VkDevice *out)
{
    VkLayerDeviceCreateInfo *link = (VkLayerDeviceCreateInfo *)info->pNext;
    while (link && !(link->sType == VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO && link->function == VK_LAYER_LINK_INFO))
        link = (VkLayerDeviceCreateInfo *)link->pNext;
    if (!link) return VK_ERROR_INITIALIZATION_FAILED;

    PFN_vkGetInstanceProcAddr gipa = link->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    PFN_vkGetDeviceProcAddr gdpa = link->u.pLayerInfo->pfnNextGetDeviceProcAddr;
    lock();
    struct instance *inst = find_instance(dispatch_key(physical));
    VkInstance instance = inst ? inst->handle : NULL;
    unlock();
    PFN_vkCreateDevice create = (PFN_vkCreateDevice)gipa(instance, "vkCreateDevice");
    if (!create) return VK_ERROR_INITIALIZATION_FAILED;
    link->u.pLayerInfo = link->u.pLayerInfo->pNext;

    VkResult res = create(physical, info, alloc, out);
    if (res != VK_SUCCESS) return res;

    /* vkWaitForPresentKHR is only valid with the presentWait feature enabled. */
    const VkPhysicalDevicePresentWaitFeaturesKHR *feature =
        find_struct(info->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_WAIT_FEATURES_KHR);
    int can_wait = has_extension(info, "VK_KHR_present_wait") && feature && feature->presentWait;

    struct device new_dev = {
        .key = dispatch_key(*out),
        .handle = *out,
        .gdpa = gdpa,
        .destroy = (PFN_vkDestroyDevice)gdpa(*out, "vkDestroyDevice"),
        .present = (PFN_vkQueuePresentKHR)gdpa(*out, "vkQueuePresentKHR"),
        .wait = can_wait ? (PFN_vkWaitForPresentKHR)gdpa(*out, "vkWaitForPresentKHR") : NULL,
    };
    lock();
    struct device *dev = find_device(NULL);
    if (dev) *dev = new_dev;
    unlock();
    if (!dev) log_line("too many devices, not tracking this one");
    else if (debug()) log_line("device %p: %s", (void *)*out, new_dev.wait ? "waiting for presents" : "no present wait, passing through");
    return res;
}

static void layer_DestroyDevice(VkDevice handle, const void *alloc)
{
    PFN_vkDestroyDevice destroy = NULL;
    lock();
    struct device *dev = find_device(dispatch_key(handle));
    if (dev) { destroy = dev->destroy; dev->key = NULL; }
    unlock();
    if (destroy) destroy(handle, alloc);
}

static VkResult layer_QueuePresentKHR(VkQueue queue, const VkPresentInfoKHR *info)
{
    lock();
    struct device *found = find_device(dispatch_key(queue));
    struct device dev = found ? *found : (struct device){0};
    unlock();
    if (!dev.present) return VK_ERROR_INITIALIZATION_FAILED; /* cannot happen: queues come from tracked devices */

    VkResult res = dev.present(queue, info);
    const VkPresentIdKHR *ids = find_struct(info->pNext, VK_STRUCTURE_TYPE_PRESENT_ID_KHR);
    if (!dev.wait || !ids || !ids->pPresentIds) return res;

    for (uint32_t i = 0; i < info->swapchainCount && i < ids->swapchainCount; i++) {
        VkResult swapchain_res = info->pResults ? info->pResults[i] : res;
        if (swapchain_res < 0 || !ids->pPresentIds[i]) continue; /* 0 = no ID for this swapchain */
        uint64_t start = debug() ? now_ns() : 0;
        VkResult wait_res = dev.wait(dev.handle, info->pSwapchains[i], ids->pPresentIds[i], WAIT_TIMEOUT_NS);
        if (debug())
            log_line("present %llu: waited %.2f ms, result %d", (unsigned long long)ids->pPresentIds[i],
                     (now_ns() - start) / 1e6, wait_res);
        else if (wait_res == VK_TIMEOUT)
            log_line("present %llu not completed after 100 ms", (unsigned long long)ids->pPresentIds[i]);
    }
    return res;
}

/* ---- Entry points --------------------------------------------------------- */

EXPORT PFN_vkVoidFunction vkGetDeviceProcAddr(VkDevice handle, const char *name);

EXPORT PFN_vkVoidFunction vkGetInstanceProcAddr(VkInstance handle, const char *name)
{
    if (!strcmp(name, "vkGetInstanceProcAddr")) return (PFN_vkVoidFunction)vkGetInstanceProcAddr;
    if (!strcmp(name, "vkCreateInstance")) return (PFN_vkVoidFunction)layer_CreateInstance;
    if (!strcmp(name, "vkDestroyInstance")) return (PFN_vkVoidFunction)layer_DestroyInstance;
    if (!strcmp(name, "vkCreateDevice")) return (PFN_vkVoidFunction)layer_CreateDevice;
    if (!strcmp(name, "vkGetDeviceProcAddr")) return (PFN_vkVoidFunction)vkGetDeviceProcAddr;
    if (!handle) return NULL;

    lock();
    struct instance *inst = find_instance(dispatch_key(handle));
    PFN_vkGetInstanceProcAddr gipa = inst ? inst->gipa : NULL;
    unlock();
    return gipa ? gipa(handle, name) : NULL;
}

EXPORT PFN_vkVoidFunction vkGetDeviceProcAddr(VkDevice handle, const char *name)
{
    if (!strcmp(name, "vkGetDeviceProcAddr")) return (PFN_vkVoidFunction)vkGetDeviceProcAddr;
    if (!handle) return NULL;

    lock();
    struct device *dev = find_device(dispatch_key(handle));
    struct device copy = dev ? *dev : (struct device){0};
    unlock();
    if (!copy.gdpa) return NULL;
    if (!strcmp(name, "vkDestroyDevice")) return (PFN_vkVoidFunction)layer_DestroyDevice;
    /* Without present wait there is nothing to do: hand out the next layer's function. */
    if (!strcmp(name, "vkQueuePresentKHR") && copy.wait) return (PFN_vkVoidFunction)layer_QueuePresentKHR;
    return copy.gdpa(handle, name);
}

EXPORT VkResult vkNegotiateLoaderLayerInterfaceVersion(VkNegotiateLayerInterface *iface)
{
    if (iface->loaderLayerInterfaceVersion > 2) iface->loaderLayerInterfaceVersion = 2;
    iface->pfnGetInstanceProcAddr = vkGetInstanceProcAddr;
    iface->pfnGetDeviceProcAddr = vkGetDeviceProcAddr;
    iface->pfnGetPhysicalDeviceProcAddr = NULL;
    return VK_SUCCESS;
}
