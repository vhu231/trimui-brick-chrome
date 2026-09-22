/*
 * VK_LAYER_BRICK_x11_wsi -- VK_KHR_xcb_surface for a Vulkan driver that has none.
 *
 * The PowerVR driver in the stock firmware only has headless and direct-display
 * WSI, so nothing can present to an X11 window.  This layer implements the X11
 * surface and swapchain itself:
 *
 *   - swapchain images are ordinary device-local images the application renders
 *     into;
 *   - vkQueuePresentKHR records a copy of the damaged rectangles (from
 *     VK_KHR_incremental_present, which the layer also provides) into a
 *     host-cached buffer on the application's queue and returns at once;
 *   - a worker thread per swapchain waits for that copy, moves the pixels into
 *     a MIT-SHM segment (forcing alpha to opaque: the sunxi display engine
 *     treats the framebuffer's fourth byte as alpha) and puts them into the
 *     window with xcb_shm_put_image.
 *
 * So the GPU renders the next frame while the CPU delivers the last one; nothing
 * waits for a readback on the rendering thread, and no pixels go through the X
 * socket.
 *
 * Acquire has to signal the application's semaphore without touching its
 * queues, so the layer hides the driver's second queue and keeps it for itself.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>
#include <vulkan/vulkan.h>
#include <vulkan/vk_layer.h>

#define EXPORT __attribute__((visibility("default")))
#define LAYER_NAME "VK_LAYER_BRICK_x11_wsi"
#define MAX_IMAGES 6
#define MAX_RECTS 16

static int g_debug;
#define LOG(...) fprintf(stderr, "brick-wsi: " __VA_ARGS__)
#define DBG(...) do { if (g_debug) LOG(__VA_ARGS__); } while (0)

/* ------------------------------------------------------------------ xcb -- */
/* Declared here and loaded with dlopen, so the layer builds without X headers. */

typedef struct xcb_connection_t xcb_connection_t;
typedef uint32_t xcb_window_t;
typedef struct { unsigned int sequence; } xcb_cookie_t;
typedef struct {
    uint8_t response_type, error_code;
    uint16_t sequence;
    uint32_t resource_id;
    uint16_t minor_code;
    uint8_t major_code, pad0;
    uint32_t pad[5];
    uint32_t full_sequence;
} xcb_generic_error_t;
typedef struct {
    uint8_t response_type, pad0;
    uint16_t sequence;
    uint32_t pad[7];
    uint32_t full_sequence;
} xcb_generic_event_t;
typedef struct {
    uint8_t response_type, depth;
    uint16_t sequence;
    uint32_t length;
    xcb_window_t root;
    int16_t x, y;
    uint16_t width, height, border_width;
    uint8_t pad0[2];
} xcb_get_geometry_reply_t;
typedef struct {
    uint8_t response_type, shared_pixmaps;
    uint16_t sequence;
    uint32_t length;
    uint16_t major_version, minor_version, uid, gid;
    uint8_t pixmap_format, pad0[15];
} xcb_shm_query_version_reply_t;
typedef struct {
    uint8_t response_type, pad0;
    uint16_t sequence;
    xcb_window_t event, window, above_sibling;
    int16_t x, y;
    uint16_t width, height, border_width;
    uint8_t override_redirect, pad1;
} xcb_configure_notify_event_t;

#define XCB_CONFIGURE_NOTIFY 22
#define XCB_CW_EVENT_MASK 2048
#define XCB_EVENT_MASK_STRUCTURE_NOTIFY 131072
#define XCB_IMAGE_FORMAT_Z_PIXMAP 2

static struct {
    xcb_connection_t *(*connect)(const char *, int *);
    int (*connection_has_error)(xcb_connection_t *);
    void (*disconnect)(xcb_connection_t *);
    int (*flush)(xcb_connection_t *);
    uint32_t (*generate_id)(xcb_connection_t *);
    xcb_cookie_t (*get_geometry)(xcb_connection_t *, uint32_t);
    xcb_get_geometry_reply_t *(*get_geometry_reply)(xcb_connection_t *, xcb_cookie_t, xcb_generic_error_t **);
    xcb_cookie_t (*create_gc)(xcb_connection_t *, uint32_t, uint32_t, uint32_t, const void *);
    xcb_cookie_t (*free_gc)(xcb_connection_t *, uint32_t);
    xcb_cookie_t (*change_window_attributes)(xcb_connection_t *, xcb_window_t, uint32_t, const void *);
    xcb_generic_event_t *(*poll_for_event)(xcb_connection_t *);
    xcb_cookie_t (*get_input_focus)(xcb_connection_t *);
    void *(*get_input_focus_reply)(xcb_connection_t *, xcb_cookie_t, xcb_generic_error_t **);
    xcb_generic_error_t *(*request_check)(xcb_connection_t *, xcb_cookie_t);
    xcb_cookie_t (*shm_query_version)(xcb_connection_t *);
    xcb_shm_query_version_reply_t *(*shm_query_version_reply)(xcb_connection_t *, xcb_cookie_t, xcb_generic_error_t **);
    xcb_cookie_t (*shm_attach_fd_checked)(xcb_connection_t *, uint32_t, int32_t, uint8_t);
    xcb_cookie_t (*shm_detach)(xcb_connection_t *, uint32_t);
    xcb_cookie_t (*shm_put_image)(xcb_connection_t *, uint32_t, uint32_t, uint16_t, uint16_t, uint16_t,
                                  uint16_t, uint16_t, uint16_t, int16_t, int16_t, uint8_t, uint8_t,
                                  uint8_t, uint32_t, uint32_t);
} X;

static int g_xcb_ok;

static void load_xcb_once(void)
{
    {
        void *xcb = dlopen("libxcb.so.1", RTLD_NOW | RTLD_LOCAL);
        void *shm = dlopen("libxcb-shm.so.0", RTLD_NOW | RTLD_LOCAL);
        if (!xcb || !shm) {
            LOG("cannot load libxcb / libxcb-shm\n");
            return;
        }
#define SYM(lib, field, name) if (!(*(void **)&X.field = dlsym(lib, name))) { LOG("missing %s\n", name); return; }
        SYM(xcb, connect, "xcb_connect");
        SYM(xcb, connection_has_error, "xcb_connection_has_error");
        SYM(xcb, disconnect, "xcb_disconnect");
        SYM(xcb, flush, "xcb_flush");
        SYM(xcb, generate_id, "xcb_generate_id");
        SYM(xcb, get_geometry, "xcb_get_geometry");
        SYM(xcb, get_geometry_reply, "xcb_get_geometry_reply");
        SYM(xcb, create_gc, "xcb_create_gc");
        SYM(xcb, free_gc, "xcb_free_gc");
        SYM(xcb, change_window_attributes, "xcb_change_window_attributes");
        SYM(xcb, poll_for_event, "xcb_poll_for_event");
        SYM(xcb, get_input_focus, "xcb_get_input_focus");
        SYM(xcb, get_input_focus_reply, "xcb_get_input_focus_reply");
        SYM(xcb, request_check, "xcb_request_check");
        SYM(shm, shm_query_version, "xcb_shm_query_version");
        SYM(shm, shm_query_version_reply, "xcb_shm_query_version_reply");
        SYM(shm, shm_attach_fd_checked, "xcb_shm_attach_fd_checked");
        SYM(shm, shm_detach, "xcb_shm_detach");
        SYM(shm, shm_put_image, "xcb_shm_put_image");
#undef SYM
        g_xcb_ok = 1;
    }
}

static int load_xcb(void)
{
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    pthread_once(&once, load_xcb_once);
    return g_xcb_ok;
}

/* One connection for the whole process, opened while the instance is created:
 * Chrome's GPU process is sandboxed by the time it makes a swapchain.  xcb is
 * thread-safe, and every request we make is self-contained. */
static xcb_connection_t *g_conn;
static pthread_mutex_t g_conn_lock = PTHREAD_MUTEX_INITIALIZER;

static xcb_connection_t *x_conn(void)
{
    pthread_mutex_lock(&g_conn_lock);
    if (!g_conn && load_xcb()) {
        xcb_connection_t *c = X.connect(NULL, NULL);
        if (c && !X.connection_has_error(c))
            g_conn = c;
        else {
            LOG("cannot connect to the X server\n");
            if (c)
                X.disconnect(c);
        }
    }
    pthread_mutex_unlock(&g_conn_lock);
    return g_conn;
}

static int x_geometry(xcb_window_t w, uint32_t *width, uint32_t *height, uint8_t *depth)
{
    xcb_connection_t *c = x_conn();
    if (!c)
        return 0;
    xcb_get_geometry_reply_t *g = X.get_geometry_reply(c, X.get_geometry(c, w), NULL);
    if (!g)
        return 0;
    *width = g->width;
    *height = g->height;
    if (depth)
        *depth = g->depth;
    free(g);
    return 1;
}

/* ---------------------------------------------------------- dispatch maps -- */

static void *dkey(const void *dispatchable) { return *(void *const *)dispatchable; }

