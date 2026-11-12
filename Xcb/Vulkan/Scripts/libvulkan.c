/* libvulkan.c
libvulkan.so.1 replacement: applications linking with -lvulkan find all symbols here.
   Each Vulkan function is an aarch64 stub that jumps through a lookup table (signature-independent);
   the table is populated with real Mali driver (or WSI layer) function pointers
   after vkCreateInstance / vkCreateDevice. Single instance and single device per process.

   Steps:
     sh gen_shim.sh $PREFIX/include > vulkan_calls.inc
     LD_LIBRARY_PATH=<path_to_libvulkan.so.1> vulkaninfo --summary
   Without Xlib: remove -DWSI_XLIB, -lX11, -lX11-xcb.
   Env vars: MALI_ICD_LIB (driver path), WSI_CPU=1, WSI_SYNC=1, WSI_PRESENT=1. */
#ifndef __aarch64__
#error "Stubs are written specifically for AArch64"
#endif
#include "../Include/vulkan_wsi_x11.h"
#include <signal.h>
#include <ucontext.h>

#define SHIM_MAX 2048
#define S2(x) #x
#define S(x) S2(x)

/* Jump table; hidden visibility, referenced by name in assembly */
__attribute__((visibility("hidden"))) void* g_tab[SHIM_MAX];
/* Index of the last Vulkan function called through a stub (for SIGSEGV diagnostics) */
__attribute__((visibility("hidden"))) volatile unsigned g_last = 0xffffffffu;

/* Stub: loads pointer from g_tab[IDX] and branches to it. x16/x17 are scratch veneer
   registers, so function arguments remain untouched. If the slot is empty: mov w17,IDX
   and branches to shim_unresolved, which logs the missing function name. */
