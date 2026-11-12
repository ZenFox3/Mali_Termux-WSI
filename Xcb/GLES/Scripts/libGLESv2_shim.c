/*
 * libGLESv2_shim.c - Universal shim for GLES 1.x / 2.0 / 3.x over Mali SPHAL
 * with direct WSI: AHardwareBuffer (linear dma-buf) + DRI3 + Present (IDLE_NOTIFY), zero CPU copy.
 *
 * Pipeline: app -> offscreen FBO -> GPU pass (flip Y + swizzle) -> AHB -> DRI3 pixmap -> Present
 * Lazy initialization (first glBindFramebuffer(0) or first eglSwapBuffers with GLES3 context).
 *
 * Link: -ldl -lxcb -lxcb-dri3 -lxcb-present
 * Env:  GL_DEBUG=1 | SHIM_DMABUF=0 | SHIM_SYNC=flush | SHIM_PRESENT_COPY=1 | SHIM_FORMAT=1|5
 *       SHIM_W / SHIM_H (default 1280x720) | SHIM_REAL_SWAP=1 (also call real eglSwapBuffers)
 */
#define _GNU_SOURCE
#include "mali_sphal.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>
#include <time.h>
#include <pthread.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <dlfcn.h>

typedef unsigned int EGLBoolean;
typedef void* EGLDisplay;
typedef void* EGLSurface;
#ifndef EGL_TRUE
#define EGL_TRUE 1
#endif

#include <xcb/xcb.h>
#include <xcb/dri3.h>
#include <xcb/present.h>

#if !defined(__aarch64__) && !defined(GLES_SHIM_ANY_ARCH)
#error "Trampolines are written exclusively for AArch64 (ARM64) architecture"
#endif

#define EXPORT __attribute__((visibility("default")))
#define HIDDEN __attribute__((visibility("hidden")))

#define NUM_BUF 3

/* ---------- GL Constants ---------- */
#define C_TEXTURE_2D        0x0DE1
#define C_FRAMEBUFFER       0x8D40
#define C_RENDERBUFFER      0x8D41
#define C_READ_FB           0x8CA8
#define C_DRAW_FB           0x8CA9
#define C_DRAW_FB_BINDING   0x8CA6
#define C_READ_FB_BINDING   0x8CAA
#define C_RB_BINDING        0x8CA7
#define C_COLOR_ATT0        0x8CE0
#define C_DEPTH_STENCIL_ATT 0x821A
#define C_FB_COMPLETE       0x8CD5
#define C_DEPTH24_STENCIL8  0x88F0
#define C_RGBA8             0x8058
#define C_RGBA              0x1908
#define C_UBYTE             0x1401
#define C_FLOAT             0x1406
#define C_NEAREST           0x2600
#define C_MIN_FILTER        0x2801
#define C_MAG_FILTER        0x2800
#define C_WRAP_S            0x2802
#define C_WRAP_T            0x2803
#define C_CLAMP_EDGE        0x812F
#define C_TEXTURE0          0x84C0
#define C_ACTIVE_TEXTURE    0x84E0
#define C_TEX_BINDING_2D    0x8069
#define C_SAMPLER_BINDING   0x8919
#define C_CURRENT_PROGRAM   0x8B8D
#define C_VAO_BINDING       0x85B5
#define C_ARRAY_BUFFER      0x8892
#define C_ARRAY_BUF_BINDING 0x8894
#define C_STATIC_DRAW       0x88E4
#define C_VIEWPORT          0x0BA2
#define C_COLOR_WRITEMASK   0x0C23
#define C_SCISSOR_TEST      0x0C11
#define C_DEPTH_TEST        0x0B71
#define C_STENCIL_TEST      0x0B90
#define C_BLEND             0x0BE2
#define C_CULL_FACE         0x0B44
#define C_TRI_STRIP         0x0005
#define C_VERTEX_SHADER     0x8B31
#define C_FRAGMENT_SHADER   0x8B30
#define C_COMPILE_STATUS    0x8B81
#define C_LINK_STATUS       0x8B82
#define C_VERSION           0x1F02

/* ---------- Debug Logging Helper ---------- */
static inline int gles_debug_enabled(void) {
    const char* env_gl = getenv("GL_DEBUG");
    const char* env_mali = getenv("MALI_DEBUG");
    const char* env_egl = getenv("EGL_DEBUG");
    return (env_gl && env_gl[0] != '0') || 
           (env_mali && env_mali[0] != '0') || 
           (env_egl && env_egl[0] != '0');
}

/* ---------- AHardwareBuffer (manual declarations) ---------- */
typedef struct {
    uint32_t width, height, layers, format;
    uint64_t usage;
    uint32_t stride, rfu0;
    uint64_t rfu1;
} AHBDesc;
typedef struct { int version, numFds, numInts; int data[]; } native_handle_t;

#define AHB_USAGE_CPU_READ_RARELY 0x2ULL   /* forces linear layout (no AFBC) */
#define AHB_USAGE_GPU_SAMPLED     0x100ULL
#define AHB_USAGE_GPU_COLOR_OUT   0x200ULL

static int  (*ahb_alloc)(const AHBDesc*, void**);
static void (*ahb_desc)(const void*, AHBDesc*);
static const native_handle_t* (*ahb_handle)(const void*);
static void (*ahb_release)(void*);
static int ahb_state;
static void* ahb_lib;

static int ahb_init(void) {
    if (ahb_state) return ahb_state > 0 ? 0 : -1;
    void* h = dlopen("libnativewindow.so", RTLD_NOW);
    if (!h) h = dlopen("/system/lib64/libnativewindow.so", RTLD_NOW);
    if (h) {
        ahb_lib = h;
        *(void**)&ahb_alloc   = dlsym(h, "AHardwareBuffer_allocate");
        *(void**)&ahb_desc    = dlsym(h, "AHardwareBuffer_describe");
        *(void**)&ahb_handle  = dlsym(h, "AHardwareBuffer_getNativeHandle");
        *(void**)&ahb_release = dlsym(h, "AHardwareBuffer_release");
    }
    ahb_state = (ahb_alloc && ahb_desc && ahb_handle && ahb_release) ? 1 : -1;
    if (ahb_state < 0 && gles_debug_enabled()) {
        fprintf(stderr, "[gles-shim] AHardwareBuffer unavailable\n");
    }
    return ahb_state > 0 ? 0 : -1;
}