struct inst {
    VkInstance instance;
    PFN_vkGetInstanceProcAddr gipa;
    PFN_vkDestroyInstance DestroyInstance;
    PFN_vkEnumerateDeviceExtensionProperties EnumerateDeviceExtensionProperties;
    PFN_vkGetPhysicalDeviceMemoryProperties GetPhysicalDeviceMemoryProperties;
    PFN_vkGetPhysicalDeviceQueueFamilyProperties GetPhysicalDeviceQueueFamilyProperties;
    PFN_vkGetPhysicalDeviceQueueFamilyProperties2 GetPhysicalDeviceQueueFamilyProperties2;
    PFN_vkDestroySurfaceKHR DestroySurfaceKHR;
    PFN_vkGetPhysicalDeviceSurfaceSupportKHR GetPhysicalDeviceSurfaceSupportKHR;
    PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR GetPhysicalDeviceSurfaceCapabilitiesKHR;
    PFN_vkGetPhysicalDeviceSurfaceCapabilities2KHR GetPhysicalDeviceSurfaceCapabilities2KHR;
    PFN_vkGetPhysicalDeviceSurfaceFormatsKHR GetPhysicalDeviceSurfaceFormatsKHR;
    PFN_vkGetPhysicalDeviceSurfaceFormats2KHR GetPhysicalDeviceSurfaceFormats2KHR;
    PFN_vkGetPhysicalDeviceSurfacePresentModesKHR GetPhysicalDeviceSurfacePresentModesKHR;
    PFN_vkGetPhysicalDevicePresentRectanglesKHR GetPhysicalDevicePresentRectanglesKHR;
};

struct dev {
    VkDevice device;
    VkPhysicalDevice phys;
    struct inst *inst;
    PFN_vkGetDeviceProcAddr gdpa;
    PFN_vkSetDeviceLoaderData set_loader_data;
    VkQueue queue; /* ours: hidden from the application */
    pthread_mutex_t queue_lock;
    uint32_t family;
    VkPhysicalDeviceMemoryProperties mem;
#define DEVFN(name) PFN_vk##name name
    DEVFN(DestroyDevice); DEVFN(GetDeviceQueue); DEVFN(QueueSubmit); DEVFN(QueueWaitIdle);
    DEVFN(CreateImage); DEVFN(DestroyImage); DEVFN(GetImageMemoryRequirements); DEVFN(BindImageMemory);
    DEVFN(CreateBuffer); DEVFN(DestroyBuffer); DEVFN(GetBufferMemoryRequirements); DEVFN(BindBufferMemory);
    DEVFN(AllocateMemory); DEVFN(FreeMemory); DEVFN(MapMemory); DEVFN(UnmapMemory);
    DEVFN(InvalidateMappedMemoryRanges);
    DEVFN(CreateCommandPool); DEVFN(DestroyCommandPool); DEVFN(AllocateCommandBuffers);
    DEVFN(ResetCommandBuffer); DEVFN(BeginCommandBuffer); DEVFN(EndCommandBuffer);
    DEVFN(CmdPipelineBarrier); DEVFN(CmdCopyImageToBuffer);
    DEVFN(CreateFence); DEVFN(DestroyFence); DEVFN(WaitForFences); DEVFN(ResetFences);
    DEVFN(CreateSwapchainKHR); DEVFN(DestroySwapchainKHR); DEVFN(GetSwapchainImagesKHR);
    DEVFN(AcquireNextImageKHR); DEVFN(AcquireNextImage2KHR); DEVFN(QueuePresentKHR);
    DEVFN(GetSwapchainStatusKHR);
#undef DEVFN
};

#define MAP_SIZE 32
struct map_entry { void *key, *val; };
static struct map_entry g_inst_map[MAP_SIZE], g_dev_map[MAP_SIZE];
static pthread_mutex_t g_map_lock = PTHREAD_MUTEX_INITIALIZER;

static void map_put(struct map_entry *m, void *key, void *val)
{
    pthread_mutex_lock(&g_map_lock);
    for (int i = 0; i < MAP_SIZE; i++)
        if (!m[i].key || m[i].key == key) {
            m[i].key = key;
            m[i].val = val;
            break;
        }
    pthread_mutex_unlock(&g_map_lock);
}

static void *map_get(struct map_entry *m, void *key)
{
    void *val = NULL;
    pthread_mutex_lock(&g_map_lock);
    for (int i = 0; i < MAP_SIZE; i++)
        if (m[i].key == key) {
            val = m[i].val;
            break;
        }
    pthread_mutex_unlock(&g_map_lock);
    return val;
}

static void map_del(struct map_entry *m, void *key)
{
    pthread_mutex_lock(&g_map_lock);
    for (int i = 0; i < MAP_SIZE; i++)
        if (m[i].key == key)
            m[i].key = m[i].val = NULL;
    pthread_mutex_unlock(&g_map_lock);
}

#define INST(h) ((struct inst *)map_get(g_inst_map, dkey(h)))
#define DEV(h) ((struct dev *)map_get(g_dev_map, dkey(h)))

/* Surfaces and swapchains are non-dispatchable: ours are pointers, tracked so
 * anything the driver created itself is passed through untouched. */
struct surface {
    xcb_window_t window;
    uint32_t width, height; /* from ConfigureNotify, 0 until the first query */
};

enum { IMG_FREE, IMG_ACQUIRED, IMG_PRESENTING };

struct rect { uint32_t x, y, w, h; };

struct image {
    VkImage image;
    VkDeviceMemory memory;
    VkBuffer buffer;
    VkDeviceMemory buffer_memory;
    uint8_t *pixels;
    VkCommandBuffer cmd;
    VkFence fence;
    int state;
    uint64_t presented_at;
    uint32_t nrects;
    struct rect rects[MAX_RECTS];
};

struct swapchain {
    struct dev *dev;
    struct surface *surface;
    VkExtent2D extent;
    uint32_t stride, count, next;
    struct image img[MAX_IMAGES];
    VkCommandPool pool;
    int coherent;
    pthread_mutex_t lock;
    pthread_cond_t cond;
    uint32_t fifo[MAX_IMAGES], fifo_head, fifo_len;
    pthread_t worker;
    int worker_started, stop, retired;
    /* X side, touched only by the worker after creation */
    uint32_t gc, seg;
    uint8_t *shm;
    size_t shm_size;
    uint8_t depth;
    int sync_pending;
    xcb_cookie_t sync_cookie;
    /* stats (BRICK_WSI_DEBUG=1) */
    uint64_t frames, pixels, blit_ns, t0;
    uint64_t acquire_wait_ns, fence_wait_ns, latency_ns, xsync_ns, submit_ns, acquires;
};

#define MAX_OBJECTS 64
static void *g_surfaces[MAX_OBJECTS], *g_swapchains[MAX_OBJECTS];
static pthread_mutex_t g_obj_lock = PTHREAD_MUTEX_INITIALIZER;

static void obj_add(void **set, void *p)
{
    pthread_mutex_lock(&g_obj_lock);
    for (int i = 0; i < MAX_OBJECTS; i++)
        if (!set[i]) {
            set[i] = p;
            break;
        }
    pthread_mutex_unlock(&g_obj_lock);
}

static int obj_has(void **set, const void *p)
{
    int found = 0;
    pthread_mutex_lock(&g_obj_lock);
    for (int i = 0; i < MAX_OBJECTS && !found; i++)
        found = set[i] == p;
    pthread_mutex_unlock(&g_obj_lock);
    return p && found;
}

static void obj_del(void **set, const void *p)
{
    pthread_mutex_lock(&g_obj_lock);
    for (int i = 0; i < MAX_OBJECTS; i++)
        if (set[i] == p)
            set[i] = NULL;
    pthread_mutex_unlock(&g_obj_lock);
}

#define OUR_SURFACE(s) ((struct surface *)(obj_has(g_surfaces, (void *)(uintptr_t)(s)) ? (void *)(uintptr_t)(s) : NULL))
#define OUR_SWAPCHAIN(s) ((struct swapchain *)(obj_has(g_swapchains, (void *)(uintptr_t)(s)) ? (void *)(uintptr_t)(s) : NULL))

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* ------------------------------------------------------------- instance -- */

static int is_x11_surface_ext(const char *name)
{
    return !strcmp(name, "VK_KHR_xcb_surface") || !strcmp(name, "VK_KHR_xlib_surface");
}

