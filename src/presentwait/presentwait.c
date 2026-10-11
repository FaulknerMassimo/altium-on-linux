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
 * This layer sits between Wine and the driver. After vkQueuePresentKHR it waits until
 * the X server has copied the image into the window, so the frame is there when Wine
 * copies it. The copy shows up as a DAMAGE event on the window, which the layer watches
 * on its own X connection (one per swapchain). Present completion, which the X server
 * reports only at its next tick (17 ms under Xwayland for an off-screen window), also
 * ends the wait: it covers windows that get no damage, such as unmapped ones, and the
 * case where the layer cannot watch the window at all. The present IDs come from the
 * application (DXVK passes them when the device has VK_KHR_present_id and
 * VK_KHR_present_wait); presents without one, and devices without present wait, are
 * passed through untouched. A wait gives up after 100 ms.
 *
 * Loaded as an implicit layer: scripts/install.sh points the bottle's
 * VK_ADD_IMPLICIT_LAYER_PATH at a folder holding this library and its manifest
 * (VkLayer_altium_present_wait.json). DISABLE_ALTIUM_PRESENT_WAIT=1 turns it off;
 * ALTIUM_PRESENT_WAIT_DEBUG=1 logs how long every wait took and what ended it;
 * ALTIUM_PRESENT_WAIT_NO_DAMAGE=1 waits for completion only, as the first version did.
 *
 * Build: scripts/build.sh. No Vulkan or XCB headers needed: the few types used are
 * declared below, from vulkan_core.h, vk_layer.h, xcb.h and damage.h. libxcb and
 * libxcb-damage are opened at run time.
 */
#include <dlfcn.h>
#include <poll.h>
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
typedef uint64_t VkSurfaceKHR;
typedef uint64_t VkSwapchainKHR;
typedef void (*PFN_vkVoidFunction)(void);
typedef PFN_vkVoidFunction (*PFN_vkGetInstanceProcAddr)(VkInstance, const char *);
typedef PFN_vkVoidFunction (*PFN_vkGetDeviceProcAddr)(VkDevice, const char *);

#define VK_SUCCESS 0
#define VK_TIMEOUT 2
#define VK_ERROR_INITIALIZATION_FAILED (-3)
#define VK_ERROR_DEVICE_LOST (-4)
#define VK_SUBOPTIMAL_KHR 1000001003
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

typedef struct {
    VkStructureType sType;
    const void *pNext;
    uint32_t flags;
    void *dpy;
    unsigned long window;
} VkXlibSurfaceCreateInfoKHR;

typedef struct {
    VkStructureType sType;
    const void *pNext;
    uint32_t flags;
    void *connection;
    uint32_t window;
} VkXcbSurfaceCreateInfoKHR;

/* Only the start of VkSwapchainCreateInfoKHR is read. */
typedef struct {
    VkStructureType sType;
    const void *pNext;
    uint32_t flags;
    VkSurfaceKHR surface;
} VkSwapchainCreateInfoKHR;

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
typedef VkResult (*PFN_vkCreateXlibSurfaceKHR)(VkInstance, const VkXlibSurfaceCreateInfoKHR *, const void *, VkSurfaceKHR *);
typedef VkResult (*PFN_vkCreateXcbSurfaceKHR)(VkInstance, const VkXcbSurfaceCreateInfoKHR *, const void *, VkSurfaceKHR *);
typedef void (*PFN_vkDestroySurfaceKHR)(VkInstance, VkSurfaceKHR, const void *);
typedef VkResult (*PFN_vkCreateDevice)(VkPhysicalDevice, const VkDeviceCreateInfo *, const void *, VkDevice *);
typedef void (*PFN_vkDestroyDevice)(VkDevice, const void *);
typedef VkResult (*PFN_vkCreateSwapchainKHR)(VkDevice, const VkSwapchainCreateInfoKHR *, const void *, VkSwapchainKHR *);
typedef void (*PFN_vkDestroySwapchainKHR)(VkDevice, VkSwapchainKHR, const void *);
typedef VkResult (*PFN_vkQueuePresentKHR)(VkQueue, const VkPresentInfoKHR *);
typedef VkResult (*PFN_vkWaitForPresentKHR)(VkDevice, VkSwapchainKHR, uint64_t, uint64_t);