/* ---------- Global State ---------- */
typedef struct {
    void* ab;
    xcb_pixmap_t pixmap;
    void* image;
    unsigned tex, fbo;
    int busy;
} Slot;

static void* G_mali;
static void* (*real_gpa)(const char*);
static void* (*m_eglGetCurrentContext)(void);
static void* (*m_eglGetCurrentDisplay)(void);
static void* (*m_eglCreateImageKHR)(void*, void*, unsigned, void*, const int*);
static unsigned (*m_eglDestroyImageKHR)(void*, void*);

static Slot g_slots[NUM_BUF];
static int g_current_slot, g_inited, g_failed, g_no_idle, g_swz, g_fmt;
static int g_w = 1280, g_h = 720;
static uint32_t g_serial, g_eid;
static unsigned g_app_tex, g_app_fbo, g_app_rbo, g_prog, g_vao, g_vbo;
static void* g_ctx;
static void* g_dpy;
static xcb_connection_t* g_conn;
static xcb_window_t g_win;
static xcb_special_event_t* g_se;

/* 1. Internal pointer table to real Mali driver */
#define X(n) HIDDEN void* p_##n;
#include "gles_list.h"
#undef X

/* 2. Bare-metal AArch64 trampolines */
#define X(n)                                              \
    EXPORT __attribute__((naked)) void n(void) {          \
        __asm__ volatile(                                 \
            "adrp x16, p_" #n "\n\t"                      \
            "ldr  x16, [x16, #:lo12:p_" #n "]\n\t"        \
            "br   x16\n");                                \
    }
#include "gles_list.h"
#undef X

static void (*real_bind)(unsigned, unsigned);
static void (*real_bind_oes)(unsigned, unsigned);
static EGLBoolean (*real_swap_buffers)(EGLDisplay, EGLSurface);

/* ---------- Internal GL functions (bypass exports) ---------- */
typedef struct {
    void (*genTextures)(int, unsigned*);
    void (*deleteTextures)(int, const unsigned*);
    void (*bindTexture)(unsigned, unsigned);
    void (*texImage2D)(unsigned, int, int, int, int, int, unsigned, unsigned, const void*);
    void (*texParameteri)(unsigned, unsigned, int);
    void (*genFramebuffers)(int, unsigned*);
    void (*deleteFramebuffers)(int, const unsigned*);
    void (*bindFramebuffer)(unsigned, unsigned);
    void (*framebufferTexture2D)(unsigned, unsigned, unsigned, unsigned, int);
    unsigned (*checkFramebufferStatus)(unsigned);
    void (*genRenderbuffers)(int, unsigned*);
    void (*deleteRenderbuffers)(int, const unsigned*);
    void (*bindRenderbuffer)(unsigned, unsigned);
    void (*renderbufferStorage)(unsigned, unsigned, int, int);
    void (*framebufferRenderbuffer)(unsigned, unsigned, unsigned, unsigned);
    unsigned (*createShader)(unsigned);
    void (*shaderSource)(unsigned, int, const char* const*, const int*);
    void (*compileShader)(unsigned);
    void (*getShaderiv)(unsigned, unsigned, int*);
    void (*getShaderInfoLog)(unsigned, int, int*, char*);
    void (*deleteShader)(unsigned);
    unsigned (*createProgram)(void);
    void (*attachShader)(unsigned, unsigned);
    void (*linkProgram)(unsigned);
    void (*getProgramiv)(unsigned, unsigned, int*);
    void (*getProgramInfoLog)(unsigned, int, int*, char*);
    void (*deleteProgram)(unsigned);
    void (*useProgram)(unsigned);
    void (*genVertexArrays)(int, unsigned*);
    void (*deleteVertexArrays)(int, const unsigned*);
    void (*bindVertexArray)(unsigned);
    void (*genBuffers)(int, unsigned*);
    void (*deleteBuffers)(int, const unsigned*);
    void (*bindBuffer)(unsigned, unsigned);
    void (*bufferData)(unsigned, long, const void*, unsigned);
    void (*enableVertexAttribArray)(unsigned);
    void (*vertexAttribPointer)(unsigned, int, unsigned, unsigned char, int, const void*);
    void (*drawArrays)(unsigned, int, int);
    void (*viewport)(int, int, int, int);
    void (*scissor)(int, int, int, int);
    void (*enable)(unsigned);
    void (*disable)(unsigned);
    unsigned char (*isEnabled)(unsigned);
    void (*getIntegerv)(unsigned, int*);
    void (*colorMask)(unsigned char, unsigned char, unsigned char, unsigned char);
    void (*activeTexture)(unsigned);
    void (*bindSampler)(unsigned, unsigned);
    const unsigned char* (*getString)(unsigned);
    void (*flush)(void);
    void (*finish)(void);
} gl_fns;
static gl_fns GL;
static int gl_loaded;

static void* glp(const char* n) {
    void* p = G_mali ? dlsym(G_mali, n) : NULL;
    if (!p && real_gpa) p = real_gpa(n);
    return p;
}

