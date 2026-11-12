#ifndef VULKAN_WSI_X11_H
#define VULKAN_WSI_X11_H

/* Env vars (Debug & Override):
     VULKAN_DEBUG=1      enables verbose informational logging
     VULKAN_CPU=1        forces CPU copy mode
     VULKAN_SYNC=1       synchronous zero-copy (no presentation thread)
     VULKAN_NOVSYNC=1    ignores requested present mode: always presents ASYNC (like IMMEDIATE)
     VULKAN_DMABUF=1     zero-copy using "system" dma-buf heap (libdmabufheap.so), without AHB
     VULKAN_DMABUF=name  same, using another heap (e.g. system-uncached)                     */

#ifndef VK_USE_PLATFORM_XCB_KHR
#define VK_USE_PLATFORM_XCB_KHR
#endif

#ifndef VK_USE_PLATFORM_ANDROID_KHR
#define VK_USE_PLATFORM_ANDROID_KHR
#endif

#ifdef WSI_XLIB
#define VK_USE_PLATFORM_XLIB_KHR
#include <X11/Xlib.h>
#include <X11/Xlib-xcb.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <xcb/xcb.h>
#include <xcb/xcbext.h>
#include <xcb/dri3.h>
#include <xcb/present.h>
#include <android/hardware_buffer.h>
#include <unistd.h>
#include <pthread.h>
#include <errno.h>
#include <time.h>
#include <poll.h>
#include <sys/eventfd.h>
#include <dirent.h>
#include <strings.h>
#include <vulkan/vk_layer.h>

#include "mali_loader.h"

#ifndef shim_dlclose
#define shim_dlclose dlclose
#endif

#define WSI_MAX_IMG 8
#define WSI_MAX_LY  8

#ifndef AHARDWAREBUFFER_FORMAT_B8G8R8A8_UNORM
#define AHARDWAREBUFFER_FORMAT_B8G8R8A8_UNORM 5
#endif

#ifndef AHARDWAREBUFFER_USAGE_VENDOR_3
#define AHARDWAREBUFFER_USAGE_VENDOR_3 (1ULL << 51)
#endif

static const char* const wsi_dev_exts[] = {
    "VK_ANDROID_external_memory_android_hardware_buffer", "VK_KHR_external_memory",
    "VK_KHR_dedicated_allocation", "VK_KHR_get_memory_requirements2",
    "VK_KHR_sampler_ycbcr_conversion", "VK_KHR_maintenance1", "VK_KHR_bind_memory2",
    "VK_EXT_queue_family_foreign", "VK_KHR_image_format_list",
    "VK_KHR_external_semaphore", "VK_KHR_external_semaphore_fd",
    "VK_KHR_external_memory_fd", "VK_EXT_external_memory_dma_buf" };
#define WSI_N_DEV_EXTS (sizeof wsi_dev_exts / sizeof *wsi_dev_exts)

/* ---------- Helper for conditional debugging (Cached) ---------- */
static inline int wsi_dbg_enabled(void) {
    static int v = -1;
    if (v < 0) v = (getenv("VULKAN_DEBUG") != NULL || getenv("VULKAN_LAYER_DEBUG") != NULL) ? 1 : 0;
    return v;
}

/* ---------- Global State (Single Context) ---------- */
static mali_ctx*                        g_c;
static PFN_vkGetInstanceProcAddr        g_real_gipa;
static PFN_vkGetDeviceProcAddr          g_real_gdpa;
static VkPhysicalDeviceMemoryProperties g_mp;
static int                              g_zc_ok;

xcb_special_event_t *xcb_register_for_special_event(xcb_connection_t *c,
                                                    uint8_t extension_major_opcode,
                                                    uint32_t stamp,
                                                    uint32_t *gcontext);

typedef struct { int version, numFds, numInts; int data[]; } wsi_nh_t;
static struct {
    int  (*alloc)(const AHardwareBuffer_Desc*, AHardwareBuffer**);
    void (*release)(AHardwareBuffer*);
    void (*describe)(const AHardwareBuffer*, AHardwareBuffer_Desc*);
    const wsi_nh_t* (*nh)(const AHardwareBuffer*);
} g_ahb;

static struct {
    PFN_vkCreateImage CreateImage;
    PFN_vkDestroyImage DestroyImage;
    PFN_vkGetImageMemoryRequirements GetImageMemoryRequirements;
    PFN_vkGetImageSubresourceLayout GetImageSubresourceLayout;
    PFN_vkAllocateMemory AllocateMemory;
    PFN_vkFreeMemory FreeMemory;
    PFN_vkBindImageMemory BindImageMemory;
    PFN_vkCreateBuffer CreateBuffer;
    PFN_vkDestroyBuffer DestroyBuffer;
    PFN_vkGetBufferMemoryRequirements GetBufferMemoryRequirements;
    PFN_vkBindBufferMemory BindBufferMemory;
    PFN_vkMapMemory MapMemory;
    PFN_vkUnmapMemory UnmapMemory;
    PFN_vkInvalidateMappedMemoryRanges InvalidateMappedMemoryRanges;
    PFN_vkCreateCommandPool CreateCommandPool;
    PFN_vkDestroyCommandPool DestroyCommandPool;
    PFN_vkAllocateCommandBuffers AllocateCommandBuffers;
    PFN_vkBeginCommandBuffer BeginCommandBuffer;
    PFN_vkEndCommandBuffer EndCommandBuffer;
    PFN_vkCmdPipelineBarrier CmdPipelineBarrier;
    PFN_vkCmdCopyImageToBuffer CmdCopyImageToBuffer;
    PFN_vkCreateFence CreateFence;
    PFN_vkDestroyFence DestroyFence;
    PFN_vkResetFences ResetFences;
    PFN_vkWaitForFences WaitForFences;
    PFN_vkCreateSemaphore CreateSemaphore;
    PFN_vkDestroySemaphore DestroySemaphore;
    PFN_vkGetSemaphoreFdKHR GetSemaphoreFdKHR;
    PFN_vkGetMemoryFdPropertiesKHR GetMemoryFdPropertiesKHR;
    PFN_vkQueueSubmit QueueSubmit;
    PFN_vkDeviceWaitIdle DeviceWaitIdle;
    PFN_vkGetAndroidHardwareBufferPropertiesANDROID GetAndroidHardwareBufferPropertiesANDROID;
} w;

/* dma-buf heap & ION fallback (libdmabufheap.so), activated with VULKAN_DMABUF */
static struct {
    void*       (*create)(void);
    int         (*alloc)(void*, const char*, size_t, unsigned, size_t);
    int         (*alloc_sys)(void*, int, size_t, unsigned, size_t);
    bool        (*check_ion)(void*);
    int         (*map_ion)(void*, const char*, const char*, unsigned, unsigned, unsigned);
    void*       a;      /* BufferAllocator* */
    const char* heap;
    int         on;
} g_dh;

/* ---------- Objects ---------- */
#define WSI_SURF_MAGIC 0x57534958u
typedef struct {
    uint32_t          magic;
    xcb_connection_t* conn;
    xcb_window_t      win;
    xcb_gcontext_t    gc;
} wsi_surface;

static wsi_surface* wsi_surf(VkSurfaceKHR h) {
    wsi_surface* s = (wsi_surface*)(uintptr_t)h;
    if (!s || s->magic != WSI_SURF_MAGIC) {
        fprintf(stderr, "vulkan_error:: unknown surface (not created by us)\n");
        return NULL;
    }
    return s;
}

typedef struct {
    VkDevice        dev;
    wsi_surface*    surf;
    VkFormat        fmt;
    VkExtent2D      ext;
    uint32_t        n, next;
    int             zc, coherent;
    VkImage         img[WSI_MAX_IMG];
    VkDeviceMemory  imem[WSI_MAX_IMG];
    /* Zero-copy mode */
    AHardwareBuffer* ahb[WSI_MAX_IMG];
    xcb_pixmap_t    pm[WSI_MAX_IMG];
    /* CPU copy mode */
    VkBuffer        buf[WSI_MAX_IMG];
    VkDeviceMemory  bmem[WSI_MAX_IMG];
    uint8_t*        map[WSI_MAX_IMG];
    VkCommandBuffer cmd[WSI_MAX_IMG];
    VkCommandPool   pool;
    VkFence         fence;
    /* Asynchronous presentation */
    int             async, quit;
    pthread_t       th;
    pthread_mutex_t mu;
    pthread_cond_t  cv;
    int             state[WSI_MAX_IMG]; /* 0: free, 1: acquire, 2: queue, 3: present pending */
    uint32_t        q[WSI_MAX_IMG], qh, qn;
    VkSemaphore     isemaphore[WSI_MAX_IMG];
    int             sfd[WSI_MAX_IMG];
    /* Mode, Resize & Eventfd Handling */
    VkPresentModeKHR mode;
    int             wfd;            /* eventfd: wakes up worker from poll */
    VkExtent2D      base;           /* window extent upon creation */
    int             stale;          /* window size has changed */
    int             gck_pending;
    xcb_get_geometry_cookie_t gck;
} wsi_swapchain;

#define WSI_H2P(h) ((void*)(uintptr_t)(h))

static uint32_t wsi_find_mem(uint32_t bits, VkMemoryPropertyFlags want) {
    for (uint32_t i = 0; i < g_mp.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (g_mp.memoryTypes[i].propertyFlags & want) == want)
            return i;
    return UINT32_MAX;
}

/* Forward declarations */
static PFN_vkVoidFunction VKAPI_CALL wsi_core_gipa(VkInstance i, const char* n);
static PFN_vkVoidFunction VKAPI_CALL wsi_core_gdpa(VkDevice d, const char* n);

/* Lazy loader for device ProcAddr */
static PFN_vkGetDeviceProcAddr wsi_real_gdpa_get(void) {
    if (!g_real_gdpa && g_c && g_c->inst && g_real_gipa)
        g_real_gdpa = (PFN_vkGetDeviceProcAddr)g_real_gipa(g_c->inst, "vkGetDeviceProcAddr");
    return g_real_gdpa;
}

/* ---------- Feature Clamping System ---------- */
static const char* const wsi_feat_names[] = {
    "robustBufferAccess","fullDrawIndexUint32","imageCubeArray","independentBlend",
    "geometryShader","tessellationShader","sampleRateShading","dualSrcBlend","logicOp",
    "multiDrawIndirect","drawIndirectFirstInstance","depthClamp","depthBiasClamp",
    "fillModeNonSolid","depthBounds","wideLines","largePoints","alphaToOne","multiViewport",
    "samplerAnisotropy","textureCompressionETC2","textureCompressionASTC_LDR",
    "textureCompressionBC","occlusionQueryPrecise","pipelineStatisticsQuery",
    "vertexPipelineStoresAndAtomics","fragmentStoresAndAtomics",
    "shaderTessellationAndGeometryPointSize","shaderImageGatherExtended",
    "shaderStorageImageExtendedFormats","shaderStorageImageMultisample",
    "shaderStorageImageReadWithoutFormat","shaderStorageImageWriteWithoutFormat",
    "shaderUniformBufferArrayDynamicIndexing","shaderSampledImageArrayDynamicIndexing",
    "shaderStorageBufferArrayDynamicIndexing","shaderStorageImageArrayDynamicIndexing",
    "shaderClipDistance","shaderCullDistance","shaderFloat64","shaderInt64","shaderInt16",
    "shaderResourceResidency","shaderResourceMinLod","sparseBinding","sparseResidencyBuffer",
    "sparseResidencyImage2D","sparseResidencyImage3D","sparseResidency2Samples",
    "sparseResidency4Samples","sparseResidency8Samples","sparseResidency16Samples",
    "sparseResidencyAliased","variableMultisampleRate","inheritedQueries" };
#define WSI_N_FEATS (sizeof(VkPhysicalDeviceFeatures) / sizeof(VkBool32))

static void wsi_hw_features(VkPhysicalDevice pd, VkPhysicalDeviceFeatures* out) {
    memset(out, 0, sizeof *out);
    PFN_vkGetPhysicalDeviceFeatures f = (PFN_vkGetPhysicalDeviceFeatures)
        g_real_gipa(g_c->inst, "vkGetPhysicalDeviceFeatures");
    if (f) f(pd, out);
}

static int wsi_clamp_features(VkPhysicalDeviceFeatures* req, const VkPhysicalDeviceFeatures* hw) {
    VkBool32* r = (VkBool32*)req;
    const VkBool32* h = (const VkBool32*)hw;
    int removed = 0;
    for (size_t i = 0; i < WSI_N_FEATS; i++) {
        if (r[i] && !h[i]) {
            r[i] = VK_FALSE;
            removed++;
            if (wsi_dbg_enabled())
                fprintf(stderr, "vulkan_debug: feature '%s' requested but not supported, disabled\n",
                        i < sizeof wsi_feat_names / sizeof *wsi_feat_names ? wsi_feat_names[i] : "?");
        }
    }
    return removed;
}

/* =====================  WSI LAYER CHAIN  ===================== */
#ifndef WSI_DEFAULT_LAYER_DIR
#define WSI_DEFAULT_LAYER_DIR "/data/data/com.termux/files/home/Mali_Termux-WSI/Layers"
#endif

static struct {
    void* h; char name[VK_MAX_EXTENSION_NAME_SIZE];
    PFN_vkGetInstanceProcAddr gipa; PFN_vkGetDeviceProcAddr gdpa;
} g_ly[WSI_MAX_LY];
static int g_nly;
static VkInstance g_ly_inst;

static VkResult VKAPI_CALL wsi_set_inst_data(VkInstance i, void* o) { (void)i; (void)o; return VK_SUCCESS; }
static VkResult VKAPI_CALL wsi_set_dev_data(VkDevice d, void* o)    { (void)d; (void)o; return VK_SUCCESS; }

typedef VkResult (VKAPI_PTR *PFN_wsiNegotiate)(VkNegotiateLayerInterface*);