static VKAPI_ATTR VkResult VKAPI_CALL wsi_CreateInstance(
    const VkInstanceCreateInfo *ci, const VkAllocationCallbacks *alloc, VkInstance *instance)
{
    VkLayerInstanceCreateInfo *link = (VkLayerInstanceCreateInfo *)ci->pNext;
    while (link && !(link->sType == VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO &&
                     link->function == VK_LAYER_LINK_INFO))
        link = (VkLayerInstanceCreateInfo *)link->pNext;
    if (!link)
        return VK_ERROR_INITIALIZATION_FAILED;

    PFN_vkGetInstanceProcAddr gipa = link->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    link->u.pLayerInfo = link->u.pLayerInfo->pNext;
    PFN_vkCreateInstance create = (PFN_vkCreateInstance)gipa(VK_NULL_HANDLE, "vkCreateInstance");
    if (!create)
        return VK_ERROR_INITIALIZATION_FAILED;

    const char **names = malloc((ci->enabledExtensionCount + 1) * sizeof(*names));
    if (!names)
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    uint32_t kept = 0;
    int wants_x11 = 0;
    for (uint32_t i = 0; i < ci->enabledExtensionCount; i++) {
        if (is_x11_surface_ext(ci->ppEnabledExtensionNames[i]))
            wants_x11 = 1;
        else
            names[kept++] = ci->ppEnabledExtensionNames[i];
    }
    VkInstanceCreateInfo filtered = *ci;
    filtered.enabledExtensionCount = kept;
    filtered.ppEnabledExtensionNames = names;
    VkResult r = create(&filtered, alloc, instance);
    free(names);
    if (r != VK_SUCCESS)
        return r;

    struct inst *in = calloc(1, sizeof(*in));
    if (!in)
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    in->instance = *instance;
    in->gipa = gipa;
#define IFN(name) in->name = (PFN_vk##name)gipa(*instance, "vk" #name)
    IFN(DestroyInstance);
    IFN(EnumerateDeviceExtensionProperties);
    IFN(GetPhysicalDeviceMemoryProperties);
    IFN(GetPhysicalDeviceQueueFamilyProperties);
    IFN(GetPhysicalDeviceQueueFamilyProperties2);
    IFN(DestroySurfaceKHR);
    IFN(GetPhysicalDeviceSurfaceSupportKHR);
    IFN(GetPhysicalDeviceSurfaceCapabilitiesKHR);
    IFN(GetPhysicalDeviceSurfaceCapabilities2KHR);
    IFN(GetPhysicalDeviceSurfaceFormatsKHR);
    IFN(GetPhysicalDeviceSurfaceFormats2KHR);
    IFN(GetPhysicalDeviceSurfacePresentModesKHR);
    IFN(GetPhysicalDevicePresentRectanglesKHR);
#undef IFN
    if (!in->GetPhysicalDeviceQueueFamilyProperties2)
        in->GetPhysicalDeviceQueueFamilyProperties2 =
            (PFN_vkGetPhysicalDeviceQueueFamilyProperties2)gipa(*instance, "vkGetPhysicalDeviceQueueFamilyProperties2KHR");
    map_put(g_inst_map, dkey(*instance), in);
    if (wants_x11)
        x_conn(); /* before any sandbox closes the door */
    return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL wsi_DestroyInstance(VkInstance instance, const VkAllocationCallbacks *alloc)
{
    struct inst *in = INST(instance);
    if (!in)
        return;
    map_del(g_inst_map, dkey(instance));
    in->DestroyInstance(instance, alloc);
    free(in);
}

/* The application sees one queue fewer than the driver has; the last one is
 * the layer's, used only to signal acquire semaphores. */
static VKAPI_ATTR void VKAPI_CALL wsi_GetPhysicalDeviceQueueFamilyProperties(
    VkPhysicalDevice gpu, uint32_t *count, VkQueueFamilyProperties *props)
{
    struct inst *in = INST(gpu);
    in->GetPhysicalDeviceQueueFamilyProperties(gpu, count, props);
    if (props)
        for (uint32_t i = 0; i < *count; i++)
            if ((props[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && props[i].queueCount > 1) {
                props[i].queueCount--;
                break;
            }
}

static VKAPI_ATTR void VKAPI_CALL wsi_GetPhysicalDeviceQueueFamilyProperties2(
    VkPhysicalDevice gpu, uint32_t *count, VkQueueFamilyProperties2 *props)
{
    struct inst *in = INST(gpu);
    in->GetPhysicalDeviceQueueFamilyProperties2(gpu, count, props);
    if (props)
        for (uint32_t i = 0; i < *count; i++)
            if ((props[i].queueFamilyProperties.queueFlags & VK_QUEUE_GRAPHICS_BIT) &&
                props[i].queueFamilyProperties.queueCount > 1) {
                props[i].queueFamilyProperties.queueCount--;
                break;
            }
}

static const VkExtensionProperties k_device_exts[] = {
    { "VK_KHR_incremental_present", 2 },
};

static VKAPI_ATTR VkResult VKAPI_CALL wsi_EnumerateDeviceExtensionProperties(
    VkPhysicalDevice gpu, const char *layer, uint32_t *count, VkExtensionProperties *props)
{
    if (layer && !strcmp(layer, LAYER_NAME)) {
        uint32_t n = sizeof(k_device_exts) / sizeof(k_device_exts[0]);
        if (!props) {
            *count = n;
            return VK_SUCCESS;
        }
        uint32_t c = *count < n ? *count : n;
        memcpy(props, k_device_exts, c * sizeof(*props));
        *count = c;
        return c < n ? VK_INCOMPLETE : VK_SUCCESS;
    }
    struct inst *in = INST(gpu);
    if (layer)
        return in->EnumerateDeviceExtensionProperties(gpu, layer, count, props);

    uint32_t n = 0;
    VkResult r = in->EnumerateDeviceExtensionProperties(gpu, NULL, &n, NULL);
    if (r != VK_SUCCESS)
        return r;
    VkExtensionProperties *all = calloc(n + 1, sizeof(*all));
    if (!all)
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    r = in->EnumerateDeviceExtensionProperties(gpu, NULL, &n, all);
    if (r < 0) {
        free(all);
        return r;
    }
    int have = 0;
    for (uint32_t i = 0; i < n; i++)
        have |= !strcmp(all[i].extensionName, k_device_exts[0].extensionName);
    if (!have)
        all[n++] = k_device_exts[0];
    if (!props) {
        *count = n;
        free(all);
        return VK_SUCCESS;
    }
    uint32_t c = *count < n ? *count : n;
    memcpy(props, all, c * sizeof(*props));
    *count = c;
    free(all);
    return c < n ? VK_INCOMPLETE : VK_SUCCESS;
}

/* -------------------------------------------------------------- surface -- */

typedef struct {
    VkStructureType sType;
    const void *pNext;
    VkFlags flags;
    xcb_connection_t *connection;
    xcb_window_t window;
} XcbSurfaceCreateInfo;

static VKAPI_ATTR VkResult VKAPI_CALL wsi_CreateXcbSurfaceKHR(
    VkInstance instance, const XcbSurfaceCreateInfo *info, const VkAllocationCallbacks *alloc,
    VkSurfaceKHR *out)
{
    (void)instance; (void)alloc;
    xcb_connection_t *c = x_conn();
    if (!c)
        return VK_ERROR_INITIALIZATION_FAILED;
    struct surface *s = calloc(1, sizeof(*s));
    if (!s)
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    s->window = info->window;
    /* Our own connection sees ConfigureNotify for the window; the application's
     * event mask on it is unaffected. */
    uint32_t mask = XCB_EVENT_MASK_STRUCTURE_NOTIFY;
    X.change_window_attributes(c, s->window, XCB_CW_EVENT_MASK, &mask);
    X.flush(c);
    obj_add(g_surfaces, s);
    *out = (VkSurfaceKHR)(uintptr_t)s;
    DBG("surface for window 0x%x\n", s->window);
    return VK_SUCCESS;
}

static VKAPI_ATTR VkResult VKAPI_CALL wsi_CreateXlibSurfaceKHR(
    VkInstance instance, const void *info, const VkAllocationCallbacks *alloc, VkSurfaceKHR *out)
{
    (void)instance; (void)info; (void)alloc; (void)out;
    LOG("VK_KHR_xlib_surface is advertised but not implemented; use VK_KHR_xcb_surface\n");
    return VK_ERROR_INITIALIZATION_FAILED;
}

static VKAPI_ATTR void VKAPI_CALL wsi_DestroySurfaceKHR(
    VkInstance instance, VkSurfaceKHR surface, const VkAllocationCallbacks *alloc)
{
    struct surface *s = OUR_SURFACE(surface);
    if (!s) {
        if (surface)
            INST(instance)->DestroySurfaceKHR(instance, surface, alloc);
        return;
    }
    obj_del(g_surfaces, s);
    free(s);
}

/* Drain our connection's events, keeping each surface's size current. */
static void pump_events(void)
{
    xcb_connection_t *c = x_conn();
    xcb_generic_event_t *e;
    if (!c)
        return;
    while ((e = X.poll_for_event(c))) {
        if ((e->response_type & 0x7f) == XCB_CONFIGURE_NOTIFY) {
            xcb_configure_notify_event_t *cn = (xcb_configure_notify_event_t *)e;
            pthread_mutex_lock(&g_obj_lock);
            for (int i = 0; i < MAX_OBJECTS; i++) {
                struct surface *s = g_surfaces[i];
                if (s && s->window == cn->window) {
                    s->width = cn->width;
                    s->height = cn->height;
                }
            }
            pthread_mutex_unlock(&g_obj_lock);
        }
        free(e);
    }
}

static VkResult surface_size(struct surface *s, uint32_t *w, uint32_t *h, uint8_t *depth)
{
    if (!x_geometry(s->window, w, h, depth))
        return VK_ERROR_SURFACE_LOST_KHR;
    s->width = *w;
    s->height = *h;
    return VK_SUCCESS;
}

static VKAPI_ATTR VkResult VKAPI_CALL wsi_GetPhysicalDeviceSurfaceSupportKHR(
    VkPhysicalDevice gpu, uint32_t family, VkSurfaceKHR surface, VkBool32 *supported)
{
    if (!OUR_SURFACE(surface))
        return INST(gpu)->GetPhysicalDeviceSurfaceSupportKHR(gpu, family, surface, supported);
    uint32_t n = 0;
    INST(gpu)->GetPhysicalDeviceQueueFamilyProperties(gpu, &n, NULL);
    VkQueueFamilyProperties *p = calloc(n ? n : 1, sizeof(*p));
    INST(gpu)->GetPhysicalDeviceQueueFamilyProperties(gpu, &n, p);
    *supported = family < n && (p[family].queueFlags & VK_QUEUE_GRAPHICS_BIT);
    free(p);
    return VK_SUCCESS;
}

static void fill_caps(struct surface *s, VkSurfaceCapabilitiesKHR *caps, VkResult *r)
{
    uint32_t w = 0, h = 0;
    uint8_t depth;
    *r = surface_size(s, &w, &h, &depth);
    memset(caps, 0, sizeof(*caps));
    caps->minImageCount = 2;
    caps->maxImageCount = MAX_IMAGES;
    caps->currentExtent = (VkExtent2D){ w, h };
    caps->minImageExtent = (VkExtent2D){ 1, 1 };
    caps->maxImageExtent = (VkExtent2D){ 4096, 4096 };
    caps->maxImageArrayLayers = 1;
    caps->supportedTransforms = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    caps->currentTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    caps->supportedCompositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR | VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
    caps->supportedUsageFlags = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT;
}

static VKAPI_ATTR VkResult VKAPI_CALL wsi_GetPhysicalDeviceSurfaceCapabilitiesKHR(
    VkPhysicalDevice gpu, VkSurfaceKHR surface, VkSurfaceCapabilitiesKHR *caps)
{
    struct surface *s = OUR_SURFACE(surface);
    if (!s)
        return INST(gpu)->GetPhysicalDeviceSurfaceCapabilitiesKHR(gpu, surface, caps);
    VkResult r;
    fill_caps(s, caps, &r);
    return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL wsi_GetPhysicalDeviceSurfaceCapabilities2KHR(
    VkPhysicalDevice gpu, const VkPhysicalDeviceSurfaceInfo2KHR *info, VkSurfaceCapabilities2KHR *caps)
{
    struct surface *s = OUR_SURFACE(info->surface);
    if (!s)
        return INST(gpu)->GetPhysicalDeviceSurfaceCapabilities2KHR(gpu, info, caps);
    VkResult r;
    fill_caps(s, &caps->surfaceCapabilities, &r);
    for (VkBaseOutStructure *p = caps->pNext; p; p = p->pNext) {
        if (p->sType == VK_STRUCTURE_TYPE_SURFACE_PROTECTED_CAPABILITIES_KHR)
            ((VkSurfaceProtectedCapabilitiesKHR *)p)->supportsProtected = VK_FALSE;
        else if (p->sType == VK_STRUCTURE_TYPE_SHARED_PRESENT_SURFACE_CAPABILITIES_KHR)
            ((VkSharedPresentSurfaceCapabilitiesKHR *)p)->sharedPresentSupportedUsageFlags = 0;
    }
    return r;
}

static const VkSurfaceFormatKHR k_formats[] = {
    { VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR },
    { VK_FORMAT_B8G8R8A8_SRGB, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR },
};
#define N_FORMATS (sizeof(k_formats) / sizeof(k_formats[0]))

static VKAPI_ATTR VkResult VKAPI_CALL wsi_GetPhysicalDeviceSurfaceFormatsKHR(
    VkPhysicalDevice gpu, VkSurfaceKHR surface, uint32_t *count, VkSurfaceFormatKHR *formats)
{
    if (!OUR_SURFACE(surface))
        return INST(gpu)->GetPhysicalDeviceSurfaceFormatsKHR(gpu, surface, count, formats);
    if (!formats) {
        *count = N_FORMATS;
        return VK_SUCCESS;
    }
    uint32_t c = *count < N_FORMATS ? *count : N_FORMATS;
    memcpy(formats, k_formats, c * sizeof(*formats));
    *count = c;
    return c < N_FORMATS ? VK_INCOMPLETE : VK_SUCCESS;
}

static VKAPI_ATTR VkResult VKAPI_CALL wsi_GetPhysicalDeviceSurfaceFormats2KHR(
    VkPhysicalDevice gpu, const VkPhysicalDeviceSurfaceInfo2KHR *info, uint32_t *count,
    VkSurfaceFormat2KHR *formats)
{
    if (!OUR_SURFACE(info->surface))
        return INST(gpu)->GetPhysicalDeviceSurfaceFormats2KHR(gpu, info, count, formats);
    if (!formats) {
        *count = N_FORMATS;
        return VK_SUCCESS;
    }
    uint32_t c = *count < N_FORMATS ? *count : N_FORMATS;
    for (uint32_t i = 0; i < c; i++)
        formats[i].surfaceFormat = k_formats[i];
    *count = c;
    return c < N_FORMATS ? VK_INCOMPLETE : VK_SUCCESS;
}

static VKAPI_ATTR VkResult VKAPI_CALL wsi_GetPhysicalDeviceSurfacePresentModesKHR(
    VkPhysicalDevice gpu, VkSurfaceKHR surface, uint32_t *count, VkPresentModeKHR *modes)
{
    if (!OUR_SURFACE(surface))
        return INST(gpu)->GetPhysicalDeviceSurfacePresentModesKHR(gpu, surface, count, modes);
    if (!modes) {
        *count = 1;
        return VK_SUCCESS;
    }
    if (*count < 1)
        return VK_INCOMPLETE;
    modes[0] = VK_PRESENT_MODE_FIFO_KHR;
    *count = 1;
    return VK_SUCCESS;
}

static VKAPI_ATTR VkResult VKAPI_CALL wsi_GetPhysicalDevicePresentRectanglesKHR(
    VkPhysicalDevice gpu, VkSurfaceKHR surface, uint32_t *count, VkRect2D *rects)
{
    struct surface *s = OUR_SURFACE(surface);
    if (!s)
        return INST(gpu)->GetPhysicalDevicePresentRectanglesKHR(gpu, surface, count, rects);
    if (!rects) {
        *count = 1;
        return VK_SUCCESS;
    }
    if (*count < 1)
        return VK_INCOMPLETE;
    uint32_t w = 0, h = 0;
    VkResult r = surface_size(s, &w, &h, NULL);
    rects[0] = (VkRect2D){ { 0, 0 }, { w, h } };
    *count = 1;
    return r;
}

static VKAPI_ATTR VkBool32 VKAPI_CALL wsi_GetPhysicalDeviceXcbPresentationSupportKHR(
    VkPhysicalDevice gpu, uint32_t family, xcb_connection_t *connection, uint32_t visual)
{
    (void)gpu; (void)family; (void)connection; (void)visual;
    return VK_TRUE;
}

static VKAPI_ATTR VkBool32 VKAPI_CALL wsi_GetPhysicalDeviceXlibPresentationSupportKHR(
    VkPhysicalDevice gpu, uint32_t family, void *display, unsigned long visual)
{
    (void)gpu; (void)family; (void)display; (void)visual;
    return VK_FALSE;
}

/* --------------------------------------------------------------- device -- */

static VKAPI_ATTR VkResult VKAPI_CALL wsi_CreateDevice(
    VkPhysicalDevice gpu, const VkDeviceCreateInfo *ci, const VkAllocationCallbacks *alloc,
    VkDevice *device)
{
    VkLayerDeviceCreateInfo *link = NULL, *loader_cb = NULL;
    for (VkLayerDeviceCreateInfo *p = (VkLayerDeviceCreateInfo *)ci->pNext; p;
         p = (VkLayerDeviceCreateInfo *)p->pNext)
        if (p->sType == VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO) {
            if (p->function == VK_LAYER_LINK_INFO && !link)
                link = p;
            else if (p->function == VK_LOADER_DATA_CALLBACK && !loader_cb)
                loader_cb = p;
        }
    if (!link)
        return VK_ERROR_INITIALIZATION_FAILED;

    PFN_vkGetInstanceProcAddr gipa = link->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    PFN_vkGetDeviceProcAddr gdpa = link->u.pLayerInfo->pfnNextGetDeviceProcAddr;
    link->u.pLayerInfo = link->u.pLayerInfo->pNext;
    PFN_vkCreateDevice create = (PFN_vkCreateDevice)gipa(VK_NULL_HANDLE, "vkCreateDevice");
    if (!create)
        return VK_ERROR_INITIALIZATION_FAILED;
    struct inst *in = INST(gpu);

    /* One more queue in the graphics family, for the layer. */
    uint32_t nfam = 0;
    in->GetPhysicalDeviceQueueFamilyProperties(gpu, &nfam, NULL);
    VkQueueFamilyProperties *fam = calloc(nfam ? nfam : 1, sizeof(*fam));
    in->GetPhysicalDeviceQueueFamilyProperties(gpu, &nfam, fam);

    VkDeviceQueueCreateInfo *qci = calloc(ci->queueCreateInfoCount + 1, sizeof(*qci));
    float *prio = NULL;
    int32_t our_family = -1;
    uint32_t our_index = 0;
    memcpy(qci, ci->pQueueCreateInfos, ci->queueCreateInfoCount * sizeof(*qci));
    for (uint32_t i = 0; i < ci->queueCreateInfoCount; i++) {
        uint32_t f = qci[i].queueFamilyIndex;
        if (f < nfam && (fam[f].queueFlags & VK_QUEUE_GRAPHICS_BIT) && qci[i].queueCount < fam[f].queueCount) {
            prio = calloc(qci[i].queueCount + 1, sizeof(float));
            memcpy(prio, qci[i].pQueuePriorities, qci[i].queueCount * sizeof(float));
            prio[qci[i].queueCount] = prio[0];
            our_family = (int32_t)f;
            our_index = qci[i].queueCount;
            qci[i].queueCount++;
            qci[i].pQueuePriorities = prio;
            break;
        }
    }
    free(fam);

    const char **names = calloc(ci->enabledExtensionCount + 1, sizeof(*names));
    uint32_t kept = 0;
    for (uint32_t i = 0; i < ci->enabledExtensionCount; i++)
        if (strcmp(ci->ppEnabledExtensionNames[i], k_device_exts[0].extensionName))
            names[kept++] = ci->ppEnabledExtensionNames[i];

    VkDeviceCreateInfo mod = *ci;
    mod.pQueueCreateInfos = qci;
    mod.enabledExtensionCount = kept;
    mod.ppEnabledExtensionNames = names;
    VkResult r = create(gpu, &mod, alloc, device);
    free(names);
    free(qci);
    free(prio);
    if (r != VK_SUCCESS)
        return r;

    struct dev *d = calloc(1, sizeof(*d));
    d->device = *device;
    d->phys = gpu;
    d->inst = in;
    d->gdpa = gdpa;
    d->set_loader_data = loader_cb ? loader_cb->u.pfnSetDeviceLoaderData : NULL;
    pthread_mutex_init(&d->queue_lock, NULL);
    in->GetPhysicalDeviceMemoryProperties(gpu, &d->mem);
#define DFN(name) d->name = (PFN_vk##name)gdpa(*device, "vk" #name)
    DFN(DestroyDevice); DFN(GetDeviceQueue); DFN(QueueSubmit); DFN(QueueWaitIdle);
    DFN(CreateImage); DFN(DestroyImage); DFN(GetImageMemoryRequirements); DFN(BindImageMemory);
    DFN(CreateBuffer); DFN(DestroyBuffer); DFN(GetBufferMemoryRequirements); DFN(BindBufferMemory);
    DFN(AllocateMemory); DFN(FreeMemory); DFN(MapMemory); DFN(UnmapMemory);
    DFN(InvalidateMappedMemoryRanges);
    DFN(CreateCommandPool); DFN(DestroyCommandPool); DFN(AllocateCommandBuffers);
    DFN(ResetCommandBuffer); DFN(BeginCommandBuffer); DFN(EndCommandBuffer);
    DFN(CmdPipelineBarrier); DFN(CmdCopyImageToBuffer);
    DFN(CreateFence); DFN(DestroyFence); DFN(WaitForFences); DFN(ResetFences);
    DFN(CreateSwapchainKHR); DFN(DestroySwapchainKHR); DFN(GetSwapchainImagesKHR);
    DFN(AcquireNextImageKHR); DFN(AcquireNextImage2KHR); DFN(QueuePresentKHR);
    DFN(GetSwapchainStatusKHR);
#undef DFN
    if (our_family >= 0) {
        d->family = (uint32_t)our_family;
        d->GetDeviceQueue(*device, d->family, our_index, &d->queue);
        if (d->queue && d->set_loader_data)
            d->set_loader_data(*device, d->queue);
    }
    if (!d->queue)
        LOG("no spare queue: X11 swapchains will not be available on this device\n");
    map_put(g_dev_map, dkey(*device), d);
    return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL wsi_DestroyDevice(VkDevice device, const VkAllocationCallbacks *alloc)
{
    struct dev *d = DEV(device);
    if (!d)
        return;
    map_del(g_dev_map, dkey(device));
    d->DestroyDevice(device, alloc);
    pthread_mutex_destroy(&d->queue_lock);
    free(d);
}

/* ------------------------------------------------------------ swapchain -- */

static int32_t pick_memory(struct dev *d, uint32_t type_bits, VkMemoryPropertyFlags want, VkMemoryPropertyFlags avoid)
{
    for (uint32_t i = 0; i < d->mem.memoryTypeCount; i++) {
        VkMemoryPropertyFlags f = d->mem.memoryTypes[i].propertyFlags;
        if ((type_bits & (1u << i)) && (f & want) == want && !(f & avoid))
            return (int32_t)i;
    }
    return -1;
}

static void x_detach(struct swapchain *sc)
{
    xcb_connection_t *c = x_conn();
    if (c && sc->seg) {
        X.shm_detach(c, sc->seg);
        X.free_gc(c, sc->gc);
        X.flush(c);
    }
    if (sc->shm)
        munmap(sc->shm, sc->shm_size);
    sc->shm = NULL;
    sc->seg = 0;
}

static int x_attach(struct swapchain *sc)
{
    xcb_connection_t *c = x_conn();
    if (!c)
        return 0;
    xcb_shm_query_version_reply_t *v = X.shm_query_version_reply(c, X.shm_query_version(c), NULL);
    int ok = v && (v->major_version > 1 || v->minor_version >= 2);
    free(v);
    if (!ok) {
        LOG("the X server lacks MIT-SHM 1.2\n");
        return 0;
    }
    sc->shm_size = (size_t)sc->stride * sc->extent.height;
    int fd = (int)syscall(SYS_memfd_create, "brick-wsi", 1u /* MFD_CLOEXEC */);
    if (fd < 0 || ftruncate(fd, (off_t)sc->shm_size) < 0) {
        LOG("memfd: %s\n", strerror(errno));
        if (fd >= 0)
            close(fd);
        return 0;
    }
    sc->shm = mmap(NULL, sc->shm_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (sc->shm == MAP_FAILED) {
        sc->shm = NULL;
        close(fd);
        return 0;
    }
    sc->seg = X.generate_id(c);
    xcb_generic_error_t *err = X.request_check(c, X.shm_attach_fd_checked(c, sc->seg, fd, 0)); /* xcb closes fd */
    if (err) {
        LOG("ShmAttachFd failed (error %u)\n", err->error_code);
        free(err);
        munmap(sc->shm, sc->shm_size);
        sc->shm = NULL;
        sc->seg = 0;
        return 0;
    }
    sc->gc = X.generate_id(c);
    X.create_gc(c, sc->gc, sc->surface->window, 0, NULL);
    X.flush(c);
    return 1;
}

/* Move one presented image's damaged rectangles into the window. */
static void blit(struct swapchain *sc, struct image *im)
{
    struct dev *d = sc->dev;
    xcb_connection_t *c = x_conn();
    uint64_t t = now_ns();
    uint32_t y0 = UINT32_MAX, y1 = 0;
    for (uint32_t i = 0; i < im->nrects; i++) {
        if (im->rects[i].y < y0)
            y0 = im->rects[i].y;
        if (im->rects[i].y + im->rects[i].h > y1)
            y1 = im->rects[i].y + im->rects[i].h;
    }
    if (!im->nrects || !c)
        return;
    if (!sc->coherent) {
        VkMappedMemoryRange range = { VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, NULL, im->buffer_memory,
                                      (VkDeviceSize)y0 * sc->stride, (VkDeviceSize)(y1 - y0) * sc->stride };
        d->InvalidateMappedMemoryRanges(d->device, 1, &range);
    }
    /* The server must be done with the previous frame before we overwrite it. */
    if (sc->sync_pending) {
        uint64_t ts = now_ns();
        free(X.get_input_focus_reply(c, sc->sync_cookie, NULL));
        sc->sync_pending = 0;
        sc->xsync_ns += now_ns() - ts;
    }
    for (uint32_t i = 0; i < im->nrects; i++) {
        const struct rect *r = &im->rects[i];
        for (uint32_t y = r->y; y < r->y + r->h; y++) {
            const uint32_t *src = (const uint32_t *)(im->pixels + (size_t)y * sc->stride) + r->x;
            uint32_t *dst = (uint32_t *)(sc->shm + (size_t)y * sc->stride) + r->x;
            for (uint32_t x = 0; x < r->w; x++)
                dst[x] = src[x] | 0xff000000u;
        }
        X.shm_put_image(c, sc->surface->window, sc->gc, (uint16_t)sc->extent.width,
                        (uint16_t)sc->extent.height, (uint16_t)r->x, (uint16_t)r->y, (uint16_t)r->w,
                        (uint16_t)r->h, (int16_t)r->x, (int16_t)r->y, sc->depth,
                        XCB_IMAGE_FORMAT_Z_PIXMAP, 0, sc->seg, 0);
        sc->pixels += (uint64_t)r->w * r->h;
    }
    sc->sync_cookie = X.get_input_focus(c);
    sc->sync_pending = 1;
    X.flush(c);
    sc->blit_ns += now_ns() - t;
    sc->frames++;
    uint64_t span = now_ns() - sc->t0;
    if (g_debug && span > 2000000000ull) {
        double n = (double)sc->frames;
        LOG("window 0x%x: %.1f fps, %.2f Mpix/frame | blit %.2f  xsync %.2f  fence-wait %.2f  "
            "present->done %.2f  acquire-wait %.2f (%llu acq)  submit %.2f  ms/frame\n",
            sc->surface->window, n / (span / 1e9), sc->pixels / n / 1e6, sc->blit_ns / n / 1e6,
            sc->xsync_ns / n / 1e6, sc->fence_wait_ns / n / 1e6, sc->latency_ns / n / 1e6,
            sc->acquire_wait_ns / n / 1e6, (unsigned long long)sc->acquires, sc->submit_ns / n / 1e6);
        sc->t0 = now_ns();
        sc->frames = sc->pixels = sc->blit_ns = sc->xsync_ns = sc->fence_wait_ns = 0;
        sc->latency_ns = sc->acquire_wait_ns = sc->acquires = sc->submit_ns = 0;
    }
}

static void *worker_main(void *arg)
{
    struct swapchain *sc = arg;
    struct dev *d = sc->dev;
    for (;;) {
        pthread_mutex_lock(&sc->lock);
        while (!sc->stop && !sc->fifo_len)
            pthread_cond_wait(&sc->cond, &sc->lock);
        if (!sc->fifo_len) {
            pthread_mutex_unlock(&sc->lock);
            break;
        }
        uint32_t idx = sc->fifo[sc->fifo_head];
        pthread_mutex_unlock(&sc->lock);

        struct image *im = &sc->img[idx];
        uint64_t tw = now_ns();
        if (d->WaitForFences(d->device, 1, &im->fence, VK_TRUE, UINT64_MAX) == VK_SUCCESS) {
            uint64_t done = now_ns();
            sc->fence_wait_ns += done - tw;
            sc->latency_ns += done - im->presented_at;
            d->ResetFences(d->device, 1, &im->fence);
            if (sc->shm)
                blit(sc, im);
        }

        pthread_mutex_lock(&sc->lock);
        sc->fifo_head = (sc->fifo_head + 1) % MAX_IMAGES;
        sc->fifo_len--;
        im->state = IMG_FREE;
        pthread_cond_broadcast(&sc->cond);
        pthread_mutex_unlock(&sc->lock);
    }
    xcb_connection_t *c = x_conn();
    if (c && sc->sync_pending)
        free(X.get_input_focus_reply(c, sc->sync_cookie, NULL));
    return NULL;
}

static void destroy_swapchain(struct swapchain *sc, const VkAllocationCallbacks *alloc)
{
    struct dev *d = sc->dev;
    if (sc->worker_started) {
        pthread_mutex_lock(&sc->lock);
        sc->stop = 1;
        pthread_cond_broadcast(&sc->cond);
        pthread_mutex_unlock(&sc->lock);
        pthread_join(sc->worker, NULL);
    }
    /* Acquire may still have an empty submission in flight on our queue. */
    if (d->queue) {
        pthread_mutex_lock(&d->queue_lock);
        d->QueueWaitIdle(d->queue);
        pthread_mutex_unlock(&d->queue_lock);
    }
    x_detach(sc);
    for (uint32_t i = 0; i < sc->count; i++) {
        struct image *im = &sc->img[i];
        if (im->fence)
            d->DestroyFence(d->device, im->fence, alloc);
        if (im->buffer)
            d->DestroyBuffer(d->device, im->buffer, alloc);
        if (im->buffer_memory)
            d->FreeMemory(d->device, im->buffer_memory, alloc);
        if (im->image)
            d->DestroyImage(d->device, im->image, alloc);
        if (im->memory)
            d->FreeMemory(d->device, im->memory, alloc);
    }
    if (sc->pool)
        d->DestroyCommandPool(d->device, sc->pool, alloc);
    pthread_mutex_destroy(&sc->lock);
    pthread_cond_destroy(&sc->cond);
    free(sc);
}

static VKAPI_ATTR VkResult VKAPI_CALL wsi_CreateSwapchainKHR(
    VkDevice device, const VkSwapchainCreateInfoKHR *ci, const VkAllocationCallbacks *alloc,
    VkSwapchainKHR *out)
{
    struct dev *d = DEV(device);
    struct surface *s = OUR_SURFACE(ci->surface);
    if (!s)
        return d->CreateSwapchainKHR(device, ci, alloc, out);
    if (!d->queue)
        return VK_ERROR_INITIALIZATION_FAILED;

    struct swapchain *old = OUR_SWAPCHAIN(ci->oldSwapchain);
    if (old)
        old->retired = 1;

    struct swapchain *sc = calloc(1, sizeof(*sc));
    if (!sc)
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    sc->dev = d;
    sc->surface = s;
    sc->extent = ci->imageExtent;
    sc->stride = ci->imageExtent.width * 4;
    sc->count = ci->minImageCount < 3 ? 3 : ci->minImageCount;
    if (sc->count > MAX_IMAGES)
        sc->count = MAX_IMAGES;
    sc->t0 = now_ns();
    pthread_mutex_init(&sc->lock, NULL);
    pthread_cond_init(&sc->cond, NULL);
    uint32_t w, h;
    if (!x_geometry(s->window, &w, &h, &sc->depth))
        sc->depth = 24;

    VkResult r = VK_ERROR_OUT_OF_DEVICE_MEMORY;
    VkCommandPoolCreateInfo pci = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL,
                                    VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, d->family };
    if (d->CreateCommandPool(device, &pci, alloc, &sc->pool) != VK_SUCCESS)
        goto fail;

    VkImageFormatListCreateInfo *list = NULL;
    for (const VkBaseInStructure *p = ci->pNext; p; p = p->pNext)
        if (p->sType == VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO)
            list = (VkImageFormatListCreateInfo *)p;
    VkImageFormatListCreateInfo list_copy;
    if (list) {
        list_copy = *list;
        list_copy.pNext = NULL;
    }

    for (uint32_t i = 0; i < sc->count; i++) {
        struct image *im = &sc->img[i];
        VkImageCreateInfo ici = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
            .pNext = list ? &list_copy : NULL,
            .flags = (ci->flags & VK_SWAPCHAIN_CREATE_MUTABLE_FORMAT_BIT_KHR)
                         ? VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | VK_IMAGE_CREATE_EXTENDED_USAGE_BIT
                         : 0,
            .imageType = VK_IMAGE_TYPE_2D,
            .format = ci->imageFormat,
            .extent = { ci->imageExtent.width, ci->imageExtent.height, 1 },
            .mipLevels = 1,
            .arrayLayers = 1,
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .tiling = VK_IMAGE_TILING_OPTIMAL,
            .usage = ci->imageUsage | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
            .sharingMode = ci->imageSharingMode,
            .queueFamilyIndexCount = ci->queueFamilyIndexCount,
            .pQueueFamilyIndices = ci->pQueueFamilyIndices,
            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        };
        if ((r = d->CreateImage(device, &ici, alloc, &im->image)) != VK_SUCCESS)
            goto fail;
        VkMemoryRequirements req;
        d->GetImageMemoryRequirements(device, im->image, &req);
        int32_t type = pick_memory(d, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                                   VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT);
        if (type < 0)
            type = pick_memory(d, req.memoryTypeBits, 0, VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT);
        VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, req.size, (uint32_t)type };
        r = VK_ERROR_OUT_OF_DEVICE_MEMORY;
        if (type < 0 || (r = d->AllocateMemory(device, &mai, alloc, &im->memory)) != VK_SUCCESS)
            goto fail;
        if ((r = d->BindImageMemory(device, im->image, im->memory, 0)) != VK_SUCCESS)
            goto fail;

        VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0,
                                   (VkDeviceSize)sc->stride * sc->extent.height,
                                   VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, NULL };
        if ((r = d->CreateBuffer(device, &bci, alloc, &im->buffer)) != VK_SUCCESS)
            goto fail;
        d->GetBufferMemoryRequirements(device, im->buffer, &req);
        type = pick_memory(d, req.memoryTypeBits,
                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT, 0);
        sc->coherent = 0;
        if (type < 0) {
            type = pick_memory(d, req.memoryTypeBits,
                               VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 0);
            sc->coherent = 1;
        } else if (d->mem.memoryTypes[type].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)
            sc->coherent = 1;
        mai.allocationSize = req.size;
        mai.memoryTypeIndex = (uint32_t)type;
        r = VK_ERROR_OUT_OF_DEVICE_MEMORY;
        if (type < 0 || (r = d->AllocateMemory(device, &mai, alloc, &im->buffer_memory)) != VK_SUCCESS)
            goto fail;
        if ((r = d->BindBufferMemory(device, im->buffer, im->buffer_memory, 0)) != VK_SUCCESS)
            goto fail;
        if ((r = d->MapMemory(device, im->buffer_memory, 0, VK_WHOLE_SIZE, 0, (void **)&im->pixels)) != VK_SUCCESS)
            goto fail;

        VkCommandBufferAllocateInfo cai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, sc->pool,
                                            VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1 };
        if ((r = d->AllocateCommandBuffers(device, &cai, &im->cmd)) != VK_SUCCESS)
            goto fail;
        if (d->set_loader_data)
            d->set_loader_data(device, im->cmd);
        VkFenceCreateInfo fci = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, NULL, 0 };
        if ((r = d->CreateFence(device, &fci, alloc, &im->fence)) != VK_SUCCESS)
            goto fail;
    }

    if (!x_attach(sc)) {
        r = VK_ERROR_INITIALIZATION_FAILED;
        goto fail;
    }
    if (pthread_create(&sc->worker, NULL, worker_main, sc)) {
        r = VK_ERROR_INITIALIZATION_FAILED;
        goto fail;
    }
    sc->worker_started = 1;
    obj_add(g_swapchains, sc);
    *out = (VkSwapchainKHR)(uintptr_t)sc;
    DBG("swapchain %ux%u x%u for window 0x%x, readback memory %s\n", sc->extent.width,
        sc->extent.height, sc->count, s->window, sc->coherent ? "coherent" : "cached");
    return VK_SUCCESS;

fail:
    LOG("swapchain creation failed (%d)\n", r);
    destroy_swapchain(sc, alloc);
    return r;
}