/* The XCB calls used (xcb.h, damage.h). Cookies are returned by value in a register. */
typedef struct xcb_connection_t xcb_connection_t;
typedef struct { const char *name; int global_id; } xcb_extension_t;
typedef struct { unsigned int sequence; } xcb_cookie_t;
typedef struct { uint8_t response_type; } xcb_generic_event_t; /* the rest is not read */
typedef struct {
    uint8_t response_type, pad0;
    uint16_t sequence;
    uint32_t length;
    uint8_t present, major_opcode, first_event, first_error;
} xcb_query_extension_reply_t;

#define XCB_DAMAGE_NOTIFY 0
#define XCB_DAMAGE_REPORT_LEVEL_RAW_RECTANGLES 0

static struct {
    int loaded; /* 0 not tried, 1 loaded, -1 unavailable */
    xcb_connection_t *(*connect)(const char *, int *);
    int (*connection_has_error)(xcb_connection_t *);
    void (*disconnect)(xcb_connection_t *);
    int (*get_file_descriptor)(xcb_connection_t *);
    int (*flush)(xcb_connection_t *);
    uint32_t (*generate_id)(xcb_connection_t *);
    const xcb_query_extension_reply_t *(*get_extension_data)(xcb_connection_t *, xcb_extension_t *);
    xcb_generic_event_t *(*poll_for_event)(xcb_connection_t *);
    xcb_cookie_t (*get_input_focus)(xcb_connection_t *);
    void *(*get_input_focus_reply)(xcb_connection_t *, xcb_cookie_t, void **);
    xcb_extension_t *damage_id;
    xcb_cookie_t (*damage_query_version)(xcb_connection_t *, uint32_t, uint32_t);
    void *(*damage_query_version_reply)(xcb_connection_t *, xcb_cookie_t, void **);
    xcb_cookie_t (*damage_create)(xcb_connection_t *, uint32_t, uint32_t, uint8_t);
} xcb;

#define WAIT_TIMEOUT_NS 100000000ull /* 100 ms */
#define MAX_PRESENT_SWAPCHAINS 8

/* Every dispatchable handle starts with the loader's dispatch table pointer, shared by an
 * instance and its physical devices, and by a device and its queues. */
static void *dispatch_key(const void *handle) { return *(void *const *)handle; }

struct instance {
    void *key;
    VkInstance handle;
    PFN_vkGetInstanceProcAddr gipa;
    PFN_vkDestroyInstance destroy;
    PFN_vkCreateXlibSurfaceKHR create_xlib_surface;
    PFN_vkCreateXcbSurfaceKHR create_xcb_surface;
    PFN_vkDestroySurfaceKHR destroy_surface;
};

struct device {
    void *key;
    VkDevice handle;
    PFN_vkGetDeviceProcAddr gdpa;
    PFN_vkDestroyDevice destroy;
    PFN_vkCreateSwapchainKHR create_swapchain;
    PFN_vkDestroySwapchainKHR destroy_swapchain;
    PFN_vkQueuePresentKHR present;
    PFN_vkWaitForPresentKHR wait; /* NULL: present wait not enabled, pass presents through */
};

/* The X window behind a surface, for watching it once a swapchain targets it. */
struct surface {
    void *key; /* instance key; NULL = free slot */
    VkSurfaceKHR handle;
    uint32_t window;
};

/* A swapchain's own X connection, on which the server reports damage to its window. */
struct swapchain {
    void *key; /* device key; NULL = free slot */
    VkSwapchainKHR handle;
    xcb_connection_t *conn;
    uint8_t damage_event;
};

#define MAX_OBJECTS 64
#define MAX_SURFACES 256
static struct instance instances[MAX_OBJECTS];
static struct device devices[MAX_OBJECTS];
static struct surface surfaces[MAX_SURFACES];
static struct swapchain swapchains[MAX_SURFACES];
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

static struct surface *find_surface(void *key, VkSurfaceKHR handle)
{
    for (int i = 0; i < MAX_SURFACES; i++)
        if (surfaces[i].key == key && (!key || surfaces[i].handle == handle)) return &surfaces[i];
    return NULL;
}