static int gl_load(void) {
    if (gl_loaded) return gl_loaded > 0 ? 0 : -1;
    int miss = 0;
#define L(f, sym) do { *(void**)&GL.f = glp(sym); if (!GL.f) { if (gles_debug_enabled()) fprintf(stderr, "[gles-shim] Missing symbol: %s\n", sym); miss++; } } while (0)
    L(genTextures, "glGenTextures"); L(deleteTextures, "glDeleteTextures");
    L(bindTexture, "glBindTexture"); L(texImage2D, "glTexImage2D"); L(texParameteri, "glTexParameteri");
    L(genFramebuffers, "glGenFramebuffers"); L(deleteFramebuffers, "glDeleteFramebuffers");
    L(bindFramebuffer, "glBindFramebuffer"); L(framebufferTexture2D, "glFramebufferTexture2D");
    L(checkFramebufferStatus, "glCheckFramebufferStatus");
    L(genRenderbuffers, "glGenRenderbuffers"); L(deleteRenderbuffers, "glDeleteRenderbuffers");
    L(bindRenderbuffer, "glBindRenderbuffer"); L(renderbufferStorage, "glRenderbufferStorage");
    L(framebufferRenderbuffer, "glFramebufferRenderbuffer");
    L(createShader, "glCreateShader"); L(shaderSource, "glShaderSource"); L(compileShader, "glCompileShader");
    L(getShaderiv, "glGetShaderiv"); L(getShaderInfoLog, "glGetShaderInfoLog"); L(deleteShader, "glDeleteShader");
    L(createProgram, "glCreateProgram"); L(attachShader, "glAttachShader"); L(linkProgram, "glLinkProgram");
    L(getProgramiv, "glGetProgramiv"); L(getProgramInfoLog, "glGetProgramInfoLog");
    L(deleteProgram, "glDeleteProgram"); L(useProgram, "glUseProgram");
    L(genVertexArrays, "glGenVertexArrays"); L(deleteVertexArrays, "glDeleteVertexArrays");
    L(bindVertexArray, "glBindVertexArray");
    L(genBuffers, "glGenBuffers"); L(deleteBuffers, "glDeleteBuffers"); L(bindBuffer, "glBindBuffer");
    L(bufferData, "glBufferData"); L(enableVertexAttribArray, "glEnableVertexAttribArray");
    L(vertexAttribPointer, "glVertexAttribPointer"); L(drawArrays, "glDrawArrays");
    L(viewport, "glViewport"); L(scissor, "glScissor");
    L(enable, "glEnable"); L(disable, "glDisable"); L(isEnabled, "glIsEnabled");
    L(getIntegerv, "glGetIntegerv"); L(colorMask, "glColorMask"); L(activeTexture, "glActiveTexture");
    L(bindSampler, "glBindSampler"); L(getString, "glGetString");
    L(flush, "glFlush"); L(finish, "glFinish");
#undef L
    gl_loaded = miss ? -1 : 1;
    return miss ? -1 : 0;
}

/* ---------- X11 / Present Utilities ---------- */
__attribute__((unused)) static long now_ms(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000L + t.tv_nsec / 1000000L;
}

static void x_sync(void) {
    free(xcb_get_input_focus_reply(g_conn, xcb_get_input_focus(g_conn), NULL));
    xcb_generic_event_t* e;
    while ((e = xcb_poll_for_event(g_conn))) free(e);
}

static pthread_t g_th;
static int g_th_started, g_stop, g_qhead, g_qcount, g_inflight;
static pthread_mutex_t g_mu;
static pthread_cond_t g_cv_job, g_cv_free;
static struct { int slot; void* sync; } g_q[NUM_BUF];

static void ms_deadline(struct timespec* ts, long ms) {
    clock_gettime(CLOCK_MONOTONIC, ts);
    ts->tv_sec += ms / 1000;
    ts->tv_nsec += (ms % 1000) * 1000000L;
    if (ts->tv_nsec >= 1000000000L) { ts->tv_sec++; ts->tv_nsec -= 1000000000L; }
}

static void drain_events(void) {
    xcb_generic_event_t* ev;
    while ((ev = xcb_poll_for_special_event(g_conn, g_se))) {
        xcb_present_generic_event_t* ge = (xcb_present_generic_event_t*)ev;
        if (ge->evtype == XCB_PRESENT_EVENT_IDLE_NOTIFY) {
            xcb_present_idle_notify_event_t* in = (xcb_present_idle_notify_event_t*)ev;
            pthread_mutex_lock(&g_mu);
            for (int i = 0; i < NUM_BUF; i++)
                if (g_slots[i].pixmap == in->pixmap && g_slots[i].busy) {
                    g_slots[i].busy = 0;
                    if (g_inflight > 0) g_inflight--;
                }
            pthread_cond_broadcast(&g_cv_free);
            pthread_mutex_unlock(&g_mu);
        }
        free(ev);
    }
}

static int first_free(void) {
    for (int i = 0; i < NUM_BUF; i++) {
        int k = (g_current_slot + i) % NUM_BUF;
        if (!g_slots[k].busy) return k;
    }
    return -1;
}

static void present_slot(int slot) {
    static int copy = -1;
    if (copy < 0) { const char* v = getenv("SHIM_PRESENT_COPY"); copy = v && atoi(v); }
    xcb_present_pixmap(g_conn, g_win, g_slots[slot].pixmap, ++g_serial,
                       XCB_NONE, XCB_NONE, 0, 0, XCB_NONE, XCB_NONE, XCB_NONE,
                       copy ? XCB_PRESENT_OPTION_COPY : 0, 0, 0, 0, 0, NULL);
    xcb_flush(g_conn);
}

static void* present_worker(void* arg) {
    (void)arg;
    int (*wait_sync)(void*, void*, int, uint64_t) = (int (*)(void*, void*, int, uint64_t))dlsym(G_mali, "eglClientWaitSyncKHR");
    unsigned (*destroy_sync)(void*, void*) = (unsigned (*)(void*, void*))dlsym(G_mali, "eglDestroySyncKHR");

    pthread_mutex_lock(&g_mu);
    while (!g_stop) {
        if (g_qcount > 0) {
            int slot = g_q[g_qhead].slot;
            void* sync = g_q[g_qhead].sync;
            g_qhead = (g_qhead + 1) % NUM_BUF;
            g_qcount--;
            pthread_mutex_unlock(&g_mu);

            if (sync && wait_sync) wait_sync(g_dpy, sync, 0, 1000000000ULL);
            if (sync && destroy_sync) destroy_sync(g_dpy, sync);
            present_slot(slot);

            pthread_mutex_lock(&g_mu);
            if (g_no_idle) { g_slots[slot].busy = 0; pthread_cond_broadcast(&g_cv_free); }
            else g_inflight++;
            continue;
        }
        if (g_inflight > 0 && !g_no_idle) {
            struct timespec ts; ms_deadline(&ts, 1);
            pthread_cond_timedwait(&g_cv_job, &g_mu, &ts);
            if (g_qcount > 0 || g_stop) continue;
            pthread_mutex_unlock(&g_mu);
            x_sync();
            drain_events();
            pthread_mutex_lock(&g_mu);
            continue;
        }
        pthread_cond_wait(&g_cv_job, &g_mu);
    }
    pthread_mutex_unlock(&g_mu);
    return NULL;
}

static int worker_start(void) {
    pthread_condattr_t ca;
    pthread_condattr_init(&ca);
    pthread_condattr_setclock(&ca, CLOCK_MONOTONIC);
    pthread_mutex_init(&g_mu, NULL);
    pthread_cond_init(&g_cv_job, &ca);
    pthread_cond_init(&g_cv_free, &ca);
    pthread_condattr_destroy(&ca);
    g_stop = g_qhead = g_qcount = g_inflight = 0;
    if (pthread_create(&g_th, NULL, present_worker, NULL)) return -1;
    g_th_started = 1;
    return 0;
}

