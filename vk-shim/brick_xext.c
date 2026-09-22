/*
 * Xorg module "brickext": registers an empty X extension named ATIFGLRXDRI.
 *
 * Chrome only renders into a Vulkan swapchain on X11 when the server has one
 * of DRI3, NV-CONTROL or ATIFGLRXDRI (ui::IsVulkanSurfaceSupported); otherwise
 * it reads every frame back and pushes it through the X socket.  The fbdev
 * server here has none of them, but VK_LAYER_BRICK_x11_wsi provides the
 * swapchain, so all Chrome needs is to find the name.  ATIFGLRXDRI belonged to
 * AMD's long-dead fglrx driver and nothing else in Chrome looks at it, so
 * claiming it switches on exactly that one code path.  Every request to the
 * extension fails with BadRequest.
 *
 * Built without the Xorg SDK: the few ABI structures are declared here, and
 * AddExtension / LoadExtensionList / StandardMinorOpcode resolve against the
 * server binary when the module loads.
 */
#include <stdint.h>

#define EXPORT __attribute__((visibility("default")))

typedef struct _Client *ClientPtr;
typedef struct _ExtensionEntry ExtensionEntry;

extern ExtensionEntry *AddExtension(const char *name, int num_events, int num_errors,
                                    int (*main_proc)(ClientPtr), int (*swapped_main_proc)(ClientPtr),
                                    void (*close_down_proc)(ExtensionEntry *),
                                    unsigned short (*minor_opcode_proc)(ClientPtr));
extern unsigned short StandardMinorOpcode(ClientPtr client);

typedef struct {
    void (*init)(void);
    const char *name;
    int *disable;
} ExtensionModule;
extern void LoadExtensionList(const ExtensionModule ext[], int size, int builtin);

typedef struct {
    const char *modname;
    const char *vendor;
    uint32_t modinfo1, modinfo2;
    uint32_t xf86version;
    uint8_t majorversion, minorversion;
    uint16_t patchlevel;
    const char *abiclass;
    uint32_t abiversion;
    const char *moduleclass;
    uint32_t checksum[4];
} XF86ModuleVersionInfo;

typedef struct {
    XF86ModuleVersionInfo *vers;
    void *(*setup)(void *module, void *opts, int *errmaj, int *errmin);
    void (*teardown)(void *module);
} XF86ModuleData;

#define BadRequest 1

static int proc_bad_request(ClientPtr client)
{
    (void)client;
    return BadRequest;
}

static void brickext_init(void)
{
    AddExtension("ATIFGLRXDRI", 0, 0, proc_bad_request, proc_bad_request, 0, StandardMinorOpcode);
}

static const ExtensionModule brickext_list[] = {
    { brickext_init, "ATIFGLRXDRI", 0 },
};

static void *brickext_setup(void *module, void *opts, int *errmaj, int *errmin)
{
    (void)opts; (void)errmaj; (void)errmin;
    LoadExtensionList(brickext_list, 1, 0);
    return module;
}

static XF86ModuleVersionInfo brickext_version = {
    "brickext", "brick-chrome", 0xef23fdc5, 0x10dc023a,
    (21u * 10000000u) + (1u * 100000u) + (4u * 1000u), /* built for Xorg 21.1.4 */
    1, 0, 0,
    "X.Org Server Extension", 10u << 16, /* extension ABI 10.0 */
    "X.Org Server Extension", { 0, 0, 0, 0 },
};

EXPORT XF86ModuleData brickextModuleData = { &brickext_version, brickext_setup, 0 };