static int wsi_ly_try(const char* dir, const char* want) {
    if (g_nly >= WSI_MAX_LY) return 0;
    for (int i = 0; i < g_nly; i++) if (!strcmp(g_ly[i].name, want)) return 1;

    int dbg = wsi_dbg_enabled();

    if (dbg) {
        fprintf(stderr, "vulkan_debug: searching for layers in '%s'\n", dir);
    }

    DIR* d = opendir(dir);
    if (!d) {
        if (dbg) {
            fprintf(stderr, "vulkan_debug: failed to open layer directory '%s': %s\n", dir, strerror(errno));
        }
        return 0;
    }

    struct dirent* e;
    while ((e = readdir(d))) {
        size_t n = strlen(e->d_name);
        if (n <= 3 || strcmp(e->d_name + n - 3, ".so")) continue;
        if (strncmp(e->d_name, "libVkLayer", 10) && strncmp(e->d_name, "libvk", 5)) continue;

        char path[1024];
        snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
        void* h = shim_dlopen(path, RTLD_NOW | RTLD_LOCAL);
        if (!h) {
            if (dbg) {
                fprintf(stderr, "vulkan_debug: failed to load layer library '%s': %s\n", path, shim_dlerror());
            }
            continue;
        }

        PFN_vkGetInstanceProcAddr gipa = NULL; PFN_vkGetDeviceProcAddr gdpa = NULL;
        PFN_wsiNegotiate neg = (PFN_wsiNegotiate)shim_dlsym(h, "vkNegotiateLoaderLayerInterfaceVersion");
        if (neg) {
            VkNegotiateLayerInterface ni; memset(&ni, 0, sizeof ni);
            ni.sType = LAYER_NEGOTIATE_INTERFACE_STRUCT;
            ni.loaderLayerInterfaceVersion = 2;
            if (neg(&ni) == VK_SUCCESS) { gipa = ni.pfnGetInstanceProcAddr; gdpa = ni.pfnGetDeviceProcAddr; }
        }
        if (!gipa) gipa = (PFN_vkGetInstanceProcAddr)shim_dlsym(h, "vkGetInstanceProcAddr");
        if (!gipa) {
            if (dbg) {
                fprintf(stderr, "vulkan_debug: Failed to locate vkGetInstanceProcAddr in layer %s\n", path);
            }
            shim_dlclose(h);
            continue;
        }

        int ok = 0;
        PFN_vkEnumerateInstanceLayerProperties en =
            (PFN_vkEnumerateInstanceLayerProperties)shim_dlsym(h, "vkEnumerateInstanceLayerProperties");
        uint32_t pc = 0;
        if (en && en(&pc, NULL) == VK_SUCCESS && pc) {
            VkLayerProperties* pr = calloc(pc, sizeof *pr);
            if (pr && en(&pc, pr) == VK_SUCCESS)
                for (uint32_t k = 0; k < pc; k++)
                    if (!strcasecmp(pr[k].layerName, want)) ok = 1;
            free(pr);
        }
        if (!ok) {
            const char* s = strncmp(e->d_name, "lib", 3) ? e->d_name : e->d_name + 3;
            size_t bl = strlen(s) - 3;
            ok = strlen(want) == bl && !strncasecmp(s, want, bl);
        }
        if (!ok) {
            shim_dlclose(h);
            continue;
        }

        g_ly[g_nly].h = h; g_ly[g_nly].gipa = gipa; g_ly[g_nly].gdpa = gdpa;
        snprintf(g_ly[g_nly].name, sizeof g_ly[g_nly].name, "%s", want);
        g_nly++;
        if (dbg) {
            fprintf(stderr, "vulkan_debug: added layer [%d] '%s' from library '%s'\n", g_nly, want, path);
        }
        closedir(d);
        return 1;
    }
    closedir(d);
    if (dbg) {
        fprintf(stderr, "vulkan_debug: Failed to find layer %s in %s\n", want, dir);
    }
    return 0;
}

static void wsi_ly_add(const char* name) {
    const char* paths = getenv("VK_LAYER_PATH");
    
    if (!paths || !*paths) {
        paths = WSI_DEFAULT_LAYER_DIR;
    }

    char* cp = strdup(paths);
    for (char* t = strtok(cp, ":"); t; t = strtok(NULL, ":")) {
        if (wsi_ly_try(t, name)) break;
    }
    free(cp);
}

static void wsi_ly_load(const VkInstanceCreateInfo* ci) {
    g_nly = 0;
    for (uint32_t i = 0; i < ci->enabledLayerCount; i++) wsi_ly_add(ci->ppEnabledLayerNames[i]);
    const char* env = getenv("VK_INSTANCE_LAYERS");
    if (env && *env) {
        char* cp = strdup(env);
        for (char* t = strtok(cp, ":,"); t; t = strtok(NULL, ":,")) {
            while (*t == ' ') t++;
            if (*t) wsi_ly_add(t);
        }
        free(cp);
    }

    if (!g_nly && (ci->enabledLayerCount || (env && *env))) {
        fprintf(stderr, "vulkan_warning: requested layers not found, running without\n");
    }
}

/* =====================  SURFACE  ===================== */

static VkResult VKAPI_CALL wsi_CreateXcbSurfaceKHR(VkInstance i,
        const VkXcbSurfaceCreateInfoKHR* ci, const VkAllocationCallbacks* a, VkSurfaceKHR* out) {
    (void)i; (void)a;
    wsi_surface* s = calloc(1, sizeof *s);
    if (!s) return VK_ERROR_OUT_OF_HOST_MEMORY;
    s->magic = WSI_SURF_MAGIC;
    s->conn = ci->connection;
    s->win  = ci->window;
    s->gc   = xcb_generate_id(s->conn);
    xcb_create_gc(s->conn, s->gc, s->win, 0, NULL);
    xcb_flush(s->conn);
    *out = (VkSurfaceKHR)(uintptr_t)s;
    return VK_SUCCESS;
}

static void VKAPI_CALL wsi_DestroySurfaceKHR(VkInstance i, VkSurfaceKHR surf,
        const VkAllocationCallbacks* a) {
    (void)i; (void)a;
    wsi_surface* s = wsi_surf(surf);
    if (!s) return;
    xcb_free_gc(s->conn, s->gc);
    xcb_flush(s->conn);
    s->magic = 0;
    free(s);
}

static VkBool32 VKAPI_CALL wsi_GetXcbPresentationSupportKHR(VkPhysicalDevice pd,
        uint32_t qf, xcb_connection_t* c, xcb_visualid_t v) {
    (void)pd; (void)qf; (void)c; (void)v;
    return VK_TRUE;
}

static VkResult VKAPI_CALL wsi_GetPDSurfaceSupportKHR(VkPhysicalDevice pd, uint32_t qf,
        VkSurfaceKHR surf, VkBool32* sup) {
    (void)surf;
    PFN_vkGetPhysicalDeviceQueueFamilyProperties qfp = (PFN_vkGetPhysicalDeviceQueueFamilyProperties)
        g_real_gipa(g_c->inst, "vkGetPhysicalDeviceQueueFamilyProperties");
    uint32_t n = 0; qfp(pd, &n, NULL);
    VkQueueFamilyProperties q[16]; if (n > 16) n = 16;
    qfp(pd, &n, q);
    *sup = (qf < n && (q[qf].queueFlags & VK_QUEUE_GRAPHICS_BIT)) ? VK_TRUE : VK_FALSE;
    return VK_SUCCESS;
}

static VkResult VKAPI_CALL wsi_GetPDSurfaceCapabilitiesKHR(VkPhysicalDevice pd,
        VkSurfaceKHR surf, VkSurfaceCapabilitiesKHR* caps) {
    (void)pd;
    wsi_surface* s = wsi_surf(surf);
    if (!s) return VK_ERROR_SURFACE_LOST_KHR;
    xcb_get_geometry_reply_t* g =
        xcb_get_geometry_reply(s->conn, xcb_get_geometry(s->conn, s->win), NULL);
    if (!g) return VK_ERROR_SURFACE_LOST_KHR;
    VkExtent2D e = { g->width, g->height };
    free(g);
    memset(caps, 0, sizeof *caps);
    caps->minImageCount = 3;
    caps->maxImageCount = WSI_MAX_IMG;
    caps->currentExtent = e;
    caps->minImageExtent = (VkExtent2D){ 1, 1 };
    caps->maxImageExtent = (VkExtent2D){ 4096, 4096 };
    caps->maxImageArrayLayers = 1;
    caps->supportedTransforms = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    caps->currentTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    caps->supportedCompositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    caps->supportedUsageFlags = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
        VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
        VK_IMAGE_USAGE_SAMPLED_BIT;
    return VK_SUCCESS;
}

static const VkSurfaceFormatKHR wsi_surf_fmts[] = {
    { VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR },
    { VK_FORMAT_B8G8R8A8_SRGB,  VK_COLOR_SPACE_SRGB_NONLINEAR_KHR },
    { VK_FORMAT_R8G8B8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR },
    { VK_FORMAT_R8G8B8A8_SRGB,  VK_COLOR_SPACE_SRGB_NONLINEAR_KHR },
};
#define WSI_N_SURF_FMTS (sizeof wsi_surf_fmts / sizeof wsi_surf_fmts[0])

static VkResult VKAPI_CALL wsi_GetPDSurfaceFormatsKHR(VkPhysicalDevice pd,
        VkSurfaceKHR surf, uint32_t* n, VkSurfaceFormatKHR* f) {
    (void)pd; (void)surf;
    if (!f) { *n = (uint32_t)WSI_N_SURF_FMTS; return VK_SUCCESS; }
    uint32_t k = *n < WSI_N_SURF_FMTS ? *n : (uint32_t)WSI_N_SURF_FMTS;
    for (uint32_t i = 0; i < k; i++) f[i] = wsi_surf_fmts[i];
    *n = k;
    return k < WSI_N_SURF_FMTS ? VK_INCOMPLETE : VK_SUCCESS;
}

static VkResult VKAPI_CALL wsi_GetPDSurfacePresentModesKHR(VkPhysicalDevice pd,
        VkSurfaceKHR surf, uint32_t* n, VkPresentModeKHR* m) {
    (void)pd; (void)surf;
    static const VkPresentModeKHR modes[3] = {
        VK_PRESENT_MODE_FIFO_KHR, VK_PRESENT_MODE_MAILBOX_KHR, VK_PRESENT_MODE_IMMEDIATE_KHR };
    if (!m) { *n = 3; return VK_SUCCESS; }
    uint32_t k = *n < 3 ? *n : 3;
    for (uint32_t i = 0; i < k; i++) m[i] = modes[i];
    VkResult r = k < 3 ? VK_INCOMPLETE : VK_SUCCESS;
    *n = k;
    return r;
}

/* =====================  SWAPCHAIN  ===================== */

static void wsi_setup(wsi_swapchain* s, VkDevice dev, const VkSwapchainCreateInfoKHR* ci) {
    memset(s, 0, sizeof *s);
    s->dev  = dev;
    s->surf = wsi_surf(ci->surface);
    s->fmt  = ci->imageFormat;
    s->ext  = ci->imageExtent;
    s->n    = ci->minImageCount < 3 ? 3 : ci->minImageCount;
    if (s->n > WSI_MAX_IMG) s->n = WSI_MAX_IMG;

    s->mode = getenv("VULKAN_NOVSYNC") ? VK_PRESENT_MODE_IMMEDIATE_KHR : ci->presentMode;
    s->wfd  = -1;
    s->base = s->ext;
    if (s->surf) {
        xcb_connection_t* cn = s->surf->conn;
        xcb_get_geometry_reply_t* g =
            xcb_get_geometry_reply(cn, xcb_get_geometry(cn, s->surf->win), NULL);
        if (g) { s->base = (VkExtent2D){ g->width, g->height }; free(g); }
    }
}

static void wsi_release(wsi_swapchain* s) {
    VkDevice d = s->dev;
    w.DeviceWaitIdle(d);
    if (s->gck_pending) {
        xcb_discard_reply(s->surf->conn, s->gck.sequence);
        s->gck_pending = 0;
    }
    if (s->fence) w.DestroyFence(d, s->fence, NULL);
    if (s->pool)  w.DestroyCommandPool(d, s->pool, NULL);
    int freed_pm = 0;
    for (uint32_t i = 0; i < s->n; i++) {
        if (s->isemaphore[i]) w.DestroySemaphore(d, s->isemaphore[i], NULL);
        if (s->map[i])  w.UnmapMemory(d, s->bmem[i]);
        if (s->buf[i])  w.DestroyBuffer(d, s->buf[i], NULL);
        if (s->bmem[i]) w.FreeMemory(d, s->bmem[i], NULL);
        if (s->img[i])  w.DestroyImage(d, s->img[i], NULL);
        if (s->imem[i]) w.FreeMemory(d, s->imem[i], NULL);
        if (s->pm[i])   { xcb_free_pixmap(s->surf->conn, s->pm[i]); freed_pm = 1; }
        if (s->ahb[i])  g_ahb.release(s->ahb[i]);
    }
    if (freed_pm) xcb_flush(s->surf->conn);
}

static void wsi_wake(wsi_swapchain* s) {
    if (s->wfd < 0) return;
    uint64_t one = 1;
    if (write(s->wfd, &one, sizeof one) < 0) { }
}

static void wsi_stop_worker(wsi_swapchain* s) {
    if (!s->async) return;
    pthread_mutex_lock(&s->mu);
    s->quit = 1;
    pthread_cond_broadcast(&s->cv);
    pthread_mutex_unlock(&s->mu);
    wsi_wake(s);
    pthread_join(s->th, NULL);
    if (s->wfd >= 0) { close(s->wfd); s->wfd = -1; }
    pthread_mutex_destroy(&s->mu);
    pthread_cond_destroy(&s->cv);
    s->async = 0;
}

static void wsi_free_swapchain(wsi_swapchain* s) {
    wsi_stop_worker(s);
    wsi_release(s);
    free(s);
}

static VkResult wsi_wait_fd(int fd) {
    struct pollfd p = { fd, POLLIN, 0 };
    int pr;
    do { pr = poll(&p, 1, 5000); } while (pr < 0 && errno == EINTR);
    return pr > 0 ? VK_SUCCESS : VK_TIMEOUT;
}