static void worker_stop(void) {
    if (!g_th_started) return;
    pthread_mutex_lock(&g_mu);
    g_stop = 1;
    pthread_cond_broadcast(&g_cv_job);
    pthread_cond_broadcast(&g_cv_free);
    pthread_mutex_unlock(&g_mu);
    pthread_join(g_th, NULL);
    unsigned (*destroy_sync)(void*, void*) = (unsigned (*)(void*, void*))dlsym(G_mali, "eglDestroySyncKHR");
    while (g_qcount > 0) {
        if (g_q[g_qhead].sync && destroy_sync) destroy_sync(g_dpy, g_q[g_qhead].sync);
        g_qhead = (g_qhead + 1) % NUM_BUF; g_qcount--;
    }
    pthread_mutex_destroy(&g_mu);
    pthread_cond_destroy(&g_cv_job);
    pthread_cond_destroy(&g_cv_free);
    g_th_started = 0;
}

static int acquire_slot(void) {
    pthread_mutex_lock(&g_mu);
    int k = first_free();
    if (k < 0 && !g_no_idle) {
        struct timespec ts; ms_deadline(&ts, 300);
        while ((k = first_free()) < 0) {
            if (pthread_cond_timedwait(&g_cv_free, &g_mu, &ts) == ETIMEDOUT) {
                if (gles_debug_enabled()) {
                    fprintf(stderr, "[gles-shim] Present: Server not sending IDLE_NOTIFY, continuing without wait\n");
                }
                g_no_idle = 1;
                g_inflight = 0;
                for (int i = 0; i < NUM_BUF; i++) g_slots[i].busy = 0;
                k = g_current_slot;
                break;
            }
        }
    }
    if (k < 0) k = g_current_slot;
    pthread_mutex_unlock(&g_mu);
    return k;
}

static int x_setup(void) {
    g_conn = xcb_connect(NULL, NULL);
    if (!g_conn || xcb_connection_has_error(g_conn)) {
        if (gles_debug_enabled()) fprintf(stderr, "[gles-shim] No X11 connection\n"); 
        g_conn = NULL; return -1;
    }
    const xcb_query_extension_reply_t* e3 = xcb_get_extension_data(g_conn, &xcb_dri3_id);
    const xcb_query_extension_reply_t* ep = xcb_get_extension_data(g_conn, &xcb_present_id);
    if (!e3 || !e3->present || !ep || !ep->present) {
        if (gles_debug_enabled()) fprintf(stderr, "[gles-shim] DRI3/Present extension missing\n"); 
        return -1;
    }
    xcb_dri3_query_version_reply_t* r3 =
        xcb_dri3_query_version_reply(g_conn, xcb_dri3_query_version(g_conn, 1, 0), NULL);
    if (!r3) return -1;
    free(r3);
    xcb_present_query_version_reply_t* rp =
        xcb_present_query_version_reply(g_conn, xcb_present_query_version(g_conn, 1, 0), NULL);
    if (!rp) return -1;
    free(rp);

    xcb_screen_t* screen = xcb_setup_roots_iterator(xcb_get_setup(g_conn)).data;
    g_win = xcb_generate_id(g_conn);
    uint32_t vals[2] = { screen->black_pixel, XCB_EVENT_MASK_EXPOSURE };
    xcb_create_window(g_conn, XCB_COPY_FROM_PARENT, g_win, screen->root, 0, 0, g_w, g_h, 0,
                      XCB_WINDOW_CLASS_INPUT_OUTPUT, screen->root_visual,
                      XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK, vals);
    xcb_map_window(g_conn, g_win);

    g_eid = xcb_generate_id(g_conn);
    g_se = xcb_register_for_special_xge(g_conn, &xcb_present_id, g_eid, NULL);
    xcb_void_cookie_t ck = xcb_present_select_input_checked(
        g_conn, g_eid, g_win, XCB_PRESENT_EVENT_MASK_IDLE_NOTIFY);
    xcb_generic_error_t* err = xcb_request_check(g_conn, ck);
    if (err || !g_se) { 
        free(err); 
        if (gles_debug_enabled()) fprintf(stderr, "[gles-shim] present_select_input failed\n"); 
        return -1; 
    }
    return 0;
}

/* ---------- Save / Restore GL State ---------- */
typedef struct {
    int act, tex0, samp, draw_fb, read_fb, rb, arr, vao, prog, vp[4], cm[4];
    int en[5];
} gl_saved;
static const unsigned EN_CAPS[5] = { C_SCISSOR_TEST, C_DEPTH_TEST, C_STENCIL_TEST, C_BLEND, C_CULL_FACE };

static void gl_save(gl_saved* st) {
    GL.getIntegerv(C_ACTIVE_TEXTURE, &st->act);
    GL.activeTexture(C_TEXTURE0);
    GL.getIntegerv(C_TEX_BINDING_2D, &st->tex0);
    GL.getIntegerv(C_SAMPLER_BINDING, &st->samp);
    GL.getIntegerv(C_DRAW_FB_BINDING, &st->draw_fb);
    GL.getIntegerv(C_READ_FB_BINDING, &st->read_fb);
    GL.getIntegerv(C_RB_BINDING, &st->rb);
    GL.getIntegerv(C_ARRAY_BUF_BINDING, &st->arr);
    GL.getIntegerv(C_VAO_BINDING, &st->vao);
    GL.getIntegerv(C_CURRENT_PROGRAM, &st->prog);
    GL.getIntegerv(C_VIEWPORT, st->vp);
    GL.getIntegerv(C_COLOR_WRITEMASK, st->cm);
    for (int i = 0; i < 5; i++) st->en[i] = GL.isEnabled(EN_CAPS[i]);
}