static VKAPI_ATTR void VKAPI_CALL wsi_DestroySwapchainKHR(
    VkDevice device, VkSwapchainKHR swapchain, const VkAllocationCallbacks *alloc)
{
    struct swapchain *sc = OUR_SWAPCHAIN(swapchain);
    if (!sc) {
        if (swapchain)
            DEV(device)->DestroySwapchainKHR(device, swapchain, alloc);
        return;
    }
    obj_del(g_swapchains, sc);
    destroy_swapchain(sc, alloc);
}

static VKAPI_ATTR VkResult VKAPI_CALL wsi_GetSwapchainImagesKHR(
    VkDevice device, VkSwapchainKHR swapchain, uint32_t *count, VkImage *images)
{
    struct swapchain *sc = OUR_SWAPCHAIN(swapchain);
    if (!sc)
        return DEV(device)->GetSwapchainImagesKHR(device, swapchain, count, images);
    if (!images) {
        *count = sc->count;
        return VK_SUCCESS;
    }
    uint32_t c = *count < sc->count ? *count : sc->count;
    for (uint32_t i = 0; i < c; i++)
        images[i] = sc->img[i].image;
    *count = c;
    return c < sc->count ? VK_INCOMPLETE : VK_SUCCESS;
}

static VkResult swapchain_status(struct swapchain *sc)
{
    pump_events();
    if (sc->retired)
        return VK_ERROR_OUT_OF_DATE_KHR;
    struct surface *s = sc->surface;
    if (s->width && (s->width != sc->extent.width || s->height != sc->extent.height))
        return VK_ERROR_OUT_OF_DATE_KHR;
    return VK_SUCCESS;
}