#define WSI_COMP_TIMEOUT_NS 250000000ull

typedef struct {
    xcb_special_event_t* se;
    uint32_t eid;
    int      use_comp;
    int      inflight;
    uint64_t last_msc, t_pres;
} wsi_comp;

static uint64_t wsi_now_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

static void wsi_comp_close(wsi_swapchain* s, wsi_comp* id) {
    if (!id->se) return;
    xcb_connection_t* cn = s->surf->conn;
    xcb_generic_error_t* err = xcb_request_check(cn,
        xcb_present_select_input_checked(cn, id->eid, s->surf->win, XCB_PRESENT_EVENT_MASK_NO_EVENT));
    free(err);
    xcb_unregister_for_special_event(cn, id->se);
    id->se = NULL;
}

static void wsi_ev_drain(wsi_swapchain* s, wsi_comp* id) {
    xcb_connection_t* cn = s->surf->conn;
    xcb_generic_event_t* e;
    while ((e = xcb_poll_for_special_event(cn, id->se))) {
        if (((xcb_present_generic_event_t*)e)->evtype == XCB_PRESENT_EVENT_COMPLETE_NOTIFY) {
            xcb_present_complete_notify_event_t* c = (xcb_present_complete_notify_event_t*)e;
            if (c->kind == XCB_PRESENT_COMPLETE_KIND_PIXMAP) {
                if (id->inflight > 0) id->inflight--;
                id->last_msc = c->msc;
                pthread_mutex_lock(&s->mu);
                if (c->serial < s->n && s->state[c->serial] == 3) s->state[c->serial] = 0;
                pthread_cond_broadcast(&s->cv);
                pthread_mutex_unlock(&s->mu);
            }
        }
        free(e);
    }
}

static void wsi_ev_service(wsi_swapchain* s, wsi_comp* id, int wait_ms) {
    xcb_connection_t* cn = s->surf->conn;
    if (id->se) {
        int infl = id->inflight;
        wsi_ev_drain(s, id);
        if (id->inflight == infl) {
            struct pollfd p[2] = { { xcb_get_file_descriptor(cn), POLLIN, 0 },
                                   { s->wfd, POLLIN, 0 } };
            int pr;
            do { pr = poll(p, 2, wait_ms); } while (pr < 0 && errno == EINTR);
            if (pr > 0 && (p[1].revents & POLLIN)) {
                uint64_t v;
                if (read(s->wfd, &v, sizeof v) < 0) { }
            }
            if (pr > 0 && (p[0].revents & POLLIN)) {
                free(xcb_get_input_focus_reply(cn, xcb_get_input_focus(cn), NULL));
                wsi_ev_drain(s, id);
            }
        }
    }
    if (id->inflight > 0 && wsi_now_ns() - id->t_pres > WSI_COMP_TIMEOUT_NS) {
        id->inflight = 0;
        id->use_comp = 0;
        if (wsi_dbg_enabled()) {
            fprintf(stderr, "vulkan_debug: no PresentCompleteNotify within 250 ms, vsync gating disabled\n");
        }
        pthread_mutex_lock(&s->mu);
        for (uint32_t i = 0; i < s->n; i++) if (s->state[i] == 3) s->state[i] = 0;
        pthread_cond_broadcast(&s->cv);
        pthread_mutex_unlock(&s->mu);
    }
    if (!id->use_comp) wsi_comp_close(s, id);
}

static void* wsi_worker(void* arg) {
    wsi_swapchain* s = (wsi_swapchain*)arg;
    xcb_connection_t* cn = s->surf->conn;
    int use_present = 1;
    int vsync = s->mode != VK_PRESENT_MODE_IMMEDIATE_KHR;
    wsi_comp id;
    memset(&id, 0, sizeof id);

    xcb_present_query_version_reply_t* pv =
        xcb_present_query_version_reply(cn, xcb_present_query_version(cn, 1, 0), NULL);
    const xcb_query_extension_reply_t* ext = pv ? xcb_get_extension_data(cn, &xcb_present_id) : NULL;
    if (!pv || !ext || !ext->present) {
        if (wsi_dbg_enabled()) {
            fprintf(stderr, "vulkan_debug: Present extension not available, using copy_area (no vsync)\n");
        }
        use_present = 0;
    } else {
        if (vsync) {
            id.eid = xcb_generate_id(cn);
            id.se = xcb_register_for_special_xge(cn, (xcb_extension_t*)&xcb_present_id, id.eid, NULL);
            if (id.se) {
                xcb_generic_error_t* err = xcb_request_check(cn,
                    xcb_present_select_input_checked(cn, id.eid, s->surf->win,
                                                     XCB_PRESENT_EVENT_MASK_COMPLETE_NOTIFY));
                if (err) { free(err); xcb_unregister_for_special_event(cn, id.se); id.se = NULL; }
            }
            id.use_comp = id.se != NULL;
        }
        if (wsi_dbg_enabled()) {
            fprintf(stderr, "vulkan_debug: vsync gating (present mode %d) = %d\n", (int)s->mode, id.use_comp);
        }
    }
    free(pv);

    for (;;) {
        if (s->gck_pending) {
            xcb_get_geometry_reply_t* g = xcb_get_geometry_reply(cn, s->gck, NULL);
            s->gck_pending = 0;
            if (g) {
                if (g->width != s->base.width || g->height != s->base.height)
                    __atomic_store_n(&s->stale, 1, __ATOMIC_RELAXED);
                free(g);
            }
        } else {
            s->gck = xcb_get_geometry(cn, s->surf->win);
            s->gck_pending = 1;
        }

        pthread_mutex_lock(&s->mu);
        for (;;) {
            if (s->quit) break;
            int gated = id.use_comp && id.inflight > 0;
            if (s->qn > 0 && !gated) break;
            if (id.inflight > 0) {
                pthread_mutex_unlock(&s->mu);
                wsi_ev_service(s, &id, 4);
                pthread_mutex_lock(&s->mu);
            } else {
                pthread_cond_wait(&s->cv, &s->mu);
            }
        }
        if (s->quit) { pthread_mutex_unlock(&s->mu); break; }

        uint32_t drop[WSI_MAX_IMG]; int nd = 0;
        if (s->mode == VK_PRESENT_MODE_MAILBOX_KHR)
            while (s->qn > 1) {
                drop[nd++] = s->q[s->qh];
                s->qh = (s->qh + 1) % WSI_MAX_IMG;
                s->qn--;
            }
        uint32_t idx = s->q[s->qh];
        s->qh = (s->qh + 1) % WSI_MAX_IMG;
        s->qn--;
        pthread_mutex_unlock(&s->mu);

        if (nd) {
            for (int k = 0; k < nd; k++) {
                int dfd = s->sfd[drop[k]];
                if (dfd >= 0) { wsi_wait_fd(dfd); close(dfd); }
            }
            pthread_mutex_lock(&s->mu);
            for (int k = 0; k < nd; k++) s->state[drop[k]] = 0;
            pthread_cond_broadcast(&s->cv);
            pthread_mutex_unlock(&s->mu);
        }

        int fd = s->sfd[idx];
        VkResult r = VK_SUCCESS;
        if (fd >= 0) { r = wsi_wait_fd(fd); close(fd); }

        if (!r) {
            if (use_present) {
                uint32_t opt = XCB_PRESENT_OPTION_COPY;
                uint64_t target = 0;
                if (!vsync)           opt |= XCB_PRESENT_OPTION_ASYNC;
                else if (id.use_comp) target = id.last_msc + 1;

                xcb_present_pixmap(cn, s->surf->win, s->pm[idx], (uint32_t)idx,
                                   XCB_NONE, XCB_NONE, 0, 0,
                                   XCB_NONE, XCB_NONE, XCB_NONE,
                                   opt, target, 0, 0, 0, NULL);
                xcb_flush(cn);
            } else {
                xcb_copy_area(cn, s->pm[idx], s->surf->win, s->surf->gc,
                              0, 0, 0, 0, s->ext.width, s->ext.height);
                xcb_flush(cn);
            }
        }

        pthread_mutex_lock(&s->mu);
        s->state[idx] = (!r && use_present && id.use_comp) ? 3 : 0;
        if (!r && use_present && id.use_comp) { id.inflight++; id.t_pres = wsi_now_ns(); }
        pthread_cond_broadcast(&s->cv);
        pthread_mutex_unlock(&s->mu);
    }

    pthread_mutex_lock(&s->mu);
    while (s->qn) {
        int fd = s->sfd[s->q[s->qh]];
        if (fd >= 0) close(fd);
        s->qh = (s->qh + 1) % WSI_MAX_IMG;
        s->qn--;
    }
    pthread_mutex_unlock(&s->mu);

    wsi_comp_close(s, &id);
    return NULL;
}

static void wsi_dh_init(void) {
    const char* e = getenv("VULKAN_DMABUF");
    if (!e || !*e || !strcmp(e, "0") || g_dh.on) return;
    if (!w.GetImageSubresourceLayout) return;

    void* l = shim_dlopen("libdmabufheap.so", RTLD_NOW);
    if (!l) l = shim_dlopen("/system/lib64/libdmabufheap.so", RTLD_NOW);
    if (l) {
        g_dh.create    = (void* (*)(void))shim_dlsym(l, "CreateDmabufHeapBufferAllocator");
        g_dh.alloc     = (int (*)(void*, const char*, size_t, unsigned, size_t))
                             shim_dlsym(l, "DmabufHeapAlloc");
        g_dh.alloc_sys = (int (*)(void*, int, size_t, unsigned, size_t))
                             shim_dlsym(l, "DmabufHeapAllocSystem");
        g_dh.check_ion = (bool (*)(void*))shim_dlsym(l, "CheckIonSupport");
        g_dh.map_ion   = (int (*)(void*, const char*, const char*, unsigned, unsigned, unsigned))
                             shim_dlsym(l, "MapDmabufHeapNameToIonHeap");
    }

    if (g_dh.create && g_dh.alloc) {
        g_dh.a = g_dh.create();
        if (g_dh.a) {
            g_dh.heap = strcmp(e, "1") ? e : "system";
            g_dh.on = 1;

            int has_ion = g_dh.check_ion ? g_dh.check_ion(g_dh.a) : 0;
            if (has_ion && g_dh.map_ion) {
                g_dh.map_ion(g_dh.a, "mtk_mm", "ion_mm_heap", 0, 1u << 10, 0);
                g_dh.map_ion(g_dh.a, "mtk_mm", "ion_mm", 0, 1u << 10, 0);

                g_dh.map_ion(g_dh.a, "system", "ion_system_heap", 0, 0, 0);
                g_dh.map_ion(g_dh.a, "system", "system", 0, 0, 0);
                g_dh.map_ion(g_dh.a, "system", "ion_system", 0, 0, 0);

                g_dh.map_ion(g_dh.a, "system-uncached", "system", 0, 0, 0);
                g_dh.map_ion(g_dh.a, "system-uncached", "ion_system", 0, 0, 0);

                if (!strcmp(g_dh.heap, "ion_mm") || !strcmp(g_dh.heap, "mtk_mm")) {
                    g_dh.heap = "mtk_mm";
                }
            }
        }
    }
}

static int wsi_dh_alloc(size_t len) {
    if (!g_dh.on) return -1;

    int fd = -1;

    fd = g_dh.alloc(g_dh.a, g_dh.heap, len, 0, 0);
    if (fd >= 0) return fd;

    if (strcmp(g_dh.heap, "mtk_mm")) {
        fd = g_dh.alloc(g_dh.a, "mtk_mm", len, 0, 0);
        if (fd >= 0) return fd;
    }

    if (g_dh.alloc_sys) {
        fd = g_dh.alloc_sys(g_dh.a, 0, len, 0, 0);
        if (fd >= 0) return fd;
    }

    return fd;
}