static void gl_restore(const gl_saved* st) {
    for (int i = 0; i < 5; i++) { if (st->en[i]) GL.enable(EN_CAPS[i]); else GL.disable(EN_CAPS[i]); }
    GL.colorMask(st->cm[0], st->cm[1], st->cm[2], st->cm[3]);
    GL.viewport(st->vp[0], st->vp[1], st->vp[2], st->vp[3]);
    GL.useProgram(st->prog);
    GL.bindVertexArray(st->vao);
    GL.bindBuffer(C_ARRAY_BUFFER, st->arr);
    GL.bindRenderbuffer(C_RENDERBUFFER, st->rb);
    GL.bindFramebuffer(C_DRAW_FB, st->draw_fb);
    GL.bindFramebuffer(C_READ_FB, st->read_fb);
    GL.activeTexture(C_TEXTURE0);
    GL.bindSampler(0, st->samp);
    GL.bindTexture(C_TEXTURE_2D, st->tex0);
    GL.activeTexture(st->act);
}

/* ---------- Blit Shader (flip Y + optional swizzle + alpha=1) ---------- */
static unsigned compile_sh(unsigned type, const char* src) {
    unsigned sh = GL.createShader(type);
    GL.shaderSource(sh, 1, &src, NULL);
    GL.compileShader(sh);
    int ok = 0; GL.getShaderiv(sh, C_COMPILE_STATUS, &ok);
    if (!ok) {
        if (gles_debug_enabled()) {
            char log[512]; GL.getShaderInfoLog(sh, sizeof log, NULL, log);
            fprintf(stderr, "[gles-shim] Shader compilation failed: %s\n", log);
        }
        GL.deleteShader(sh); return 0;
    }
    return sh;
}

static unsigned build_blit_program(int swz) {
    static const char* vs =
        "#version 300 es\n"
        "layout(location=0) in vec2 p;\n"
        "void main(){ gl_Position = vec4(p, 0.0, 1.0); }\n";
    char fs[512];
    snprintf(fs, sizeof fs,
        "#version 300 es\n"
        "precision highp float;\n"
        "uniform sampler2D t;\n"
        "out vec4 c;\n"
        "void main(){\n"
        "  ivec2 sz = textureSize(t, 0);\n"
        "  vec4 v = texelFetch(t, ivec2(int(gl_FragCoord.x), sz.y - 1 - int(gl_FragCoord.y)), 0);\n"
        "  c = vec4(v.%s, 1.0);\n"
        "}\n", swz ? "bgr" : "rgb");
    unsigned v = compile_sh(C_VERTEX_SHADER, vs);
    unsigned f = compile_sh(C_FRAGMENT_SHADER, fs);
    if (!v || !f) return 0;
    unsigned p = GL.createProgram();
    GL.attachShader(p, v); GL.attachShader(p, f);
    GL.linkProgram(p);
    GL.deleteShader(v); GL.deleteShader(f);
    int ok = 0; GL.getProgramiv(p, C_LINK_STATUS, &ok);
    if (!ok) {
        if (gles_debug_enabled()) {
            char log[512]; GL.getProgramInfoLog(p, sizeof log, NULL, log);
            fprintf(stderr, "[gles-shim] Program linking failed: %s\n", log);
        }
        GL.deleteProgram(p); return 0;
    }
    return p;
}

/* ---------- AHB ---------- */
static void* alloc_ahb(int fmt, AHBDesc* out) {
    AHBDesc d = { .width = g_w, .height = g_h, .layers = 1, .format = fmt,
                  .usage = AHB_USAGE_GPU_COLOR_OUT | AHB_USAGE_GPU_SAMPLED | AHB_USAGE_CPU_READ_RARELY };
    void* ab = NULL;
    if (ahb_alloc(&d, &ab) || !ab) return NULL;
    memset(out, 0, sizeof *out);
    ahb_desc(ab, out);
    return ab;
}

static int pick_dmabuf_fd(const native_handle_t* nh) {
    for (int i = 0; i < nh->numFds; i++) {
        char link[64], path[256] = {0};
        snprintf(link, sizeof link, "/proc/self/fd/%d", nh->data[i]);
        ssize_t n = readlink(link, path, sizeof path - 1);
        if (n > 0 && strstr(path, "dmabuf")) return nh->data[i];
    }
    return nh->numFds > 1 ? nh->data[1] : -1;
}

/* ---------- Teardown ---------- */
static void wsi_teardown(int with_gl) {
    worker_stop();
    if (with_gl && gl_loaded > 0) {
        for (int i = 0; i < NUM_BUF; i++) {
            if (g_slots[i].fbo) GL.deleteFramebuffers(1, &g_slots[i].fbo);
            if (g_slots[i].tex) GL.deleteTextures(1, &g_slots[i].tex);
        }
        if (g_app_fbo) GL.deleteFramebuffers(1, &g_app_fbo);
        if (g_app_tex) GL.deleteTextures(1, &g_app_tex);
        if (g_app_rbo) GL.deleteRenderbuffers(1, &g_app_rbo);
        if (g_prog) GL.deleteProgram(g_prog);
        if (g_vao) GL.deleteVertexArrays(1, &g_vao);
        if (g_vbo) GL.deleteBuffers(1, &g_vbo);
    }
    for (int i = 0; i < NUM_BUF; i++) {
        Slot* sl = &g_slots[i];
        if (sl->image && m_eglDestroyImageKHR) m_eglDestroyImageKHR(g_dpy, sl->image);
        if (sl->pixmap && g_conn) xcb_free_pixmap(g_conn, sl->pixmap);
        if (sl->ab && ahb_release) ahb_release(sl->ab);
        memset(sl, 0, sizeof *sl);
    }
    if (g_se && g_conn) { xcb_unregister_for_special_event(g_conn, g_se); g_se = NULL; }
    if (g_conn) {
        if (g_win) xcb_destroy_window(g_conn, g_win);
        xcb_flush(g_conn);
        xcb_disconnect(g_conn);
        g_conn = NULL; g_win = 0;
    }
    g_app_fbo = g_app_tex = g_app_rbo = g_prog = g_vao = g_vbo = 0;
    g_inited = 0;
}

