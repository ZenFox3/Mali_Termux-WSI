/*
 * mali_native_fence.h - Native Fence FD & ANativeWindowBuffer Helper for Mali GPUs
 *
 * Exposes hidden EGL_ANDROID_native_fence_sync functionality from Mali vendor
 * drivers (ARM Bifrost / Valhall, DDK r32p1 - r43p0 tested).
 *
 * Bypasses AOSP linking errors ("cannot locate symbol eglDupNativeFenceFDANDROID")
 * and provides explicit hardware synchronization for X11 DRI3 / Wayland.
 *
 * BACKGROUND (what we learned from reverse engineering):
 *   - ARM hides eglDupNativeFenceFDANDROID from the exported symbol table (nm -D),
 *     but the function exists in .text and is reachable via eglGetProcAddress().
 *   - The `type` parameter of eglCreateSyncKHR must be EGL_SYNC_NATIVE_FENCE_ANDROID
 *     (0x3144), NOT an attribute! This is a very common pitfall: eglCreateSyncKHR
 *     returns EGL_BAD_ATTRIBUTE if 0x3144 is passed inside the attribute list.
 *   - Calling eglDupNativeFenceFDANDROID on a regular EGL_SYNC_FENCE_KHR sync
 *     returns -1 (EGL_NO_NATIVE_FENCE_FD_ANDROID), which is correct behavior.
 *   - On some MediaTek DDKs, destroying the EGLSyncKHR before the fd is consumed
 *     by the downstream consumer (X11 / Wayland) can cause the fd to become stale
 *     or, worse, trigger an internal assert in the driver. Keep the sync handle
 *     alive until the consumer has finished with the fd.
 *
 * OWNERSHIP:
 *   mali_fence_t owns BOTH the fd AND the EGLSyncKHR. The caller must call
 *   mali_fence_release() only AFTER the fd has been consumed by X11 (typically
 *   after xcb_present_pixmap has queued the frame and IDLE_NOTIFY was received,
 *   or after the first frame drawn).
 */

#ifndef MALI_NATIVE_FENCE_H
#define MALI_NATIVE_FENCE_H

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <dlfcn.h>
#include <poll.h>
#include <errno.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * EGL constants for EGL_ANDROID_native_fence_sync
 * ========================================================================== */

#ifndef EGL_SYNC_NATIVE_FENCE_ANDROID
#define EGL_SYNC_NATIVE_FENCE_ANDROID          0x3144  /* used as <type> */
#endif

#ifndef EGL_SYNC_NATIVE_FENCE_FD_ANDROID
#define EGL_SYNC_NATIVE_FENCE_FD_ANDROID       0x3143  /* used as attribute (import path) */
#endif

#ifndef EGL_NO_NATIVE_FENCE_FD_ANDROID
#define EGL_NO_NATIVE_FENCE_FD_ANDROID         -1
#endif

/* Standard EGL types (avoid pulling in <EGL/egl.h> to keep header standalone) */
typedef void*         EGLDisplay;
typedef void*         EGLSyncKHR;
typedef int           EGLint;
typedef unsigned int  EGLBoolean;

/* ============================================================================
 * Function pointer typedefs
 * ========================================================================== */

typedef EGLSyncKHR  (*PFN_eglCreateSyncKHR_t)(EGLDisplay dpy, unsigned int type,
                                              const EGLint* attrib_list);
typedef EGLBoolean  (*PFN_eglDestroySyncKHR_t)(EGLDisplay dpy, EGLSyncKHR sync);
typedef EGLint      (*PFN_eglDupNativeFenceFDANDROID_t)(EGLDisplay dpy, EGLSyncKHR sync);
typedef void*       (*PFN_eglGetProcAddress_t)(const char* procname);
typedef void        (*PFN_glFlush_t)(void);

/* ============================================================================
 * ANativeWindowBuffer layout (ARM64 / AOSP 13-15)
 *
 * This layout is stable across AOSP releases. Verified against:
 *   - /system/include/ui/ANativeWindowBuffer.h
 *   - live inspection on Mali-G52 MC2, Mali-G78, MediaTek DDK r32p1-r43p0
 *
 * NOTE: `handle` offset is validated at runtime by mali_find_anwb_handle()
 * because MediaTek has been observed to shift the reserved[] block on some
 * DDK versions. Do not hardcode 0x98 unless the diagnostic confirms it.
 * ========================================================================== */