static VkResult wsi_zc_image_dh(wsi_swapchain* s, const VkSwapchainCreateInfoKHR* ci, uint32_t i) {
    VkDevice dev = s->dev;
    xcb_connection_t* cn = s->surf->conn;
    VkResult r = VK_ERROR_INITIALIZATION_FAILED;

    uint32_t pitch = (s->ext.width * 4 + 63u) & ~63u;
    VkSubresourceLayout pl = { .offset = 0, .size = 0, .rowPitch = pitch };
    VkImageDrmFormatModifierExplicitCreateInfoEXT mod = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT,
        .drmFormatModifier = 0,
        .drmFormatModifierPlaneCount = 1, .pPlaneLayouts = &pl };
    VkExternalMemoryImageCreateInfo emi = {
        .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT };
    
    VkFormat vf[] = { 
        VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_B8G8R8A8_SRGB, 
        VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_SRGB 
    };
    VkImageFormatListCreateInfo fl = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO,
        .pNext = &emi, .viewFormatCount = sizeof(vf)/sizeof(vf[0]), .pViewFormats = vf };
    VkImageCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D, .format = s->fmt,
        .extent = { s->ext.width, s->ext.height, 1 }, .mipLevels = 1, .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .usage = ci->imageUsage, .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED };

    int got_mod = 0;
    for (int att = 0; att < 4; att++) {
        int mod_on = att < 2, mut = !(att & 1);
        const void* tail = mut ? (const void*)&fl : (const void*)&emi;
        mod.pNext = tail;
        ici.pNext = mod_on ? (const void*)&mod : tail;
        ici.flags = mut ? VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT : 0;
        ici.tiling = mod_on ? VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT : VK_IMAGE_TILING_LINEAR;
        s->img[i] = VK_NULL_HANDLE;
        r = w.CreateImage(dev, &ici, NULL, &s->img[i]);
        if (!r) { got_mod = mod_on; break; }
        if (wsi_dbg_enabled()) {
            fprintf(stderr, "vulkan_debug: dma-buf CreateImage (%s, mutable=%d) failed (%d)\n",
                    mod_on ? "drm-modifier linear" : "tiling linear", mut, r);
        }
        s->img[i] = VK_NULL_HANDLE;
    }
    if (r) return r;

    VkMemoryRequirements mr;
    w.GetImageMemoryRequirements(dev, s->img[i], &mr);
    VkDeviceSize rowPitch = pitch;
    if (!got_mod) {
        VkImageSubresource sub = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0 };
        VkSubresourceLayout lay;
        w.GetImageSubresourceLayout(dev, s->img[i], &sub, &lay);
        if (lay.offset != 0) return VK_ERROR_FORMAT_NOT_SUPPORTED;
        rowPitch = lay.rowPitch;
    }
    if (rowPitch < (VkDeviceSize)s->ext.width * 4 || rowPitch > 0xffff)
        return VK_ERROR_FORMAT_NOT_SUPPORTED;

    size_t need = (size_t)rowPitch * s->ext.height;
    size_t len = (size_t)mr.size > need ? (size_t)mr.size : need;
    len = (len + 4095) & ~(size_t)4095;
    int fd = wsi_dh_alloc(len);
    if (fd < 0) { fprintf(stderr, "vulkan_error:: dma-buf alloc of %zu bytes failed\n", len); return VK_ERROR_OUT_OF_DEVICE_MEMORY; }
    int vfd = dup(fd);
    if (vfd < 0) { close(fd); return VK_ERROR_OUT_OF_HOST_MEMORY; }

    uint32_t bits = mr.memoryTypeBits;
    if (w.GetMemoryFdPropertiesKHR) {
        VkMemoryFdPropertiesKHR fp = { .sType = VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR };
        if (!w.GetMemoryFdPropertiesKHR(dev, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, vfd, &fp))
            bits &= fp.memoryTypeBits;
    }
    if (!bits) { close(fd); close(vfd); return VK_ERROR_FORMAT_NOT_SUPPORTED; }

    VkImportMemoryFdInfoKHR imp = { .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR,
        .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, .fd = vfd };
    VkMemoryDedicatedAllocateInfo ded = { .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
        .pNext = &imp, .image = s->img[i] };
    VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .pNext = &ded,
        .allocationSize = len, .memoryTypeIndex = (uint32_t)__builtin_ctz(bits) };
    if ((r = w.AllocateMemory(dev, &mai, NULL, &s->imem[i]))) {
        fprintf(stderr, "vulkan_error:: dma-buf import (AllocateMemory) failed (%d)\n", r);
        s->imem[i] = VK_NULL_HANDLE;
        close(fd); close(vfd);
        return r;
    }
    if ((r = w.BindImageMemory(dev, s->img[i], s->imem[i], 0))) { close(fd); return r; }

    xcb_pixmap_t pm = xcb_generate_id(cn);
    xcb_void_cookie_t ck = xcb_dri3_pixmap_from_buffer_checked(cn, pm, s->surf->win,
        (uint32_t)len, s->ext.width, s->ext.height, (uint16_t)rowPitch, 24, 32, fd);
    xcb_generic_error_t* err = xcb_request_check(cn, ck);
    if (err) {
        fprintf(stderr, "vulkan_error:: DRI3 PixmapFromBuffer rejected (X error %d)\n", err->error_code);
        free(err);
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    s->pm[i] = pm;
    return VK_SUCCESS;
}

static VkResult wsi_build_zc(wsi_swapchain* s, const VkSwapchainCreateInfoKHR* ci) {
    if (!g_zc_ok) return VK_ERROR_FEATURE_NOT_PRESENT;
    xcb_connection_t* cn = s->surf->conn;
    xcb_dri3_query_version_reply_t* dv =
        xcb_dri3_query_version_reply(cn, xcb_dri3_query_version(cn, 1, 0), NULL);
    if (!dv) return VK_ERROR_FEATURE_NOT_PRESENT;
    free(dv);

    VkDevice dev = s->dev;
    VkResult r;
    VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    if ((r = w.CreateFence(dev, &fci, NULL, &s->fence))) return r;

        uint64_t ahb_usage = AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE |
                         AHARDWAREBUFFER_USAGE_GPU_COLOR_OUTPUT;
    const char* lm = getenv("VULKAN_AHB_LINEAR");
    if (lm && !strcmp(lm, "vendor3"))
        ahb_usage |= AHARDWAREBUFFER_USAGE_VENDOR_3;
    else if (lm && !strcmp(lm, "none"))
        ;   /* GPU only: gralloc poate alege AFBC */
    else
        ahb_usage |= AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN;

    for (uint32_t i = 0; i < s->n; i++) {
        if (g_dh.on) {
            if ((r = wsi_zc_image_dh(s, ci, i))) return r;
            continue;
        }

        uint32_t ahb_fmt = (s->fmt == VK_FORMAT_B8G8R8A8_UNORM || s->fmt == VK_FORMAT_B8G8R8A8_SRGB) 
                           ? AHARDWAREBUFFER_FORMAT_B8G8R8A8_UNORM 
                           : AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM;

                AHardwareBuffer_Desc d = {
            .width  = s->ext.width,
            .height = s->ext.height,
            .layers = 1,
            .format = ahb_fmt,
            .usage  = ahb_usage
        };
      
        if (g_ahb.alloc(&d, &s->ahb[i]) || !s->ahb[i]) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
        AHardwareBuffer_Desc got; g_ahb.describe(s->ahb[i], &got);
        const wsi_nh_t* nh = g_ahb.nh(s->ahb[i]);
        if (!nh) return VK_ERROR_INITIALIZATION_FAILED;

                for (int k = 0; k < nh->numFds && !s->pm[i]; k++) {
            off_t real = lseek(nh->data[k], 0, SEEK_END);
            lseek(nh->data[k], 0, SEEK_SET);
            if (wsi_dbg_enabled())
                fprintf(stderr, "vulkan_debug: ahb fd[%d] size=%lld linear_need=%llu\n",
                        k, (long long)real,
                        (unsigned long long)got.stride * 4 * got.height);
            if (real <= 0) continue;
            int fd = dup(nh->data[k]);
            if (fd < 0) continue;
            xcb_pixmap_t cand = xcb_generate_id(cn);
            xcb_void_cookie_t ck = xcb_dri3_pixmap_from_buffer_checked(cn, cand, s->surf->win,
                (uint32_t)real, s->ext.width, s->ext.height, (uint16_t)(got.stride * 4), 24, 32, fd);
            xcb_generic_error_t* err = xcb_request_check(cn, ck);
            if (err) { free(err); continue; }
            s->pm[i] = cand;
        }
        if (!s->pm[i]) return VK_ERROR_INITIALIZATION_FAILED;

        VkAndroidHardwareBufferFormatPropertiesANDROID fp = {
            .sType = VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_FORMAT_PROPERTIES_ANDROID };
        VkAndroidHardwareBufferPropertiesANDROID ap = {
            .sType = VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_PROPERTIES_ANDROID, .pNext = &fp };
        if ((r = w.GetAndroidHardwareBufferPropertiesANDROID(dev, s->ahb[i], &ap))) return r;
        if (!ap.memoryTypeBits)
            return VK_ERROR_FORMAT_NOT_SUPPORTED;

        VkExternalMemoryImageCreateInfo emi = {
            .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
            .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID };
        
        VkFormat vf[] = { 
            VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_B8G8R8A8_SRGB, 
            VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_SRGB 
        };
        VkImageFormatListCreateInfo fl = { .sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO,
            .pNext = &emi, .viewFormatCount = sizeof(vf)/sizeof(vf[0]), .pViewFormats = vf };
        VkImageCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .pNext = &fl,
            .flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT,
            .imageType = VK_IMAGE_TYPE_2D, .format = s->fmt,
            .extent = { s->ext.width, s->ext.height, 1 }, .mipLevels = 1, .arrayLayers = 1,
            .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
            .usage = ci->imageUsage, .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED };
        if ((r = w.CreateImage(dev, &ici, NULL, &s->img[i]))) return r;

        VkImportAndroidHardwareBufferInfoANDROID imp = {
            .sType = VK_STRUCTURE_TYPE_IMPORT_ANDROID_HARDWARE_BUFFER_INFO_ANDROID,
            .buffer = s->ahb[i] };
        VkMemoryDedicatedAllocateInfo ded = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
            .pNext = &imp, .image = s->img[i] };
        VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .pNext = &ded,
            .allocationSize = ap.allocationSize,
            .memoryTypeIndex = (uint32_t)__builtin_ctz(ap.memoryTypeBits) };
        if ((r = w.AllocateMemory(dev, &mai, NULL, &s->imem[i]))) return r;
        if ((r = w.BindImageMemory(dev, s->img[i], s->imem[i], 0))) return r;
    }
    s->zc = 1;

    if (!getenv("VULKAN_SYNC") && w.GetSemaphoreFdKHR) {
        VkExportSemaphoreCreateInfo esi = {
            .sType = VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO,
            .handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT };
        VkSemaphoreCreateInfo xsci = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
            .pNext = &esi };
        for (uint32_t i = 0; i < s->n; i++)
            if ((r = w.CreateSemaphore(dev, &xsci, NULL, &s->isemaphore[i]))) return r;
        pthread_mutex_init(&s->mu, NULL);
        pthread_cond_init(&s->cv, NULL);
        s->wfd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
        if (pthread_create(&s->th, NULL, wsi_worker, s)) {
            if (s->wfd >= 0) { close(s->wfd); s->wfd = -1; }
            pthread_mutex_destroy(&s->mu);
            pthread_cond_destroy(&s->cv);
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        s->async = 1;
    }
    return VK_SUCCESS;
}

static VkResult wsi_build_cpu(wsi_swapchain* s, const VkSwapchainCreateInfoKHR* ci) {
    VkDevice dev = s->dev;
    VkResult r;
    VkCommandPoolCreateInfo pci = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .queueFamilyIndex = g_c->qfi };
    if ((r = w.CreateCommandPool(dev, &pci, NULL, &s->pool))) return r;
    VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    if ((r = w.CreateFence(dev, &fci, NULL, &s->fence))) return r;

    VkDeviceSize bsize = (VkDeviceSize)s->ext.width * s->ext.height * 4;
    s->coherent = 1;

    int mut = (ci->flags & VK_SWAPCHAIN_CREATE_MUTABLE_FORMAT_BIT_KHR) != 0;
    VkImageFormatListCreateInfo mfl; int have_mfl = 0;
    for (const VkBaseInStructure* p = (const VkBaseInStructure*)ci->pNext; p; p = p->pNext)
        if (p->sType == VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO) {
            mfl = *(const VkImageFormatListCreateInfo*)p;
            mfl.pNext = NULL;
            have_mfl = 1;
            break;
        }

    for (uint32_t i = 0; i < s->n; i++) {
        VkImageCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
            .pNext = (mut && have_mfl) ? &mfl : NULL,
            .flags = mut ? VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT : 0,
            .imageType = VK_IMAGE_TYPE_2D, .format = s->fmt,
            .extent = { s->ext.width, s->ext.height, 1 }, .mipLevels = 1, .arrayLayers = 1,
            .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
            .usage = ci->imageUsage | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED };
        if ((r = w.CreateImage(dev, &ici, NULL, &s->img[i]))) return r;
        VkMemoryRequirements ir; w.GetImageMemoryRequirements(dev, s->img[i], &ir);
        uint32_t it = wsi_find_mem(ir.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (it == UINT32_MAX) it = wsi_find_mem(ir.memoryTypeBits, 0);
        if (it == UINT32_MAX) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
        VkMemoryAllocateInfo iai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .allocationSize = ir.size, .memoryTypeIndex = it };
        if ((r = w.AllocateMemory(dev, &iai, NULL, &s->imem[i]))) return r;
        if ((r = w.BindImageMemory(dev, s->img[i], s->imem[i], 0))) return r;

        VkBufferCreateInfo bci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .size = bsize, .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE };
        if ((r = w.CreateBuffer(dev, &bci, NULL, &s->buf[i]))) return r;
        VkMemoryRequirements br; w.GetBufferMemoryRequirements(dev, s->buf[i], &br);
        uint32_t bt = wsi_find_mem(br.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (bt == UINT32_MAX) {
            s->coherent = 0;
            bt = wsi_find_mem(br.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
        }
        if (bt == UINT32_MAX) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
        VkMemoryAllocateInfo bai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .allocationSize = br.size, .memoryTypeIndex = bt };
        if ((r = w.AllocateMemory(dev, &bai, NULL, &s->bmem[i]))) return r;
        if ((r = w.BindBufferMemory(dev, s->buf[i], s->bmem[i], 0))) return r;
        void* m = NULL;
        if ((r = w.MapMemory(dev, s->bmem[i], 0, VK_WHOLE_SIZE, 0, &m))) return r;
        s->map[i] = (uint8_t*)m;

        VkCommandBufferAllocateInfo cai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            .commandPool = s->pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
        if ((r = w.AllocateCommandBuffers(dev, &cai, &s->cmd[i]))) return r;
        VkCommandBufferBeginInfo cbi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        if ((r = w.BeginCommandBuffer(s->cmd[i], &cbi))) return r;

        VkImageSubresourceRange rng = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        VkImageMemoryBarrier b1 = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
            .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = s->img[i], .subresourceRange = rng };
        w.CmdPipelineBarrier(s->cmd[i], VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b1);
        VkBufferImageCopy reg = { .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
            .imageExtent = { s->ext.width, s->ext.height, 1 } };
        w.CmdCopyImageToBuffer(s->cmd[i], s->img[i], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            s->buf[i], 1, &reg);
        VkImageMemoryBarrier b2 = b1;
        b2.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT; b2.dstAccessMask = 0;
        b2.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        b2.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        VkBufferMemoryBarrier bb = { .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .buffer = s->buf[i], .offset = 0, .size = VK_WHOLE_SIZE };
        w.CmdPipelineBarrier(s->cmd[i], VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_HOST_BIT | VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
            0, 0, NULL, 1, &bb, 1, &b2);
        if ((r = w.EndCommandBuffer(s->cmd[i]))) return r;
    }
    s->zc = 0;
    return VK_SUCCESS;
}