/* ---------- EGLImage from AHB ---------- */
static void* make_egl_image(void* ab, void* (*get_cb)(void*), int fd, int w, int h,
                            uint32_t stride, int fmt, int verbose) {
    int (*get_err)(void) = (int (*)(void))dlsym(G_mali, "eglGetError");
    if (verbose && gles_debug_enabled()) {
        const char* (*qs)(void*, int) = (const char* (*)(void*, int))dlsym(G_mali, "eglQueryString");
        const char* ex = qs ? qs(g_dpy, 0x3055) : NULL;
        fprintf(stderr, "[gles-shim] EGL ext: native_buffer=%d dma_buf_import=%d\n",
                ex && strstr(ex, "EGL_ANDROID_image_native_buffer") != NULL,
                ex && strstr(ex, "EGL_EXT_image_dma_buf_import") != NULL);
    }
    void* (*to_anwb)(void*) = ahb_lib ? (void* (*)(void*))dlsym(ahb_lib, "AHardwareBuffer_to_ANativeWindowBuffer") : NULL;
    void* cbs[3]; const char* names[3]; int n = 0;
    if (get_cb)  { cbs[n] = get_cb(ab);  names[n++] = "eglGetNativeClientBufferANDROID"; }
    if (to_anwb) { cbs[n] = to_anwb(ab); names[n++] = "AHardwareBuffer_to_ANativeWindowBuffer"; }
    cbs[n] = ab; names[n++] = "raw AHB pointer";

    static const int attr_pres[] = { 0x30D2 /* EGL_IMAGE_PRESERVED_KHR */, 1, 0x3038 };
    static const int attr_none[] = { 0x3038 };
    for (int i = 0; i < n; i++) {
        if (!cbs[i]) continue;
        for (int a = 0; a < 2; a++) {
            void* img = m_eglCreateImageKHR(g_dpy, NULL, 0x3140 /* EGL_NATIVE_BUFFER_ANDROID */, cbs[i],
                                            a ? attr_none : attr_pres);
            if (img) {
                if (verbose && gles_debug_enabled()) {
                    fprintf(stderr, "[gles-shim] EGLImage via native_buffer (%s)\n", names[i]);
                }
                return img;
            }
            if (verbose && gles_debug_enabled()) {
                fprintf(stderr, "[gles-shim] native_buffer (%s, attr=%d) failed: eglError=0x%x\n",
                        names[i], a, get_err ? get_err() : 0);
            }
        }
    }
    int fourcc = (fmt == 5) ? 0x34325241 : 0x34324241;
    int da[] = { 0x3057, w, 0x3056, h, 0x3271, fourcc, 0x3272, fd,
                 0x3273, 0, 0x3274, (int)stride, 0x30D2 /* EGL_IMAGE_PRESERVED_KHR */, 1, 0x3038 };
    void* img = m_eglCreateImageKHR(g_dpy, NULL, 0x3270 /* EGL_LINUX_DMA_BUF_EXT */, NULL, da);
    if (img) { 
        if (verbose && gles_debug_enabled()) fprintf(stderr, "[gles-shim] EGLImage via dma_buf_import\n"); 
        return img; 
    }
    if (verbose && gles_debug_enabled()) {
        fprintf(stderr, "[gles-shim] dma_buf_import failed: eglError=0x%x\n", get_err ? get_err() : 0);
    }
    return NULL;
}

/* ---------- Build WSI ---------- */
static int wsi_build(void) {
    void* (*get_cb)(void*) = real_gpa ? (void* (*)(void*))real_gpa("eglGetNativeClientBufferANDROID") : NULL;
    void (*img_target)(unsigned, void*) = (void (*)(unsigned, void*))glp("glEGLImageTargetTexture2DOES");
    if (!m_eglCreateImageKHR || !img_target) {
        if (gles_debug_enabled()) {
            fprintf(stderr, "[gles-shim] Missing eglCreateImageKHR / glEGLImageTargetTexture2DOES\n");
        }
        return -1;
    }

    GL.genTextures(1, &g_app_tex);
    GL.bindTexture(C_TEXTURE_2D, g_app_tex);
    GL.texImage2D(C_TEXTURE_2D, 0, C_RGBA8, g_w, g_h, 0, C_RGBA, C_UBYTE, NULL);
    GL.texParameteri(C_TEXTURE_2D, C_MIN_FILTER, C_NEAREST);
    GL.texParameteri(C_TEXTURE_2D, C_MAG_FILTER, C_NEAREST);
    GL.texParameteri(C_TEXTURE_2D, C_WRAP_S, C_CLAMP_EDGE);
    GL.texParameteri(C_TEXTURE_2D, C_WRAP_T, C_CLAMP_EDGE);
    GL.genRenderbuffers(1, &g_app_rbo);
    GL.bindRenderbuffer(C_RENDERBUFFER, g_app_rbo);
    GL.renderbufferStorage(C_RENDERBUFFER, C_DEPTH24_STENCIL8, g_w, g_h);
    GL.genFramebuffers(1, &g_app_fbo);
    GL.bindFramebuffer(C_FRAMEBUFFER, g_app_fbo);
    GL.framebufferTexture2D(C_FRAMEBUFFER, C_COLOR_ATT0, C_TEXTURE_2D, g_app_tex, 0);
    GL.framebufferRenderbuffer(C_FRAMEBUFFER, C_DEPTH_STENCIL_ATT, C_RENDERBUFFER, g_app_rbo);
    if (GL.checkFramebufferStatus(C_FRAMEBUFFER) != C_FB_COMPLETE) {
        if (gles_debug_enabled()) fprintf(stderr, "[gles-shim] Application FBO incomplete\n"); 
        return -1;
    }

    static const float quad[8] = { -1, -1, 1, -1, -1, 1, 1, 1 };
    GL.genVertexArrays(1, &g_vao);
    GL.bindVertexArray(g_vao);
    GL.genBuffers(1, &g_vbo);
    GL.bindBuffer(C_ARRAY_BUFFER, g_vbo);
    GL.bufferData(C_ARRAY_BUFFER, sizeof quad, quad, C_STATIC_DRAW);
    GL.enableVertexAttribArray(0);
    GL.vertexAttribPointer(0, 2, C_FLOAT, 0, 0, NULL);

    const char* fe = getenv("SHIM_FORMAT");
    int try_fmt[2] = { fe ? atoi(fe) : 5, 1 };   /* 5 = BGRA (native X), 1 = RGBA (+swizzle) */
    int nfmt = fe ? 1 : 2;

    for (int i = 0; i < NUM_BUF; i++) {
        Slot* sl = &g_slots[i];
        AHBDesc d;
        if (i == 0) {
            for (int f = 0; f < nfmt && !sl->ab; f++) {
                sl->ab = alloc_ahb(try_fmt[f], &d);
                if (sl->ab) g_fmt = try_fmt[f];
            }
        } else sl->ab = alloc_ahb(g_fmt, &d);
        if (!sl->ab) { 
            if (gles_debug_enabled()) fprintf(stderr, "[gles-shim] AHB allocation failed (slot %d)\n", i); 
            return -1; 
        }
        g_swz = (g_fmt != 5);
        if (i == 0 && gles_debug_enabled()) {
            fprintf(stderr, "[gles-shim] AHB format %d (%s), stride %u px\n",
                    g_fmt, g_swz ? "RGBA + swizzle" : "native BGRA", d.stride);
        }

        const native_handle_t* nh = ahb_handle(sl->ab);
        if (!nh) return -1;
        int fd = pick_dmabuf_fd(nh);
        if (fd < 0) { 
            if (gles_debug_enabled()) fprintf(stderr, "[gles-shim] Cannot find dma-buf fd\n"); 
            return -1; 
        }

        uint32_t stride = d.stride * 4;
        uint32_t size = stride * g_h;
        sl->pixmap = xcb_generate_id(g_conn);
        xcb_void_cookie_t ck = xcb_dri3_pixmap_from_buffer_checked(
            g_conn, sl->pixmap, g_win, size, g_w, g_h, (uint16_t)stride, 24, 32, dup(fd));
        xcb_generic_error_t* err = xcb_request_check(g_conn, ck);
        if (err) {
            if (gles_debug_enabled()) {
                fprintf(stderr, "[gles-shim] DRI3 pixmap_from_buffer rejected (error %d)\n", err->error_code);
            }
            free(err); sl->pixmap = 0; return -1;
        }

        sl->image = make_egl_image(sl->ab, get_cb, fd, g_w, g_h, stride, g_fmt, i == 0);
        if (!sl->image) { 
            if (gles_debug_enabled()) fprintf(stderr, "[gles-shim] All EGLImage variants failed\n"); 
            return -1; 
        }

        GL.genTextures(1, &sl->tex);
        GL.bindTexture(C_TEXTURE_2D, sl->tex);
        GL.texParameteri(C_TEXTURE_2D, C_MIN_FILTER, C_NEAREST);
        GL.texParameteri(C_TEXTURE_2D, C_MAG_FILTER, C_NEAREST);
        img_target(C_TEXTURE_2D, sl->image);
        GL.genFramebuffers(1, &sl->fbo);
        GL.bindFramebuffer(C_FRAMEBUFFER, sl->fbo);
        GL.framebufferTexture2D(C_FRAMEBUFFER, C_COLOR_ATT0, C_TEXTURE_2D, sl->tex, 0);
        if (GL.checkFramebufferStatus(C_FRAMEBUFFER) != C_FB_COMPLETE) {
            if (gles_debug_enabled()) fprintf(stderr, "[gles-shim] FBO slot %d incomplete\n", i); 
            return -1;
        }
    }

    g_prog = build_blit_program(g_swz);
    return g_prog ? 0 : -1;
}