static VkResult acquire(struct swapchain *sc, uint64_t timeout, VkSemaphore sem, VkFence fence, uint32_t *index)
{
    struct dev *d = sc->dev;
    VkResult st = swapchain_status(sc);
    if (st != VK_SUCCESS)
        return st;

    pthread_mutex_lock(&sc->lock);
    struct timespec deadline;
    if (timeout && timeout != UINT64_MAX) {
        clock_gettime(CLOCK_REALTIME, &deadline);
        uint64_t ns = (uint64_t)deadline.tv_nsec + timeout % 1000000000ull;
        deadline.tv_sec += (time_t)(timeout / 1000000000ull + ns / 1000000000ull);
        deadline.tv_nsec = (long)(ns % 1000000000ull);
    }
    int found = -1;
    uint64_t ta = now_ns();
    for (;;) {
        for (uint32_t k = 0; k < sc->count && found < 0; k++) {
            uint32_t i = (sc->next + k) % sc->count;
            if (sc->img[i].state == IMG_FREE)
                found = (int)i;
        }
        if (found >= 0)
            break;
        if (!timeout) {
            pthread_mutex_unlock(&sc->lock);
            return VK_NOT_READY;
        }
        if (timeout == UINT64_MAX)
            pthread_cond_wait(&sc->cond, &sc->lock);
        else if (pthread_cond_timedwait(&sc->cond, &sc->lock, &deadline) == ETIMEDOUT) {
            pthread_mutex_unlock(&sc->lock);
            return VK_TIMEOUT;
        }
    }
    sc->img[found].state = IMG_ACQUIRED;
    sc->next = ((uint32_t)found + 1) % sc->count;
    sc->acquire_wait_ns += now_ns() - ta;
    sc->acquires++;
    pthread_mutex_unlock(&sc->lock);

    /* The image is idle: its copy finished before it was freed.  An empty
     * submission on our own queue signals whatever the application waits on. */
    VkResult r = VK_SUCCESS;
    if (sem || fence) {
        VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                            .signalSemaphoreCount = sem ? 1 : 0,
                            .pSignalSemaphores = &sem };
        pthread_mutex_lock(&d->queue_lock);
        r = d->QueueSubmit(d->queue, 1, &si, fence);
        pthread_mutex_unlock(&d->queue_lock);
    }
    *index = (uint32_t)found;
    return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL wsi_AcquireNextImageKHR(
    VkDevice device, VkSwapchainKHR swapchain, uint64_t timeout, VkSemaphore sem, VkFence fence,
    uint32_t *index)
{
    struct swapchain *sc = OUR_SWAPCHAIN(swapchain);
    if (!sc)
        return DEV(device)->AcquireNextImageKHR(device, swapchain, timeout, sem, fence, index);
    return acquire(sc, timeout, sem, fence, index);
}