static VkResult VKAPI_CALL wsi_CreateSwapchainKHR(VkDevice dev,
        const VkSwapchainCreateInfoKHR* ci, const VkAllocationCallbacks* a, VkSwapchainKHR* out) {
    (void)a;
    if (ci->oldSwapchain != VK_NULL_HANDLE) {
        wsi_swapchain* old_s = (wsi_swapchain*)WSI_H2P(ci->oldSwapchain);
        if (old_s) {
            wsi_stop_worker(old_s);
        }
    }

    wsi_swapchain* s = calloc(1, sizeof *s);
    if (!s) return VK_ERROR_OUT_OF_HOST_MEMORY;
    wsi_setup(s, dev, ci);
    if (!s->surf) { free(s); return VK_ERROR_SURFACE_LOST_KHR; }

    int force_cpu = getenv("VULKAN_CPU") != NULL;
    VkResult r = force_cpu ? VK_ERROR_FEATURE_NOT_PRESENT : wsi_build_zc(s, ci);
    if (r) {
        if (!force_cpu && wsi_dbg_enabled()) {
            fprintf(stderr, "vulkan_debug: zero-copy unavailable (%d), falling back to CPU copy\n", r);
        }
        wsi_stop_worker(s);
        wsi_release(s);
        wsi_setup(s, dev, ci);
        r = wsi_build_cpu(s, ci);
    }
    if (r) {
        fprintf(stderr, "vulkan_error:: CreateSwapchain failed (%d)\n", r);
        wsi_free_swapchain(s);
        return r;
    }
    if (wsi_dbg_enabled()) {
        const char* src = g_dh.on ? "dma-buf" : "AHB";
        char mode[64];
        if (s->zc) snprintf(mode, sizeof mode, "ZERO-COPY %s (%s + DRI3)", s->async ? "async" : "sync", src);
        else       snprintf(mode, sizeof mode, "CPU copy");
        fprintf(stderr, "vulkan_debug: mode %s, %u images %ux%u\n", mode, s->n, s->ext.width, s->ext.height);
    }
    *out = (VkSwapchainKHR)(uintptr_t)s;
    return VK_SUCCESS;
}

static void VKAPI_CALL wsi_DestroySwapchainKHR(VkDevice d, VkSwapchainKHR sc,
        const VkAllocationCallbacks* a) {
    (void)d; (void)a;
    wsi_swapchain* s = (wsi_swapchain*)WSI_H2P(sc);
    if (s) wsi_free_swapchain(s);
}

static VkResult VKAPI_CALL wsi_GetSwapchainImagesKHR(VkDevice d, VkSwapchainKHR sc,
        uint32_t* n, VkImage* imgs) {
    (void)d;
    wsi_swapchain* s = (wsi_swapchain*)WSI_H2P(sc);
    if (!imgs) { *n = s->n; return VK_SUCCESS; }
    uint32_t k = *n < s->n ? *n : s->n;
    for (uint32_t i = 0; i < k; i++) imgs[i] = s->img[i];
    VkResult r = k < s->n ? VK_INCOMPLETE : VK_SUCCESS;
    *n = k;
    return r;
}

static VkResult VKAPI_CALL wsi_AcquireNextImageKHR(VkDevice d, VkSwapchainKHR sc,
        uint64_t timeout, VkSemaphore sem, VkFence fence, uint32_t* idx) {
    (void)d;
    wsi_swapchain* s = (wsi_swapchain*)WSI_H2P(sc);
    if (__atomic_load_n(&s->stale, __ATOMIC_RELAXED)) return VK_ERROR_OUT_OF_DATE_KHR;

    if (s->async) {
        pthread_mutex_lock(&s->mu);
        uint32_t k = s->next;
        if (s->state[k] != 0) {
            if (timeout == 0) { pthread_mutex_unlock(&s->mu); return VK_NOT_READY; }
            struct timespec dl = { 0, 0 };
            if (timeout != UINT64_MAX) {
                uint64_t ns = timeout > 60000000000ull ? 60000000000ull : timeout;
                clock_gettime(CLOCK_REALTIME, &dl);
                dl.tv_sec  += (time_t)(ns / 1000000000ull);
                dl.tv_nsec += (long)(ns % 1000000000ull);
                if (dl.tv_nsec >= 1000000000L) { dl.tv_sec++; dl.tv_nsec -= 1000000000L; }
            }
            while (s->state[k] != 0) {
                int e = timeout == UINT64_MAX ? pthread_cond_wait(&s->cv, &s->mu)
                                              : pthread_cond_timedwait(&s->cv, &s->mu, &dl);
                if (e == ETIMEDOUT) { pthread_mutex_unlock(&s->mu); return VK_TIMEOUT; }
            }
        }
        s->state[k] = 1;
        s->next = (k + 1) % s->n;
        pthread_mutex_unlock(&s->mu);
        *idx = k;
    } else {
        *idx = s->next;
        s->next = (s->next + 1) % s->n;
    }
    if (sem || fence) {
        VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .signalSemaphoreCount = sem ? 1 : 0, .pSignalSemaphores = &sem };
        return w.QueueSubmit(g_c->queue, 1, &si, fence);
    }
    return VK_SUCCESS;
}

static void wsi_put(wsi_swapchain* s, const uint8_t* px) {
    wsi_surface* sf = s->surf;
    uint32_t stride = s->ext.width * 4;
    uint32_t maxb = xcb_get_maximum_request_length(sf->conn) * 4;
    maxb = maxb > 64 ? maxb - 64 : maxb;
    uint32_t rows = maxb / stride;
    if (rows < 1) rows = 1;
    for (uint32_t y = 0; y < s->ext.height; y += rows) {
        uint32_t n = s->ext.height - y < rows ? s->ext.height - y : rows;
        xcb_put_image(sf->conn, XCB_IMAGE_FORMAT_Z_PIXMAP, sf->win, sf->gc,
                      s->ext.width, n, 0, y, 0, 24, n * stride, px + (size_t)y * stride);
    }
    xcb_flush(sf->conn);
}

static void wsi_signal_present_fence(VkQueue q, VkFence f) {
    if (!f) return;
    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO };
    w.QueueSubmit(q, 1, &si, f);
}

static VkResult VKAPI_CALL wsi_QueuePresentKHR(VkQueue q, const VkPresentInfoKHR* pi) {
    VkResult worst = VK_SUCCESS;
    VkPipelineStageFlags stages[16];
    for (int i = 0; i < 16; i++) stages[i] = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    uint32_t nw = pi->waitSemaphoreCount > 16 ? 16 : pi->waitSemaphoreCount;

    const VkSwapchainPresentFenceInfoKHR* pfi = NULL;
    for (const VkBaseInStructure* p = (const VkBaseInStructure*)pi->pNext; p; p = p->pNext)
        if (p->sType == VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_FENCE_INFO_KHR)
            pfi = (const VkSwapchainPresentFenceInfoKHR*)p;
  
    for (uint32_t i = 0; i < pi->swapchainCount; i++) {
        wsi_swapchain* s = (wsi_swapchain*)WSI_H2P(pi->pSwapchains[i]);
        uint32_t idx = pi->pImageIndices[i];
        VkFence pf = (pfi && pfi->pFences) ? pfi->pFences[i] : VK_NULL_HANDLE;

        if (__atomic_load_n(&s->stale, __ATOMIC_RELAXED)) {
            wsi_signal_present_fence(q, pf);
            if (pi->pResults) pi->pResults[i] = VK_SUBOPTIMAL_KHR;
            if (worst == VK_SUCCESS) worst = VK_SUBOPTIMAL_KHR;
            continue;
        }

        VkResult r;
        VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .waitSemaphoreCount = i == 0 ? nw : 0,
            .pWaitSemaphores = pi->pWaitSemaphores, .pWaitDstStageMask = stages,
            .signalSemaphoreCount = s->async ? 1 : 0,
            .pSignalSemaphores = s->async ? &s->isemaphore[idx] : NULL,
            .commandBufferCount = s->zc ? 0 : 1,
            .pCommandBuffers = s->zc ? NULL : &s->cmd[idx] };

        if (s->async) {
            r = w.QueueSubmit(q, 1, &si, VK_NULL_HANDLE);
            s->sfd[idx] = -2;
            if (!r) {
                VkSemaphoreGetFdInfoKHR gi = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR,
                    .semaphore = s->isemaphore[idx],
                    .handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT };
                int fd = -2;
                if (!w.GetSemaphoreFdKHR(s->dev, &gi, &fd)) s->sfd[idx] = fd;
                if (s->sfd[idx] == -2) w.DeviceWaitIdle(s->dev);

                pthread_mutex_lock(&s->mu);
                s->state[idx] = 2;
                s->q[(s->qh + s->qn) % WSI_MAX_IMG] = idx;
                s->qn++;
                pthread_cond_broadcast(&s->cv);
                pthread_mutex_unlock(&s->mu);
                wsi_wake(s);
            } else {
                pthread_mutex_lock(&s->mu);
                s->state[idx] = 0;
                pthread_cond_broadcast(&s->cv);
                pthread_mutex_unlock(&s->mu);
            }
            wsi_signal_present_fence(q, pf);
            if (pi->pResults) pi->pResults[i] = r;
            if (r && (!worst || (r < 0 && worst > 0))) worst = r;
            continue;
        }

        w.ResetFences(s->dev, 1, &s->fence);
        r = w.QueueSubmit(q, 1, &si, s->fence);
        if (!r) r = w.WaitForFences(s->dev, 1, &s->fence, VK_TRUE, 5000000000ull);
        if (!r) {
            if (s->zc) {
                xcb_copy_area(s->surf->conn, s->pm[idx], s->surf->win, s->surf->gc,
                              0, 0, 0, 0, s->ext.width, s->ext.height);
                xcb_flush(s->surf->conn);
            } else {
                if (!s->coherent) {
                    VkMappedMemoryRange mr = { .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
                        .memory = s->bmem[idx], .offset = 0, .size = VK_WHOLE_SIZE };
                    w.InvalidateMappedMemoryRanges(s->dev, 1, &mr);
                }
                wsi_put(s, s->map[idx]);
            }
        }
        wsi_signal_present_fence(q, pf);
        if (pi->pResults) pi->pResults[i] = r;
        if (r && (!worst || (r < 0 && worst > 0))) worst = r;
    }
    return worst;
}

/* =====================  EXTENSIONS + INSTANCE/DEVICE  ===================== */

static int wsi_load(mali_ctx* c);

#ifdef WSI_XLIB
static VkResult VKAPI_CALL wsi_CreateXlibSurfaceKHR(VkInstance i,
        const VkXlibSurfaceCreateInfoKHR* ci, const VkAllocationCallbacks* a, VkSurfaceKHR* out) {
    VkXcbSurfaceCreateInfoKHR x = { .sType = VK_STRUCTURE_TYPE_XCB_SURFACE_CREATE_INFO_KHR,
        .connection = XGetXCBConnection(ci->dpy), .window = (xcb_window_t)ci->window };
    return wsi_CreateXcbSurfaceKHR(i, &x, a, out);
}
static VkBool32 VKAPI_CALL wsi_GetXlibPresentationSupportKHR(VkPhysicalDevice pd,
        uint32_t qf, Display* d, VisualID v) {
    (void)pd; (void)qf; (void)d; (void)v;
    return VK_TRUE;
}
#endif

static VkResult VKAPI_CALL wsi_GetDeviceGroupPresentCapabilitiesKHR(VkDevice d,
        VkDeviceGroupPresentCapabilitiesKHR* c) {
    (void)d;
    memset(c->presentMask, 0, sizeof c->presentMask);
    c->presentMask[0] = 1;
    c->modes = VK_DEVICE_GROUP_PRESENT_MODE_LOCAL_BIT_KHR;
    return VK_SUCCESS;
}

static VkResult VKAPI_CALL wsi_GetDeviceGroupSurfacePresentModesKHR(VkDevice d, VkSurfaceKHR s,
        VkDeviceGroupPresentModeFlagsKHR* modes) {
    (void)d; (void)s;
    *modes = VK_DEVICE_GROUP_PRESENT_MODE_LOCAL_BIT_KHR;
    return VK_SUCCESS;
}

static VkResult VKAPI_CALL wsi_GetPhysicalDevicePresentRectanglesKHR(VkPhysicalDevice pd,
        VkSurfaceKHR s, uint32_t* n, VkRect2D* rects) {
    VkSurfaceCapabilitiesKHR caps;
    VkResult r = wsi_GetPDSurfaceCapabilitiesKHR(pd, s, &caps);
    if (r) return r;
    if (!rects) { *n = 1; return VK_SUCCESS; }
    if (*n < 1) return VK_INCOMPLETE;
    rects[0].offset.x = 0;
    rects[0].offset.y = 0;
    rects[0].extent = caps.currentExtent;
    *n = 1;
    return VK_SUCCESS;
}

static VkResult VKAPI_CALL wsi_GetPDSurfaceCapabilities2KHR(VkPhysicalDevice pd,
        const VkPhysicalDeviceSurfaceInfo2KHR* info, VkSurfaceCapabilities2KHR* caps) {
    VkResult r = wsi_GetPDSurfaceCapabilitiesKHR(pd, info->surface, &caps->surfaceCapabilities);
    if (r) return r;

    VkPresentModeKHR qm = VK_PRESENT_MODE_FIFO_KHR;
    for (const VkBaseInStructure* p = (const VkBaseInStructure*)info->pNext; p; p = p->pNext)
        if (p->sType == VK_STRUCTURE_TYPE_SURFACE_PRESENT_MODE_KHR)
            qm = ((const VkSurfacePresentModeKHR*)p)->presentMode;

    VkResult res = VK_SUCCESS;
    for (VkBaseOutStructure* p = (VkBaseOutStructure*)caps->pNext; p; p = p->pNext) {
        if (p->sType == VK_STRUCTURE_TYPE_SURFACE_PRESENT_SCALING_CAPABILITIES_KHR) {
            VkSurfacePresentScalingCapabilitiesKHR* sc = (VkSurfacePresentScalingCapabilitiesKHR*)p;
            sc->supportedPresentScaling  = VK_PRESENT_SCALING_ONE_TO_ONE_BIT_KHR;
            sc->supportedPresentGravityX = VK_PRESENT_GRAVITY_MIN_BIT_KHR;
            sc->supportedPresentGravityY = VK_PRESENT_GRAVITY_MIN_BIT_KHR;
            sc->minScaledImageExtent = (VkExtent2D){ 1, 1 };
            sc->maxScaledImageExtent = (VkExtent2D){ 4096, 4096 };
        } else if (p->sType == VK_STRUCTURE_TYPE_SURFACE_PRESENT_MODE_COMPATIBILITY_KHR) {
            VkSurfacePresentModeCompatibilityKHR* pc = (VkSurfacePresentModeCompatibilityKHR*)p;
            static const VkPresentModeKHR all[3] = {
                VK_PRESENT_MODE_FIFO_KHR, VK_PRESENT_MODE_MAILBOX_KHR, VK_PRESENT_MODE_IMMEDIATE_KHR };
            VkPresentModeKHR list[3]; uint32_t tot = 0;
            list[tot++] = qm;
            for (int k = 0; k < 3; k++) if (all[k] != qm) list[tot++] = all[k];
            if (!pc->pPresentModes) pc->presentModeCount = tot;
            else {
                uint32_t k = pc->presentModeCount < tot ? pc->presentModeCount : tot;
                for (uint32_t i = 0; i < k; i++) pc->pPresentModes[i] = list[i];
                if (k < tot) res = VK_INCOMPLETE;
                pc->presentModeCount = k;
            }
        }
    }
    return res;
}