typedef struct {
    int      magic;             /* +0x00 */
    int      version;           /* +0x04 */
    void*    reserved_base[4];  /* +0x08 .. +0x27 */
    void*    incRef;            /* +0x28 */
    void*    decRef;            /* +0x30 */
} mali_anwb_base_t;             /* 56 bytes = 0x38 */

typedef struct {
    mali_anwb_base_t common;    /* +0x00 .. +0x37 */
    int              width;     /* +0x38 */
    int              height;    /* +0x3c */
    int              stride;    /* +0x40 (pixels, not bytes) */
    int              format;    /* +0x44 (1=RGBA_8888, 5=BGRA_8888) */
    int              usage_deprecated;  /* +0x48 */
    uintptr_t        layerCount;        /* +0x50 */
    void*            reserved_anwb[8];  /* +0x58 .. +0x97 (standard AOSP) */
    const void*      handle;            /* +0x98 (standard AOSP; verify at runtime) */
} mali_anwb_t;

typedef mali_anwb_t* (*PFN_AHardwareBuffer_to_ANativeWindowBuffer_t)(void* ahb);

/* native_handle_t minimal (validated via version field) */
typedef struct {
    int version;
    int numFds;
    int numInts;
} mali_native_handle_head_t;

/* ============================================================================
 * Fence context
 * ========================================================================== */

typedef struct {
    void*                                   mali_handle;   /* dlopen() handle of libGLES_mali.so */
    PFN_eglCreateSyncKHR_t                  eglCreateSyncKHR;
    PFN_eglDestroySyncKHR_t                 eglDestroySyncKHR;
    PFN_eglDupNativeFenceFDANDROID_t        eglDupNativeFenceFDANDROID;
    PFN_glFlush_t                           glFlush;
    PFN_AHardwareBuffer_to_ANativeWindowBuffer_t to_anwb;
    int                                     anwb_handle_offset;  /* discovered at init, e.g. 0x98 */
} mali_fence_ctx_t;

/* Fence with proper lifetime management: caller must call mali_fence_release()
 * only AFTER the fd has been consumed by X11 / Wayland. */
typedef struct {
    int         fd;    /* kernel sync_file fd, or -1 */
    EGLSyncKHR  sync;  /* EGL sync handle; must live until fd is released */
} mali_fence_t;

/* ============================================================================
 * Context initialization
 * ========================================================================== */

/*
 * mali_fence_init - Resolve hidden symbols and probe ANWB handle offset.
 *
 * mali_dl_handle: dlopen() handle of /vendor/lib64/egl/libGLES_mali.so
 *                 (obtained from mali_sphal_load()).
 *
 * Return 1 on success, 0 on failure.
 */
static inline int mali_fence_init(mali_fence_ctx_t* ctx, void* mali_dl_handle) {
    if (!ctx || !mali_dl_handle) return 0;
    memset(ctx, 0, sizeof(*ctx));
    ctx->mali_handle = mali_dl_handle;
    ctx->anwb_handle_offset = -1;

    PFN_eglGetProcAddress_t gpa =
        (PFN_eglGetProcAddress_t)dlsym(mali_dl_handle, "eglGetProcAddress");

    ctx->eglCreateSyncKHR  = (PFN_eglCreateSyncKHR_t)dlsym(mali_dl_handle, "eglCreateSyncKHR");
    ctx->eglDestroySyncKHR = (PFN_eglDestroySyncKHR_t)dlsym(mali_dl_handle, "eglDestroySyncKHR");

    /* glFlush is usually exported, but on some DDKs it only appears via gpa. */
    ctx->glFlush = (PFN_glFlush_t)dlsym(mali_dl_handle, "glFlush");
    if (!ctx->glFlush && gpa) ctx->glFlush = (PFN_glFlush_t)gpa("glFlush");

    /* Hidden symbol: MUST go through eglGetProcAddress (not in .dynsym). */
    if (gpa) {
        ctx->eglDupNativeFenceFDANDROID =
            (PFN_eglDupNativeFenceFDANDROID_t)gpa("eglDupNativeFenceFDANDROID");
    }

    /* C++ mangled symbol for AHB -> ANativeWindowBuffer. */
    void* libnative = dlopen("libnativewindow.so", RTLD_NOW | RTLD_LOCAL);
    if (!libnative) libnative = dlopen("/system/lib64/libnativewindow.so", RTLD_NOW | RTLD_LOCAL);
    if (libnative) {
        ctx->to_anwb = (PFN_AHardwareBuffer_to_ANativeWindowBuffer_t)dlsym(
            libnative,
            "_ZN7android38AHardwareBuffer_to_ANativeWindowBufferEP15AHardwareBuffer");
    }

    return (ctx->eglCreateSyncKHR && ctx->eglDupNativeFenceFDANDROID && ctx->glFlush) ? 1 : 0;
}