#define STUB(name, idx)                                                         \
    __asm__(".text\n"                                                           \
            ".globl " #name "\n"                                                \
            ".type " #name ",%function\n"                                       \
            ".p2align 2\n"                                                      \
            #name ":\n"                                                         \
            "  adrp x16, g_last\n"                                              \
            "  mov  w17, #" S(idx) "\n"                                         \
            "  str  w17, [x16, :lo12:g_last]\n"                                 \
            "  adrp x16, g_tab+" S(idx) "*8\n"                                  \
            "  ldr  x16, [x16, :lo12:g_tab+" S(idx) "*8]\n"                     \
            "  cbz  x16, 1f\n"                                                  \
            "  br   x16\n"                                                      \
            "1:\n"                                                              \
            "  mov  w17, #" S(idx) "\n"                                         \
            "  b    shim_unresolved\n"                                          \
            ".size " #name ", .-" #name "\n");

#define F(name, idx) STUB(name, idx)
#include "../Include/vulkan_calls.inc"
#undef F

__asm__(".text\n"
        ".hidden shim_unresolved\n"
        ".type shim_unresolved,%function\n"
        ".p2align 2\n"
        "shim_unresolved:\n"
        "  mov w0, w17\n"
        "  b   shim_unresolved_c\n");

#define F(name, idx) [idx] = #name,
static const char* const g_names[] = {
#include "../Include/vulkan_calls.inc"
};
#undef F
#define NF (sizeof g_names / sizeof g_names[0])
_Static_assert(sizeof g_names / sizeof g_names[0] <= SHIM_MAX, "Increase SHIM_MAX");
_Static_assert(sizeof g_names / sizeof g_names[0] >= 300,
               "vulkan_calls.inc is empty or incomplete: run gen_shim_v2.sh");

/* Called from stub when a function is not implemented by the driver; index comes via w0 */
__attribute__((visibility("hidden"), used))
long shim_unresolved_c(unsigned idx) {
    fprintf(stderr, "vulkan-shim: %s is not implemented by driver\n",
            idx < NF && g_names[idx] ? g_names[idx] : "?");
    return VK_ERROR_EXTENSION_NOT_PRESENT;
}

static mali_ctx       g_ctx;
static pthread_once_t g_once = PTHREAD_ONCE_INIT;
static int            g_ok;
static int            g_trace; /* VK_SHIM_TRACE=1 */

static void fill_gipa(VkInstance inst) {
    for (size_t i = 0; i < NF; i++) {
        PFN_vkVoidFunction p = wsi_gipa(inst, g_names[i]);
        if (p) __atomic_store_n(&g_tab[i], (void*)p, __ATOMIC_RELEASE);
    }
}

static void fill_gdpa(VkDevice dev) {
    for (size_t i = 0; i < NF; i++) {
        PFN_vkVoidFunction p = wsi_gdpa(dev, g_names[i]);
        if (p) __atomic_store_n(&g_tab[i], (void*)p, __ATOMIC_RELEASE);
    }
}

static char g_last_lookup[96]; /* Last function name resolved via GIPA/GDPA */

/* Diagnostic handler (VK_SHIM_TRACE=1): logs program state before crashing */
static void segv_handler(int sig, siginfo_t* si, void* ctx) {
    (void)sig;
    ucontext_t* uc = (ucontext_t*)ctx;
    unsigned i = g_last;
    static const char msg[] = "vulkan-shim: SIGSEGV\n";
    (void)!write(2, msg, sizeof msg - 1);
    char buf[512];
    int n = snprintf(buf, sizeof buf,
        "  fault=%p pc=0x%llx lr=0x%llx\n"
        "  last Vulkan function called via stub: %s\n"
        "  last GIPA/GDPA lookup: %s\n",
        si->si_addr, (unsigned long long)uc->uc_mcontext.pc,
        (unsigned long long)uc->uc_mcontext.regs[30],
        i < NF ? g_names[i] : "(none)", g_last_lookup[0] ? g_last_lookup : "(none)");
    if (n > 0) (void)!write(2, buf, (size_t)n);
}

static void shim_init_once(void) {
    g_trace = getenv("VK_SHIM_TRACE") != NULL;
    if (g_trace) {
        struct sigaction sa;
        memset(&sa, 0, sizeof sa);
        sa.sa_sigaction = segv_handler;
        sa.sa_flags = SA_SIGINFO | SA_RESETHAND;
        sigaction(SIGSEGV, &sa, NULL);
    }
    const char* path = getenv("MALI_ICD_LIB");
    if (!path) path = "/vendor/lib64/egl/libGLES_mali.so";
    int rc = mali_load(&g_ctx, path);
    if (rc) { fprintf(stderr, "vulkan-shim: mali_load(%s) = %d\n", path, rc); return; }
    wsi_attach(&g_ctx);
    fill_gipa(NULL); /* Global functions: enumerations, EnumerateInstanceVersion */
    g_ok = 1;
    if (g_trace) {
        uint32_t n = 0;
        wsi_EnumerateInstanceExtensionProperties(NULL, &n, NULL);
        VkExtensionProperties* e = malloc((n + 1) * sizeof *e);
        if (e && !wsi_EnumerateInstanceExtensionProperties(NULL, &n, e))
            for (uint32_t i = 0; i < n; i++)
                fprintf(stderr, "vulkan-shim: instance ext %s v%u\n", e[i].extensionName, e[i].specVersion);
        free(e);
    }
}
static void shim_init(void) { pthread_once(&g_once, shim_init_once); }

/* Load the shim early so vkEnumerateInstance* works prior to vkCreateInstance */
__attribute__((constructor)) static void shim_ctor(void) { shim_init(); }

#define EXPORT __attribute__((visibility("default")))

EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkCreateInstance(const VkInstanceCreateInfo* ci,
        const VkAllocationCallbacks* a, VkInstance* out) {
    shim_init();
    if (!g_ok) return VK_ERROR_INITIALIZATION_FAILED;
    VkResult r = wsi_CreateInstance(ci, a, out);
    if (!r) fill_gipa(*out);
    if (g_trace) fprintf(stderr, "vulkan-shim: vkCreateInstance = %d\n", r);
    return r;
}

EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkCreateDevice(VkPhysicalDevice pd,
        const VkDeviceCreateInfo* ci, const VkAllocationCallbacks* a, VkDevice* out) {
    shim_init();
    if (!g_ok) return VK_ERROR_INITIALIZATION_FAILED;
    VkResult r = wsi_CreateDevice(pd, ci, a, out);
    if (!r) fill_gdpa(*out);
    if (g_trace) fprintf(stderr, "vulkan-shim: vkCreateDevice = %d\n", r);
    return r;
}

static PFN_vkVoidFunction trace_pa(const char* who, const char* n, PFN_vkVoidFunction f) {
    if (g_trace) {
        strncpy(g_last_lookup, n, sizeof g_last_lookup - 1);
        fprintf(stderr, "vulkan-shim: %s(%s) -> %s\n", who, n, f ? "ok" : "NULL");
    }
    return f;
}
EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(VkDevice d, const char* n);

EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance i, const char* n) {
    shim_init();
    if (!g_ok || !n) return NULL;
    if (!strcmp(n, "vkCreateInstance")) return trace_pa("GIPA", n, (PFN_vkVoidFunction)vkCreateInstance);
    if (!strcmp(n, "vkCreateDevice"))   return trace_pa("GIPA", n, (PFN_vkVoidFunction)vkCreateDevice);
    if (!strcmp(n, "vkGetDeviceProcAddr"))   return trace_pa("GIPA", n, (PFN_vkVoidFunction)vkGetDeviceProcAddr);
    if (!strcmp(n, "vkGetInstanceProcAddr")) return trace_pa("GIPA", n, (PFN_vkVoidFunction)vkGetInstanceProcAddr);
    return trace_pa("GIPA", n, wsi_gipa(i, n));
}

EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(VkDevice d, const char* n) {
    shim_init();
    if (!g_ok || !n) return NULL;
    if (!strcmp(n, "vkGetDeviceProcAddr")) return trace_pa("GDPA", n, (PFN_vkVoidFunction)vkGetDeviceProcAddr);
    return trace_pa("GDPA", n, wsi_gdpa(d, n));
}