static VkResult VKAPI_CALL wsi_GetPDSurfaceFormats2KHR(VkPhysicalDevice pd,
        const VkPhysicalDeviceSurfaceInfo2KHR* info, uint32_t* n, VkSurfaceFormat2KHR* f) {
    if (!f) return wsi_GetPDSurfaceFormatsKHR(pd, info->surface, n, NULL);
    VkSurfaceFormatKHR tmp[WSI_N_SURF_FMTS];
    uint32_t k = *n < WSI_N_SURF_FMTS ? *n : (uint32_t)WSI_N_SURF_FMTS;
    VkResult r = wsi_GetPDSurfaceFormatsKHR(pd, info->surface, &k, tmp);
    for (uint32_t i = 0; i < k; i++) f[i].surfaceFormat = tmp[i];
    *n = k;
    return r;
}

static VkResult VKAPI_CALL wsi_AcquireNextImage2KHR(VkDevice d,
        const VkAcquireNextImageInfoKHR* info, uint32_t* idx) {
    return wsi_AcquireNextImageKHR(d, info->swapchain, info->timeout,
                                   info->semaphore, info->fence, idx);
}

static VkResult VKAPI_CALL wsi_GetPhysicalDeviceDisplayPropertiesKHR(VkPhysicalDevice pd,
        uint32_t* n, VkDisplayPropertiesKHR* p) { (void)pd; (void)p; *n = 0; return VK_SUCCESS; }
static VkResult VKAPI_CALL wsi_GetPhysicalDeviceDisplayPlanePropertiesKHR(VkPhysicalDevice pd,
        uint32_t* n, VkDisplayPlanePropertiesKHR* p) { (void)pd; (void)p; *n = 0; return VK_SUCCESS; }
static VkResult VKAPI_CALL wsi_GetDisplayPlaneSupportedDisplaysKHR(VkPhysicalDevice pd,
        uint32_t plane, uint32_t* n, VkDisplayKHR* d) { (void)pd; (void)plane; (void)d; *n = 0; return VK_SUCCESS; }
static VkResult VKAPI_CALL wsi_GetDisplayModePropertiesKHR(VkPhysicalDevice pd, VkDisplayKHR d,
        uint32_t* n, VkDisplayModePropertiesKHR* p) { (void)pd; (void)d; (void)p; *n = 0; return VK_SUCCESS; }
static VkResult VKAPI_CALL wsi_GetDisplayPlaneCapabilitiesKHR(VkPhysicalDevice pd, VkDisplayModeKHR m,
        uint32_t plane, VkDisplayPlaneCapabilitiesKHR* c) {
    (void)pd; (void)m; (void)plane; (void)c;
    return VK_ERROR_INITIALIZATION_FAILED;
}

static VkResult VKAPI_CALL wsi_CreateDebugReportCallbackEXT(VkInstance i,
        const VkDebugReportCallbackCreateInfoEXT* ci, const VkAllocationCallbacks* a,
        VkDebugReportCallbackEXT* out) {
    (void)i; (void)ci; (void)a;
    *out = (VkDebugReportCallbackEXT)(uintptr_t)1;
    return VK_SUCCESS;
}
static void VKAPI_CALL wsi_DestroyDebugReportCallbackEXT(VkInstance i,
        VkDebugReportCallbackEXT cb, const VkAllocationCallbacks* a) {
    (void)i; (void)cb; (void)a;
}
static void VKAPI_CALL wsi_DebugReportMessageEXT(VkInstance i, VkDebugReportFlagsEXT f,
        VkDebugReportObjectTypeEXT t, uint64_t o, size_t l, int32_t c, const char* p, const char* m) {
    (void)i; (void)f; (void)t; (void)o; (void)l; (void)c; (void)p; (void)m;
}
static VkResult VKAPI_CALL wsi_CreateDebugUtilsMessengerEXT(VkInstance i,
        const VkDebugUtilsMessengerCreateInfoEXT* ci, const VkAllocationCallbacks* a,
        VkDebugUtilsMessengerEXT* out) {
    (void)i; (void)ci; (void)a;
    *out = (VkDebugUtilsMessengerEXT)(uintptr_t)1;
    return VK_SUCCESS;
}
static void VKAPI_CALL wsi_DestroyDebugUtilsMessengerEXT(VkInstance i,
        VkDebugUtilsMessengerEXT m, const VkAllocationCallbacks* a) {
    (void)i; (void)m; (void)a;
}
static void VKAPI_CALL wsi_SubmitDebugUtilsMessageEXT(VkInstance i,
        VkDebugUtilsMessageSeverityFlagBitsEXT s, VkDebugUtilsMessageTypeFlagsEXT t,
        const VkDebugUtilsMessengerCallbackDataEXT* d) {
    (void)i; (void)s; (void)t; (void)d;
}
static VkResult VKAPI_CALL wsi_SetDebugUtilsObjectNameEXT(VkDevice d,
        const VkDebugUtilsObjectNameInfoEXT* n) { (void)d; (void)n; return VK_SUCCESS; }
static VkResult VKAPI_CALL wsi_SetDebugUtilsObjectTagEXT(VkDevice d,
        const VkDebugUtilsObjectTagInfoEXT* n) { (void)d; (void)n; return VK_SUCCESS; }
static void VKAPI_CALL wsi_QueueBeginDebugUtilsLabelEXT(VkQueue q, const VkDebugUtilsLabelEXT* l) { (void)q; (void)l; }
static void VKAPI_CALL wsi_QueueEndDebugUtilsLabelEXT(VkQueue q) { (void)q; }
static void VKAPI_CALL wsi_QueueInsertDebugUtilsLabelEXT(VkQueue q, const VkDebugUtilsLabelEXT* l) { (void)q; (void)l; }
static void VKAPI_CALL wsi_CmdBeginDebugUtilsLabelEXT(VkCommandBuffer c, const VkDebugUtilsLabelEXT* l) { (void)c; (void)l; }
static void VKAPI_CALL wsi_CmdEndDebugUtilsLabelEXT(VkCommandBuffer c) { (void)c; }
static void VKAPI_CALL wsi_CmdInsertDebugUtilsLabelEXT(VkCommandBuffer c, const VkDebugUtilsLabelEXT* l) { (void)c; (void)l; }

static const VkExtensionProperties wsi_adv_inst[] = {
    { "VK_KHR_surface_maintenance1", 1 },
    { "VK_EXT_surface_maintenance1", 1 },
    { "VK_KHR_surface", 25 }, { "VK_KHR_xcb_surface", 6 },
    { "VK_KHR_get_surface_capabilities2", 1 },
    { "VK_EXT_debug_report", 10 }, { "VK_EXT_debug_utils", 2 },
    { "VK_EXT_tooling_info", 1 },
#ifdef WSI_XLIB
    { "VK_KHR_xlib_surface", 6 },
#endif
};

static const VkExtensionProperties wsi_adv_dev[] = {
    { "VK_KHR_swapchain_maintenance1", 1 },
    { "VK_EXT_swapchain_maintenance1", 1 },
    { "VK_KHR_swapchain", 70 },
    { "VK_KHR_swapchain_mutable_format", 1 },
};
#define WSI_N(a) (sizeof(a) / sizeof((a)[0]))

static int wsi_is_adv(const VkExtensionProperties* adv, size_t n, const char* name) {
    for (size_t i = 0; i < n; i++) if (!strcmp(adv[i].extensionName, name)) return 1;
    return 0;
}

static VkResult wsi_merge_exts(const VkExtensionProperties* real, uint32_t nreal,
        const VkExtensionProperties* adv, size_t nadv, uint32_t* n, VkExtensionProperties* out) {
    VkExtensionProperties* all = malloc((nreal + nadv + 1) * sizeof *all);
    if (!all) return VK_ERROR_OUT_OF_HOST_MEMORY;
    uint32_t total = 0;
    for (uint32_t i = 0; i < nreal; i++) all[total++] = real[i];
    for (size_t k = 0; k < nadv; k++) {
        int have = 0;
        for (uint32_t i = 0; i < nreal; i++)
            if (!strcmp(real[i].extensionName, adv[k].extensionName)) have = 1;
        if (!have) all[total++] = adv[k];
    }
    VkResult r = VK_SUCCESS;
    if (!out) { *n = total; }
    else {
        uint32_t k = *n < total ? *n : total;
        for (uint32_t i = 0; i < k; i++) out[i] = all[i];
        if (k < total) r = VK_INCOMPLETE;
        *n = k;
    }
    free(all);
    return r;
}

static VkResult VKAPI_CALL wsi_EnumerateInstanceExtensionProperties(const char* layer,
        uint32_t* n, VkExtensionProperties* out) {
    PFN_vkEnumerateInstanceExtensionProperties real = g_c->hal->EnumerateInstanceExtensionProperties;
    if (layer) return real(layer, n, out);
    uint32_t nr = 0;
    VkResult r = real(NULL, &nr, NULL);
    if (r) return r;
    VkExtensionProperties* rl = malloc((nr + 1) * sizeof *rl);
    if (!rl) return VK_ERROR_OUT_OF_HOST_MEMORY;
    r = real(NULL, &nr, rl);
    if (!r) r = wsi_merge_exts(rl, nr, wsi_adv_inst, WSI_N(wsi_adv_inst), n, out);
    free(rl);
    return r;
}

static VkResult VKAPI_CALL wsi_EnumerateDeviceExtensionProperties(VkPhysicalDevice pd,
        const char* layer, uint32_t* n, VkExtensionProperties* out) {
    PFN_vkEnumerateDeviceExtensionProperties real = (PFN_vkEnumerateDeviceExtensionProperties)
        g_real_gipa(g_c->inst, "vkEnumerateDeviceExtensionProperties");
    if (layer) return real(pd, layer, n, out);
    uint32_t nr = 0;
    VkResult r = real(pd, NULL, &nr, NULL);
    if (r) return r;
    VkExtensionProperties* rl = malloc((nr + 1) * sizeof *rl);
    if (!rl) return VK_ERROR_OUT_OF_HOST_MEMORY;
    r = real(pd, NULL, &nr, rl);
    if (!r) r = wsi_merge_exts(rl, nr, wsi_adv_dev, WSI_N(wsi_adv_dev), n, out);
    free(rl);
    return r;
}

static PFN_vkVoidFunction VKAPI_CALL wsi_real_driver_gipa(VkInstance instance, const char* pName) {
    if (!pName) return NULL;
    
    /* 1. vkCreateInstance from HAL */
    if (!strcmp(pName, "vkCreateInstance")) {
        return (PFN_vkVoidFunction)g_c->hal->CreateInstance;
    }

    /* 2. vkCreateDevice from HAL/gipa */
    if (!strcmp(pName, "vkCreateDevice")) {
        VkInstance inst = instance ? instance : (g_c ? g_c->inst : VK_NULL_HANDLE);
        return g_real_gipa ? g_real_gipa(inst, "vkCreateDevice") : NULL;
    }
    
    /* 3. Any other instance function */
    VkInstance inst = instance ? instance : (g_c ? g_c->inst : VK_NULL_HANDLE);
    return g_real_gipa ? g_real_gipa(inst, pName) : NULL;
}

static PFN_vkVoidFunction VKAPI_CALL wsi_real_driver_gdpa(VkDevice device, const char* pName) {
    if (!pName) return NULL;
    PFN_vkGetDeviceProcAddr real_gdpa = wsi_real_gdpa_get();
    
    if (!strcmp(pName, "vkCreateDevice")) {
        return real_gdpa ? real_gdpa(device, "vkCreateDevice") 
                         : g_real_gipa(g_c->inst, "vkCreateDevice");
    }
    
    return real_gdpa ? real_gdpa(device, pName) 
                     : (g_c ? g_real_gipa(g_c->inst, pName) : NULL);
}