static struct swapchain *find_swapchain(void *key, VkSwapchainKHR handle)
{
    for (int i = 0; i < MAX_SURFACES; i++)
        if (swapchains[i].key == key && (!key || swapchains[i].handle == handle)) return &swapchains[i];
    return NULL;
}

static int env_flag(const char *name)
{
    const char *e = getenv(name);
    return e && *e == '1';
}

static int debug(void)
{
    static int cached = -1;
    if (cached < 0) cached = env_flag("ALTIUM_PRESENT_WAIT_DEBUG");
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

/* ---- Damage watch ---------------------------------------------------------- */

/* Open libxcb and libxcb-damage (both in the Flatpak runtime). */
static int open_xcb(void)
{
    if (env_flag("ALTIUM_PRESENT_WAIT_NO_DAMAGE")) return 0;
    void *lib = dlopen("libxcb.so.1", RTLD_NOW | RTLD_LOCAL);
    void *damage = dlopen("libxcb-damage.so.0", RTLD_NOW | RTLD_LOCAL);
    if (!lib || !damage) {
        log_line("libxcb or libxcb-damage not found, waiting for present completion instead");
        return 0;
    }
#define LOAD(lib, field, name) if (!(*(void **)&xcb.field = dlsym(lib, name))) goto missing
    LOAD(lib, connect, "xcb_connect");
    LOAD(lib, connection_has_error, "xcb_connection_has_error");
    LOAD(lib, disconnect, "xcb_disconnect");
    LOAD(lib, get_file_descriptor, "xcb_get_file_descriptor");
    LOAD(lib, flush, "xcb_flush");
    LOAD(lib, generate_id, "xcb_generate_id");
    LOAD(lib, get_extension_data, "xcb_get_extension_data");
    LOAD(lib, poll_for_event, "xcb_poll_for_event");
    LOAD(lib, get_input_focus, "xcb_get_input_focus");
    LOAD(lib, get_input_focus_reply, "xcb_get_input_focus_reply");
    LOAD(damage, damage_id, "xcb_damage_id");
    LOAD(damage, damage_query_version, "xcb_damage_query_version");
    LOAD(damage, damage_query_version_reply, "xcb_damage_query_version_reply");
    LOAD(damage, damage_create, "xcb_damage_create");
#undef LOAD
    return 1;
missing:
    log_line("libxcb symbol missing, waiting for present completion instead");
    return 0;
}

/* Concurrent first calls both open the libraries and store the same pointers. */
static int load_xcb(void)
{
    if (!xcb.loaded) xcb.loaded = open_xcb() ? 1 : -1;
    return xcb.loaded > 0;
}

/* Connect to the X server (the one Wine uses: $DISPLAY) and ask for an event on every
 * change to the window's contents. */
static xcb_connection_t *watch_window(uint32_t window, uint8_t *damage_event)
{
    if (!window || !load_xcb()) return NULL;
    xcb_connection_t *conn = xcb.connect(NULL, NULL);
    if (xcb.connection_has_error(conn)) {
        log_line("cannot connect to the X server, waiting for present completion instead");
        xcb.disconnect(conn);
        return NULL;
    }
    const xcb_query_extension_reply_t *ext = xcb.get_extension_data(conn, xcb.damage_id);
    /* The server accepts no other DAMAGE request before QueryVersion. */
    void *version = ext && ext->present
        ? xcb.damage_query_version_reply(conn, xcb.damage_query_version(conn, 1, 1), NULL) : NULL;
    if (!version) {
        log_line("no DAMAGE extension, waiting for present completion instead");
        xcb.disconnect(conn);
        return NULL;
    }
    free(version);
    *damage_event = ext->first_event + XCB_DAMAGE_NOTIFY;
    /* Destroyed with the connection. */
    xcb.damage_create(conn, xcb.generate_id(conn), window, XCB_DAMAGE_REPORT_LEVEL_RAW_RECTANGLES);
    xcb.flush(conn);
    return conn;
}

/* Drop the damage events the window has had so far: a round trip makes the server send
 * them first. Done before a present, so only the copy of that present ends its wait. */
static void drain_damage(xcb_connection_t *conn)
{
    free(xcb.get_input_focus_reply(conn, xcb.get_input_focus(conn), NULL));
    xcb_generic_event_t *event;
    while ((event = xcb.poll_for_event(conn))) free(event);
}

enum wait_end { END_DAMAGE, END_COMPLETE, END_TIMEOUT, END_ERROR };
static const char *const wait_end_names[] = { "damage", "complete", "timeout", "error" };

static enum wait_end wait_end_of(VkResult res)
{
    return res == VK_SUCCESS || res == VK_SUBOPTIMAL_KHR ? END_COMPLETE : res == VK_TIMEOUT ? END_TIMEOUT : END_ERROR;
}

/* Wait for damage to the window (the X server has copied the image into it) or for the
 * present to complete, whichever comes first. Completion is only polled, every 1 ms;
 * damage wakes the wait at once. *res: the last vkWaitForPresentKHR result. */
static enum wait_end wait_for_copy(const struct device *dev, const struct swapchain *sc, uint64_t id, VkResult *res)
{
    uint64_t deadline = now_ns() + WAIT_TIMEOUT_NS;
    struct pollfd fd = { .fd = xcb.get_file_descriptor(sc->conn), .events = POLLIN };
    *res = VK_TIMEOUT;
    for (;;) {
        xcb_generic_event_t *event;
        while ((event = xcb.poll_for_event(sc->conn))) {
            int damage = (event->response_type & 0x7f) == sc->damage_event;
            free(event); /* errors too: a destroyed window leaves completion to end the wait */
            if (damage) return END_DAMAGE;
        }
        if (xcb.connection_has_error(sc->conn)) {
            uint64_t now = now_ns();
            *res = dev->wait(dev->handle, sc->handle, id, now < deadline ? deadline - now : 0);
            return wait_end_of(*res);
        }
        /* Mesa's X11 vkWaitForPresentKHR reports a timeout as VK_ERROR_DEVICE_LOST once the
         * image has rendered (x11_wait_for_present compares the thrd_timedout from
         * u_cnd_monotonic_timedwait with ETIMEDOUT): the present is just not complete. */
        *res = dev->wait(dev->handle, sc->handle, id, 0);
        if (*res != VK_TIMEOUT && *res != VK_ERROR_DEVICE_LOST) return wait_end_of(*res);
        if (now_ns() >= deadline) return END_TIMEOUT;
        poll(&fd, 1, 1);
    }
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
        .create_xlib_surface = (PFN_vkCreateXlibSurfaceKHR)gipa(*out, "vkCreateXlibSurfaceKHR"),
        .create_xcb_surface = (PFN_vkCreateXcbSurfaceKHR)gipa(*out, "vkCreateXcbSurfaceKHR"),
        .destroy_surface = (PFN_vkDestroySurfaceKHR)gipa(*out, "vkDestroySurfaceKHR"),
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
    void *key = dispatch_key(handle);
    lock();
    struct instance *inst = find_instance(key);
    if (inst) { destroy = inst->destroy; inst->key = NULL; }
    for (int i = 0; i < MAX_SURFACES; i++) if (surfaces[i].key == key) surfaces[i].key = NULL;
    unlock();
    if (destroy) destroy(handle, alloc);
}

static void add_surface(VkInstance instance, VkSurfaceKHR handle, uint32_t window)
{
    lock();
    struct surface *surface = find_surface(NULL, 0);
    if (surface) *surface = (struct surface){ .key = dispatch_key(instance), .handle = handle, .window = window };
    unlock();
    if (!surface) log_line("too many surfaces, not tracking this one");
}

static VkResult layer_CreateXlibSurfaceKHR(VkInstance handle, const VkXlibSurfaceCreateInfoKHR *info, const void *alloc,
                                           VkSurfaceKHR *out)
{
    lock();
    struct instance *inst = find_instance(dispatch_key(handle));
    PFN_vkCreateXlibSurfaceKHR create = inst ? inst->create_xlib_surface : NULL;
    unlock();
    if (!create) return VK_ERROR_INITIALIZATION_FAILED;
    VkResult res = create(handle, info, alloc, out);
    if (res == VK_SUCCESS) add_surface(handle, *out, (uint32_t)info->window);
    return res;
}

static VkResult layer_CreateXcbSurfaceKHR(VkInstance handle, const VkXcbSurfaceCreateInfoKHR *info, const void *alloc,
                                          VkSurfaceKHR *out)
{
    lock();
    struct instance *inst = find_instance(dispatch_key(handle));
    PFN_vkCreateXcbSurfaceKHR create = inst ? inst->create_xcb_surface : NULL;
    unlock();
    if (!create) return VK_ERROR_INITIALIZATION_FAILED;
    VkResult res = create(handle, info, alloc, out);
    if (res == VK_SUCCESS) add_surface(handle, *out, info->window);
    return res;
}

static void layer_DestroySurfaceKHR(VkInstance handle, VkSurfaceKHR surface_handle, const void *alloc)
{
    lock();
    struct instance *inst = find_instance(dispatch_key(handle));
    PFN_vkDestroySurfaceKHR destroy = inst ? inst->destroy_surface : NULL;
    struct surface *surface = surface_handle ? find_surface(dispatch_key(handle), surface_handle) : NULL;
    if (surface) surface->key = NULL;
    unlock();
    if (destroy) destroy(handle, surface_handle, alloc);
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
        .create_swapchain = (PFN_vkCreateSwapchainKHR)gdpa(*out, "vkCreateSwapchainKHR"),
        .destroy_swapchain = (PFN_vkDestroySwapchainKHR)gdpa(*out, "vkDestroySwapchainKHR"),
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
    void *key = dispatch_key(handle);
    xcb_connection_t *conns[MAX_SURFACES];
    int count = 0;
    lock();
    struct device *dev = find_device(key);
    if (dev) { destroy = dev->destroy; dev->key = NULL; }
    for (int i = 0; i < MAX_SURFACES; i++)
        if (swapchains[i].key == key) { conns[count++] = swapchains[i].conn; swapchains[i].key = NULL; }
    unlock();
    for (int i = 0; i < count; i++) if (conns[i]) xcb.disconnect(conns[i]);
    if (destroy) destroy(handle, alloc);
}

static VkResult layer_CreateSwapchainKHR(VkDevice handle, const VkSwapchainCreateInfoKHR *info, const void *alloc,
                                         VkSwapchainKHR *out)
{
    lock();
    struct device *dev = find_device(dispatch_key(handle));
    PFN_vkCreateSwapchainKHR create = dev ? dev->create_swapchain : NULL;
    unlock();
    if (!create) return VK_ERROR_INITIALIZATION_FAILED;
    VkResult res = create(handle, info, alloc, out);
    if (res < 0) return res;

    /* Surfaces belong to the instance; their handles are unique within it, and there is
     * one instance per process in practice, so any instance's entry will do. */
    uint32_t window = 0;
    lock();
    for (int i = 0; i < MAX_SURFACES; i++)
        if (surfaces[i].key && surfaces[i].handle == info->surface) { window = surfaces[i].window; break; }
    unlock();

    struct swapchain new_sc = { .key = dispatch_key(handle), .handle = *out };
    new_sc.conn = watch_window(window, &new_sc.damage_event);
    lock();
    struct swapchain *sc = find_swapchain(NULL, 0);
    if (sc) *sc = new_sc;
    unlock();
    if (!sc) {
        log_line("too many swapchains, waiting for present completion on this one");
        if (new_sc.conn) xcb.disconnect(new_sc.conn);
    } else if (debug()) {
        log_line("swapchain 0x%llx: window 0x%x, %s", (unsigned long long)*out, window,
                 new_sc.conn ? "watching for damage" : "waiting for completion only");
    }
    return res;
}

static void layer_DestroySwapchainKHR(VkDevice handle, VkSwapchainKHR swapchain, const void *alloc)
{
    xcb_connection_t *conn = NULL;
    lock();
    struct device *dev = find_device(dispatch_key(handle));
    PFN_vkDestroySwapchainKHR destroy = dev ? dev->destroy_swapchain : NULL;
    struct swapchain *sc = swapchain ? find_swapchain(dispatch_key(handle), swapchain) : NULL;
    if (sc) { conn = sc->conn; sc->key = NULL; }
    unlock();
    if (conn) xcb.disconnect(conn);
    if (destroy) destroy(handle, swapchain, alloc);
}

static VkResult layer_QueuePresentKHR(VkQueue queue, const VkPresentInfoKHR *info)
{
    lock();
    struct device *found = find_device(dispatch_key(queue));
    struct device dev = found ? *found : (struct device){0};
    unlock();
    if (!dev.present) return VK_ERROR_INITIALIZATION_FAILED; /* cannot happen: queues come from tracked devices */

    const VkPresentIdKHR *ids = find_struct(info->pNext, VK_STRUCTURE_TYPE_PRESENT_ID_KHR);
    if (!dev.wait || !ids || !ids->pPresentIds) return dev.present(queue, info);

    /* The swapchains are externally synchronized during a present: nothing else uses or
     * destroys their connections until it returns. */
    /* Beyond the first few swapchains of a present, wait for completion only. */
    struct swapchain scs[MAX_PRESENT_SWAPCHAINS] = {0};
    uint32_t count = info->swapchainCount < ids->swapchainCount ? info->swapchainCount : ids->swapchainCount;
    uint32_t watched = count < MAX_PRESENT_SWAPCHAINS ? count : MAX_PRESENT_SWAPCHAINS;
    lock();
    for (uint32_t i = 0; i < watched; i++) {
        struct swapchain *sc = find_swapchain(dev.key, info->pSwapchains[i]);
        if (sc) scs[i] = *sc;
    }
    unlock();
    for (uint32_t i = 0; i < watched; i++) if (scs[i].conn && ids->pPresentIds[i]) drain_damage(scs[i].conn);

    VkResult res = dev.present(queue, info);

    for (uint32_t i = 0; i < count; i++) {
        VkResult swapchain_res = info->pResults ? info->pResults[i] : res;
        if (swapchain_res < 0 || !ids->pPresentIds[i]) continue; /* 0 = no ID for this swapchain */
        uint64_t start = now_ns();
        enum wait_end end;
        VkResult wait_res;
        if (i < watched && scs[i].conn) {
            end = wait_for_copy(&dev, &scs[i], ids->pPresentIds[i], &wait_res);
        } else {
            wait_res = dev.wait(dev.handle, info->pSwapchains[i], ids->pPresentIds[i], WAIT_TIMEOUT_NS);
            end = wait_end_of(wait_res);
        }
        if (debug()) {
            uint64_t now = now_ns();
            log_line("present %llu at %.6f s: waited %.2f ms, ended by %s (swapchain 0x%llx, present %d, wait %d)",
                     (unsigned long long)ids->pPresentIds[i], now / 1e9, (now - start) / 1e6, wait_end_names[end],
                     (unsigned long long)info->pSwapchains[i], swapchain_res, wait_res);
        }
        else if (end == END_TIMEOUT)
            log_line("present %llu not copied after 100 ms", (unsigned long long)ids->pPresentIds[i]);
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
    struct instance copy = inst ? *inst : (struct instance){0};
    unlock();
    if (!copy.gipa) return NULL;
    /* Only the extensions the instance has enabled, which the next layer hands out. */
    if (!strcmp(name, "vkCreateXlibSurfaceKHR") && copy.create_xlib_surface)
        return (PFN_vkVoidFunction)layer_CreateXlibSurfaceKHR;
    if (!strcmp(name, "vkCreateXcbSurfaceKHR") && copy.create_xcb_surface)
        return (PFN_vkVoidFunction)layer_CreateXcbSurfaceKHR;
    if (!strcmp(name, "vkDestroySurfaceKHR") && copy.destroy_surface)
        return (PFN_vkVoidFunction)layer_DestroySurfaceKHR;
    return copy.gipa(handle, name);
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
    if (copy.wait) {
        if (!strcmp(name, "vkQueuePresentKHR")) return (PFN_vkVoidFunction)layer_QueuePresentKHR;
        if (!strcmp(name, "vkCreateSwapchainKHR") && copy.create_swapchain)
            return (PFN_vkVoidFunction)layer_CreateSwapchainKHR;
        if (!strcmp(name, "vkDestroySwapchainKHR") && copy.destroy_swapchain)
            return (PFN_vkVoidFunction)layer_DestroySwapchainKHR;
    }
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
