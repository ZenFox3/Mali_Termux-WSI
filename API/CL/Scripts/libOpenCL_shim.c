/*
 * libOpenCL_shim.c - libOpenCL.so.1 for Mali: AArch64 trampolines to cl* in libGLES_mali.so (SPHAL).
 * Regular OpenCL apps (clinfo, pyopencl, ffmpeg...) see the driver without an ICD loader.
 */
#define _GNU_SOURCE
#include "mali_sphal.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <dlfcn.h>

#if !defined(__aarch64__) && !defined(CL_SHIM_ANY_ARCH)
#error "Trampolines are written for AArch64"
#endif

#define EXPORT __attribute__((visibility("default")))
#define HIDDEN __attribute__((visibility("hidden")))

#define X(n) HIDDEN void* p_##n;
#include "cl_list.h"
#undef X

#define X(n)                                              \
    EXPORT __attribute__((naked)) void n(void) {          \
        __asm__ volatile(                                 \
            "adrp x16, p_" #n "\n\t"                      \
            "ldr  x16, [x16, #:lo12:p_" #n "]\n\t"        \
            "br   x16\n");                                \
    }
#include "cl_list.h"
#undef X

__attribute__((constructor))
static void cl_shim_init(void) {
    void* h = mali_sphal_load(MALI_SPHAL_DEFAULT);
    if (!h) { fprintf(stderr, "[cl-shim] failed to load Mali driver\n"); return; }
#define X(n) p_##n = dlsym(h, #n);
#include "cl_list.h"
#undef X
}
