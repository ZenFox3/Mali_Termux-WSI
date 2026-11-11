#ifndef MALI_LOADER_H
#define MALI_LOADER_H

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <android/dlext.h>
#include <dlfcn.h>
#include "shim_dl.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define HWVULKAN_DEVICE_0 "vk0"
#define HWDT (('H'<<24)|('W'<<16)|('D'<<8)|'T')

/* ---------- Helper pentru Debug Log ---------- */
static inline int mali_dbg_enabled(void) {
    return getenv("VULKAN_DEBUG") != NULL || 
           getenv("MALI_DEBUG") != NULL || 
           getenv("VULKAN_LAYER_DEBUG") != NULL;
}

/* ---- Minimal HAL (hardware.h / hwvulkan.h) ---- */
struct hw_module_t; struct hw_device_t;
typedef struct hw_module_methods_t {
    int (*open)(const struct hw_module_t*, const char*, struct hw_device_t**);
} hw_module_methods_t;
typedef struct hw_module_t {
    uint32_t tag; uint16_t module_api_version, hal_api_version;
    const char *id, *name, *author;
    hw_module_methods_t* methods; void* dso;
#ifdef __LP64__
    uint64_t reserved[32-7];
#else
    uint32_t reserved[32-7];
#endif
} hw_module_t;
typedef struct hw_device_t {
    uint32_t tag, version; hw_module_t* module;
#ifdef __LP64__
    uint64_t reserved[12];
#else
    uint32_t reserved[12];
#endif
    int (*close)(struct hw_device_t*);
} hw_device_t;
typedef struct {
    hw_device_t common;
    PFN_vkEnumerateInstanceExtensionProperties EnumerateInstanceExtensionProperties;
    PFN_vkCreateInstance CreateInstance;
    PFN_vkGetInstanceProcAddr GetInstanceProcAddr;
} hwvulkan_device_t;

/* ---- Context ---- */
typedef struct {
    hwvulkan_device_t*        hal;
    PFN_vkGetInstanceProcAddr gipa;
    PFN_vkGetDeviceProcAddr   gdpa;
    VkInstance                inst;
    VkPhysicalDevice          pd;
    VkDevice                  dev;
    uint32_t                  qfi;
    VkQueue                   queue;
    int                       has_native_buffer;
} mali_ctx;

/* Lookup macros: MI(c, vkFoo) for instance level, MD(c, vkFoo) for device level */
#define MI(c, name) ((PFN_##name)(c)->gipa((c)->inst, #name))
#define MD(c, name) ((PFN_##name)(c)->gdpa((c)->dev,  #name))

/* ---- 1. SPHAL Loading ---- */
typedef void* (*mali_get_ns_fn)(const char*);
typedef void* (*mali_sphal_fn)(const char*, int);

#ifdef SHIM_GLIBC
/* glibc guest: the Android HAL is loaded through libhybris (preloaded by proot-sphal-run). */
static void* mali_sphal_load(const char* path) {
    void* h = shim_dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!h) fprintf(stderr, "mali error: hybris_dlopen(%s) failed: %s\n", path, shim_dlerror());
    return h;
}
#else
static void* mali_sphal_load(const char* path) {
    void* l = dlopen("libdl_android.so", RTLD_NOW);
    mali_get_ns_fn get_ns = l ? (mali_get_ns_fn)dlsym(l, "android_get_exported_namespace") : NULL;
    void* ns = get_ns ? get_ns("sphal") : NULL;
    if (ns) {
        android_dlextinfo info = { .flags = ANDROID_DLEXT_USE_NAMESPACE,
                                   .library_namespace = ns };
        void* h = android_dlopen_ext(path, RTLD_NOW, &info);
        if (h) return h;
        fprintf(stderr, "mali error: dlopen_ext failed: %s\n", dlerror());
    }
    void* v = dlopen("libvndksupport.so", RTLD_NOW);
    mali_sphal_fn f = v ? (mali_sphal_fn)dlsym(v, "android_load_sphal_library") : NULL;
    if (f) return f(path, RTLD_NOW);
    fprintf(stderr, "mali error: SPHAL unavailable\n");
    return NULL;
}
#endif