static VkResult VKAPI_CALL wsi_CreateInstance(const VkInstanceCreateInfo* ci,
        const VkAllocationCallbacks* a, VkInstance* out) {
    if (!ci) return VK_ERROR_INITIALIZATION_FAILED;

    if (wsi_dbg_enabled()) {
        fprintf(stderr, "vulkan_debug: entering wsi_CreateInstance...\n");
    }

    if (ci->pApplicationInfo) {
        const char* app_name = ci->pApplicationInfo->pApplicationName;
        const char* eng_name = ci->pApplicationInfo->pEngineName;

        int is_zink = (app_name && strstr(app_name, "zink")) ||
                      (eng_name && strstr(eng_name, "zink"));

        if (is_zink) {
            fprintf(stderr,
                "\n"
                "=========================================================================================\n"
                "[NOTICE] Zink (OpenGL over Vulkan) detected on Mali GPU.\n"
                "         For optimal stability and performance, using native Vulkan or direct GLES\n"
                "         is highly recommended on this hardware.\n"
                "=========================================================================================\n\n"
            );
        }
    }

    wsi_ly_load(ci);

    const char* keep[128]; uint32_t nk = 0;
    for (uint32_t i = 0; i < ci->enabledExtensionCount && nk < 128; i++)
        if (!wsi_is_adv(wsi_adv_inst, WSI_N(wsi_adv_inst), ci->ppEnabledExtensionNames[i]))
            keep[nk++] = ci->ppEnabledExtensionNames[i];

    VkInstanceCreateInfo c2 = *ci;
    c2.enabledExtensionCount = nk;
    c2.ppEnabledExtensionNames = nk ? keep : NULL;
    c2.enabledLayerCount = 0;
    c2.ppEnabledLayerNames = NULL;

    VkApplicationInfo app2;
    if (c2.pApplicationInfo) {
        app2 = *c2.pApplicationInfo;
        if (app2.apiVersion > VK_API_VERSION_1_1) app2.apiVersion = VK_API_VERSION_1_1;
        c2.pApplicationInfo = &app2;
    }

    if (!g_nly) {
        VkResult r = g_c->hal->CreateInstance(&c2, a, out);
        if (r) return r;
        g_c->inst = *out;
        g_real_gdpa = wsi_real_gdpa_get();
        return g_real_gdpa ? VK_SUCCESS : VK_ERROR_INITIALIZATION_FAILED;
    }

    VkLayerInstanceLink lk[WSI_MAX_LY];
    for (int i = 0; i < g_nly; i++) {
        lk[i].pfnNextGetInstanceProcAddr = (i + 1 < g_nly) ? g_ly[i + 1].gipa : wsi_real_driver_gipa;
        lk[i].pfnNextGetPhysicalDeviceProcAddr = NULL;
        lk[i].pNext = (i + 1 < g_nly) ? &lk[i + 1] : NULL;
    }

    VkLayerInstanceCreateInfo li; memset(&li, 0, sizeof li);
    li.sType = VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO;
    li.pNext = c2.pNext;
    li.function = VK_LAYER_LINK_INFO;
    li.u.pLayerInfo = &lk[0];

    VkLayerInstanceCreateInfo cb = li;
    cb.pNext = &li;
    cb.function = VK_LOADER_DATA_CALLBACK;
    cb.u.pfnSetInstanceLoaderData = wsi_set_inst_data;
    c2.pNext = &cb;

    PFN_vkCreateInstance head = (PFN_vkCreateInstance)g_ly[0].gipa(VK_NULL_HANDLE, "vkCreateInstance");
    if (!head) return VK_ERROR_INITIALIZATION_FAILED;

    VkResult r = head(&c2, a, out);
    if (!r) {
        g_c->inst = *out;
        g_ly_inst = *out;
        g_real_gdpa = wsi_real_gdpa_get();
    }
    return r;
}

/* Common bookkeeping after the real vkCreateDevice succeeded */
static VkResult wsi_device_created(VkPhysicalDevice pd, const VkDeviceCreateInfo* ci, VkDevice dev) {
    g_c->pd  = pd;
    g_c->dev = dev;
    g_c->qfi = ci->queueCreateInfoCount ? ci->pQueueCreateInfos[0].queueFamilyIndex : 0;
    PFN_vkGetDeviceProcAddr gd = wsi_real_gdpa_get();
    PFN_vkGetDeviceQueue gq = gd ? (PFN_vkGetDeviceQueue)gd(dev, "vkGetDeviceQueue") : NULL;
    if (!gq) return VK_ERROR_INITIALIZATION_FAILED;
    gq(dev, g_c->qfi, 0, &g_c->queue);
    return wsi_load(g_c) ? VK_ERROR_INITIALIZATION_FAILED : VK_SUCCESS;
}

static VkResult VKAPI_CALL wsi_CreateDevice(VkPhysicalDevice pd, const VkDeviceCreateInfo* ci,
        const VkAllocationCallbacks* a, VkDevice* out) {
    /* ---- 1. Extensions: strip virtual ones, add what zero-copy needs ---- */
    PFN_vkEnumerateDeviceExtensionProperties edep = (PFN_vkEnumerateDeviceExtensionProperties)
        g_real_gipa(g_c->inst, "vkEnumerateDeviceExtensionProperties");
    uint32_t nd = 0; edep(pd, NULL, &nd, NULL);
    VkExtensionProperties* dx = malloc((nd + 1) * sizeof *dx);
    if (!dx) return VK_ERROR_OUT_OF_HOST_MEMORY;
    edep(pd, NULL, &nd, dx);

    const char* ext[160]; uint32_t ne = 0;
    for (uint32_t i = 0; i < ci->enabledExtensionCount && ne < 128; i++)
        if (!wsi_is_adv(wsi_adv_dev, WSI_N(wsi_adv_dev), ci->ppEnabledExtensionNames[i]))
            ext[ne++] = ci->ppEnabledExtensionNames[i];
    for (size_t k = 0; k < WSI_N_DEV_EXTS && ne < 160; k++) {
        int have = 0, sup = 0;
        for (uint32_t i = 0; i < ne; i++) if (!strcmp(ext[i], wsi_dev_exts[k])) have = 1;
        for (uint32_t j = 0; j < nd; j++) if (!strcmp(dx[j].extensionName, wsi_dev_exts[k])) sup = 1;
        if (!have && sup) ext[ne++] = wsi_dev_exts[k];
    }
    free(dx);

    VkDeviceCreateInfo c2 = *ci;
    c2.enabledExtensionCount = ne;
    c2.ppEnabledExtensionNames = ext;

    /* ---- 2. Features: clamp to what the hardware really supports (copies only) ---- */
    VkPhysicalDeviceFeatures  hw, f1;
    VkPhysicalDeviceFeatures2 f2;
    wsi_hw_features(pd, &hw);

    if (c2.pEnabledFeatures) {
        f1 = *c2.pEnabledFeatures;
        if (wsi_clamp_features(&f1, &hw)) c2.pEnabledFeatures = &f1;
    }

    const VkBaseInStructure* head = (const VkBaseInStructure*)c2.pNext;
    if (head && head->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2) {
        f2 = *(const VkPhysicalDeviceFeatures2*)head;   /* keeps its own pNext chain */
        if (wsi_clamp_features(&f2.features, &hw)) c2.pNext = &f2;
    } else if (wsi_dbg_enabled()) {
        /* Features2 deeper in the chain cannot be copied safely (unknown struct sizes) */
        for (const VkBaseInStructure* p = head; p; p = p->pNext)
            if (p->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2) {
                fprintf(stderr, "vulkan_debug: VkPhysicalDeviceFeatures2 is not first in pNext, not clamped\n");
                break;
            }
    }

    /* ---- 3. No layers: call the real driver directly ---- */
    if (!g_nly) {
        PFN_vkCreateDevice real = (PFN_vkCreateDevice)g_real_gipa(g_c->inst, "vkCreateDevice");
        VkResult r = real(pd, &c2, a, out);
        if (r) {
            fprintf(stderr, "vulkan_error:: vkCreateDevice failed (%d)\n", r);
            return r;
        }
        return wsi_device_created(pd, ci, *out);
    }

    /* ---- 4. With layers: build the device chain ---- */
    for (int i = 0; i < g_nly; i++) {
        PFN_vkGetDeviceProcAddr g = (PFN_vkGetDeviceProcAddr)g_ly[i].gipa(g_ly_inst, "vkGetDeviceProcAddr");
        if (g) g_ly[i].gdpa = g;
    }

    VkLayerDeviceLink lk[WSI_MAX_LY];
    for (int i = 0; i < g_nly; i++) {
        lk[i].pfnNextGetInstanceProcAddr = (i + 1 < g_nly) ? g_ly[i + 1].gipa : wsi_real_driver_gipa;
        lk[i].pfnNextGetDeviceProcAddr   = (i + 1 < g_nly) ? g_ly[i + 1].gdpa : wsi_real_driver_gdpa;
        lk[i].pNext = (i + 1 < g_nly) ? &lk[i + 1] : NULL;
    }

    VkLayerDeviceCreateInfo ld; memset(&ld, 0, sizeof ld);
    ld.sType = VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO;
    ld.pNext = c2.pNext;
    ld.function = VK_LAYER_LINK_INFO;
    ld.u.pLayerInfo = &lk[0];

    VkLayerDeviceCreateInfo cb = ld;
    cb.pNext = &ld;
    cb.function = VK_LOADER_DATA_CALLBACK;
    cb.u.pfnSetDeviceLoaderData = wsi_set_dev_data;
    c2.pNext = &cb;

    PFN_vkCreateDevice headfn = (PFN_vkCreateDevice)g_ly[0].gipa(g_ly_inst, "vkCreateDevice");
    if (!headfn) return VK_ERROR_INITIALIZATION_FAILED;

    VkResult r = headfn(pd, &c2, a, out);
    if (r) {
        fprintf(stderr, "vulkan_error:: vkCreateDevice failed (%d)\n", r);
        return r;
    }
    return wsi_device_created(pd, ci, *out);
}

static VkResult VKAPI_CALL wsi_GetPhysicalDeviceToolPropertiesEXT(
        VkPhysicalDevice pd, uint32_t* pToolCount, VkPhysicalDeviceToolPropertiesEXT* pToolProperties) {
    (void)pd; (void)pToolProperties;
    if (pToolCount) *pToolCount = 0;
    return VK_SUCCESS;
}

/* =====================  VULKAN 1.2 TIMELINE SEMAPHORE ALIASES  ===================== */

typedef VkResult (VKAPI_PTR *PFN_vkWaitSemaphoresKHR)(VkDevice device, const VkSemaphoreWaitInfo* pWaitInfo, uint64_t timeout);
typedef VkResult (VKAPI_PTR *PFN_vkSignalSemaphoreKHR)(VkDevice device, const VkSemaphoreSignalInfo* pSignalInfo);
typedef VkResult (VKAPI_PTR *PFN_vkGetSemaphoreCounterValueKHR)(VkDevice device, VkSemaphore semaphore, uint64_t* pValue);

static PFN_vkVoidFunction wsi_real_dev_fn(VkDevice d, const char* core, const char* khr) {
    PFN_vkGetDeviceProcAddr real_gdpa = wsi_real_gdpa_get();
    PFN_vkVoidFunction f = real_gdpa ? real_gdpa(d, core) : NULL;
    if (!f && real_gdpa) f = real_gdpa(d, khr);
    if (!f && g_c && g_c->inst) f = g_real_gipa(g_c->inst, khr);
    return f;
}

static VkResult VKAPI_CALL wsi_WaitSemaphores(VkDevice d, const VkSemaphoreWaitInfo* pWaitInfo, uint64_t timeout) {
    PFN_vkWaitSemaphoresKHR fn = (PFN_vkWaitSemaphoresKHR)
        wsi_real_dev_fn(d, "vkWaitSemaphores", "vkWaitSemaphoresKHR");
    return fn ? fn(d, pWaitInfo, timeout) : VK_ERROR_FEATURE_NOT_PRESENT;
}

static VkResult VKAPI_CALL wsi_SignalSemaphore(VkDevice d, const VkSemaphoreSignalInfo* pSignalInfo) {
    PFN_vkSignalSemaphoreKHR fn = (PFN_vkSignalSemaphoreKHR)
        wsi_real_dev_fn(d, "vkSignalSemaphore", "vkSignalSemaphoreKHR");
    return fn ? fn(d, pSignalInfo) : VK_ERROR_FEATURE_NOT_PRESENT;
}

static VkResult VKAPI_CALL wsi_GetSemaphoreCounterValue(VkDevice d, VkSemaphore sem, uint64_t* pVal) {
    PFN_vkGetSemaphoreCounterValueKHR fn = (PFN_vkGetSemaphoreCounterValueKHR)
        wsi_real_dev_fn(d, "vkGetSemaphoreCounterValue", "vkGetSemaphoreCounterValueKHR");
    return fn ? fn(d, sem, pVal) : VK_ERROR_FEATURE_NOT_PRESENT;
}

static void VKAPI_CALL wsi_GetPhysicalDeviceFeatures2(VkPhysicalDevice pd,
        VkPhysicalDeviceFeatures2* f) {
    PFN_vkGetPhysicalDeviceFeatures2 real = (PFN_vkGetPhysicalDeviceFeatures2)
        g_real_gipa(g_c->inst, "vkGetPhysicalDeviceFeatures2");
    if (!real) real = (PFN_vkGetPhysicalDeviceFeatures2)
        g_real_gipa(g_c->inst, "vkGetPhysicalDeviceFeatures2KHR");
    if (real) real(pd, f); else memset(&f->features, 0, sizeof f->features);
    for (VkBaseOutStructure* p = (VkBaseOutStructure*)f->pNext; p; p = p->pNext)
        if (p->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SWAPCHAIN_MAINTENANCE_1_FEATURES_KHR)
            ((VkPhysicalDeviceSwapchainMaintenance1FeaturesKHR*)p)->swapchainMaintenance1 = VK_TRUE;
}

static VkResult VKAPI_CALL wsi_ReleaseSwapchainImagesKHR(VkDevice d,
        const VkReleaseSwapchainImagesInfoKHR* ri) {
    (void)d;
    wsi_swapchain* s = (wsi_swapchain*)WSI_H2P(ri->swapchain);
    if (!s || !s->async) return VK_SUCCESS;
    pthread_mutex_lock(&s->mu);
    for (uint32_t i = 0; i < ri->imageIndexCount; i++) {
        uint32_t idx = ri->pImageIndices[i];
        if (idx < s->n && s->state[idx] == 1) s->state[idx] = 0;
    }
    pthread_cond_broadcast(&s->cv);
    pthread_mutex_unlock(&s->mu);
    return VK_SUCCESS;
}

/* =====================  DISPATCH  ===================== */

typedef struct { const char* n; PFN_vkVoidFunction f; } wsi_ov;

#define OV(name, fn) { name, (PFN_vkVoidFunction)(fn) }