/* Lazy Initialization. Returns 1 if zero-copy path is active for current context. */
static int ensure_init(void) {
    if (g_inited) return m_eglGetCurrentContext && m_eglGetCurrentContext() == g_ctx;
    if (g_failed) return 0;

    const char* e = getenv("SHIM_DMABUF");
    if (e && !atoi(e)) { g_failed = 1; return 0; }
    if (!G_mali || !m_eglGetCurrentContext || !m_eglGetCurrentDisplay) { g_failed = 1; return 0; }

    void* ctx = m_eglGetCurrentContext();
    if (!ctx) return 0;

    if (gl_load() || ahb_init()) { g_failed = 1; return 0; }
    const char* ver = (const char*)GL.getString(C_VERSION);
    if (!ver || !strstr(ver, "OpenGL ES 3")) {
        if (gles_debug_enabled()) {
            fprintf(stderr, "[gles-shim] Requires GLES3 context (got '%s'), using original path\n", ver ? ver : "?");
        }
        g_failed = 1; return 0;
    }

    const char* sw = getenv("SHIM_W"); if (sw && atoi(sw) > 0) g_w = atoi(sw);
    const char* sh = getenv("SHIM_H"); if (sh && atoi(sh) > 0) g_h = atoi(sh);

    g_dpy = m_eglGetCurrentDisplay();
    g_ctx = ctx;

    if (x_setup()) { wsi_teardown(0); g_failed = 1; return 0; }

    gl_saved st;
    gl_save(&st);
    int r = wsi_build();
    gl_restore(&st);
    if (r) {
        if (gles_debug_enabled()) fprintf(stderr, "[gles-shim] Init failed, using original path\n");
        wsi_teardown(1); g_failed = 1; return 0;
    }

    GL.bindFramebuffer(C_FRAMEBUFFER, g_app_fbo);
    GL.viewport(0, 0, g_w, g_h);
    GL.scissor(0, 0, g_w, g_h);
    if (worker_start()) { 
        if (gles_debug_enabled()) fprintf(stderr, "[gles-shim] Failed to start worker thread\n"); 
        wsi_teardown(1); g_failed = 1; return 0; 
    }
    g_inited = 1;
    if (gles_debug_enabled()) {
        fprintf(stderr, "[gles-shim] ZERO-COPY PATH ACTIVE (%dx%d, %d buffers)\n", g_w, g_h, NUM_BUF);
    }
    return 1;
}