/* ---- 2. HAL Initialization ---- */
static hwvulkan_device_t* mali_hal_open(void* h) {
    hw_module_t* m = (hw_module_t*)shim_dlsym(h, "HMI");
    if (!m) { fprintf(stderr, "mali error: Missing HMI symbol: %s\n", shim_dlerror()); return NULL; }
    if (mali_dbg_enabled()) {
        fprintf(stderr, "HAL: id=%s name=%s\n", m->id, m->name);
    }
    hw_device_t* dev = NULL;
    int r = m->methods->open((const hw_module_t*)m, HWVULKAN_DEVICE_0, &dev);
    if (r) { fprintf(stderr, "mali error: open() failed = %d\n", r); return NULL; }
    if (dev->tag != HWDT) { fprintf(stderr, "mali error: Invalid device tag\n"); return NULL; }
    return (hwvulkan_device_t*)dev;
}

/* ---- 3. Pointer Verification (/proc/self/maps) ---- */
static char g_mali_name[128] = "libGLES_mali";

static int mali_find_map(const void* p, char* perms, char* path, size_t plen) {
    FILE* f = fopen("/proc/self/maps", "r");
    if (!f) return 0;
    char line[1024];
    uintptr_t a = (uintptr_t)p;
    int found = 0;
    while (fgets(line, sizeof line, f)) {
        unsigned long lo, hi; char pr[8]; int off = 0;
        if (sscanf(line, "%lx-%lx %7s %*s %*s %*s %n", &lo, &hi, pr, &off) < 3)
            continue;
        if (a >= lo && a < hi) {
            strcpy(perms, pr);
            const char* s = line + off;
            size_t n = strcspn(s, "\n");
            if (n >= plen) n = plen - 1;
            memcpy(path, s, n); path[n] = 0;
            found = 1; break;
        }
    }
    fclose(f);
    return found;
}

static int mali_ptr_in_text(const void* p) {
    char pr[8], path[512];
    return p && mali_find_map(p, pr, path, sizeof path)
             && pr[2] == 'x' && strstr(path, g_mali_name);
}

static PFN_vkGetInstanceProcAddr mali_resolve_gipa(hwvulkan_device_t* dev) {
    uintptr_t* vt = (uintptr_t*)dev;
    static const int idxs[] = { 17, 18, 19 };
    for (size_t i = 0; i < sizeof idxs / sizeof idxs[0]; i++) {
        void* p = (void*)vt[idxs[i]];
        if (!mali_ptr_in_text(p)) continue;
        PFN_vkGetInstanceProcAddr cand = (PFN_vkGetInstanceProcAddr)p;
        void* f = (void*)cand(NULL, "vkCreateInstance");
        if (f && mali_ptr_in_text(f)) {
            if (mali_dbg_enabled()) {
                fprintf(stderr, "GIPA found at slot %d\n", idxs[i]);
            }
            return cand;
        }
    }
    return NULL;
}

static const char* mali_classify(const void* p) {
    if (!p) return "NULL";
    return mali_ptr_in_text(p) ? "ok" : "stub";
}

static void mali_report(mali_ctx* c, const char* const* names, size_t n, int dev_level) {
    for (size_t i = 0; i < n; i++) {
        void* p = dev_level ? (void*)c->gdpa(c->dev, names[i])
                            : (void*)c->gipa(c->inst, names[i]);
        fprintf(stderr, "vulkan_debug: %-5s %s\n", mali_classify(p), names[i]);
    }
}

/* ---- 4. Complete Initialization: Instance, GPU, Device, Queue ----
   Return codes: 0 ok, 1 sphal error, 2 HAL error, 3 GIPA resolution error,
   4 instance creation error, 5 no GPU found, 6 no graphics queue, 7 device creation error */
static int mali_load(mali_ctx* c, const char* path) {
    memset(c, 0, sizeof *c);

    const char* b = strrchr(path, '/');
    b = b ? b + 1 : path;
    snprintf(g_mali_name, sizeof g_mali_name, "%s", b);

    void* h = mali_sphal_load(path);
    if (!h) return 1;
    c->hal = mali_hal_open(h);
    if (!c->hal) return 2;
    c->gipa = mali_resolve_gipa(c->hal);
    if (!c->gipa) return 3;
    return 0;
}