static VKAPI_ATTR VkResult VKAPI_CALL wsi_AcquireNextImage2KHR(
    VkDevice device, const VkAcquireNextImageInfoKHR *info, uint32_t *index)
{
    struct swapchain *sc = OUR_SWAPCHAIN(info->swapchain);
    if (!sc)
        return DEV(device)->AcquireNextImage2KHR(device, info, index);
    return acquire(sc, info->timeout, info->semaphore, info->fence, index);
}

static VKAPI_ATTR VkResult VKAPI_CALL wsi_GetSwapchainStatusKHR(VkDevice device, VkSwapchainKHR swapchain)
{
    struct swapchain *sc = OUR_SWAPCHAIN(swapchain);
    if (!sc)
        return DEV(device)->GetSwapchainStatusKHR(device, swapchain);
    return swapchain_status(sc);
}

static void set_rects(struct swapchain *sc, struct image *im, const VkPresentRegionKHR *region)
{
    im->nrects = 0;
    if (region && region->rectangleCount && region->pRectangles) {
        for (uint32_t i = 0; i < region->rectangleCount; i++) {
            const VkRectLayerKHR *r = &region->pRectangles[i];
            int64_t x0 = r->offset.x, y0 = r->offset.y;
            int64_t x1 = x0 + r->extent.width, y1 = y0 + r->extent.height;
            if (x0 < 0) x0 = 0;
            if (y0 < 0) y0 = 0;
            if (x1 > sc->extent.width) x1 = sc->extent.width;
            if (y1 > sc->extent.height) y1 = sc->extent.height;
            if (x1 <= x0 || y1 <= y0)
                continue;
            if (im->nrects == MAX_RECTS) { /* too many: fall back to the bounding box */
                uint32_t bx0 = UINT32_MAX, by0 = UINT32_MAX, bx1 = 0, by1 = 0;
                for (uint32_t k = 0; k < im->nrects; k++) {
                    struct rect *q = &im->rects[k];
                    if (q->x < bx0) bx0 = q->x;
                    if (q->y < by0) by0 = q->y;
                    if (q->x + q->w > bx1) bx1 = q->x + q->w;
                    if (q->y + q->h > by1) by1 = q->y + q->h;
                }
                if ((uint32_t)x0 < bx0) bx0 = (uint32_t)x0;
                if ((uint32_t)y0 < by0) by0 = (uint32_t)y0;
                if ((uint32_t)x1 > bx1) bx1 = (uint32_t)x1;
                if ((uint32_t)y1 > by1) by1 = (uint32_t)y1;
                im->rects[0] = (struct rect){ bx0, by0, bx1 - bx0, by1 - by0 };
                im->nrects = 1;
                continue;
            }
            im->rects[im->nrects++] = (struct rect){ (uint32_t)x0, (uint32_t)y0, (uint32_t)(x1 - x0), (uint32_t)(y1 - y0) };
        }
        if (im->nrects)
            return;
    }
    im->rects[0] = (struct rect){ 0, 0, sc->extent.width, sc->extent.height };
    im->nrects = 1;
}