/* ---------- Swap ---------- */
static void wsi_swap(void) {
    static int mode = -1;
    static void* (*create_sync)(void*, unsigned, const int*);
    if (mode < 0) {
        const char* v = getenv("SHIM_SYNC");
        mode = (v && !strcmp(v, "finish")) ? 1 : (v && !strcmp(v, "flush")) ? 2 :
               (v && !strcmp(v, "implicit")) ? 3 : 0;
        create_sync = (void* (*)(void*, unsigned, const int*))dlsym(G_mali, "eglCreateSyncKHR");
        if ((mode == 0 || mode == 3) && (!create_sync || !dlsym(G_mali, "eglClientWaitSyncKHR"))) mode = 1;
        if (gles_debug_enabled()) {
            fprintf(stderr, "[gles-shim] sync: %s, Present on worker thread\n",
                    mode == 0 ? "EGL fence (full)" : mode == 1 ? "glFinish" : mode == 2 ? "glFlush"
                                     : "ARM implicit_external_sync fence");
        }
    }

    int k = acquire_slot();
    Slot* sl = &g_slots[k];

    gl_saved st;
    gl_save(&st);
    GL.bindFramebuffer(C_DRAW_FB, sl->fbo);
    GL.viewport(0, 0, g_w, g_h);
    for (int i = 0; i < 5; i++) GL.disable(EN_CAPS[i]);
    GL.colorMask(1, 1, 1, 1);
    GL.activeTexture(C_TEXTURE0);
    GL.bindSampler(0, 0);
    GL.bindTexture(C_TEXTURE_2D, g_app_tex);
    GL.useProgram(g_prog);
    GL.bindVertexArray(g_vao);
    GL.drawArrays(C_TRI_STRIP, 0, 4);
    gl_restore(&st);

    void* sync = NULL;
    if (mode == 0 || mode == 3) {
        static const int impl[] = { 0x30F8 /* EGL_SYNC_CONDITION_KHR */, 0x328A /* ..._IMPLICIT_EXTERNAL_ARM */, 0x3038 };
        sync = create_sync(g_dpy, 0x30F9 /* EGL_SYNC_FENCE_KHR */, mode == 3 ? impl : NULL);
        if (!sync && mode == 3) sync = create_sync(g_dpy, 0x30F9, NULL);
        GL.flush();
        if (!sync) GL.finish();
    } else if (mode == 1) GL.finish();
    else GL.flush();

    pthread_mutex_lock(&g_mu);
    sl->busy = 1;
    g_q[(g_qhead + g_qcount) % NUM_BUF].slot = k;
    g_q[(g_qhead + g_qcount) % NUM_BUF].sync = sync;
    g_qcount++;
    pthread_cond_signal(&g_cv_job);
    pthread_mutex_unlock(&g_mu);

    g_current_slot = (k + 1) % NUM_BUF;
}

/* ---------- Library Initialization ---------- */
__attribute__((constructor))
static void gles_shim_init(void) {
    G_mali = mali_sphal_load(MALI_SPHAL_DEFAULT);
    if (!G_mali) return;

#define X(n) p_##n = dlsym(G_mali, #n);
#include "gles_list.h"
#undef X

    *(void**)&real_bind         = dlsym(G_mali, "glBindFramebuffer");
    *(void**)&real_bind_oes     = dlsym(G_mali, "glBindFramebufferOES");
    *(void**)&real_swap_buffers = dlsym(G_mali, "eglSwapBuffers");
    *(void**)&real_gpa          = dlsym(G_mali, "eglGetProcAddress");
    *(void**)&m_eglGetCurrentContext = dlsym(G_mali, "eglGetCurrentContext");
    *(void**)&m_eglGetCurrentDisplay = dlsym(G_mali, "eglGetCurrentDisplay");
    *(void**)&m_eglCreateImageKHR    = dlsym(G_mali, "eglCreateImageKHR");
    *(void**)&m_eglDestroyImageKHR   = dlsym(G_mali, "eglDestroyImageKHR");
}

/* ---------- Exports ---------- */
static unsigned own_default_fbo(void) {
    return ensure_init() ? g_app_fbo : 0;
}

static unsigned (*egl_fbo_fn)(void);
static EGLBoolean (*egl_swap_fn)(EGLDisplay, EGLSurface);
static int deleg = -1;
static int delegated(void) {
    if (deleg > 0) return 1;
    if (g_inited) return 0;            /* WSI propriu deja pornit, nu mai comutăm */
    void* fn = dlsym(RTLD_DEFAULT, "shim_x11_default_fbo");
    if (!fn) {
        /* EGL încărcat cu RTLD_LOCAL: încearcă prin handle fără a-l încărca */
        void* h = dlopen("libEGL.so.1", RTLD_NOW | RTLD_NOLOAD);
        if (!h) h = dlopen("libEGL.so", RTLD_NOW | RTLD_NOLOAD);
        if (h) fn = dlsym(h, "shim_x11_default_fbo");
        if (h && fn) {
            *(void**)&egl_fbo_fn = fn;
            *(void**)&egl_swap_fn = dlsym(h, "eglSwapBuffers");
            deleg = 1;
            if (gles_debug_enabled())
                fprintf(stderr, "[gles-shim] EGL shim detected: WSI inside GLES shim disabled\n");
            return 1;
        }
        return 0;                      /* nu fixa -1/0, reîncearcă data viitoare */
    }
    *(void**)&egl_fbo_fn = fn;
    Dl_info di;
    if (dladdr(fn, &di) && di.dli_fname) {
        void* h = dlopen(di.dli_fname, RTLD_NOW | RTLD_NOLOAD);
        if (h) *(void**)&egl_swap_fn = dlsym(h, "eglSwapBuffers");
    }
    deleg = 1;
    if (gles_debug_enabled())
        fprintf(stderr, "[gles-shim] EGL shim detected: WSI inside GLES shim disabled\n");
    return 1;
}

static unsigned map_fb0(unsigned fb) {
    if (fb) return fb;
    if (delegated()) return egl_fbo_fn();
    return own_default_fbo();
}

EXPORT void glBindFramebuffer(unsigned target, unsigned fb) {
    if (real_bind) real_bind(target, map_fb0(fb));
}

EXPORT void glBindFramebufferOES(unsigned target, unsigned fb) {
    if (real_bind_oes) real_bind_oes(target, map_fb0(fb));
}

EXPORT EGLBoolean eglSwapBuffers(EGLDisplay dpy, EGLSurface surface) {
    static int real_too = -1;
    if (real_too < 0) { const char* v = getenv("SHIM_REAL_SWAP"); real_too = v && atoi(v); }

    if (delegated()) {
        static EGLBoolean (*nxt)(EGLDisplay, EGLSurface);
        if (!nxt) {
            nxt = egl_swap_fn;
            if (!nxt || nxt == eglSwapBuffers) *(void**)&nxt = dlsym(RTLD_NEXT, "eglSwapBuffers");
        }
        if (nxt && nxt != eglSwapBuffers) return nxt(dpy, surface);
        return real_swap_buffers ? real_swap_buffers(dpy, surface) : EGL_TRUE;
    }

    int first = !g_inited;
    if (ensure_init()) {
        if (first) {
            real_bind(C_FRAMEBUFFER, g_app_fbo);
        } else {
            wsi_swap();
        }
        if (real_too && real_swap_buffers) return real_swap_buffers(dpy, surface);
        return EGL_TRUE;
    }
    if (real_swap_buffers) return real_swap_buffers(dpy, surface);
    return EGL_TRUE;
}