static int mali_init_ex(mali_ctx* c, const char* path, int want_native_buffer,
                        const char* const* extra, uint32_t n_extra) {
    int lrc = mali_load(c, path);
    if (lrc) return lrc;

    VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                              .apiVersion = VK_API_VERSION_1_1 };
    VkInstanceCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
                                .pApplicationInfo = &app };
    VkResult r = c->hal->CreateInstance(&ci, NULL, &c->inst);
    if (r) { fprintf(stderr, "mali error: vkCreateInstance failed = %d\n", r); return 4; }

    PFN_vkEnumeratePhysicalDevices enumPD = MI(c, vkEnumeratePhysicalDevices);
    uint32_t n = 0;
    enumPD(c->inst, &n, NULL);
    if (!n) { fprintf(stderr, "mali error: no GPU found\n"); return 5; }
    n = 1;
    enumPD(c->inst, &n, &c->pd); /* VK_INCOMPLETE is acceptable */

    if (mali_dbg_enabled()) {
        VkPhysicalDeviceProperties p;
        MI(c, vkGetPhysicalDeviceProperties)(c->pd, &p);
        fprintf(stderr, "GPU: %s api=%u.%u.%u\n", p.deviceName, VK_VERSION_MAJOR(p.apiVersion),
               VK_VERSION_MINOR(p.apiVersion), VK_VERSION_PATCH(p.apiVersion));
    }

    /* Graphics Queue Family */
    uint32_t nq = 0;
    PFN_vkGetPhysicalDeviceQueueFamilyProperties qfp = MI(c, vkGetPhysicalDeviceQueueFamilyProperties);
    qfp(c->pd, &nq, NULL);
    VkQueueFamilyProperties qf[16]; if (nq > 16) nq = 16;
    qfp(c->pd, &nq, qf);
    c->qfi = UINT32_MAX;
    for (uint32_t i = 0; i < nq; i++)
        if (qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { c->qfi = i; break; }
    if (c->qfi == UINT32_MAX) { fprintf(stderr, "mali error: no graphics queue family found\n"); return 6; }

    /* Device Extensions */
    if (want_native_buffer) {
        PFN_vkEnumerateDeviceExtensionProperties edep = MI(c, vkEnumerateDeviceExtensionProperties);
        uint32_t nd = 0; edep(c->pd, NULL, &nd, NULL);
        VkExtensionProperties* dx = malloc(nd * sizeof *dx);
        edep(c->pd, NULL, &nd, dx);
        for (uint32_t i = 0; i < nd; i++)
            if (!strcmp(dx[i].extensionName, "VK_ANDROID_native_buffer")) c->has_native_buffer = 1;
        free(dx);
    }

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = c->qfi, .queueCount = 1, .pQueuePriorities = &prio };
    const char* dext[32]; uint32_t nde = 0;
    if (c->has_native_buffer) dext[nde++] = "VK_ANDROID_native_buffer";
    for (uint32_t i = 0; i < n_extra && nde < 32; i++) dext[nde++] = extra[i];
    VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci,
        .enabledExtensionCount = nde,
        .ppEnabledExtensionNames = nde ? dext : NULL };
    r = MI(c, vkCreateDevice)(c->pd, &dci, NULL, &c->dev);
    if (r) { fprintf(stderr, "mali error: vkCreateDevice failed = %d\n", r); return 7; }

    c->gdpa = (PFN_vkGetDeviceProcAddr)MI(c, vkGetDeviceProcAddr);
    if (!c->gdpa) return 7;
    MD(c, vkGetDeviceQueue)(c->dev, c->qfi, 0, &c->queue);
    
    if (mali_dbg_enabled()) {
        fprintf(stderr, "Device initialized successfully (queue family %u, native_buffer=%d, extra=%u)\n",
               c->qfi, c->has_native_buffer, n_extra);

        static const char* const inst_fns[] = {
            "vkGetPhysicalDeviceFeatures2", "vkGetPhysicalDeviceFeatures2KHR",
            "vkGetPhysicalDeviceProperties2", "vkGetPhysicalDeviceProperties2KHR",
            "vkEnumeratePhysicalDeviceGroups", "vkGetPhysicalDeviceMemoryProperties2" };
        static const char* const dev_fns[] = {
            "vkGetSemaphoreFdKHR", "vkGetMemoryFdPropertiesKHR",
            "vkGetAndroidHardwareBufferPropertiesANDROID", "vkGetImageSubresourceLayout",
            "vkWaitSemaphores", "vkWaitSemaphoresKHR", "vkSignalSemaphore",
            "vkGetSemaphoreCounterValue", "vkCmdBeginRenderPass2", "vkCmdBeginRenderPass2KHR",
            "vkCreateRenderPass2", "vkCreateRenderPass2KHR", "vkGetBufferDeviceAddress",
            "vkResetQueryPool", "vkResetQueryPoolEXT" };
        mali_report(c, inst_fns, sizeof inst_fns / sizeof *inst_fns, 0);
        mali_report(c, dev_fns,  sizeof dev_fns  / sizeof *dev_fns,  1);
    }
    return 0;
}

static int mali_init(mali_ctx* c, const char* path, int want_native_buffer) {
    return mali_init_ex(c, path, want_native_buffer, NULL, 0);
}

#endif