/* ============================================================================
 * Fence lifecycle
 * ========================================================================== */

/*
 * mali_fence_create - Create a native fence and dupe its fd.
 *
 * Returns {fd, sync}. On failure returns {-1, NULL}.
 *
 * CRITICAL: The caller MUST call mali_fence_release() only AFTER the fd
 * has been consumed by X11 / Wayland. Destroying the sync too early has
 * been observed to invalidate the fd on MediaTek DDKs.
 */
static inline mali_fence_t mali_fence_create(mali_fence_ctx_t* ctx, EGLDisplay dpy) {
    mali_fence_t out = { .fd = -1, .sync = NULL };
    if (!ctx || !ctx->eglCreateSyncKHR || !ctx->eglDupNativeFenceFDANDROID || !ctx->glFlush)
        return out;

    /* Capture any pending GL commands so the fence covers them. */
    ctx->glFlush();

    /* Create with type = EGL_SYNC_NATIVE_FENCE_ANDROID (not attribute!). */
    out.sync = ctx->eglCreateSyncKHR(dpy, EGL_SYNC_NATIVE_FENCE_ANDROID, NULL);
    if (!out.sync) return out;

    /* Ensure the job is submitted to the GPU before duping the fd. */
    ctx->glFlush();

    /* Extract the sync_file fd. Returns -1 if the sync is not native. */
    out.fd = ctx->eglDupNativeFenceFDANDROID(dpy, out.sync);
    if (out.fd < 0) {
        if (ctx->eglDestroySyncKHR) ctx->eglDestroySyncKHR(dpy, out.sync);
        out.sync = NULL;
    }
    return out;
}

/*
 * mali_fence_release - Destroy the EGL sync and close our fd copy.
 *
 * Call ONLY after the downstream consumer (X11 / Wayland) has finished
 * waiting on the fence fd.
 */
static inline void mali_fence_release(mali_fence_ctx_t* ctx, EGLDisplay dpy, mali_fence_t* f) {
    if (!f) return;
    if (f->fd >= 0) { close(f->fd); f->fd = -1; }
    if (f->sync && ctx && ctx->eglDestroySyncKHR) {
        ctx->eglDestroySyncKHR(dpy, f->sync);
        f->sync = NULL;
    }
}

/* ============================================================================
 * Fence waiting
 * ========================================================================== */

/*
 * mali_fence_wait - Block until the fence is signaled, or timeout.
 *
 * timeout_ms: 0 = non-blocking; -1 = infinite.
 * Returns 1 if signaled, 0 on timeout, -1 on error.
 */
static inline int mali_fence_wait(int fd, int timeout_ms) {
    if (fd < 0) return -1;
    struct pollfd p = { .fd = fd, .events = POLLIN, .revents = 0 };
    int r;
    do { r = poll(&p, 1, timeout_ms); } while (r < 0 && errno == EINTR);
    if (r < 0) return -1;
    return (r > 0) ? 1 : 0;
}

/* ============================================================================
 * ANativeWindowBuffer conversion & introspection
 * ========================================================================== */

/*
 * mali_ahb_to_anwb - Convert AHardwareBuffer* to ANativeWindowBuffer*.
 *
 * This is the exact same 16-byte offset trick ARM uses internally:
 * ANativeWindowBuffer lives at (AHardwareBuffer* + sizeof(common_header)).
 */