static VKAPI_ATTR VkResult VKAPI_CALL wsi_QueuePresentKHR(VkQueue queue, const VkPresentInfoKHR *pi)
{
    struct dev *d = DEV(queue);
    const VkPresentRegionsKHR *regions = NULL;
    for (const VkBaseInStructure *p = pi->pNext; p; p = p->pNext)
        if (p->sType == VK_STRUCTURE_TYPE_PRESENT_REGIONS_KHR)
            regions = (const VkPresentRegionsKHR *)p;

    VkPipelineStageFlags stages[16];
    for (uint32_t i = 0; i < 16; i++)
        stages[i] = VK_PIPELINE_STAGE_TRANSFER_BIT;
    if (pi->waitSemaphoreCount > 16)
        return VK_ERROR_OUT_OF_HOST_MEMORY;

    VkResult final = VK_SUCCESS;
    int waited = 0;
    for (uint32_t i = 0; i < pi->swapchainCount; i++) {
        struct swapchain *sc = OUR_SWAPCHAIN(pi->pSwapchains[i]);
        VkResult res;
        if (!sc) {
            VkPresentInfoKHR one = *pi;
            one.pNext = NULL;
            one.swapchainCount = 1;
            one.pSwapchains = &pi->pSwapchains[i];
            one.pImageIndices = &pi->pImageIndices[i];
            one.pResults = NULL;
            one.waitSemaphoreCount = waited ? 0 : pi->waitSemaphoreCount;
            waited = 1;
            res = d->QueuePresentKHR(queue, &one);
        } else {
            uint32_t idx = pi->pImageIndices[i];
            struct image *im = &sc->img[idx];
            set_rects(sc, im, regions && i < regions->swapchainCount ? &regions->pRegions[i] : NULL);

            VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL,
                                            VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, NULL };
            VkImageMemoryBarrier to_src = {
                VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL, 0, VK_ACCESS_TRANSFER_READ_BIT,
                VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, im->image,
                { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
            VkImageMemoryBarrier to_present = to_src;
            to_present.srcAccessMask = 0;
            to_present.dstAccessMask = 0;
            to_present.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            to_present.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
            VkBufferMemoryBarrier to_host = { VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER, NULL,
                                              VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT,
                                              VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED,
                                              im->buffer, 0, VK_WHOLE_SIZE };
            VkBufferImageCopy copies[MAX_RECTS];
            for (uint32_t k = 0; k < im->nrects; k++) {
                const struct rect *r = &im->rects[k];
                copies[k] = (VkBufferImageCopy){
                    .bufferOffset = (VkDeviceSize)r->y * sc->stride + (VkDeviceSize)r->x * 4,
                    .bufferRowLength = sc->extent.width,
                    .bufferImageHeight = sc->extent.height,
                    .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
                    .imageOffset = { (int32_t)r->x, (int32_t)r->y, 0 },
                    .imageExtent = { r->w, r->h, 1 } };
            }
            d->ResetCommandBuffer(im->cmd, 0);
            d->BeginCommandBuffer(im->cmd, &bi);
            d->CmdPipelineBarrier(im->cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                  0, 0, NULL, 0, NULL, 1, &to_src);
            d->CmdCopyImageToBuffer(im->cmd, im->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, im->buffer,
                                    im->nrects, copies);
            d->CmdPipelineBarrier(im->cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                  VK_PIPELINE_STAGE_HOST_BIT | VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0,
                                  NULL, 1, &to_host, 1, &to_present);
            d->EndCommandBuffer(im->cmd);

            VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                                .waitSemaphoreCount = waited ? 0 : pi->waitSemaphoreCount,
                                .pWaitSemaphores = pi->pWaitSemaphores,
                                .pWaitDstStageMask = stages,
                                .commandBufferCount = 1,
                                .pCommandBuffers = &im->cmd };
            waited = 1;
            uint64_t ts = now_ns();
            res = d->QueueSubmit(queue, 1, &si, im->fence);
            im->presented_at = now_ns();
            sc->submit_ns += im->presented_at - ts;
            if (res == VK_SUCCESS) {
                pthread_mutex_lock(&sc->lock);
                im->state = IMG_PRESENTING;
                sc->fifo[(sc->fifo_head + sc->fifo_len) % MAX_IMAGES] = idx;
                sc->fifo_len++;
                pthread_cond_broadcast(&sc->cond);
                pthread_mutex_unlock(&sc->lock);
                res = swapchain_status(sc);
            }
        }
        if (pi->pResults)
            pi->pResults[i] = res;
        if (final == VK_SUCCESS || (res < 0 && final >= 0))
            final = res;
    }
    return final;
}