static const wsi_ov wsi_inst_ov[] = {
    OV("vkGetInstanceProcAddr",                      wsi_core_gipa),
    OV("vkGetDeviceProcAddr",                        wsi_core_gdpa),
    OV("vkCreateXcbSurfaceKHR",                      wsi_CreateXcbSurfaceKHR),
    OV("vkCreateInstance",                           wsi_CreateInstance),
    OV("vkGetPhysicalDeviceToolPropertiesEXT",       wsi_GetPhysicalDeviceToolPropertiesEXT),
    OV("vkGetPhysicalDeviceToolProperties",          wsi_GetPhysicalDeviceToolPropertiesEXT),
    OV("vkGetPhysicalDevicePresentRectanglesKHR",    wsi_GetPhysicalDevicePresentRectanglesKHR),
    OV("vkGetPhysicalDeviceDisplayPropertiesKHR",       wsi_GetPhysicalDeviceDisplayPropertiesKHR),
    OV("vkGetPhysicalDeviceDisplayPlanePropertiesKHR",  wsi_GetPhysicalDeviceDisplayPlanePropertiesKHR),
    OV("vkGetDisplayPlaneSupportedDisplaysKHR",         wsi_GetDisplayPlaneSupportedDisplaysKHR),
    OV("vkGetDisplayModePropertiesKHR",                 wsi_GetDisplayModePropertiesKHR),
    OV("vkGetDisplayPlaneCapabilitiesKHR",              wsi_GetDisplayPlaneCapabilitiesKHR),
    OV("vkCreateDebugReportCallbackEXT",             wsi_CreateDebugReportCallbackEXT),
    OV("vkDestroyDebugReportCallbackEXT",            wsi_DestroyDebugReportCallbackEXT),
    OV("vkDebugReportMessageEXT",                    wsi_DebugReportMessageEXT),
    OV("vkCreateDebugUtilsMessengerEXT",             wsi_CreateDebugUtilsMessengerEXT),
    OV("vkDestroyDebugUtilsMessengerEXT",            wsi_DestroyDebugUtilsMessengerEXT),
    OV("vkSubmitDebugUtilsMessageEXT",               wsi_SubmitDebugUtilsMessageEXT),
    OV("vkCreateDevice",                             wsi_CreateDevice),
    OV("vkEnumerateInstanceExtensionProperties",     wsi_EnumerateInstanceExtensionProperties),
    OV("vkEnumerateDeviceExtensionProperties",       wsi_EnumerateDeviceExtensionProperties),
#ifdef WSI_XLIB
    OV("vkCreateXlibSurfaceKHR",                     wsi_CreateXlibSurfaceKHR),
    OV("vkGetPhysicalDeviceXlibPresentationSupportKHR", wsi_GetXlibPresentationSupportKHR),
#endif
    OV("vkDestroySurfaceKHR",                        wsi_DestroySurfaceKHR),
    OV("vkGetPhysicalDeviceXcbPresentationSupportKHR", wsi_GetXcbPresentationSupportKHR),
    OV("vkGetPhysicalDeviceSurfaceSupportKHR",       wsi_GetPDSurfaceSupportKHR),
    OV("vkGetPhysicalDeviceSurfaceCapabilitiesKHR",  wsi_GetPDSurfaceCapabilitiesKHR),
    OV("vkGetPhysicalDeviceSurfaceFormatsKHR",       wsi_GetPDSurfaceFormatsKHR),
    OV("vkGetPhysicalDeviceSurfacePresentModesKHR",  wsi_GetPDSurfacePresentModesKHR),
    OV("vkGetPhysicalDeviceSurfaceCapabilities2KHR", wsi_GetPDSurfaceCapabilities2KHR),
    OV("vkGetPhysicalDeviceSurfaceFormats2KHR",      wsi_GetPDSurfaceFormats2KHR),
    OV("vkGetPhysicalDeviceFeatures2",    wsi_GetPhysicalDeviceFeatures2),
    OV("vkGetPhysicalDeviceFeatures2KHR", wsi_GetPhysicalDeviceFeatures2),
    { NULL, NULL }
};

static const wsi_ov wsi_dev_ov[] = {
    OV("vkCreateSwapchainKHR",    wsi_CreateSwapchainKHR),
    OV("vkDestroySwapchainKHR",   wsi_DestroySwapchainKHR),
    OV("vkGetSwapchainImagesKHR", wsi_GetSwapchainImagesKHR),
    OV("vkAcquireNextImageKHR",   wsi_AcquireNextImageKHR),
    OV("vkQueuePresentKHR",       wsi_QueuePresentKHR),
    OV("vkGetDeviceProcAddr",     wsi_core_gdpa),
    OV("vkAcquireNextImage2KHR",                 wsi_AcquireNextImage2KHR),
    OV("vkGetDeviceGroupPresentCapabilitiesKHR", wsi_GetDeviceGroupPresentCapabilitiesKHR),
    OV("vkGetDeviceGroupSurfacePresentModesKHR", wsi_GetDeviceGroupSurfacePresentModesKHR),
    OV("vkSetDebugUtilsObjectNameEXT",    wsi_SetDebugUtilsObjectNameEXT),
    OV("vkSetDebugUtilsObjectTagEXT",     wsi_SetDebugUtilsObjectTagEXT),
    OV("vkQueueBeginDebugUtilsLabelEXT",  wsi_QueueBeginDebugUtilsLabelEXT),
    OV("vkQueueEndDebugUtilsLabelEXT",    wsi_QueueEndDebugUtilsLabelEXT),
    OV("vkQueueInsertDebugUtilsLabelEXT", wsi_QueueInsertDebugUtilsLabelEXT),
    OV("vkCmdBeginDebugUtilsLabelEXT",    wsi_CmdBeginDebugUtilsLabelEXT),
    OV("vkCmdEndDebugUtilsLabelEXT",      wsi_CmdEndDebugUtilsLabelEXT),
    OV("vkCmdInsertDebugUtilsLabelEXT",   wsi_CmdInsertDebugUtilsLabelEXT),
    OV("vkWaitSemaphores",             wsi_WaitSemaphores),
    OV("vkWaitSemaphoresKHR",          wsi_WaitSemaphores),
    OV("vkSignalSemaphore",            wsi_SignalSemaphore),
    OV("vkSignalSemaphoreKHR",         wsi_SignalSemaphore),
    OV("vkGetSemaphoreCounterValue",   wsi_GetSemaphoreCounterValue),
    OV("vkGetSemaphoreCounterValueKHR",wsi_GetSemaphoreCounterValue),
    OV("vkReleaseSwapchainImagesKHR", wsi_ReleaseSwapchainImagesKHR),
    OV("vkReleaseSwapchainImagesEXT", wsi_ReleaseSwapchainImagesKHR),
    { NULL, NULL }
};

static PFN_vkVoidFunction wsi_lookup(const wsi_ov* t, const char* n) {
    for (; t->n; t++) if (!strcmp(t->n, n)) return t->f;
    return NULL;
}

static PFN_vkVoidFunction VKAPI_CALL wsi_core_gipa(VkInstance i, const char* n) {
    if (!n) return NULL;
    PFN_vkVoidFunction f = wsi_lookup(wsi_inst_ov, n);
    if (!f) f = wsi_lookup(wsi_dev_ov, n);
    return f ? f : g_real_gipa(i, n);
}

static int wsi_has_suffix(const char* n, const char* suf) {
    size_t ln = strlen(n), ls = strlen(suf);
    return ln >= ls && !strcmp(n + ln - ls, suf);
}

static PFN_vkVoidFunction VKAPI_CALL wsi_core_gdpa(VkDevice d, const char* n) {
    if (!n) return NULL;

    PFN_vkVoidFunction f = wsi_lookup(wsi_dev_ov, n);
    if (f) return f;
    f = wsi_lookup(wsi_inst_ov, n);
    if (f) return f;

    if (!d) {
        if (g_c && g_c->inst) return g_real_gipa(g_c->inst, n);
        return NULL;
    }

    PFN_vkGetDeviceProcAddr real_gdpa = wsi_real_gdpa_get();
    if (!real_gdpa) return NULL;

    f = real_gdpa(d, n);

    if (!f && !wsi_has_suffix(n, "KHR") && !wsi_has_suffix(n, "EXT")) {
        char khr_name[128];
        snprintf(khr_name, sizeof(khr_name), "%sKHR", n);
        f = real_gdpa(d, khr_name);
        if (!f && g_c && g_c->inst) f = g_real_gipa(g_c->inst, khr_name);
    }

    if (!f && g_c && g_c->inst) f = g_real_gipa(g_c->inst, n);
    return f;
}


static PFN_vkVoidFunction VKAPI_CALL wsi_gipa(VkInstance i, const char* n) {
    if (!n) return NULL;
    if (!strcmp(n, "vkGetInstanceProcAddr")) return (PFN_vkVoidFunction)wsi_gipa;
    if (!strcmp(n, "vkCreateInstance"))      return (PFN_vkVoidFunction)wsi_CreateInstance;
    if (!strcmp(n, "vkCreateDevice"))        return (PFN_vkVoidFunction)wsi_CreateDevice;
    if (g_nly && i) { PFN_vkVoidFunction f = g_ly[0].gipa(i, n); if (f) return f; }
    return wsi_core_gipa(i, n);
}

static PFN_vkVoidFunction VKAPI_CALL wsi_gdpa(VkDevice d, const char* n) {
    if (!n) return NULL;
    if (g_nly && d && g_ly[0].gdpa) { PFN_vkVoidFunction f = g_ly[0].gdpa(d, n); if (f) return f; }
    return wsi_core_gdpa(d, n);
}

#define WSI_LD(n) do { PFN_vkGetDeviceProcAddr real_gdpa = wsi_real_gdpa_get(); \
    w.n = (PFN_vk##n)(real_gdpa ? real_gdpa(c->dev, "vk" #n) : NULL); \
    if (!w.n) { fprintf(stderr, "vulkan_error:: missing vk%s\n", #n); return 1; } } while (0)

static int wsi_load(mali_ctx* c) {
    MI(c, vkGetPhysicalDeviceMemoryProperties)(c->pd, &g_mp);
    WSI_LD(CreateImage); WSI_LD(DestroyImage); WSI_LD(GetImageMemoryRequirements);
    WSI_LD(GetImageSubresourceLayout);
    WSI_LD(AllocateMemory); WSI_LD(FreeMemory); WSI_LD(BindImageMemory);
    WSI_LD(CreateBuffer); WSI_LD(DestroyBuffer); WSI_LD(GetBufferMemoryRequirements);
    WSI_LD(BindBufferMemory); WSI_LD(MapMemory); WSI_LD(UnmapMemory);
    WSI_LD(InvalidateMappedMemoryRanges);
    WSI_LD(CreateCommandPool); WSI_LD(DestroyCommandPool); WSI_LD(AllocateCommandBuffers);
    WSI_LD(BeginCommandBuffer); WSI_LD(EndCommandBuffer);
    WSI_LD(CmdPipelineBarrier); WSI_LD(CmdCopyImageToBuffer);
    WSI_LD(CreateFence); WSI_LD(DestroyFence); WSI_LD(ResetFences); WSI_LD(WaitForFences);
    WSI_LD(CreateSemaphore); WSI_LD(DestroySemaphore);
    WSI_LD(QueueSubmit); WSI_LD(DeviceWaitIdle);

    PFN_vkGetDeviceProcAddr real_gdpa = wsi_real_gdpa_get();
    w.GetAndroidHardwareBufferPropertiesANDROID = (PFN_vkGetAndroidHardwareBufferPropertiesANDROID)
        (real_gdpa ? real_gdpa(c->dev, "vkGetAndroidHardwareBufferPropertiesANDROID") : NULL);
    w.GetMemoryFdPropertiesKHR = (PFN_vkGetMemoryFdPropertiesKHR)
        (real_gdpa ? real_gdpa(c->dev, "vkGetMemoryFdPropertiesKHR") : NULL);

    w.GetSemaphoreFdKHR = (PFN_vkGetSemaphoreFdKHR)
        (real_gdpa ? real_gdpa(c->dev, "vkGetSemaphoreFdKHR") : NULL);
    if (wsi_dbg_enabled()) {
        fprintf(stderr, "vulkan_debug: semaphore fd export = %d\n", w.GetSemaphoreFdKHR != NULL);
    }

    const char* ahb_lib = getenv("HYBRIS_AHB_LIBRARY");
    void* l = shim_dlopen(ahb_lib && *ahb_lib ? ahb_lib : "libnativewindow.so", RTLD_NOW);
    if (l) {
        g_ahb.alloc    = (int (*)(const AHardwareBuffer_Desc*, AHardwareBuffer**))
                            shim_dlsym(l, "AHardwareBuffer_allocate");
        g_ahb.release  = (void (*)(AHardwareBuffer*))shim_dlsym(l, "AHardwareBuffer_release");
        g_ahb.describe = (void (*)(const AHardwareBuffer*, AHardwareBuffer_Desc*))
                            shim_dlsym(l, "AHardwareBuffer_describe");
        g_ahb.nh       = (const wsi_nh_t* (*)(const AHardwareBuffer*))
                            shim_dlsym(l, "AHardwareBuffer_getNativeHandle");
    }
    g_zc_ok = g_ahb.alloc && g_ahb.release && g_ahb.describe && g_ahb.nh &&
              w.GetAndroidHardwareBufferPropertiesANDROID;

    wsi_dh_init();
    if (g_dh.on) g_zc_ok = 1;

    if (wsi_dbg_enabled()) {
        fprintf(stderr, "vulkan_debug: zero-copy available = %d\n", g_zc_ok);
    }
    return 0;
}

static int wsi_install(mali_ctx* c) {
    g_c = c;
    g_real_gipa = c->gipa;
    g_real_gdpa = c->gdpa;
    if (wsi_load(c)) return 1;
    c->gipa = wsi_gipa;
    c->gdpa = wsi_gdpa;
    return 0;
}

static void wsi_attach(mali_ctx* c) {
    g_c = c;
    g_real_gipa = c->gipa;
    c->gipa = wsi_gipa;
    c->gdpa = wsi_gdpa;
}

#endif