static inline mali_anwb_t* mali_ahb_to_anwb(mali_fence_ctx_t* ctx, void* ahb) {
    if (!ctx || !ctx->to_anwb || !ahb) return NULL;
    return ctx->to_anwb(ahb);
}

/*
 * mali_anwb_find_handle - Discover the native_handle_t* offset at runtime.
 *
 * Scans offsets 0x58..0xA8 in 8-byte strides for a pointer whose first int
 * equals sizeof(native_handle_t) (0x38 on ARM64, sometimes 0x30).
 *
 * Call ONCE per DDK version. Caches the result in ctx->anwb_handle_offset.
 * Returns the offset on success, -1 if not found.
 */
static inline int mali_anwb_find_handle(mali_fence_ctx_t* ctx, mali_anwb_t* anwb) {
    if (!ctx || !anwb) return -1;
    if (ctx->anwb_handle_offset > 0) return ctx->anwb_handle_offset;

    for (int off = 0x58; off <= 0xA8; off += 8) {
        void* candidate = *(void**)((char*)anwb + off);
        if (!candidate) continue;
        /* A valid ART heap pointer on arm64 typically starts with 0xb4... */
        if ((((uintptr_t)candidate) >> 40) != 0xb4) continue;

        mali_native_handle_head_t* h = (mali_native_handle_head_t*)candidate;
        if (h->version == 0x38 || h->version == 0x30) {
            ctx->anwb_handle_offset = off;
            return off;
        }
    }
    return -1;
}

/*
 * mali_anwb_get_handle - Return native_handle_t* using the discovered offset.
 */
static inline const void* mali_anwb_get_handle(mali_fence_ctx_t* ctx, mali_anwb_t* anwb) {
    int off = mali_anwb_find_handle(ctx, anwb);
    if (off < 0) return NULL;
    return *(const void**)((char*)anwb + off);
}

/* ============================================================================
 * X11 / DRI3 integration helper
 * ========================================================================== */

/*
 * mali_fence_to_xcb - Prepare a fence fd for xcb_present_pixmap.
 *
 * DRI3 takes ownership of the fd passed to xcb_present_pixmap (the X server
 * dups it internally). We therefore dup() before passing, and keep our own
 * copy alive until mali_fence_release() is called.
 *
 * Returns a dup'd fd that the caller may pass to xcb_present_pixmap.
 * Returns -1 on failure.
 *
 * Example usage:
 *     mali_fence_t f = mali_fence_create(&ctx, dpy);
 *     int present_fd = mali_fence_to_xcb(&f);
 *     xcb_present_pixmap(..., NULL, present_fd);
 *     // Later, when IDLE_NOTIFY arrives for this frame:
 *     mali_fence_release(&ctx, dpy, &f);
 */
static inline int mali_fence_to_xcb(const mali_fence_t* f) {
    if (!f || f->fd < 0) return -1;
    return dup(f->fd);
}

/* ============================================================================
 * Diagnostic helper - safe to call once at startup
 * ========================================================================== */

/*
 * mali_fence_dump_anwb - Print ANativeWindowBuffer fields for debugging.
 */
static inline void mali_fence_dump_anwb(mali_fence_ctx_t* ctx, mali_anwb_t* anwb) {
    if (!anwb) { fprintf(stderr, "[fence] ANWB is NULL\n"); return; }
    fprintf(stderr, "[fence] ANWB @ %p:\n", anwb);
    fprintf(stderr, "[fence]   width  = %d\n", anwb->width);
    fprintf(stderr, "[fence]   height = %d\n", anwb->height);
    fprintf(stderr, "[fence]   stride = %d (pixels)\n", anwb->stride);
    fprintf(stderr, "[fence]   format = %d\n", anwb->format);
    if (ctx) {
        int off = mali_anwb_find_handle(ctx, anwb);
        if (off > 0) {
            const void* h = *(const void**)((char*)anwb + off);
            fprintf(stderr, "[fence]   handle @ +0x%x = %p\n", off, h);
        } else {
            fprintf(stderr, "[fence]   handle offset NOT detected\n");
        }
    }
}

#ifdef __cplusplus
}
#endif

#endif /* MALI_NATIVE_FENCE_H */