/* ------------------------------------------------------------ proc addr -- */

EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL brick_GetDeviceProcAddr(VkDevice device, const char *name);

#define HOOK(fn, impl) if (!strcmp(name, fn)) return (PFN_vkVoidFunction)(impl)

static PFN_vkVoidFunction device_hook(const char *name)
{
    HOOK("vkGetDeviceProcAddr", brick_GetDeviceProcAddr);
    HOOK("vkDestroyDevice", wsi_DestroyDevice);
    HOOK("vkCreateSwapchainKHR", wsi_CreateSwapchainKHR);
    HOOK("vkDestroySwapchainKHR", wsi_DestroySwapchainKHR);
    HOOK("vkGetSwapchainImagesKHR", wsi_GetSwapchainImagesKHR);
    HOOK("vkAcquireNextImageKHR", wsi_AcquireNextImageKHR);
    HOOK("vkAcquireNextImage2KHR", wsi_AcquireNextImage2KHR);
    HOOK("vkQueuePresentKHR", wsi_QueuePresentKHR);
    HOOK("vkGetSwapchainStatusKHR", wsi_GetSwapchainStatusKHR);
    return NULL;
}

EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL brick_GetDeviceProcAddr(VkDevice device, const char *name)
{
    PFN_vkVoidFunction f = device_hook(name);
    if (f)
        return f;
    struct dev *d = DEV(device);
    return d ? d->gdpa(device, name) : NULL;
}

EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL brick_GetInstanceProcAddr(VkInstance instance, const char *name)
{
    static int init;
    if (!init) {
        const char *e = getenv("BRICK_WSI_DEBUG");
        g_debug = e && *e == '1';
        init = 1;
    }
    HOOK("vkGetInstanceProcAddr", brick_GetInstanceProcAddr);
    HOOK("vkCreateInstance", wsi_CreateInstance);
    HOOK("vkDestroyInstance", wsi_DestroyInstance);
    HOOK("vkCreateDevice", wsi_CreateDevice);
    HOOK("vkEnumerateDeviceExtensionProperties", wsi_EnumerateDeviceExtensionProperties);
    HOOK("vkGetPhysicalDeviceQueueFamilyProperties", wsi_GetPhysicalDeviceQueueFamilyProperties);
    if (!strcmp(name, "vkGetPhysicalDeviceQueueFamilyProperties2") ||
        !strcmp(name, "vkGetPhysicalDeviceQueueFamilyProperties2KHR")) {
        struct inst *in = instance ? INST(instance) : NULL;
        return in && in->GetPhysicalDeviceQueueFamilyProperties2
                   ? (PFN_vkVoidFunction)wsi_GetPhysicalDeviceQueueFamilyProperties2 : NULL;
    }
    HOOK("vkCreateXcbSurfaceKHR", wsi_CreateXcbSurfaceKHR);
    HOOK("vkCreateXlibSurfaceKHR", wsi_CreateXlibSurfaceKHR);
    HOOK("vkDestroySurfaceKHR", wsi_DestroySurfaceKHR);
    HOOK("vkGetPhysicalDeviceSurfaceSupportKHR", wsi_GetPhysicalDeviceSurfaceSupportKHR);
    HOOK("vkGetPhysicalDeviceSurfaceCapabilitiesKHR", wsi_GetPhysicalDeviceSurfaceCapabilitiesKHR);
    HOOK("vkGetPhysicalDeviceSurfaceCapabilities2KHR", wsi_GetPhysicalDeviceSurfaceCapabilities2KHR);
    HOOK("vkGetPhysicalDeviceSurfaceFormatsKHR", wsi_GetPhysicalDeviceSurfaceFormatsKHR);
    HOOK("vkGetPhysicalDeviceSurfaceFormats2KHR", wsi_GetPhysicalDeviceSurfaceFormats2KHR);
    HOOK("vkGetPhysicalDeviceSurfacePresentModesKHR", wsi_GetPhysicalDeviceSurfacePresentModesKHR);
    HOOK("vkGetPhysicalDevicePresentRectanglesKHR", wsi_GetPhysicalDevicePresentRectanglesKHR);
    HOOK("vkGetPhysicalDeviceXcbPresentationSupportKHR", wsi_GetPhysicalDeviceXcbPresentationSupportKHR);
    HOOK("vkGetPhysicalDeviceXlibPresentationSupportKHR", wsi_GetPhysicalDeviceXlibPresentationSupportKHR);
    PFN_vkVoidFunction f = device_hook(name);
    if (f)
        return f;
    if (!instance)
        return NULL;
    struct inst *in = INST(instance);
    return in ? in->gipa(instance, name) : NULL;
}
