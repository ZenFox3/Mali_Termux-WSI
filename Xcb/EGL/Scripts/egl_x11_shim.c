/*
 * egl_x11_shim.c - libEGL.so / libEGL.so.1 for Mali blob (sphal) with X11 & GBM windows.
 *
 * Supported platforms: 
 *   EGL_PLATFORM_X11_EXT (Xlib), EGL_PLATFORM_XCB_EXT (xcb), EGL_PLATFORM_GBM_KHR (GBM/AHB).
 *
 * Fast path (zero CPU copy):
 *   app -> offscreen FBO (GLES3) -> GPU pass (flip Y + swizzle) -> AHardwareBuffer
 *   (linear dma-buf) -> DRI3 pixmap_from_buffer -> Present (IDLE_NOTIFY, N buffers)
 * Automatic fallback to legacy WSI if anything fails.
 */
#define _GNU_SOURCE
#include "wsi_egl_x11.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <errno.h>
#include <time.h>
#include <pthread.h>
#include <poll.h>
#include <unistd.h>
#include <fcntl.h>
#include <dlfcn.h>
#include <sys/types.h>
#include "mali_native_fence.h"
#include <xcb/xcb.h>
#include <xcb/dri3.h>
#include <xcb/present.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <vulkan/vulkan.h>

#define EXPORT __attribute__((visibility("default")))
#define SURF_MAGIC 0x58313153u

#define E_SUCCESS           0x3000
#define E_BAD_CURRENT_SURF  0x3007
#define E_BAD_DISPLAY       0x3008
#define E_BAD_NATIVE_PIXMAP 0x300A
#define E_BAD_NATIVE_WINDOW 0x300B
#define E_BAD_PARAMETER     0x300C
#define E_BAD_ALLOC         0x3003
#define E_BAD_ATTRIBUTE     0x3004
#define E_BAD_MATCH         0x3009
#define E_BAD_SURFACE       0x300D
#define E_NONE              0x3038
#define E_NOT_INITIALIZED   0x3001
#define E_BAD_DEVICE_EXT    0x322B
#define E_BAD_CONTEXT       0x3006
#define E_BAD_ACCESS        0x3002

/* EGL platform enums */
#define EGL_PLATFORM_ANGLE_ANGLE 0x3202
#define EGL_PLATFORM_X11_EXT          0x31D5
#define EGL_PLATFORM_X11_SCREEN_EXT   0x31D6
#define EGL_PLATFORM_XCB_EXT          0x31DC
#define EGL_PLATFORM_XCB_SCREEN_EXT   0x31DE
#define EGL_PLATFORM_SURFACELESS_MESA 0x31DD
#define EGL_PLATFORM_GBM_KHR          0x31D7
#define EGL_PLATFORM_DEVICE_EXT       0x313F
#define EGL_DEVICE_EXT                0x322C
#define EGL_RENDERER_EXT              0x335F
#define EGL_DRM_DEVICE_FILE_EXT        0x3233
#define EGL_DRM_RENDER_NODE_FILE_EXT   0x3377
#define EGL_DRM_MASTER_FD_EXT          0x333C
#define EGL_DEVICE_UUID_EXT           0x335C
#define EGL_DRIVER_UUID_EXT           0x335D
#define EGL_DRIVER_NAME_EXT           0x335E
#define EGL_TRACK_REFERENCES_KHR      0x3352

#define MAX_BUFFERS 8
#define GBM_MAX_SLOTS 6

#define DBG(...) do { if (shim_debug_enabled()) fprintf(stderr, __VA_ARGS__); } while (0)

/* ---------- GL Constants (ES3) ---------- */
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

/* ---------- AHardwareBuffer (manual declarations) ---------- */
typedef struct {
    uint32_t width, height, layers, format;
    uint64_t usage;
    uint32_t stride, rfu0;
    uint64_t rfu1;
} AHBDesc;
typedef struct { int version, numFds, numInts; int data[]; } native_handle_t;

#define AHB_USAGE_CPU_READ_RARELY 0x2ULL
#define AHB_USAGE_GPU_SAMPLED     0x100ULL
#define AHB_USAGE_GPU_COLOR_OUT   0x200ULL

static int  (*ahb_alloc)(const AHBDesc*, void**);
static void (*ahb_desc)(const void*, AHBDesc*);
static const native_handle_t* (*ahb_handle)(const void*);
static void (*ahb_release)(void*);
static void (*ahb_acquire)(void*);
static void* (*ahb_to_anwb)(void*);
static int ahb_state;
static void* ahb_lib;

static inline int shim_debug_enabled(void) {
    const char* env_egl = getenv("EGL_DEBUG");
    const char* env_mali = getenv("MALI_DEBUG");
    const char* env_gl = getenv("GL_DEBUG");
    return (env_egl && env_egl[0] != '0') ||
           (env_mali && env_mali[0] != '0') ||
           (env_gl && env_gl[0] != '0');
}

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
        *(void**)&ahb_acquire = dlsym(h, "AHardwareBuffer_acquire");
        static const char* mangled =
            "_ZN7android38AHardwareBuffer_to_ANativeWindowBufferEP15AHardwareBuffer";
        *(void**)&ahb_to_anwb = dlsym(h, mangled);
        if (!ahb_to_anwb) *(void**)&ahb_to_anwb = dlsym(RTLD_DEFAULT, mangled);
        if (!ahb_to_anwb) {
            void* h2 = dlopen("/system/lib64/libnativewindow.so", RTLD_NOW);
            if (h2) *(void**)&ahb_to_anwb = dlsym(h2, mangled);
        }
        DBG("[shim] AHardwareBuffer_to_ANativeWindowBuffer: %s\n", ahb_to_anwb ? "found" : "NOT found");
    }
    ahb_state = (ahb_alloc && ahb_desc && ahb_handle && ahb_release) ? 1 : -1;
    if (ahb_state < 0) DBG("[shim] AHardwareBuffer unavailable\n");
    return ahb_state > 0 ? 0 : -1;
}

static char g_device_obj;

/* ---------- GL Functions ---------- */
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
    void (*flush)(void);
    void (*finish)(void);
} gl_fns;
static gl_fns GL;
static int gl_state_loaded;

typedef struct {
    void* ab;
    xcb_pixmap_t pixmap;
    void* image;
    GLuint tex, fbo;
    int busy;
} dmabuf_slot_t;

typedef struct shim_surf {
    uint32_t magic;
    struct shim_surf* next;
    xcb_window_t win;
    int w, h, frames;
    int x, y, bw, depth, cfg_sent;
    wsi_x11* wsi;
    void* ctx;
    int interval;
    void* dpy;      /* display that owns this surface */
    struct shim_surf* rel_next;   /* temporary list used in eglDestroyContext */
    /* dmabuf path */
    int releasing;   /* claimed by a single teardown */
    int dmabuf_inited, dmabuf_failed, no_idle, swz, fmt;
    dmabuf_slot_t slots[MAX_BUFFERS];
    int current_slot;
    uint32_t present_serial;
    GLuint app_tex, app_fbo, app_rbo, prog, vao, vbo;
    void *app_ab, *app_img;
    xcb_special_event_t* se;
    uint32_t eid;

    /* Present worker */
    pthread_t th;
    int th_started, stop, qhead, qcount, inflight;
    pthread_mutex_t mu;
    pthread_cond_t cv_job, cv_free;
    struct { int slot; void* sync; int fd; } q[MAX_BUFFERS];

    /* stats */
    long t_pres[MAX_BUFFERS], st_acq, st_blit, st_fence, st_idle, st_t0;
    int st_frames, st_idle_n, st_app, st_drop;

    /* configuration + event thread */
    int nbuf, mailbox;
    xcb_connection_t *ec, *sec;
    int ev_active, ev_started, wake[2];
    pthread_t evth;
    int pend_w, pend_h;

    /* GBM path */
    int is_gbm, gbm_ok, gcur, ngsl;
    void *gs, *cur_bo;
    GLuint grbo;
    struct { void* bo; void* img; GLuint tex, fbo; } gsl[GBM_MAX_SLOTS];
} shim_surf;

static struct {
    pthread_mutex_t mu;
    int ready;
    void *mali, *dpy, *gdpy;
    void* (*gpa)(const char*);
    xcb_connection_t* xc;
    int own_xc;
    int xcb;
    uint32_t root_visual;
    shim_surf* surfs;
    void* ctx[64];
    int ver[64];
    int rst[64];
    void* gctx[64];
    int nctx;
    mali_fence_ctx_t fctx;
    int fctx_ok;
} G = { .mu = PTHREAD_MUTEX_INITIALIZER };

static __thread int T_err;
static __thread shim_surf* T_surf;

static inline shim_surf* cur_surf(void) {
    shim_surf* s = T_surf;
    if (s && __atomic_load_n(&s->magic, __ATOMIC_ACQUIRE) != SURF_MAGIC) {
        T_surf = NULL;
        return NULL;
    }
    return s;
}

/* ---------- EGL_KHR_debug ---------- */
#define EGL_OBJECT_THREAD_KHR   0x33B0
#define EGL_OBJECT_DISPLAY_KHR  0x33B1
#define EGL_OBJECT_CONTEXT_KHR  0x33B2
#define EGL_OBJECT_SURFACE_KHR  0x33B3
#define EGL_OBJECT_IMAGE_KHR    0x33B4
#define EGL_OBJECT_SYNC_KHR     0x33B5
#define EGL_OBJECT_STREAM_KHR   0x33B6
#define EGL_DEBUG_CALLBACK_KHR  0x33B8
#define EGL_DEBUG_MSG_CRITICAL_KHR 0x33B9
#define EGL_DEBUG_MSG_ERROR_KHR    0x33BA
#define EGL_DEBUG_MSG_WARN_KHR     0x33BB
#define EGL_DEBUG_MSG_INFO_KHR     0x33BC

typedef void (*dbg_cb_t)(unsigned, const char*, int, void*, void*, const char*);

static struct {
    pthread_mutex_t mu;
    dbg_cb_t cb;
    int on[4];                       /* CRITICAL, ERROR, WARN, INFO */
} D = { .mu = PTHREAD_MUTEX_INITIALIZER, .on = {1, 1, 0, 0} };

static __thread void* T_label;

typedef struct dbg_lbl { struct dbg_lbl* next; int type; void* obj; void* label; } dbg_lbl;
static dbg_lbl* g_lbls;

static void* lbl_get(int type, void* obj) {
    void* r = NULL;
    pthread_mutex_lock(&D.mu);
    for (dbg_lbl* l = g_lbls; l; l = l->next)
        if (l->type == type && l->obj == obj) { r = l->label; break; }
    pthread_mutex_unlock(&D.mu);
    return r;
}

static void dbg_msg(unsigned err, const char* cmd, int mtype,
                    int otype, void* obj, const char* msg) {
    dbg_cb_t cb; int en;
    pthread_mutex_lock(&D.mu);
    cb = D.cb; en = D.on[mtype - EGL_DEBUG_MSG_CRITICAL_KHR];
    pthread_mutex_unlock(&D.mu);
    if (!cb || !en) return;          /* invoke callback unlocked (not reentrant) */
    cb(err, cmd, mtype, T_label, otype ? lbl_get(otype, obj) : NULL, msg);
}

static void shim_set_err(int e, const char* cmd) {
    T_err = e;
    int crit = (e == E_BAD_ALLOC || e == 0x300E /*CONTEXT_LOST*/);
    dbg_msg(e, cmd, crit ? EGL_DEBUG_MSG_CRITICAL_KHR : EGL_DEBUG_MSG_ERROR_KHR,
            0, NULL, "EGL shim error");
}
#define SET_ERR(e) shim_set_err((e), __func__)

EXPORT int eglDebugMessageControlKHR(dbg_cb_t cb, const intptr_t* at) {
    int nv[4]; 
    pthread_mutex_lock(&D.mu);
    memcpy(nv, D.on, sizeof nv);
    pthread_mutex_unlock(&D.mu);
    for (; at && at[0] != E_NONE; at += 2) {
        if (at[0] < EGL_DEBUG_MSG_CRITICAL_KHR || at[0] > EGL_DEBUG_MSG_INFO_KHR ||
            (at[1] != 0 && at[1] != 1)) {
            T_err = E_BAD_ATTRIBUTE; return E_BAD_ATTRIBUTE;
        }
        nv[at[0] - EGL_DEBUG_MSG_CRITICAL_KHR] = (int)at[1];
    }
    pthread_mutex_lock(&D.mu);
    D.cb = cb;
    if (cb) memcpy(D.on, nv, sizeof nv);
    else { D.on[0] = D.on[1] = 1; D.on[2] = D.on[3] = 0; }   /* reset to defaults */
    pthread_mutex_unlock(&D.mu);
    return E_SUCCESS;
}

EXPORT unsigned eglQueryDebugKHR(int attr, intptr_t* v) {
    if (!v) { T_err = E_BAD_PARAMETER; return 0; }
    pthread_mutex_lock(&D.mu);
    unsigned ok = 1;
    if (attr == EGL_DEBUG_CALLBACK_KHR) *v = (intptr_t)D.cb;
    else if (attr >= EGL_DEBUG_MSG_CRITICAL_KHR && attr <= EGL_DEBUG_MSG_INFO_KHR)
        *v = D.on[attr - EGL_DEBUG_MSG_CRITICAL_KHR];
    else ok = 0;
    pthread_mutex_unlock(&D.mu);
    if (!ok) T_err = E_BAD_ATTRIBUTE;
    return ok;
}

EXPORT int eglLabelObjectKHR(void* d, unsigned type, void* obj, void* label) {
    if (type < EGL_OBJECT_THREAD_KHR || type > EGL_OBJECT_STREAM_KHR ||
        type == EGL_OBJECT_STREAM_KHR) { T_err = E_BAD_PARAMETER; return E_BAD_PARAMETER; }
    if (type == EGL_OBJECT_THREAD_KHR) { T_label = label; return E_SUCCESS; }
    if (!obj || (type == EGL_OBJECT_DISPLAY_KHR && obj != d)) {
        T_err = E_BAD_PARAMETER; return E_BAD_PARAMETER;
    }
    pthread_mutex_lock(&D.mu);
    dbg_lbl* l = g_lbls;
    for (; l; l = l->next) if (l->type == (int)type && l->obj == obj) break;
    if (!l && (l = calloc(1, sizeof *l))) {
        l->type = type; l->obj = obj; l->next = g_lbls; g_lbls = l;
    }
    if (l) l->label = label;
    pthread_mutex_unlock(&D.mu);
    if (!l) { T_err = E_BAD_ALLOC; return E_BAD_ALLOC; }
    return E_SUCCESS;
}

/* Forward declarations */
static void* rsym(const char* n);
static int pick_dmabuf_fd(const native_handle_t* nh, off_t min_size);

/* ---------- Init / Mali ---------- */
static int shim_init(void) {
    if (G.ready) return 0;
    pthread_mutex_lock(&G.mu);
    if (!G.ready) {
        G.mali = mali_sphal_load(MALI_SPHAL_DEFAULT);
        if (G.mali) {
            *(void**)&G.gpa = dlsym(G.mali, "eglGetProcAddress");
            G.ready = 1;
        } else {
            DBG("[shim] Failed to load Mali driver\n");
        }
    }
    pthread_mutex_unlock(&G.mu);
    return G.ready ? 0 : -1;
}

static void* rsym(const char* n) { return dlsym(G.mali, n); }

static __thread int T_ctx_e;

static void mali_err(const char* cmd) {
    static int (*ge)(void);
    if (!ge) *(void**)&ge = rsym("eglGetError");
    int e = ge ? ge() : E_SUCCESS;
    if (e == E_SUCCESS) return;
    T_err = e;                          /* application still observes it via eglGetError */
    int crit = (e == E_BAD_ALLOC || e == 0x300E);
    dbg_msg(e, cmd, crit ? EGL_DEBUG_MSG_CRITICAL_KHR : EGL_DEBUG_MSG_ERROR_KHR,
            0, NULL, "EGL error from driver");
}

#define PT(ret, name, params, args)                                         \
    EXPORT ret name params {                                                \
        if (shim_init()) return (ret)0;                                     \
        static void* p_;                                                    \
        if (!p_) p_ = rsym(#name);                                          \
        if (!p_) return (ret)0;                                             \
        ret r_ = ((ret (*) params)p_) args;                                 \
        if (!r_) mali_err(#name);                                           \
        return r_;                                                          \
}

static shim_surf* find_surf(void* p) {
    shim_surf* r = NULL;
    if (!p) return NULL;
    pthread_mutex_lock(&G.mu);
    for (shim_surf* s = G.surfs; s; s = s->next) if (s == p) { r = s; break; }
    pthread_mutex_unlock(&G.mu);
    return r;
}

static int ctx_ver(void* ctx) {
    int v = 2;
    pthread_mutex_lock(&G.mu);
    for (int i = 0; i < G.nctx; i++) if (G.ctx[i] == ctx) { v = G.ver[i]; break; }
    pthread_mutex_unlock(&G.mu);
    return v;
}

static void* glp(const char* n) {
    void* p = G.gpa ? G.gpa(n) : NULL;
    if (!p) p = rsym(n);
    return p;
}

static int gl_load(void) {
    if (gl_state_loaded) return gl_state_loaded > 0 ? 0 : -1;
    int miss = 0;
#define L(f, sym) do { *(void**)&GL.f = glp(sym); if (!GL.f) { DBG("[shim] Missing GL symbol: %s\n", sym); miss++; } } while (0)
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
    L(bindSampler, "glBindSampler"); L(flush, "glFlush"); L(finish, "glFinish");
#undef L
    gl_state_loaded = miss ? -1 : 1;
    return miss ? -1 : 0;
}

/* ---------- X11 ---------- */
static xcb_connection_t* xlib_to_xcb(void* xdpy) {
    static xcb_connection_t* (*f)(void*);
    if (!f) {
        void* h = dlopen("libX11-xcb.so", RTLD_NOW);
        if (!h) h = dlopen("libX11-xcb.so.1", RTLD_NOW);
        if (h) *(void**)&f = dlsym(h, "XGetXCBConnection");
    }
    return f ? f(xdpy) : NULL;
}

static int xlib_default_screen(void* xdpy) {
    static int (*f)(void*);
    if (!f) {
        void* h = dlopen("libX11.so", RTLD_NOW);
        if (!h) h = dlopen("libX11.so.6", RTLD_NOW);
        if (h) *(void**)&f = dlsym(h, "XDefaultScreen");
    }
    return f ? f(xdpy) : 0;
}

static void* shim_display(void* native, int scr, int is_xcb) {
    if (shim_init()) return NULL;
    pthread_mutex_lock(&G.mu);
    if (!G.dpy) {
        xcb_connection_t* xc = NULL;
        int own = 0, dscr = 0;
        if (native) {
            if (is_xcb) {
                xc = (xcb_connection_t*)native;
                if (scr < 0) scr = 0;
            } else {
                xc = xlib_to_xcb(native);
                if (scr < 0) scr = xlib_default_screen(native);
            }
        } else {
            xc = xcb_connect(NULL, &dscr);
            own = 1;
            if (scr < 0) scr = dscr;
        }
        if (!xc || xcb_connection_has_error(xc)) {
            DBG("[shim] No X11 connection (native=%p, xcb=%d)\n", native, is_xcb);
            if (own && xc) xcb_disconnect(xc);
            pthread_mutex_unlock(&G.mu);
            SET_ERR(E_BAD_DISPLAY);
            return NULL;
        }
        xcb_screen_iterator_t it = xcb_setup_roots_iterator(xcb_get_setup(xc));
        for (int i = 0; i < scr && it.rem; i++) xcb_screen_next(&it);
        if (scr < 0 || !it.rem) {
            DBG("[shim] Invalid X11 screen %d\n", scr);
            if (own) xcb_disconnect(xc);
            pthread_mutex_unlock(&G.mu);
            SET_ERR(E_BAD_ATTRIBUTE);
            return NULL;
        }
        G.root_visual = it.data ? it.data->root_visual : 0;
        G.xc = xc; G.own_xc = own; G.xcb = is_xcb;
        G.dpy = ((void* (*)(void*))rsym("eglGetDisplay"))(NULL);
        DBG("[shim] X11 %s (%s), screen %d, root visual 0x%x\n",
            own ? "own connection" : "shared connection", is_xcb ? "xcb" : "Xlib", scr, G.root_visual);
    }
    void* d = G.dpy;
    pthread_mutex_unlock(&G.mu);
    return d;
}

/* ====================================================================== */
/*                  DMA-BUF PATH: AHB + DRI3 + Present                   */
/* ====================================================================== */

static long now_us(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000000L + t.tv_nsec / 1000L;
}

static void x_sync(void) {
    free(xcb_get_input_focus_reply(G.xc, xcb_get_input_focus(G.xc), NULL));
}

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

static int present_setup(shim_surf* s) {
    const xcb_query_extension_reply_t* e3 = xcb_get_extension_data(G.xc, &xcb_dri3_id);
    const xcb_query_extension_reply_t* ep = xcb_get_extension_data(G.xc, &xcb_present_id);
    if (!e3 || !e3->present || !ep || !ep->present) {
        DBG("[shim] dmabuf: DRI3/Present extension missing\n");
        return -1;
    }
    xcb_dri3_query_version_reply_t* r3 =
        xcb_dri3_query_version_reply(G.xc, xcb_dri3_query_version(G.xc, 1, 0), NULL);
    if (!r3) { DBG("[shim] dmabuf: DRI3 query_version failed\n"); return -1; }
    free(r3);
    xcb_present_query_version_reply_t* rp =
        xcb_present_query_version_reply(G.xc, xcb_present_query_version(G.xc, 1, 0), NULL);
    if (!rp) { DBG("[shim] dmabuf: Present query_version failed\n"); return -1; }
    free(rp);

    xcb_connection_t* c = G.xc;
    const char* ne = getenv("SHIM_EVENT_THREAD");
    if (!(ne && !atoi(ne))) {
        xcb_connection_t* ec = xcb_connect(NULL, NULL);
        if (ec && !xcb_connection_has_error(ec)) {
            xcb_present_query_version_reply_t* r =
                xcb_present_query_version_reply(ec, xcb_present_query_version(ec, 1, 0), NULL);
            if (r) { free(r); s->ec = ec; c = ec; s->ev_active = 1; }
            else xcb_disconnect(ec);
        } else if (ec) xcb_disconnect(ec);
    }
    s->sec = c;
    s->eid = xcb_generate_id(c);
    s->se = xcb_register_for_special_xge(c, &xcb_present_id, s->eid, NULL);
    xcb_void_cookie_t ck = xcb_present_select_input_checked(
        c, s->eid, s->win, XCB_PRESENT_EVENT_MASK_IDLE_NOTIFY);
    xcb_generic_error_t* err = xcb_request_check(c, ck);
    if (err || !s->se) {
        DBG("[shim] dmabuf: present_select_input failed\n");
        free(err);
        if (s->se) { xcb_unregister_for_special_event(c, s->se); s->se = NULL; }
        if (s->ec) { xcb_disconnect(s->ec); s->ec = NULL; }
        s->sec = NULL; s->ev_active = 0;
        return -1;
    }
    if (s->ev_active) {
        uint32_t m = XCB_EVENT_MASK_STRUCTURE_NOTIFY;
        xcb_change_window_attributes(s->ec, s->win, XCB_CW_EVENT_MASK, &m);
        xcb_flush(s->ec);
    }
    DBG("[shim] Present: IDLE_NOTIFY via %s\n",
        s->ev_active ? "dedicated thread (separate connection)" : "polling");
    return 0;
}

static void ms_deadline(struct timespec* ts, long ms) {
    clock_gettime(CLOCK_MONOTONIC, ts);
    ts->tv_sec += ms / 1000;
    ts->tv_nsec += (ms % 1000) * 1000000L;
    if (ts->tv_nsec >= 1000000000L) { ts->tv_sec++; ts->tv_nsec -= 1000000000L; }
}

static void drain_events(shim_surf* s) {
    xcb_generic_event_t* ev;
    while ((ev = xcb_poll_for_special_event(s->sec, s->se))) {
        xcb_present_generic_event_t* ge = (xcb_present_generic_event_t*)ev;
        if (ge->evtype == XCB_PRESENT_EVENT_IDLE_NOTIFY) {
            xcb_present_idle_notify_event_t* in = (xcb_present_idle_notify_event_t*)ev;
            pthread_mutex_lock(&s->mu);
            for (int i = 0; i < s->nbuf; i++)
                if (s->slots[i].pixmap == in->pixmap && s->slots[i].busy) {
                    s->st_idle += now_us() - s->t_pres[i];
                    s->st_idle_n++;
                    s->slots[i].busy = 0;
                    if (s->inflight > 0) s->inflight--;
                }
            pthread_cond_broadcast(&s->cv_free);
            pthread_mutex_unlock(&s->mu);
        }
        free(ev);
    }
}

static int first_free(shim_surf* s) {
    for (int i = 0; i < s->nbuf; i++) {
        int k = (s->current_slot + i) % s->nbuf;
        if (!s->slots[k].busy) return k;
    }
    return -1;
}

static void present_slot(shim_surf* s, int slot) {
    static int copy = -1;
    if (copy < 0) { const char* v = getenv("SHIM_PRESENT_COPY"); copy = v && atoi(v); }
    uint32_t opts = (s->interval <= 0 ? XCB_PRESENT_OPTION_ASYNC : 0) | (copy ? XCB_PRESENT_OPTION_COPY : 0);
    
    if (shim_debug_enabled() && s->present_serial < 3) {
        xcb_void_cookie_t ck = xcb_present_pixmap_checked(G.xc, s->win, s->slots[slot].pixmap,
            ++s->present_serial, XCB_NONE, XCB_NONE, 0, 0, XCB_NONE, XCB_NONE, XCB_NONE,
            opts, 0, 0, 0, 0, NULL);
        xcb_generic_error_t* e = xcb_request_check(G.xc, ck);
        if (e) { DBG("[shim] present_pixmap ERROR %d (depth %d)\n", e->error_code, s->depth); free(e); }
    } else {
        xcb_present_pixmap(G.xc, s->win, s->slots[slot].pixmap, ++s->present_serial,
            XCB_NONE, XCB_NONE, 0, 0, XCB_NONE, XCB_NONE, XCB_NONE, opts, 0, 0, 0, 0, NULL);
    }
    xcb_flush(G.xc);
}

static void* present_worker(void* arg) {
    shim_surf* s = arg;
    int (*wait_sync)(void*, void*, int, uint64_t) = rsym("eglClientWaitSyncKHR");
    unsigned (*destroy_sync)(void*, void*) = rsym("eglDestroySyncKHR");

    pthread_mutex_lock(&s->mu);
    while (!s->stop) {
        if (s->qcount > 0) {
            int slot = s->q[s->qhead].slot;
            void* sync = s->q[s->qhead].sync;
            int ffd = s->q[s->qhead].fd;
            s->qhead = (s->qhead + 1) % MAX_BUFFERS;
            s->qcount--;
            pthread_mutex_unlock(&s->mu);

            long tf0 = now_us();
            if (ffd >= 0) {
                mali_fence_t f = { .fd = ffd, .sync = sync };
                int wr = mali_fence_wait(ffd, 1000);
                if (wr <= 0) DBG("[shim] native fence wait %s\n", wr == 0 ? "timed out" : "failed");
                mali_fence_release(&G.fctx, G.dpy, &f);
            } else {
                if (sync && wait_sync) wait_sync(G.dpy, sync, 0, 1000000000ULL);
                if (sync && destroy_sync) destroy_sync(G.dpy, sync);
            }
            long tf = now_us() - tf0;
            s->t_pres[slot] = now_us();
            present_slot(s, slot);

            pthread_mutex_lock(&s->mu);
            s->st_fence += tf;
            if (s->no_idle) { s->slots[slot].busy = 0; pthread_cond_broadcast(&s->cv_free); }
            else if (s->slots[slot].busy) s->inflight++;
            continue;
        }
        if (s->inflight > 0 && !s->no_idle && !s->ev_active) {
            struct timespec ts; ms_deadline(&ts, 1);
            pthread_cond_timedwait(&s->cv_job, &s->mu, &ts);
            if (s->qcount > 0 || s->stop) continue;
            pthread_mutex_unlock(&s->mu);
            x_sync();
            drain_events(s);
            pthread_mutex_lock(&s->mu);
            continue;
        }
        pthread_cond_wait(&s->cv_job, &s->mu);
    }
    pthread_mutex_unlock(&s->mu);
    return NULL;
}

static void* idle_thread(void* arg) {
    shim_surf* s = arg;
    struct pollfd pf[2];
    pf[0].fd = xcb_get_file_descriptor(s->ec); pf[0].events = POLLIN;
    pf[1].fd = s->wake[0];                     pf[1].events = POLLIN;
    for (;;) {
        pf[0].revents = pf[1].revents = 0;
        int r = poll(pf, 2, 500);
        if (pf[1].revents) break;
        if (r < 0 && errno != EINTR) break;
        xcb_generic_event_t* e;
        while ((e = xcb_poll_for_event(s->ec))) {
            if ((e->response_type & 0x7f) == XCB_CONFIGURE_NOTIFY) {
                xcb_configure_notify_event_t* cn = (xcb_configure_notify_event_t*)e;
                if (cn->window == s->win) {
                    __atomic_store_n(&s->pend_w, (int)cn->width, __ATOMIC_RELAXED);
                    __atomic_store_n(&s->pend_h, (int)cn->height, __ATOMIC_RELAXED);
                }
            }
            free(e);
        }
        drain_events(s);
        if (xcb_connection_has_error(s->ec)) break;
    }
    return NULL;
}

static int worker_start(shim_surf* s) {
    pthread_condattr_t ca;
    pthread_condattr_init(&ca);
    pthread_condattr_setclock(&ca, CLOCK_MONOTONIC);
    pthread_mutex_init(&s->mu, NULL);
    pthread_cond_init(&s->cv_job, &ca);
    pthread_cond_init(&s->cv_free, &ca);
    pthread_condattr_destroy(&ca);
    s->stop = s->qhead = s->qcount = s->inflight = 0;
    if (s->ev_active) {
        if (pipe2(s->wake, O_CLOEXEC)) return -1;
        if (pthread_create(&s->evth, NULL, idle_thread, s)) { close(s->wake[0]); close(s->wake[1]); return -1; }
        s->ev_started = 1;
    }
    if (pthread_create(&s->th, NULL, present_worker, s)) {
        if (s->ev_started) {
            char c = 1; (void)!write(s->wake[1], &c, 1);
            pthread_join(s->evth, NULL);
            close(s->wake[0]); close(s->wake[1]);
            s->ev_started = 0;
        }
        return -1;
    }
    s->th_started = 1;
    return 0;
}

static void worker_stop(shim_surf* s) {
    if (!s->th_started) return;
    pthread_mutex_lock(&s->mu);
    s->stop = 1;
    pthread_cond_broadcast(&s->cv_job);
    pthread_cond_broadcast(&s->cv_free);
    pthread_mutex_unlock(&s->mu);
    pthread_join(s->th, NULL);
    if (s->ev_started) {
        char c = 1; (void)!write(s->wake[1], &c, 1);
        pthread_join(s->evth, NULL);
        close(s->wake[0]); close(s->wake[1]);
        s->ev_started = 0;
    }
    unsigned (*destroy_sync)(void*, void*) = rsym("eglDestroySyncKHR");
    while (s->qcount > 0) {
        if (s->q[s->qhead].fd >= 0) {
            mali_fence_t f = { .fd = s->q[s->qhead].fd, .sync = s->q[s->qhead].sync };
            mali_fence_release(&G.fctx, G.dpy, &f);
        } else if (s->q[s->qhead].sync && destroy_sync) {
            destroy_sync(G.dpy, s->q[s->qhead].sync);
        }
        s->qhead = (s->qhead + 1) % MAX_BUFFERS; s->qcount--;
    }
    pthread_mutex_destroy(&s->mu);
    pthread_cond_destroy(&s->cv_job);
    pthread_cond_destroy(&s->cv_free);
    s->th_started = 0;
}

static int acquire_slot(shim_surf* s) {
    pthread_mutex_lock(&s->mu);
    int k = first_free(s);
    if (k < 0 && !s->no_idle) {
        struct timespec ts; ms_deadline(&ts, 300);
        while ((k = first_free(s)) < 0) {
            if (pthread_cond_timedwait(&s->cv_free, &s->mu, &ts) == ETIMEDOUT) {
                DBG("[shim] present: server not sending IDLE_NOTIFY, continuing without waiting\n");
                s->no_idle = 1;
                s->inflight = 0;
                for (int i = 0; i < s->nbuf; i++) s->slots[i].busy = 0;
                k = s->current_slot;
                break;
            }
        }
    }
    if (k < 0) k = s->current_slot;
    pthread_mutex_unlock(&s->mu);
    return k;
}

static void slot_free(dmabuf_slot_t* sl, int with_gl) {
    unsigned (*destroy_img)(void*, void*) = rsym("eglDestroyImageKHR");
    if (with_gl && gl_state_loaded > 0) {
        if (sl->fbo) GL.deleteFramebuffers(1, &sl->fbo);
        if (sl->tex) GL.deleteTextures(1, &sl->tex);
    }
    if (sl->image && destroy_img) destroy_img(G.dpy, sl->image);
    if (sl->pixmap) xcb_free_pixmap(G.xc, sl->pixmap);
    if (sl->ab && ahb_release) ahb_release(sl->ab);
    memset(sl, 0, sizeof *sl);
}

static void dmabuf_teardown(shim_surf* s, int with_gl) {
    worker_stop(s);
    if (with_gl && gl_state_loaded > 0) {
        if (s->app_fbo) GL.deleteFramebuffers(1, &s->app_fbo);
        if (s->app_tex) GL.deleteTextures(1, &s->app_tex);
        if (s->app_rbo) GL.deleteRenderbuffers(1, &s->app_rbo);
        if (s->prog) GL.deleteProgram(s->prog);
        if (s->vao) GL.deleteVertexArrays(1, &s->vao);
        if (s->vbo) GL.deleteBuffers(1, &s->vbo);
    }
    if (s->app_img) {
        unsigned (*di)(void*, void*) = rsym("eglDestroyImageKHR");
        if (di) di(G.dpy, s->app_img);
        s->app_img = NULL;
    }
    if (s->app_ab && ahb_release) { ahb_release(s->app_ab); s->app_ab = NULL; }
    for (int i = 0; i < s->nbuf; i++) slot_free(&s->slots[i], with_gl);
    if (s->se && s->sec) { xcb_unregister_for_special_event(s->sec, s->se); s->se = NULL; }
    if (s->ec) { xcb_disconnect(s->ec); s->ec = NULL; }
    s->sec = NULL; s->ev_active = 0;
    xcb_flush(G.xc);
    s->app_fbo = s->app_tex = s->app_rbo = s->prog = s->vao = s->vbo = 0;
    s->dmabuf_inited = 0;
    s->current_slot = 0;
    s->no_idle = 0;
}

static unsigned compile_sh(unsigned type, const char* src) {
    unsigned sh = GL.createShader(type);
    GL.shaderSource(sh, 1, &src, NULL);
    GL.compileShader(sh);
    int ok = 0; GL.getShaderiv(sh, C_COMPILE_STATUS, &ok);
    if (!ok) {
        if (shim_debug_enabled()) {
            char log[512]; GL.getShaderInfoLog(sh, sizeof log, NULL, log);
            fprintf(stderr, "[shim] Shader compilation failed: %s\n", log);
        }
        GL.deleteShader(sh); return 0;
    }
    return sh;
}

static unsigned build_blit_program(int swz, int argb) {
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
        "  c = vec4(v.%s, %s);\n"
        "}\n", swz ? "bgr" : "rgb", argb ? "v.a" : "1.0");
    unsigned v = compile_sh(C_VERTEX_SHADER, vs);
    unsigned f = compile_sh(C_FRAGMENT_SHADER, fs);
    if (!v || !f) {
        if (v) GL.deleteShader(v);
        if (f) GL.deleteShader(f);
        return 0;
    }
    unsigned p = GL.createProgram();
    GL.attachShader(p, v); GL.attachShader(p, f);
    GL.linkProgram(p);
    GL.deleteShader(v); GL.deleteShader(f);
    int ok = 0; GL.getProgramiv(p, C_LINK_STATUS, &ok);
    if (!ok) {
        if (shim_debug_enabled()) {
            char log[512]; GL.getProgramInfoLog(p, sizeof log, NULL, log);
            fprintf(stderr, "[shim] Program linking failed: %s\n", log);
        }
        GL.deleteProgram(p); return 0;
    }
    return p;
}

static void* alloc_ahb(int w, int h, int fmt, AHBDesc* out) {
    AHBDesc d = { .width = w, .height = h, .layers = 1, .format = fmt,
                  .usage = AHB_USAGE_GPU_COLOR_OUT | AHB_USAGE_GPU_SAMPLED | AHB_USAGE_CPU_READ_RARELY };
    void* ab = NULL;
    if (ahb_alloc(&d, &ab) || !ab) return NULL;
    memset(out, 0, sizeof *out);
    ahb_desc(ab, out);
    return ab;
}

static int pick_dmabuf_fd(const native_handle_t* nh, off_t min_size) {
    for (int i = 0; i < nh->numFds; i++) {
        off_t sz = lseek(nh->data[i], 0, SEEK_END);
        if (sz < 0) continue;
        lseek(nh->data[i], 0, SEEK_SET);
        if (sz >= min_size) return nh->data[i];
    }
    for (int i = 0; i < nh->numFds; i++) {
        char link[64], path[256] = {0};
        snprintf(link, sizeof link, "/proc/self/fd/%d", nh->data[i]);
        ssize_t n = readlink(link, path, sizeof path - 1);
        if (n > 0 && strstr(path, "dmabuf")) return nh->data[i];
    }
    return nh->numFds > 1 ? nh->data[1] : (nh->numFds == 1 ? nh->data[0] : -1);
}

static void* make_egl_image(void* ab, void* (*get_cb)(void*),
                            void* (*create_img)(void*, void*, unsigned, void*, const int*),
                            int fd, int w, int h, uint32_t stride, int fmt, int verbose) {
    int (*get_err)(void) = rsym("eglGetError");
    const char* imp = getenv("SHIM_IMPORT");
    int use_native = !(imp && !strcmp(imp, "dmabuf"));
    int use_dmabuf = !(imp && !strcmp(imp, "native"));
    if (verbose) DBG("[shim] import mode: %s\n", !use_dmabuf ? "native_buffer only" :
                                                 !use_native ? "dma-buf only" : "native_buffer, then dma-buf");
    if (verbose && shim_debug_enabled()) {
        const char* (*qs)(void*, int) = rsym("eglQueryString");
        const char* ex = qs ? qs(G.dpy, 0x3055) : NULL;
        fprintf(stderr, "[shim] EGL ext: native_buffer=%d dma_buf_import=%d\n",
                ex && strstr(ex, "EGL_ANDROID_image_native_buffer") != NULL,
                ex && strstr(ex, "EGL_EXT_image_dma_buf_import") != NULL);
    }
    void* cbs[2]; const char* names[2]; int n = 0;
    if (use_native && get_cb)      { cbs[n] = get_cb(ab);      names[n++] = "eglGetNativeClientBufferANDROID"; }
    if (use_native && ahb_to_anwb) { cbs[n] = ahb_to_anwb(ab); names[n++] = "AHardwareBuffer_to_ANativeWindowBuffer"; }
    if (verbose)
        DBG("[shim] client buffer sources: eglGetNativeClientBufferANDROID=%s, to_ANativeWindowBuffer=%s\n",
            get_cb ? "yes" : "no", ahb_to_anwb ? "yes" : "no");

    static const int attr_pres[] = { 0x30D2, 1, 0x3038 };
    static const int attr_none[] = { 0x3038 };
    for (int i = 0; i < n; i++) {
        if (!cbs[i]) continue;
        for (int a = 0; a < 2; a++) {
            void* img = create_img(G.dpy, NULL, 0x3140, cbs[i], a ? attr_none : attr_pres);
            if (img) {
                if (verbose) DBG("[shim] EGLImage via native_buffer (%s)\n", names[i]);
                return img;
            }
            if (verbose)
                DBG("[shim] native_buffer (%s, attr=%d) failed: eglError=0x%x\n",
                    names[i], a, get_err ? get_err() : 0);
        }
    }
    if (!use_dmabuf) {
        if (verbose) DBG("[shim] native_buffer failed and SHIM_IMPORT=native: not trying dma-buf\n");
        return NULL;
    }
    int fourcc = (fmt == 5) ? 0x34325241 : 0x34324241;
    int da[] = { 0x3057, w, 0x3056, h, 0x3271, fourcc, 0x3272, fd,
                 0x3273, 0, 0x3274, (int)stride,
                 0x3443, 0, 0x3444, 0,
                 0x30D2, 1, 0x3038 };
    void* img = create_img(G.dpy, NULL, 0x3270, NULL, da);
    if (img) {
        if (verbose) DBG("[shim] EGLImage via dma_buf_import\n");
        return img;
    }
    if (verbose) DBG("[shim] dma_buf_import failed: eglError=0x%x\n", get_err ? get_err() : 0);
    return NULL;
}

static int build_slot(shim_surf* s, int idx, int fmt, int verbose,
                      void* (*get_cb)(void*),
                      void* (*create_img)(void*, void*, unsigned, void*, const int*),
                      void (*img_target)(unsigned, void*)) {
    dmabuf_slot_t* sl = &s->slots[idx];
    AHBDesc d;
    sl->ab = alloc_ahb(s->w, s->h, fmt, &d);
    if (!sl->ab) {
        if (verbose) DBG("[shim] dmabuf: AHB allocation failed (format %d, slot %d)\n", fmt, idx);
        return -1;
    }
    uint32_t stride = d.stride * 4;
    if (stride == 0 || stride > 65535) {
        if (verbose) DBG("[shim] dmabuf: stride %u bytes does not fit DRI3 1.0\n", stride);
        goto fail;
    }
    if (verbose) {
        DBG("[shim] dmabuf: AHB format %d (%s), stride %u px (%u bytes)\n",
            fmt, fmt == 5 ? "native BGRA" : "RGBA + swizzle", d.stride, stride);
    }

    const native_handle_t* nh = ahb_handle(sl->ab);
    if (!nh) { if (verbose) DBG("[shim] dmabuf: getNativeHandle returned NULL\n"); goto fail; }
    uint32_t size = stride * s->h;
    int fd = pick_dmabuf_fd(nh, (off_t)size);
    if (fd < 0) { if (verbose) DBG("[shim] dmabuf: Cannot find dma-buf fd\n"); goto fail; }
    if (verbose) DBG("[shim] dmabuf: using fd %d of %d\n", fd, nh->numFds);

    int dfd = dup(fd);
    if (dfd < 0) goto fail;
    sl->pixmap = xcb_generate_id(G.xc);
    xcb_void_cookie_t ck = xcb_dri3_pixmap_from_buffer_checked(
        G.xc, sl->pixmap, s->win, size, s->w, s->h, (uint16_t)stride,
        (uint8_t)(s->depth ? s->depth : 24), 32, dfd);
    xcb_generic_error_t* err = xcb_request_check(G.xc, ck);
    if (err) {
        if (verbose)
            DBG("[shim] dmabuf: DRI3 pixmap_from_buffer rejected (error %d, format %d)\n",
                err->error_code, fmt);
        free(err); sl->pixmap = 0; goto fail;
    }

    sl->image = make_egl_image(sl->ab, get_cb, create_img, fd, s->w, s->h, stride, fmt, verbose);
    if (!sl->image) { if (verbose) DBG("[shim] dmabuf: All EGLImage variants failed (format %d)\n", fmt); goto fail; }

    GL.genTextures(1, &sl->tex);
    GL.bindTexture(C_TEXTURE_2D, sl->tex);
    GL.texParameteri(C_TEXTURE_2D, C_MIN_FILTER, C_NEAREST);
    GL.texParameteri(C_TEXTURE_2D, C_MAG_FILTER, C_NEAREST);
    img_target(C_TEXTURE_2D, sl->image);
    GL.genFramebuffers(1, &sl->fbo);
    GL.bindFramebuffer(C_FRAMEBUFFER, sl->fbo);
    GL.framebufferTexture2D(C_FRAMEBUFFER, C_COLOR_ATT0, C_TEXTURE_2D, sl->tex, 0);
    if (GL.checkFramebufferStatus(C_FRAMEBUFFER) != C_FB_COMPLETE) {
        if (verbose) DBG("[shim] dmabuf: FBO slot %d incomplete (format %d)\n", idx, fmt);
        goto fail;
    }
    return 0;
fail:
    slot_free(sl, 1);
    return -1;
}

static int app_tex_afbc(shim_surf* s, void* (*get_cb)(void*),
                        void* (*create_img)(void*, void*, unsigned, void*, const int*),
                        void (*img_target)(unsigned, void*)) {
    AHBDesc q = { .width = s->w, .height = s->h, .layers = 1, .format = 1,
                  .usage = AHB_USAGE_GPU_COLOR_OUT | AHB_USAGE_GPU_SAMPLED };
    AHBDesc d;
    void* ab = NULL;
    if (ahb_alloc(&q, &ab) || !ab) { DBG("[shim] app texture AFBC: alloc failed\n"); return 0; }
    memset(&d, 0, sizeof d);
    ahb_desc(ab, &d);

    void* cb = ahb_to_anwb ? ahb_to_anwb(ab) : (get_cb ? get_cb(ab) : NULL);
    static const int attr[] = { 0x30D2, 1, 0x3038 };
    void* img = cb ? create_img(G.dpy, NULL, 0x3140, cb, attr) : NULL;
    if (!img) { DBG("[shim] app texture AFBC: EGLImage failed\n"); ahb_release(ab); return 0; }

    img_target(C_TEXTURE_2D, img);

    unsigned f = 0;
    GL.genFramebuffers(1, &f);
    GL.bindFramebuffer(C_FRAMEBUFFER, f);
    GL.framebufferTexture2D(C_FRAMEBUFFER, C_COLOR_ATT0, C_TEXTURE_2D, s->app_tex, 0);
    unsigned st = GL.checkFramebufferStatus(C_FRAMEBUFFER);
    GL.bindFramebuffer(C_FRAMEBUFFER, 0);
    GL.deleteFramebuffers(1, &f);
    if (st != C_FB_COMPLETE) {
        unsigned (*di)(void*, void*) = rsym("eglDestroyImageKHR");
        DBG("[shim] app texture AFBC: FBO incomplete, fallback\n");
        if (di) di(G.dpy, img);
        ahb_release(ab);
        return 0;
    }

    s->app_ab = ab; s->app_img = img;
    if (shim_debug_enabled()) {
        const native_handle_t* nh = ahb_handle(ab);
        int fd = nh ? pick_dmabuf_fd(nh, 1) : -1;
        long long sz = -1;
        if (fd >= 0) { sz = lseek(fd, 0, SEEK_END); lseek(fd, 0, SEEK_SET); }
        long long lin = (long long)d.stride * 4 * s->h;
        fprintf(stderr, "[shim] app texture: AHB stride %u px, fd size %lld, linear %lld => %s\n",
                d.stride, sz, lin, sz > lin ? "AFBC (headers + body)" : "appears LINEAR");
    }
    return 1;
}

static int dmabuf_build(shim_surf* s) {
    void* (*create_img)(void*, void*, unsigned, void*, const int*) = rsym("eglCreateImageKHR");
    void* (*get_cb)(void*) = G.gpa ? (void* (*)(void*))G.gpa("eglGetNativeClientBufferANDROID") : NULL;
    void (*img_target)(unsigned, void*) =
        G.gpa ? (void (*)(unsigned, void*))G.gpa("glEGLImageTargetTexture2DOES") : NULL;
    if (!create_img || !img_target) {
        DBG("[shim] dmabuf: Missing eglCreateImageKHR / glEGLImageTargetTexture2DOES\n");
        return -1;
    }

    GL.genTextures(1, &s->app_tex);
    GL.bindTexture(C_TEXTURE_2D, s->app_tex);
    const char* af = getenv("SHIM_AFBC");
    if (!(af && atoi(af) && app_tex_afbc(s, get_cb, create_img, img_target)))
        GL.texImage2D(C_TEXTURE_2D, 0, C_RGBA8, s->w, s->h, 0, C_RGBA, C_UBYTE, NULL);
    GL.texParameteri(C_TEXTURE_2D, C_MIN_FILTER, C_NEAREST);
    GL.texParameteri(C_TEXTURE_2D, C_MAG_FILTER, C_NEAREST);
    GL.texParameteri(C_TEXTURE_2D, C_WRAP_S, C_CLAMP_EDGE);
    GL.texParameteri(C_TEXTURE_2D, C_WRAP_T, C_CLAMP_EDGE);
    GL.genRenderbuffers(1, &s->app_rbo);
    GL.bindRenderbuffer(C_RENDERBUFFER, s->app_rbo);
    GL.renderbufferStorage(C_RENDERBUFFER, C_DEPTH24_STENCIL8, s->w, s->h);
    GL.genFramebuffers(1, &s->app_fbo);
    GL.bindFramebuffer(C_FRAMEBUFFER, s->app_fbo);
    GL.framebufferTexture2D(C_FRAMEBUFFER, C_COLOR_ATT0, C_TEXTURE_2D, s->app_tex, 0);
    GL.framebufferRenderbuffer(C_FRAMEBUFFER, C_DEPTH_STENCIL_ATT, C_RENDERBUFFER, s->app_rbo);
    if (GL.checkFramebufferStatus(C_FRAMEBUFFER) != C_FB_COMPLETE) {
        DBG("[shim] dmabuf: Application FBO incomplete\n");
        return -1;
    }

    static const float quad[8] = { -1, -1, 1, -1, -1, 1, 1, 1 };
    GL.genVertexArrays(1, &s->vao);
    GL.bindVertexArray(s->vao);
    GL.genBuffers(1, &s->vbo);
    GL.bindBuffer(C_ARRAY_BUFFER, s->vbo);
    GL.bufferData(C_ARRAY_BUFFER, sizeof quad, quad, C_STATIC_DRAW);
    GL.enableVertexAttribArray(0);
    GL.vertexAttribPointer(0, 2, C_FLOAT, 0, 0, NULL);

    const char* fe = getenv("SHIM_FORMAT");
    int try_fmt[2] = { fe ? atoi(fe) : 5, 1 };
    int nfmt = fe ? 1 : 2;
    int ok = 0;
    for (int f = 0; f < nfmt && !ok; f++) {
        if (build_slot(s, 0, try_fmt[f], 1, get_cb, create_img, img_target) == 0) {
            s->fmt = try_fmt[f];
            ok = 1;
        } else if (f + 1 < nfmt) {
            DBG("[shim] dmabuf: format %d unusable, trying format %d\n", try_fmt[f], try_fmt[f + 1]);
        }
    }
    if (!ok) return -1;
    s->swz = (s->fmt != 5);

    for (int i = 1; i < s->nbuf; i++)
        if (build_slot(s, i, s->fmt, 0, get_cb, create_img, img_target)) {
            DBG("[shim] dmabuf: building slot %d failed\n", i);
            return -1;
        }

    int argb = (s->depth == 32) && !getenv("SHIM_OPAQUE");
    DBG("[shim] blit: depth=%d fmt=%d alpha=%s\n", s->depth, s->fmt, argb ? "copy" : "1.0");
    s->prog = build_blit_program(s->swz, argb);
    if (!s->prog) return -1;
    return 0;
}

static int dmabuf_init(shim_surf* s, int ver) {
    const char* e = getenv("SHIM_DMABUF");
    if (e && !atoi(e)) return -1;
    const char* nb = getenv("SHIM_BUFFERS");
    s->nbuf = nb ? atoi(nb) : 4;
    if (s->nbuf < 2) s->nbuf = 2;
    if (s->nbuf > MAX_BUFFERS) s->nbuf = MAX_BUFFERS;
    const char* mb = getenv("SHIM_MAILBOX");
    s->mailbox = mb && atoi(mb);
    if (ver < 3) {
        DBG("[shim] dmabuf: Requires GLES3 (ver=%d)\n", ver);
        return -1;
    }
    if (s->w <= 0 || s->h <= 0) { DBG("[shim] dmabuf: invalid size %dx%d\n", s->w, s->h); return -1; }
    if (ahb_init()) return -1;
    if (gl_load()) return -1;
    if (present_setup(s)) return -1;

    gl_saved st;
    gl_save(&st);
    int r = dmabuf_build(s);
    gl_restore(&st);
    if (r) { dmabuf_teardown(s, 1); return -1; }

    GL.bindFramebuffer(C_FRAMEBUFFER, s->app_fbo);
    GL.viewport(0, 0, s->w, s->h);
    GL.scissor(0, 0, s->w, s->h);
    if (worker_start(s)) {
        DBG("[shim] dmabuf: Failed to start worker thread\n");
        dmabuf_teardown(s, 1);
        return -1;
    }
    s->dmabuf_inited = 1;
    s->frames = 0;
    DBG("[shim] dmabuf: ZERO-COPY PATH ACTIVE (%dx%d, %d buffers)\n", s->w, s->h, s->nbuf);
    return 0;
}

static int dmabuf_resize(shim_surf* s, int nw, int nh) {
    int ver = ctx_ver(s->ctx);
    int ow = s->w, oh = s->h;
    DBG("[shim] resize %dx%d -> %dx%d\n", ow, oh, nw, nh);
    dmabuf_teardown(s, 1);
    s->pend_w = s->pend_h = 0;
    s->w = nw; s->h = nh;
    if (dmabuf_init(s, ver) == 0) return 0;
    DBG("[shim] resize failed, restoring old size\n");
    s->w = ow; s->h = oh;
    if (dmabuf_init(s, ver) == 0) return 0;
    s->dmabuf_failed = 1;
    DBG("[shim] resize: could not restore the old buffers, presenting disabled\n");
    return -1;
}

static void dmabuf_rebind(shim_surf* s) {
    int cur = 0;
    GL.getIntegerv(C_DRAW_FB_BINDING, &cur);
    if (cur == 0) GL.bindFramebuffer(C_FRAMEBUFFER, s->app_fbo);
}

static void stats_report(shim_surf* s, long now) {
    if (!s->st_t0) s->st_t0 = now;
    if (now - s->st_t0 < 1000000) return;
    pthread_mutex_lock(&s->mu);
    double secs = (now - s->st_t0) / 1e6;
    double n = s->st_frames ? s->st_frames : 1;
    DBG("[stats] app %.0f fps | presented %.0f/s | dropped %d | wait %.2f ms | blit+enqueue %.2f ms | "
        "fence %.2f ms | presented->idle %.2f ms\n",
        s->st_app / secs, s->st_frames / secs, s->st_drop,
        s->st_acq / 1000.0 / n, s->st_blit / 1000.0 / n, s->st_fence / 1000.0 / n,
        s->st_idle_n ? s->st_idle / 1000.0 / s->st_idle_n : 0.0);
    s->st_acq = s->st_blit = s->st_fence = s->st_idle = 0;
    s->st_frames = s->st_idle_n = s->st_app = s->st_drop = 0;
    s->st_t0 = now;
    pthread_mutex_unlock(&s->mu);
}

static void dmabuf_swap(shim_surf* s) {
    static int mode = -1;
    static void* (*create_sync)(void*, unsigned, const int*);
    if (mode < 0) {
        const char* v = getenv("SHIM_SYNC");
        mode = (v && !strcmp(v, "finish")) ? 1 : (v && !strcmp(v, "flush")) ? 2 :
               (v && !strcmp(v, "implicit")) ? 3 : (v && !strcmp(v, "native")) ? 4 : 0;
        create_sync = rsym("eglCreateSyncKHR");
        if (mode == 4) {
            G.fctx_ok = mali_fence_init(&G.fctx, G.mali);
            if (!G.fctx_ok) {
                DBG("[shim] native fence unavailable (eglDupNativeFenceFDANDROID missing), "
                    "using EGL fence\n");
                mode = 0;
            }
        }
        if ((mode == 0 || mode == 3) && (!create_sync || !rsym("eglClientWaitSyncKHR"))) mode = 1;
        DBG("[shim] sync: %s | buffers: %d | mailbox: %s\n",
            mode == 0 ? "EGL fence (full)" : mode == 1 ? "glFinish" : mode == 2 ? "glFlush"
            : mode == 3 ? "ARM implicit_external_sync fence" : "native fence fd (poll)",
            s->nbuf, (s->mailbox) ? "yes" : "no");
    }
    static int stats = -1;
    if (stats < 0) { const char* v = getenv("SHIM_STATS"); stats = v && atoi(v); }

    if (s->ev_active) {
        int pw = __atomic_load_n(&s->pend_w, __ATOMIC_RELAXED);
        int ph = __atomic_load_n(&s->pend_h, __ATOMIC_RELAXED);
        if (pw > 0 && ph > 0 && (pw != s->w || ph != s->h)) {
            dmabuf_resize(s, pw, ph);
            return;
        }
    }

    if (stats) s->st_app++;

    long ta0 = now_us();
    int k;
    if (s->mailbox) {
        pthread_mutex_lock(&s->mu);
        k = first_free(s);
        pthread_mutex_unlock(&s->mu);
        if (k < 0 && !s->no_idle) {
            s->st_drop++;
            if (stats) stats_report(s, now_us());
            return;
        }
        if (k < 0) k = s->current_slot;
    } else k = acquire_slot(s);
    long ta1 = now_us();
    dmabuf_slot_t* sl = &s->slots[k];

    gl_saved st;
    gl_save(&st);
    GL.bindFramebuffer(C_DRAW_FB, sl->fbo);
    GL.viewport(0, 0, s->w, s->h);
    for (int i = 0; i < 5; i++) GL.disable(EN_CAPS[i]);
    GL.colorMask(1, 1, 1, 1);
    GL.activeTexture(C_TEXTURE0);
    GL.bindSampler(0, 0);
    GL.bindTexture(C_TEXTURE_2D, s->app_tex);
    GL.useProgram(s->prog);
    GL.bindVertexArray(s->vao);
    GL.drawArrays(C_TRI_STRIP, 0, 4);
    gl_restore(&st);

    void* sync = NULL;
    int ffd = -1;
    if (mode == 4) {
        mali_fence_t f = mali_fence_create(&G.fctx, G.dpy);
        if (f.fd >= 0) { sync = f.sync; ffd = f.fd; }
        else GL.finish();
    } else if (mode == 0 || mode == 3) {
        static const int impl[] = { 0x30F8, 0x328A, 0x3038 };
        sync = create_sync(G.dpy, 0x30F9, mode == 3 ? impl : NULL);
        if (!sync && mode == 3) sync = create_sync(G.dpy, 0x30F9, NULL);
        GL.flush();
        if (!sync) GL.finish();
    } else if (mode == 1) GL.finish();
    else GL.flush();

    pthread_mutex_lock(&s->mu);
    sl->busy = 1;
    s->q[(s->qhead + s->qcount) % MAX_BUFFERS].slot = k;
    s->q[(s->qhead + s->qcount) % MAX_BUFFERS].sync = sync;
    s->q[(s->qhead + s->qcount) % MAX_BUFFERS].fd = ffd;
    s->qcount++;
    pthread_cond_signal(&s->cv_job);
    pthread_mutex_unlock(&s->mu);

    s->current_slot = (k + 1) % s->nbuf;
    s->frames++;

    if (stats) {
        long now = now_us();
        s->st_frames++;
        s->st_acq += ta1 - ta0;
        s->st_blit += now - ta1;
        stats_report(s, now);
    }
}

/* ====================================================================== */
/*          GBM PATH: gbm_surface (libgbm_ahb) -> EGLImage -> FBO         */
/* ====================================================================== */
static struct {
    int state;
    void* (*anwb)(void*);
    void* (*deq)(void*);
    void  (*enq)(void*, void*);
    int   (*size)(void*, uint32_t*, uint32_t*);
} GB;

static int gbm_load(void) {
    if (GB.state) return GB.state > 0 ? 0 : -1;
    void* h = dlopen("libgbm.so.1", RTLD_NOW | RTLD_GLOBAL);
    if (h) {
        *(void**)&GB.anwb = dlsym(h, "gbm_bo_get_anwb");
        *(void**)&GB.deq  = dlsym(h, "gbm_surface_ahb_dequeue");
        *(void**)&GB.enq  = dlsym(h, "gbm_surface_ahb_queue");
        *(void**)&GB.size = dlsym(h, "gbm_surface_ahb_size");
    }
    GB.state = (GB.anwb && GB.deq && GB.enq && GB.size) ? 1 : -1;
    if (GB.state < 0) DBG("[shim] gbm: libgbm.so.1 (AHB) missing or incomplete\n");
    return GB.state > 0 ? 0 : -1;
}

static void* gbm_display(void* native) {
    (void)native;
    if (shim_init()) { SET_ERR(E_BAD_DISPLAY); return NULL; }
    pthread_mutex_lock(&G.mu);
    if (!G.gdpy) {
        void* (*gd)(void*) = (void* (*)(void*))rsym("eglGetDisplay");
        G.gdpy = gd ? gd(NULL) : NULL;
    }
    void* d = G.gdpy;
    pthread_mutex_unlock(&G.mu);
    if (!d) SET_ERR(E_BAD_DISPLAY);
    DBG("[shim] GBM platform display %p\n", d);
    return d;
}

static int is_gbm_display(void* d) { return G.gdpy && d == G.gdpy && !G.dpy; }

static void gbm_teardown(shim_surf* s, int with_gl) {
    unsigned (*di)(void*, void*) = rsym("eglDestroyImageKHR");
    for (int i = 0; i < s->ngsl; i++) {
        if (with_gl && gl_state_loaded > 0) {
            if (s->gsl[i].fbo) GL.deleteFramebuffers(1, &s->gsl[i].fbo);
            if (s->gsl[i].tex) GL.deleteTextures(1, &s->gsl[i].tex);
        }
        if (s->gsl[i].img && di) di(G.gdpy, s->gsl[i].img);
    }
    if (with_gl && s->grbo && gl_state_loaded > 0) GL.deleteRenderbuffers(1, &s->grbo);
    memset(s->gsl, 0, sizeof s->gsl);
    s->ngsl = 0; s->grbo = 0; s->cur_bo = NULL; s->gcur = -1; s->gbm_ok = 0;
}

static int gbm_slot(shim_surf* s, void* bo) {
    for (int i = 0; i < s->ngsl; i++) if (s->gsl[i].bo == bo) return i;
    if (s->ngsl >= GBM_MAX_SLOTS) return -1;
    void* (*create_img)(void*, void*, unsigned, void*, const int*) = rsym("eglCreateImageKHR");
    void (*img_target)(unsigned, void*) =
        G.gpa ? (void (*)(unsigned, void*))G.gpa("glEGLImageTargetTexture2DOES") : NULL;
    void* anwb = GB.anwb(bo);
    if (!create_img || !img_target || !anwb) return -1;

    static const int attr[] = { 0x30D2, 1, 0x3038 };
    void* img = create_img(G.gdpy, NULL, 0x3140, anwb, attr);
    if (!img) { DBG("[shim] gbm: eglCreateImageKHR failed\n"); return -1; }

    gl_saved st; gl_save(&st);
    int k = s->ngsl;
    unsigned tex = 0, fbo = 0;
    GL.genTextures(1, &tex);
    GL.bindTexture(C_TEXTURE_2D, tex);
    img_target(C_TEXTURE_2D, img);
    GL.genFramebuffers(1, &fbo);
    GL.bindFramebuffer(C_FRAMEBUFFER, fbo);
    GL.framebufferTexture2D(C_FRAMEBUFFER, C_COLOR_ATT0, C_TEXTURE_2D, tex, 0);
    if (s->grbo)
        GL.framebufferRenderbuffer(C_FRAMEBUFFER, C_DEPTH_STENCIL_ATT, C_RENDERBUFFER, s->grbo);
    unsigned fs = GL.checkFramebufferStatus(C_FRAMEBUFFER);
    gl_restore(&st);
    if (fs != C_FB_COMPLETE) {
        DBG("[shim] gbm: FBO incomplete (0x%x)\n", fs);
        unsigned (*di)(void*, void*) = rsym("eglDestroyImageKHR");
        GL.deleteFramebuffers(1, &fbo); GL.deleteTextures(1, &tex);
        if (di) di(G.gdpy, img);
        return -1;
    }
    s->gsl[k].bo = bo; s->gsl[k].img = img; s->gsl[k].tex = tex; s->gsl[k].fbo = fbo;
    s->ngsl++;
    DBG("[shim] gbm: slot %d bo=%p fbo=%u\n", k, bo, fbo);
    return k;
}

static void* make_gbm_surface(void* gs) {
    if (gbm_load()) { SET_ERR(E_BAD_NATIVE_WINDOW); return NULL; }
    uint32_t w = 0, h = 0;
    if (!gs || GB.size(gs, &w, &h)) { SET_ERR(E_BAD_NATIVE_WINDOW); return NULL; }
    shim_surf* s = calloc(1, sizeof *s);
    if (!s) { SET_ERR(E_BAD_ALLOC); return NULL; }
    s->magic = SURF_MAGIC; s->is_gbm = 1; s->gs = gs;
    s->w = (int)w; s->h = (int)h; s->interval = 1; s->gcur = -1;
    s->dpy = G.gdpy;
    pthread_mutex_lock(&G.mu);
    s->next = G.surfs; G.surfs = s;
    pthread_mutex_unlock(&G.mu);
    return s;
}

static int gbm_attach(shim_surf* s, void* ctx) {
    if (s->gbm_ok) {
        if (s->ctx == ctx) {
            int cur = 0; GL.getIntegerv(C_DRAW_FB_BINDING, &cur);
            if (cur == 0) GL.bindFramebuffer(C_FRAMEBUFFER, s->gsl[s->gcur].fbo);
            return 0;
        }
        gbm_teardown(s, 0);
    }
    if (gbm_load() || gl_load()) return -1;

    gl_saved st; gl_save(&st);
    GL.genRenderbuffers(1, &s->grbo);
    GL.bindRenderbuffer(C_RENDERBUFFER, s->grbo);
    GL.renderbufferStorage(C_RENDERBUFFER, C_DEPTH24_STENCIL8, s->w, s->h);
    gl_restore(&st);

    void* bo = GB.deq(s->gs);
    if (!bo) return -1;
    int k = gbm_slot(s, bo);
    if (k < 0) return -1;
    s->cur_bo = bo; s->gcur = k; s->ctx = ctx; s->gbm_ok = 1;
    GL.bindFramebuffer(C_FRAMEBUFFER, s->gsl[k].fbo);
    GL.viewport(0, 0, s->w, s->h);
    GL.scissor(0, 0, s->w, s->h);
    DBG("[shim] GBM surface active (%dx%d)\n", s->w, s->h);
    return 0;
}

static void gbm_swap(shim_surf* s) {
    if (!s->gbm_ok) return;
    int cur = 0; GL.getIntegerv(C_DRAW_FB_BINDING, &cur);
    unsigned old = s->gsl[s->gcur].fbo;
    GL.finish();
    GB.enq(s->gs, s->cur_bo);
    void* bo = NULL;
    for (int i = 0; i < 1000 && !(bo = GB.deq(s->gs)); i++) usleep(1000);
    if (!bo) { DBG("[shim] gbm: no free bo available\n"); return; }
    int k = gbm_slot(s, bo);
    if (k < 0) return;
    s->cur_bo = bo; s->gcur = k;
    if ((unsigned)cur == old) GL.bindFramebuffer(C_FRAMEBUFFER, s->gsl[k].fbo);
}

static unsigned surf_default_fbo(shim_surf* s) {
    if (s->is_gbm) return s->gbm_ok ? s->gsl[s->gcur].fbo : 0;
    if (s->dmabuf_inited) return s->app_fbo;
    if (s->wsi) return wsi_x11_default_fbo(s->wsi);
    return 0;
}

/* ---------- Surfaces ---------- */

static uint32_t native_win(const void* p) {
    return G.xcb ? *(const uint32_t*)p : (uint32_t)*(const unsigned long*)p;
}

static void* make_window_surface(void* d, uint32_t win) {
    if (shim_init()) return NULL;
    if (!G.dpy || d != G.dpy) { SET_ERR(E_BAD_DISPLAY); return NULL; }
    xcb_get_geometry_reply_t* g =
        xcb_get_geometry_reply(G.xc, xcb_get_geometry(G.xc, win), NULL);
    if (!g) { SET_ERR(E_BAD_NATIVE_WINDOW); return NULL; }
    shim_surf* s = calloc(1, sizeof *s);
    if (!s) { free(g); SET_ERR(E_BAD_ALLOC); return NULL; }
    s->magic = SURF_MAGIC; s->win = win; s->w = g->width; s->h = g->height;
    s->x = g->x; s->y = g->y; s->bw = g->border_width; s->depth = g->depth;
    s->interval = 1;
    s->dpy = d;
    free(g);
    pthread_mutex_lock(&G.mu);
    s->next = G.surfs; G.surfs = s;
    pthread_mutex_unlock(&G.mu);
    return s;
}

static void send_configure(shim_surf* s) {
    if (s->cfg_sent || getenv("SHIM_NO_CONFIGURE")) return;
    s->cfg_sent = 1;
    char buf[32];
    memset(buf, 0, sizeof buf);
    xcb_configure_notify_event_t* ev = (xcb_configure_notify_event_t*)buf;
    ev->response_type = XCB_CONFIGURE_NOTIFY;
    ev->event = ev->window = s->win;
    ev->x = (int16_t)s->x; ev->y = (int16_t)s->y;
    ev->width = (uint16_t)s->w; ev->height = (uint16_t)s->h;
    ev->border_width = (uint16_t)s->bw;
    xcb_send_event(G.xc, 0, s->win, XCB_EVENT_MASK_STRUCTURE_NOTIFY, buf);
    xcb_flush(G.xc);
}

static int surf_attach(shim_surf* s, void* ctx) {
    int ver = ctx_ver(ctx);
    if (s->is_gbm) return gbm_attach(s, ctx);

    if (s->dmabuf_inited) {
        if (s->ctx == ctx) { dmabuf_rebind(s); return 0; }
        dmabuf_teardown(s, 0);
        s->dmabuf_failed = 0;
    }
    if (s->wsi && s->ctx == ctx) { wsi_x11_bind(s->wsi); return 0; }
    if (s->wsi) { wsi_x11_abandon(s->wsi); s->wsi = NULL; }

    if (!s->dmabuf_failed) {
        if (dmabuf_init(s, ver) == 0) {
            s->ctx = ctx;
            send_configure(s);
            return 0;
        }
        s->dmabuf_failed = 1;
        DBG("[shim] dmabuf failed, using legacy WSI fallback\n");
    }

    int flags = G.own_xc ? 0 : WSI_X11_SHARED;
    s->wsi = wsi_x11_create_ex(G.xc, s->win, s->w, s->h, 3, flags, G.mali, G.dpy, ver);
    if (!s->wsi) return -1;
    s->ctx = ctx;
    const char* f = getenv("WSI_FPS");
    if (f) wsi_x11_set_fps(s->wsi, atoi(f));
    void (*vp)(int, int, int, int) = wsi_x11_getproc(s->wsi, "glViewport");
    void (*sc)(int, int, int, int) = wsi_x11_getproc(s->wsi, "glScissor");
    if (vp) vp(0, 0, s->w, s->h);
    if (sc) sc(0, 0, s->w, s->h);
    send_configure(s);
    return 0;
}

/* ---------- EGL API ---------- */
EXPORT int eglGetError(void) {
    if (T_err) { int e = T_err; T_err = 0; return e; }
    if (!G.ready) return E_SUCCESS;
    static int (*f)(void);
    if (!f) *(void**)&f = rsym("eglGetError");
    return f ? f() : E_SUCCESS;
}

EXPORT void* eglGetDisplay(void* native) {
    if (getenv("SHIM_GBM")) return gbm_display(native);
    return shim_display(native, -1, 0);
}

static void* surfaceless_display(void* native) {
    if (native) { SET_ERR(E_BAD_PARAMETER); return NULL; }
    if (shim_init()) { SET_ERR(E_BAD_DISPLAY); return NULL; }
    void* (*gd)(void*) = (void* (*)(void*))rsym("eglGetDisplay");
    void* d = gd ? gd(NULL) : NULL;
    if (!d) SET_ERR(E_BAD_DISPLAY);
    DBG("[shim] surfaceless platform display %p\n", d);
    return d;
}

static int track_refs_check(const intptr_t* at, int* other) {
    int ok = 1;
    *other = 0;
    for (; at && *at != E_NONE; at += 2) {
        if (*at == EGL_TRACK_REFERENCES_KHR) { if (at[1] != 1) ok = 0; }
        else if (*at != EGL_DRM_MASTER_FD_EXT) *other = 1;
    }
    return ok;
}

static int explicit_dev_ok(const intptr_t* at) {
    for (; at && *at != E_NONE; at += 2)
        if (*at == EGL_DEVICE_EXT && (void*)at[1] != (void*)&g_device_obj) return 0;
    return 1;
}

static void* device_display(void* native, int has_attribs) {
    if (native != (void*)&g_device_obj) { SET_ERR(E_BAD_DEVICE_EXT); return NULL; }
    if (has_attribs) { SET_ERR(E_BAD_ATTRIBUTE); return NULL; }
    return surfaceless_display(NULL);
}

static void* platform_display(unsigned platform, void* native, int scr) {
    if (platform == EGL_PLATFORM_GBM_KHR) return gbm_display(native);
    if (platform == EGL_PLATFORM_SURFACELESS_MESA) return surfaceless_display(native);
    if (platform == EGL_PLATFORM_ANGLE_ANGLE)
        return shim_display(native, scr, 0); 
    if (platform != EGL_PLATFORM_X11_EXT && platform != EGL_PLATFORM_XCB_EXT) {
        SET_ERR(E_BAD_PARAMETER); return NULL;
    }
    return shim_display(native, scr, platform == EGL_PLATFORM_XCB_EXT);
}

static int screen_key(unsigned platform) {
    return platform == EGL_PLATFORM_XCB_EXT ? EGL_PLATFORM_XCB_SCREEN_EXT
                                            : EGL_PLATFORM_X11_SCREEN_EXT;
}

EXPORT void* eglGetPlatformDisplayEXT(unsigned platform, void* native, const int* at) {
    DBG("[shim] getPlatformDisplayEXT platform=0x%x native=%p\n", platform, native);
    for (const int* a = at; a && *a != E_NONE; a += 2)
        DBG("[shim]   attr 0x%x = 0x%x\n", a[0], a[1]);
    if (platform == EGL_PLATFORM_DEVICE_EXT) {
        int other = 0;
        for (const int* a = at; a && *a != E_NONE; a += 2)
            if (*a != EGL_DRM_MASTER_FD_EXT) other = 1;
        return device_display(native, other);
    }
    int scr = -1, key = screen_key(platform);
    for (; at && *at != E_NONE; at += 2) if (*at == key) scr = at[1];
    return platform_display(platform, native, scr);
}

EXPORT void* eglGetPlatformDisplay(unsigned platform, void* native, const intptr_t* at) {
    int other;
    DBG("[shim] getPlatformDisplay platform=0x%x native=%p\n", platform, native);
    for (const intptr_t* a = at; a && *a != E_NONE; a += 2)
        DBG("[shim]   attr 0x%lx = 0x%lx\n", (long)a[0], (long)a[1]);
    if (!track_refs_check(at, &other)) { SET_ERR(E_BAD_ATTRIBUTE); return NULL; }
    if (platform == EGL_PLATFORM_DEVICE_EXT)
        return device_display(native, other);
    if (!explicit_dev_ok(at)) { SET_ERR(E_BAD_DEVICE_EXT); return NULL; }
    int scr = -1, key = screen_key(platform);
    for (; at && *at != E_NONE; at += 2) if (*at == key) scr = (int)at[1];
    return platform_display(platform, native, scr);
}

PT(unsigned, eglGetConfigs, (void* d, void** c, int n, int* num), (d, c, n, num))

static __thread int g_gl_mapped;

static int want_es1(void) {
    static int v = -1;
    if (v < 0) {
        const char* e = getenv("SHIM_ES1");
        const char* g = getenv("GLX_ES");
        v = (e && atoi(e)) || (g && atoi(g) == 1);
    }
    return v;
}
  
EXPORT unsigned eglBindAPI(unsigned api) {
    if (shim_init()) return 0;
    static unsigned (*f)(unsigned);
    if (!f) *(void**)&f = rsym("eglBindAPI");
    if (!f) return 0;
    if (api == 0x30A2) {            /* EGL_OPENGL_API -> ES */
        DBG("[shim] eglBindAPI(OPENGL) -> OPENGL_ES\n");
        g_gl_mapped = 1;
        api = 0x30A0;
    } else g_gl_mapped = 0;
    return f(api);
}

EXPORT unsigned eglQueryAPI(void) {
    if (shim_init()) return 0;
    static unsigned (*f)(void);
    if (!f) *(void**)&f = rsym("eglQueryAPI");
    unsigned r = f ? f() : 0;
    return (g_gl_mapped && r == 0x30A0) ? 0x30A2 : r;
}

EXPORT unsigned eglChooseConfig(void* d, const int* at, void** c, int n, int* num) {
    if (shim_init()) return 0;
    static unsigned (*f)(void*, const int*, void**, int, int*);
    if (!f) *(void**)&f = rsym("eglChooseConfig");
    if (!f) return 0;

    int tmp[128], k = 0;
    for (const int* a = at; a && a[0] != E_NONE && k < 120; a += 2) {
        int key = a[0], val = a[1];
        if ((key == 0x3040 || key == 0x3042) && val != -1 && (val & 0x8)) {
            val = (val & ~0x8) | (want_es1() ? 0x1 : 0x4);
        }
        tmp[k++] = key; 
        tmp[k++] = val;
    }
    tmp[k] = E_NONE;

    unsigned r = f(d, tmp, c, n, num);
    DBG("[shim] eglChooseConfig -> %u (num=%d)\n", r, num ? *num : -1);
    return r;
}

PT(void*, eglCreatePbufferSurface, (void* d, void* c, const int* at), (d, c, at))
PT(void*, eglCreatePbufferFromClientBuffer, (void* d, unsigned t, void* b, void* c, const int* at), (d, t, b, c, at))
PT(unsigned, eglWaitClient, (void), ())
PT(unsigned, eglWaitGL, (void), ())
PT(unsigned, eglWaitNative, (int e), (e))
                
EXPORT unsigned eglReleaseThread(void) {
    g_gl_mapped = 0;
    T_surf = NULL;
    if (shim_init()) return 0;
    static unsigned (*f)(void);
    if (!f) *(void**)&f = rsym("eglReleaseThread");
    return f ? f() : 0;
}
  
PT(unsigned, eglBindTexImage, (void* d, void* s, int b), (d, s, b))
PT(unsigned, eglReleaseTexImage, (void* d, void* s, int b), (d, s, b))
PT(unsigned, eglCopyBuffers, (void* d, void* s, unsigned long p), (d, s, p))
  
EXPORT void* eglGetCurrentContext(void) {
    if (shim_init()) return NULL;
    static void* (*f)(void);
    if (!f) *(void**)&f = rsym("eglGetCurrentContext");
    return f ? f() : NULL;
}
EXPORT void* eglGetCurrentDisplay(void) {
    if (shim_init()) return NULL;
    static void* (*f)(void);
    if (!f) *(void**)&f = rsym("eglGetCurrentDisplay");
    return f ? f() : NULL;
}

EXPORT unsigned eglQueryContext(void* d, void* c, int a, int* v) {
    if (shim_init()) return 0;
    static unsigned (*f)(void*, void*, int, int*);
    if (!f) *(void**)&f = rsym("eglQueryContext");
    if (a == 0x3138 && v) {
        unsigned r = f ? f(d, c, a, v) : 0;
        if (r) return r;
        pthread_mutex_lock(&G.mu);
        int found = 0;
        for (int i = 0; i < G.nctx; i++)
            if (G.ctx[i] == c) { *v = G.rst[i]; found = 1; break; }
        pthread_mutex_unlock(&G.mu);
        if (found) return 1;
        SET_ERR(E_BAD_CONTEXT);
        return 0;
    }
    return f ? f(d, c, a, v) : 0;
}

typedef struct img_rec {
    struct img_rec* next;
    void* img;
    int fourcc, nplanes;
    int fd[3], off[3], pitch[3];
    uint64_t mod;
} img_rec;
static img_rec* g_imgs;

static void img_record(void* img, const int* a) {
    img_rec* r = calloc(1, sizeof *r);
    if (!r) return;
    r->img = img;
    r->mod = 0x00ffffffffffffffULL;
    for (int i = 0; i < 3; i++) r->fd[i] = -1;
    static const int FD[3]  = { 0x3272, 0x3275, 0x3278 };
    static const int OFF[3] = { 0x3273, 0x3276, 0x3279 };
    static const int PIT[3] = { 0x3274, 0x3277, 0x327A };
    static const int ML[3]  = { 0x3443, 0x3445, 0x3447 };
    static const int MH[3]  = { 0x3444, 0x3446, 0x3448 };
    unsigned lo = 0, hi = 0; int have_mod = 0;
    for (; a && a[0] != E_NONE; a += 2) {
        if (a[0] == 0x3271) r->fourcc = a[1];
        for (int p = 0; p < 3; p++) {
            if (a[0] == FD[p])  { r->fd[p] = a[1]; if (p + 1 > r->nplanes) r->nplanes = p + 1; }
            if (a[0] == OFF[p]) r->off[p] = a[1];
            if (a[0] == PIT[p]) r->pitch[p] = a[1];
        }
        if (a[0] == ML[0]) { lo = (unsigned)a[1]; have_mod = 1; }
        if (a[0] == MH[0]) { hi = (unsigned)a[1]; have_mod = 1; }
    }
    if (have_mod) r->mod = ((uint64_t)hi << 32) | lo;
    pthread_mutex_lock(&G.mu);
    r->next = g_imgs; g_imgs = r;
    pthread_mutex_unlock(&G.mu);
}

static img_rec* img_find_copy(void* img, img_rec* out) {
    img_rec* r;
    pthread_mutex_lock(&G.mu);
    for (r = g_imgs; r; r = r->next) if (r->img == img) { *out = *r; break; }
    pthread_mutex_unlock(&G.mu);
    return r ? out : NULL;
}

static void img_forget(void* img) {
    pthread_mutex_lock(&G.mu);
    for (img_rec** p = &g_imgs; *p; p = &(*p)->next)
        if ((*p)->img == img) { img_rec* d = *p; *p = d->next; free(d); break; }
    pthread_mutex_unlock(&G.mu);
}

/* ---------- EGL_MESA_drm_image ---------- */
typedef struct drm_img {
    struct drm_img* next;
    void* img; void* ab;
    int handle; uint32_t stride;
    int imported;
} drm_img;
static drm_img* g_drm_imgs;

static void* drm_sym(const char* name) {
    void* p = dlsym(RTLD_DEFAULT, name);
    if (p) return p;
    static void* h; static int tried;
    if (!tried) {
        tried = 1;
        const char* lib = getenv("SHIM_LIBDRM");
        if (lib && *lib) h = dlopen(lib, RTLD_NOW);
        if (!h) h = dlopen("libdrm.so.2", RTLD_NOW);
    }
    return h ? dlsym(h, name) : NULL;
}

static int drm_ahb_reg(void* ab, uint32_t key) {
    static int (*f)(void*, uint32_t); static int tried;
    if (!tried) { *(void**)&f = drm_sym("drm_ahb_register"); tried = 1; }
    return f ? f(ab, key) : -1;
}

static void drm_ahb_unreg(void* ab) {
    static void (*f)(void*); static int tried;
    if (!tried) { *(void**)&f = drm_sym("drm_ahb_unregister"); tried = 1; }
    if (f) f(ab);
}

static int drm_image_available(void) {
    return drm_sym("drm_ahb_register") != NULL;
}

static drm_img* drm_img_find(void* img) {
    drm_img* r;
    pthread_mutex_lock(&G.mu);
    for (r = g_drm_imgs; r; r = r->next) if (r->img == img) break;
    pthread_mutex_unlock(&G.mu);
    return r;
}

static drm_img* drm_img_find_name(int name) {
    drm_img* r;
    pthread_mutex_lock(&G.mu);
    for (r = g_drm_imgs; r; r = r->next) if (r->handle == name && !r->imported) break;
    pthread_mutex_unlock(&G.mu);
    return r;
}

static void drm_img_release(void* img) {
    drm_img* r = NULL;
    pthread_mutex_lock(&G.mu);
    for (drm_img** p = &g_drm_imgs; *p; p = &(*p)->next)
        if ((*p)->img == img) { r = *p; *p = r->next; break; }
    pthread_mutex_unlock(&G.mu);
    if (!r) return;
    if (!r->imported) drm_ahb_unreg(r->ab);
    if (ahb_release) ahb_release(r->ab);
    free(r);
}
  
EXPORT void* eglCreateImageKHR(void* d, void* c, unsigned t, void* b, const int* a) {
    if (shim_init()) return NULL;
    void* imp_ab = NULL;
    if (t == 0x31D3) {                         /* EGL_DRM_BUFFER_MESA */
        drm_img* r = drm_img_find_name((int)(intptr_t)b);
        void* anwb = (r && ahb_to_anwb) ? ahb_to_anwb(r->ab) : NULL;
        if (!anwb || !ahb_acquire) { SET_ERR(E_BAD_PARAMETER); return NULL; }
        ahb_acquire(r->ab);                    /* reference for the imported image */
        imp_ab = r->ab;
        static const int pres[] = { 0x30D2, 1, 0x3038 };
        t = 0x3140; b = anwb; c = NULL; a = pres;
    }
    if (t == 0x30B0 /* EGL_NATIVE_PIXMAP_KHR */ && gbm_load() == 0) {
        void* anwb = GB.anwb(b);
        if (anwb) { t = 0x3140; b = anwb; c = NULL; }
    }
    static void* (*f)(void*, void*, unsigned, void*, const int*);
    if (!f) *(void**)&f = rsym("eglCreateImageKHR");
    void* img = f ? f(d, c, t, b, a) : NULL;
    if (img && t == 0x3270) img_record(img, a);
    if (imp_ab) {
        drm_img* r = img ? calloc(1, sizeof *r) : NULL;
        if (!r) { 
            ahb_release(imp_ab); 
            if (img && f) { 
                unsigned (*di)(void*, void*) = rsym("eglDestroyImageKHR"); 
                if (di) di(d, img); 
            } 
            return NULL; 
        }
        r->img = img; r->ab = imp_ab; r->handle = -1; r->imported = 1;
        pthread_mutex_lock(&G.mu);
        r->next = g_drm_imgs; g_drm_imgs = r;
        pthread_mutex_unlock(&G.mu);
    }
    return img;
}

EXPORT void* eglCreateImage(void* d, void* c, unsigned t, void* b, const intptr_t* at) {
    int tmp[64], i = 0;
    if (at) for (; at[i] != E_NONE && i < 60; i += 2) { tmp[i] = (int)at[i]; tmp[i + 1] = (int)at[i + 1]; }
    tmp[i] = E_NONE;
    return eglCreateImageKHR(d, c, t, b, tmp);
}

EXPORT unsigned eglDestroyImageKHR(void* d, void* i) {
    if (shim_init()) return 0;
    static unsigned (*f)(void*, void*);
    if (!f) *(void**)&f = rsym("eglDestroyImageKHR");
    drm_img_release(i);
    img_forget(i);
    return f ? f(d, i) : 0;
}
EXPORT unsigned eglDestroyImage(void* d, void* i) { return eglDestroyImageKHR(d, i); }

EXPORT void* eglCreateDRMImageMESA(void* d, const int* at) {
    if (shim_init()) return NULL;
    int w = 0, h = 0, fmt = 0, use = 0;
    for (; at && at[0] != E_NONE; at += 2) {
        switch (at[0]) {
        case 0x3057: w = at[1]; break;
        case 0x3056: h = at[1]; break;
        case 0x31D0: fmt = at[1]; break;
        case 0x31D1: use = at[1]; break;
        default: SET_ERR(E_BAD_PARAMETER); return NULL;
        }
    }
    if (w <= 0 || h <= 0 || fmt != 0x31D2 || ((use & 4) && (w != 64 || h != 64))) {
        SET_ERR(E_BAD_PARAMETER); return NULL;
    }
    if (ahb_init()) { SET_ERR(E_BAD_ALLOC); return NULL; }

    AHBDesc ds;
    int used_fmt = 5;
    void* ab = alloc_ahb(w, h, 5, &ds);        /* BGRA = ARGB32 little-endian */
    if (!ab) { used_fmt = 1; ab = alloc_ahb(w, h, 1, &ds); }
    if (!ab) { SET_ERR(E_BAD_ALLOC); return NULL; }
    DBG("[shim] drm_image: AHB format %d (%s)\n", used_fmt,
        used_fmt == 5 ? "BGRA = correct ARGB32" : "RGBA, R/B channels inverted vs spec");

    static void* (*create_img)(void*, void*, unsigned, void*, const int*);
    if (!create_img) *(void**)&create_img = rsym("eglCreateImageKHR");
    static const int attr[] = { 0x30D2, 1, E_NONE };
    void* cb = ahb_to_anwb ? ahb_to_anwb(ab) : NULL;
    void* img = (cb && create_img) ? create_img(d, NULL, 0x3140, cb, attr) : NULL;
    drm_img* r = img ? calloc(1, sizeof *r) : NULL;
    if (!r) {
        if (img) { unsigned (*di)(void*, void*) = rsym("eglDestroyImageKHR"); if (di) di(d, img); }
        ahb_release(ab); SET_ERR(E_BAD_ALLOC); return NULL; }
    r->img = img; r->ab = ab; r->stride = ds.stride * 4; r->imported = 0;
    r->handle = drm_ahb_reg(ab, (uint32_t)(uintptr_t)img);
    pthread_mutex_lock(&G.mu);
    r->next = g_drm_imgs; g_drm_imgs = r;
    pthread_mutex_unlock(&G.mu);
    DBG("[shim] drm_image %dx%d use=0x%x handle=%d stride=%u\n", w, h, use, r->handle, r->stride);
    return img;
}

EXPORT unsigned eglExportDRMImageMESA(void* d, void* image, int* name, int* handle, int* stride) {
    (void)d;
    drm_img* r = drm_img_find(image);
    if (!r) { SET_ERR(E_BAD_PARAMETER); return 0; }
    if (r->handle < 0) { SET_ERR(E_BAD_ACCESS); return 0; }
    if (name)   *name = r->handle;
    if (handle) *handle = r->handle;
    if (stride) *stride = (int)r->stride;
    return 1;
}
  
/* ---------- Sync wrapper (EGL_EXT_sync_reuse) ---------- */
typedef struct shim_sync {
    struct shim_sync* next;
    void* real;        /* driver's sync; NULL = created/remained signaled */
    unsigned type;
    int reusable, signaled;
} shim_sync; 
static shim_sync* g_syncs;

static shim_sync* sync_find(void* p) {
    shim_sync* r = NULL;
    if (!p) return NULL;
    pthread_mutex_lock(&G.mu);
    for (shim_sync* s = g_syncs; s; s = s->next) if (s == p) { r = s; break; }
    pthread_mutex_unlock(&G.mu);
    return r;
}

static void sync_attribs(const int* at, int* out, int cap, int* st) {
    int k = 0;
    *st = 0;
    for (; at && at[0] != E_NONE && k < cap - 3; at += 2) {
        if (at[0] == 0x30F1) { *st = at[1]; continue; }
        out[k++] = at[0]; out[k++] = at[1];
    }
    out[k] = E_NONE;
}

EXPORT void* eglCreateSyncKHR(void* d, unsigned t, const int* at) {
    if (shim_init()) return NULL;
    static void* (*f)(void*, unsigned, const int*);
    if (!f) *(void**)&f = rsym("eglCreateSyncKHR");
    int tmp[32], st;
    sync_attribs(at, tmp, 32, &st);
    shim_sync* s = calloc(1, sizeof *s);
    if (!s) { SET_ERR(E_BAD_ALLOC); return NULL; }
    s->type = t;
    if (t == 0x30FA) {                      /* EGL_SYNC_REUSABLE_KHR */
        s->reusable = 1;
        s->signaled = (st == 0x30F2);
        pthread_mutex_lock(&G.mu); s->next = g_syncs; g_syncs = s; pthread_mutex_unlock(&G.mu);
        return s;
    }
    if (!f) { free(s); return NULL; }
    if (st == 0x30F2 /* SIGNALED */ && (t == 0x30F9 || t == 0x3144) && tmp[0] == E_NONE) {
        s->real = NULL;
    } else {
        s->real = f(d, t, tmp);
        if (!s->real) { free(s); return NULL; }
    }
    pthread_mutex_lock(&G.mu);
    s->next = g_syncs; g_syncs = s;
    pthread_mutex_unlock(&G.mu);
    return s;
}

EXPORT unsigned eglDestroySyncKHR(void* d, void* sync) {
    if (shim_init()) return 0;
    static unsigned (*f)(void*, void*);
    if (!f) *(void**)&f = rsym("eglDestroySyncKHR");
    shim_sync* s = NULL;
    pthread_mutex_lock(&G.mu);
    for (shim_sync** p = &g_syncs; *p; p = &(*p)->next)
        if (*p == sync) { s = *p; *p = s->next; break; }
    pthread_mutex_unlock(&G.mu);
    if (!s) return f ? f(d, sync) : 0;
    unsigned r = 1;
    if (s->real && f) r = f(d, s->real);
    free(s);
    return r;
}

EXPORT int eglClientWaitSyncKHR(void* d, void* sync, int flags, uint64_t t) {
    if (shim_init()) return 0;
    static int (*f)(void*, void*, int, uint64_t);
    if (!f) *(void**)&f = rsym("eglClientWaitSyncKHR");
    shim_sync* s = sync_find(sync);
    if (s && s->reusable) {
        uint64_t waited = 0;
        while (!__atomic_load_n(&s->signaled, __ATOMIC_ACQUIRE)) {
            if (t == 0 || (t != ~0ULL && waited >= t)) return 0x30F5;   /* TIMEOUT */
            usleep(1000); waited += 1000000ULL;
        }
        return 0x30F6;
    }
    if (s && !s->real) return 0x30F6;       /* EGL_CONDITION_SATISFIED_KHR */
    return f ? f(d, s ? s->real : sync, flags, t) : 0;
}

EXPORT int eglWaitSyncKHR(void* d, void* sync, int flags) {
    if (shim_init()) return 0;
    static int (*f)(void*, void*, int);
    if (!f) *(void**)&f = rsym("eglWaitSyncKHR");
    shim_sync* s = sync_find(sync);
    if (s && !s->real) return 1;
    if (s && s->reusable) return 1;
    return f ? f(d, s ? s->real : sync, flags) : 0;
}

EXPORT unsigned eglGetSyncAttribKHR(void* d, void* sync, int a, int* v) {
    if (shim_init()) return 0;
    static unsigned (*f)(void*, void*, int, int*);
    if (!f) *(void**)&f = rsym("eglGetSyncAttribKHR");
    shim_sync* s = sync_find(sync);
    if (s && s->reusable) {
        if (!v) { SET_ERR(E_BAD_PARAMETER); return 0; }
        if (a == 0x30F7) { *v = 0x30FA; return 1; }
        if (a == 0x30F1) { *v = __atomic_load_n(&s->signaled, __ATOMIC_ACQUIRE) ? 0x30F2 : 0x30F3; return 1; }
        SET_ERR(E_BAD_ATTRIBUTE); return 0;
    }
    if (s && !s->real) {
        if (!v) { SET_ERR(E_BAD_PARAMETER); return 0; }
        switch (a) {
        case 0x30F7: *v = (int)s->type; return 1;   /* EGL_SYNC_TYPE_KHR */
        case 0x30F1: *v = 0x30F2; return 1;         /* EGL_SYNC_STATUS_KHR = SIGNALED */
        case 0x30F8: *v = 0x30F0; return 1;         /* CONDITION = PRIOR_COMMANDS_COMPLETE */
        default: SET_ERR(E_BAD_ATTRIBUTE); return 0;
        }
    }
    return f ? f(d, s ? s->real : sync, a, v) : 0;
}

EXPORT unsigned eglUnsignalSyncEXT(void* d, void* sync, const intptr_t* at) {
    if (shim_init()) return 0;
    static void* (*cs)(void*, unsigned, const int*);
    static unsigned (*ds)(void*, void*);
    static unsigned (*ga)(void*, void*, int, int*);
    if (!cs) *(void**)&cs = rsym("eglCreateSyncKHR");
    if (!ds) *(void**)&ds = rsym("eglDestroySyncKHR");
    if (!ga) *(void**)&ga = rsym("eglGetSyncAttribKHR");
    shim_sync* s = sync_find(sync);
    if (!s) { SET_ERR(E_BAD_PARAMETER); return 0; }
    if (s->reusable) { SET_ERR(E_BAD_MATCH); return 0; }
    int tmp[16], k = 0;
    for (; at && at[0] != E_NONE && k < 12; at += 2) {
        if (s->type == 0x3144 && at[0] == 0x3145) { tmp[k++] = 0x3145; tmp[k++] = (int)at[1]; }
        else { SET_ERR(E_BAD_ATTRIBUTE); return 0; }
    }
    tmp[k] = E_NONE;

    int status = 0x30F2;
    if (s->real) {
        if (!ga || !ga(d, s->real, 0x30F1, &status)) return 0;
    }
    if (status != 0x30F2) { SET_ERR(E_BAD_ACCESS); return 0; }

    void* n = cs ? cs(d, s->type, tmp) : NULL;
    if (!n) return 0;
    if (s->real && ds) ds(d, s->real);
    s->real = n;
    return 1;
}

EXPORT unsigned eglSignalSyncKHR(void* d, void* sync, unsigned mode) {
    (void)d;
    shim_sync* s = sync_find(sync);
    if (!s || !s->reusable) { SET_ERR(E_BAD_PARAMETER); return 0; }
    if (mode != 0x30F2 && mode != 0x30F3) { SET_ERR(E_BAD_PARAMETER); return 0; }
    __atomic_store_n(&s->signaled, mode == 0x30F2, __ATOMIC_RELEASE);
    return 1;
}
  
PT(unsigned, eglQueryDmaBufFormatsEXT, (void* d, int m, int* f, int* n), (d, m, f, n))
PT(unsigned, eglQueryDmaBufModifiersEXT, (void* d, int f, int m, uint64_t* mod, unsigned* ext, int* n), (d, f, m, mod, ext, n))

EXPORT unsigned eglSetDamageRegionKHR(void* d, void* surf, int* rects, int n) {
    if (find_surf(surf)) return 1;
    if (shim_init()) return 0;
    static unsigned (*f)(void*, void*, int*, int);
    if (!f) *(void**)&f = rsym("eglSetDamageRegionKHR");
    return f ? f(d, surf, rects, n) : 0;
}

EXPORT int eglDupNativeFenceFDANDROID(void* d, void* sync) {
    if (shim_init()) return -1;
    static int (*f)(void*, void*);
    if (!f) *(void**)&f = glp("eglDupNativeFenceFDANDROID");
    shim_sync* s = sync_find(sync);
    if (s) { if (!s->real) return -1; sync = s->real; }
    return f ? f(d, sync) : -1;
}

static const char CLIENT_EXTS[] =
    "EGL_EXT_client_extensions EGL_EXT_platform_base EGL_EXT_platform_x11 "
    "EGL_KHR_platform_x11 EGL_EXT_platform_xcb EGL_MESA_platform_surfaceless "
    "EGL_KHR_platform_gbm EGL_MESA_platform_gbm EGL_EXT_device_query EGL_KHR_display_reference "
    "EGL_EXT_device_enumeration EGL_EXT_device_base EGL_EXT_platform_device EGL_ANGLE_platform_angle "
    "EGL_KHR_client_get_all_proc_addresses EGL_EXT_query_reset_notification_strategy EGL_KHR_debug EGL_EXT_explicit_device ";

static const char* const CLIENT_LIST[] = {
    "EGL_EXT_client_extensions", "EGL_EXT_platform_base", "EGL_EXT_platform_x11",
    "EGL_KHR_platform_x11", "EGL_EXT_platform_xcb", "EGL_MESA_platform_surfaceless",
    "EGL_KHR_platform_gbm", "EGL_MESA_platform_gbm", "EGL_EXT_device_query",
    "EGL_EXT_device_enumeration", "EGL_EXT_device_base", "EGL_EXT_platform_device", "EGL_KHR_display_reference",
    "EGL_KHR_client_get_all_proc_addresses", "EGL_EXT_query_reset_notification_strategy", "EGL_ANGLE_platform_angle", "EGL_KHR_debug", "EGL_EXT_explicit_device",
};

static int has_token(const char* s, const char* t) {
    size_t n = strlen(t);
    for (const char* p = s; (p = strstr(p, t)); p += n)
        if ((p == s || p[-1] == ' ') && (p[n] == ' ' || !p[n])) return 1;
    return 0;
}

EXPORT const char* eglQueryString(void* d, int name) {
    if (!d) {
        if (name == 0x3055) return CLIENT_EXTS;
        SET_ERR(E_BAD_DISPLAY); return NULL;
    }
    if (name == 0x308D) return "OpenGL OpenGL_ES";   /* EGL_CLIENT_APIS */
    if (shim_init()) return NULL;
    static const char* (*f)(void*, int);
    if (!f) *(void**)&f = rsym("eglQueryString");
    const char* r = f ? f(d, name) : NULL;
    if (name != 0x3055 || !r) return r;
  
    static const char* const extra[] = {
        "EGL_KHR_surfaceless_context", "EGL_EXT_buffer_age",
        "EGL_KHR_swap_buffers_with_damage", "EGL_EXT_swap_buffers_with_damage",
        "EGL_NV_post_sub_buffer", "EGL_KHR_reusable_sync",
        "EGL_KHR_create_context_no_error", "EGL_KHR_context_flush_control",
        "EGL_KHR_image_pixmap", "EGL_ANDROID_get_native_client_buffer",
        "EGL_ANDROID_native_fence_sync", "EGL_EXT_sync_reuse", "EGL_MESA_query_driver", "EGL_MESA_image_dma_buf_export", "EGL_MESA_drm_image",
    };
    static struct { const char* src; char* out; } cache[16];
    static int ncache;

    pthread_mutex_lock(&G.mu);
    for (int i = 0; i < ncache; i++)
        if (cache[i].src == r) {
            const char* o = cache[i].out;
            pthread_mutex_unlock(&G.mu);
            return o;
        }

    size_t cap = strlen(r) + 1024, n = 0;
    char* nb = malloc(cap);
    char* tmp = strdup(r);
    if (!nb || !tmp) { free(nb); free(tmp); pthread_mutex_unlock(&G.mu); return r; }
    nb[0] = 0;

    char* sv;
    for (char* t = strtok_r(tmp, " ", &sv); t; t = strtok_r(NULL, " ", &sv)) {
        int skip = 0;
        for (unsigned i = 0; i < sizeof CLIENT_LIST / sizeof *CLIENT_LIST; i++)
            if (!strcmp(t, CLIENT_LIST[i])) { skip = 1; break; }
        if (!skip) n += snprintf(nb + n, cap - n, "%s%s", n ? " " : "", t);
    }
    free(tmp);

    for (unsigned i = 0; i < sizeof extra / sizeof *extra; i++) {
        const char* x = extra[i];
        if (has_token(nb, x)) continue;
        if (!strcmp(x, "EGL_ANDROID_native_fence_sync") && !glp("eglDupNativeFenceFDANDROID")) continue;
        if (!strcmp(x, "EGL_EXT_sync_reuse") && !has_token(nb, "EGL_KHR_fence_sync")) continue;
        if (!strcmp(x, "EGL_MESA_drm_image") &&
            (!has_token(nb, "EGL_KHR_image_base") || !drm_image_available())) continue;
        if (!strcmp(x, "EGL_KHR_create_context_no_error") &&
            !has_token(nb, "EGL_KHR_create_context")) continue;
        if (!strcmp(x, "EGL_KHR_image_pixmap") &&
            (!has_token(nb, "EGL_KHR_image_base") || gbm_load())) continue;
        if (!strcmp(x, "EGL_ANDROID_get_native_client_buffer") &&
            (!has_token(nb, "EGL_ANDROID_image_native_buffer") ||
             !glp("eglGetNativeClientBufferANDROID"))) continue;
        if (!strcmp(x, "EGL_KHR_reusable_sync") && !has_token(nb, "EGL_KHR_fence_sync")) continue;
        n += snprintf(nb + n, cap - n, "%s%s", n ? " " : "", x);
    }

    if (ncache < 16) { cache[ncache].src = r; cache[ncache].out = nb; ncache++; }
    pthread_mutex_unlock(&G.mu);
    return nb;
}

EXPORT unsigned eglGetConfigAttrib(void* d, void* cfg, int attr, int* v) {
    if (shim_init()) return 0;
    static unsigned (*f)(void*, void*, int, int*);
    if (!f) *(void**)&f = rsym("eglGetConfigAttrib");
    unsigned r = f ? f(d, cfg, attr, v) : 0;
    if (r && v) {
        if (attr == 0x302E) {
            if (G.root_visual) *v = (int)G.root_visual;
            else if (G.gdpy && !G.dpy) {
                switch (*v) {
                case 1: case 5: *v = 0x34325241; break; /* ARGB8888 */
                case 2:         *v = 0x34325258; break; /* XRGB8888 */
                case 4:         *v = 0x36314752; break; /* RGB565 */
                }
            }
        }
        if ((attr == 0x3040 || attr == 0x3042) && (*v & 0x4)) *v |= 0x8;
        if (attr == 0x302F) *v = 4;
    }
    return r;
}

EXPORT void* eglCreateWindowSurface(void* d, void* cfg, unsigned long win, const int* at) {
    (void)cfg; (void)at;
    if (is_gbm_display(d)) return make_gbm_surface((void*)win);
    return make_window_surface(d, (uint32_t)win);
}
EXPORT void* eglCreatePlatformWindowSurfaceEXT(void* d, void* cfg, void* native_window, const int* at) {
    (void)cfg; (void)at;
    if (!native_window) { SET_ERR(E_BAD_NATIVE_WINDOW); return NULL; }
    if (is_gbm_display(d)) return make_gbm_surface(native_window);
    return make_window_surface(d, native_win(native_window));
}
EXPORT void* eglCreatePlatformWindowSurface(void* d, void* cfg, void* native_window, const intptr_t* at) {
    (void)cfg; (void)at;
    if (!native_window) { SET_ERR(E_BAD_NATIVE_WINDOW); return NULL; }
    if (is_gbm_display(d)) return make_gbm_surface(native_window);
    return make_window_surface(d, native_win(native_window));
}

EXPORT void* eglCreatePixmapSurface(void* d, void* cfg, unsigned long pix, const int* at) {
    (void)d; (void)cfg; (void)pix; (void)at;
    SET_ERR(E_BAD_NATIVE_PIXMAP); return NULL;
}
EXPORT void* eglCreatePlatformPixmapSurfaceEXT(void* d, void* cfg, void* pix, const int* at) {
    (void)d; (void)cfg; (void)pix; (void)at;
    SET_ERR(E_BAD_NATIVE_PIXMAP); return NULL;
}
EXPORT void* eglCreatePlatformPixmapSurface(void* d, void* cfg, void* pix, const intptr_t* at) {
    (void)d; (void)cfg; (void)pix; (void)at;
    SET_ERR(E_BAD_NATIVE_PIXMAP); return NULL;
}
  
static shim_surf* g_grave[32];
static int g_grave_n;

static void surf_free_later(shim_surf* s) {
    pthread_mutex_lock(&G.mu);
    shim_surf* old = g_grave[g_grave_n];
    g_grave[g_grave_n] = s;
    g_grave_n = (g_grave_n + 1) % 32;
    pthread_mutex_unlock(&G.mu);
    free(old);
}

static void surf_release(shim_surf* s, int with_gl) {
    if (s->is_gbm) gbm_teardown(s, with_gl);
    if (s->dmabuf_inited) dmabuf_teardown(s, with_gl);
    if (s->wsi) {
        if (with_gl) wsi_x11_destroy(s->wsi);
        else wsi_x11_abandon(s->wsi);
        s->wsi = NULL;
    }
    __atomic_store_n(&s->magic, 0, __ATOMIC_RELEASE);
    surf_free_later(s);
}
  
EXPORT unsigned eglDestroySurface(void* d, void* surf) {
    if (shim_init()) return 0;
    shim_surf* s = NULL;

    pthread_mutex_lock(&G.mu);
    for (shim_surf** p = &G.surfs; *p; p = &(*p)->next)
        if (*p == surf) { s = *p; *p = s->next; break; }
    pthread_mutex_unlock(&G.mu);

    if (!s) {
        static unsigned (*f)(void*, void*);
        if (!f) *(void**)&f = rsym("eglDestroySurface");
        return f ? f(d, surf) : 0;
    }

    /* wait for eglDestroyContext to finish teardown on this surface */
    while (__atomic_load_n(&s->releasing, __ATOMIC_ACQUIRE)) usleep(100);

    void* (*cur)(void) = (void* (*)(void))rsym("eglGetCurrentContext");
    if (T_surf == s) T_surf = NULL;
    surf_release(s, cur && cur() == s->ctx);
    return 1;
}

EXPORT unsigned eglQuerySurface(void* d, void* surf, int attr, int* v) {
    shim_surf* s = find_surf(surf);
    if (!s) {
        if (shim_init()) return 0;
        static unsigned (*f)(void*, void*, int, int*);
        if (!f) *(void**)&f = rsym("eglQuerySurface");
        return f ? f(d, surf, attr, v) : 0;
    }
    switch (attr) {
    case 0x3057: *v = s->w; return 1;
    case 0x3056: *v = s->h; return 1;
    case 0x3086: *v = 0x3084; return 1;
    case 0x3093: *v = 0x3095; return 1;
    case 0x3099: *v = 0x309A; return 1;
    case 0x3090: case 0x3091: *v = -1; return 1;
    case 0x3092: *v = -1; return 1;
    case 0x30BE: *v = 1; return 1;
    case 0x313D:
        if (T_surf != s) { SET_ERR(E_BAD_SURFACE); return 0; }
        *v = (s->dmabuf_inited && s->frames > 0) ? 1 : 0; return 1;
    default: SET_ERR(E_BAD_ATTRIBUTE); return 0;
    }
}

EXPORT unsigned eglSurfaceAttrib(void* d, void* surf, int attr, int v) {
    if (find_surf(surf)) return 1;
    if (shim_init()) return 0;
    static unsigned (*f)(void*, void*, int, int);
    if (!f) *(void**)&f = rsym("eglSurfaceAttrib");
    return f ? f(d, surf, attr, v) : 0;
}

static void* emu_sym(const char* n) {
    static void* h;
    if (!h) {
        h = dlopen("libGL.so.1", RTLD_NOW | RTLD_NOLOAD);
        if (!h) h = dlopen("libGL.so.1", RTLD_NOW | RTLD_GLOBAL);
    }
    return h ? dlsym(h, n) : NULL;
}
  
static void set_emu_ctx(void* ctx) {
    void (*cur)(void*) = (void (*)(void*))emu_sym("glx_shim_egl_current");
    if (!cur) return;
    void* g = NULL;
    pthread_mutex_lock(&G.mu);
    for (int i = 0; i < G.nctx; i++) if (G.ctx[i] == ctx) { g = G.gctx[i]; break; }
    pthread_mutex_unlock(&G.mu);
    cur(g);
}

static int ctx_err(void) {
    int (*ge)(void) = rsym("eglGetError");
    int e = ge ? ge() : E_SUCCESS;
    T_ctx_e = e;               /* do NOT write to T_err here */
    return e;
}
  
EXPORT void* eglCreateContext(void* d, void* cfg, void* share, const int* at) {
    if (shim_init()) return NULL;
    static void* (*f)(void*, void*, void*, const int*);
    if (!f) *(void**)&f = rsym("eglCreateContext");
    if (!f) return NULL;
    T_ctx_e = 0;
    /* Generic filtering of all ANGLE attributes (0x3480 - 0x34FF) unsupported by Mali */
    int fil[64], fk = 0;
    for (const int* a = at; a && a[0] != E_NONE && fk < 60; a += 2) {
        if (a[0] >= 0x3480 && a[0] <= 0x34FF) {
            DBG("[shim] ctx attr 0x%x = 0x%x ignored (ANGLE)\n", a[0], a[1]);
            continue;
        }
        if (a[0] == 0x31B3) {              /* NO_ERROR_KHR: only TRUE(1) / FALSE(0) */
            if (a[1] != 0 && a[1] != 1) { 
                SET_ERR(E_BAD_ATTRIBUTE); 
                return NULL; 
            }
            DBG("[shim] ctx attr 0x31B3 = %d (NO_ERROR_KHR validated & ignored)\n", a[1]);
            continue;
        }
        if (a[0] == 0x2097) {              /* RELEASE_BEHAVIOR: NONE(0) / FLUSH(0x2098) */
            if (a[1] != 0 && a[1] != 0x2098) { 
                SET_ERR(E_BAD_ATTRIBUTE); 
                return NULL; 
            }
            DBG("[shim] ctx attr 0x2097 = 0x%x (RELEASE_BEHAVIOR validated & ignored)\n", a[1]);
            continue;
        }
        fil[fk++] = a[0]; fil[fk++] = a[1];
    }
    fil[fk] = E_NONE;
    at = fil;

    /* Log sanitized input attributes */
    DBG("[shim] ctx request: g_gl_mapped=%d share=%p\n", g_gl_mapped, share);
    for (const int* a = at; a && a[0] != E_NONE; a += 2)
        DBG("[shim]   ctx attr 0x%x = 0x%x\n", a[0], a[1]);

    int san[64];
    int major = 0, minor = 0, profile = 0, flags = 0, have = 0;
    if (g_gl_mapped) {
        int k = 0;
        for (const int* a = at; a && a[0] != E_NONE && k < 56; a += 2) {
            if (a[0] == 0x3098) { major = a[1]; have = 1; continue; } /* EGL_CONTEXT_CLIENT_VERSION */
            if (a[0] == 0x30FB) { minor = a[1]; continue; }          /* EGL_CONTEXT_MINOR_VERSION_KHR */
            if (a[0] == 0x30FC) { flags = a[1]; continue; }          /* EGL_CONTEXT_FLAGS_KHR */
            if (a[0] == 0x30FD) { profile = a[1]; continue; }        /* EGL_CONTEXT_OPENGL_PROFILE_MASK_KHR */
            san[k++] = a[0]; san[k++] = a[1];
        }
        int (*sel)(int, int, int) = (int (*)(int, int, int))emu_sym("glx_shim_es_version");
        int v = sel ? sel(major, profile, have) : (major >= 3 ? 3 : 2);
        if (v < 1) v = 1;
        if (v > 3) v = 3;
        san[k++] = 0x3098; san[k++] = v;
        san[k] = E_NONE;
        at = san;
    }
    int es1 = g_gl_mapped && want_es1();
    int ver = g_gl_mapped ? 2 : 1, has_ver = 0;
    int rst = 0x31BE;
    for (const int* a = at; a && a[0] != E_NONE; a += 2) {
        if (a[0] == 0x3098) { ver = a[1]; has_ver = 1; }
        if (a[0] == 0x3138) rst = a[1];
    }
    if (es1) ver = 1;

    void* c = NULL;
    if (es1) {
        int tmp[64], k = 0, seen = 0;
        for (const int* a = at; a && a[0] != E_NONE && k < 58; a += 2) {
            tmp[k++] = a[0];
            if (a[0] == 0x3098) { tmp[k++] = 1; seen = 1; }
            else tmp[k++] = a[1];
        }
        if (!seen) { tmp[k++] = 0x3098; tmp[k++] = 1; }
        tmp[k] = E_NONE;
        c = f(d, cfg, share, tmp);
        int e = c ? 0 : ctx_err();
        DBG("[shim] eglCreateContext ES1 -> %p err=0x%x\n", c, e);
    } else if (ver == 2 && !getenv("WSI_NO_UPGRADE")) {
        int tmp[64], k = 0, seen = 0;
        for (const int* a = at; a && a[0] != E_NONE && k < 58; a += 2) {
            if (a[0] == 0x3098) { seen = 1; tmp[k++] = a[0]; tmp[k++] = 3; }
            else { tmp[k++] = a[0]; tmp[k++] = a[1]; }
        }
        if (!seen) { tmp[k++] = 0x3098; tmp[k++] = 3; }
        tmp[k] = E_NONE;

        c = f(d, cfg, share, tmp);
        int e = c ? 0 : ctx_err();
        DBG("[shim] eglCreateContext ES3 -> %p err=0x%x\n", c, e);
        if (c) {
            ver = 3;
        } else if (has_ver) {
            c = f(d, cfg, share, at);
            int e_orig = c ? 0 : ctx_err();
            DBG("[shim] eglCreateContext original attrs -> %p err=0x%x\n", c, e_orig);
        } else {
            for (k = 0; tmp[k] != E_NONE; k += 2) {
                if (tmp[k] == 0x3098) tmp[k + 1] = 2;
            }
            c = f(d, cfg, share, tmp);
            int e_es2 = c ? 0 : ctx_err();
            DBG("[shim] eglCreateContext ES2 fallback -> %p err=0x%x\n", c, e_es2);
        }
    } else {
        c = f(d, cfg, share, at);
        if (!c) {
            int e_dir = ctx_err();
            DBG("[shim] eglCreateContext direct -> NULL err=0x%x\n", e_dir);
        } else {
            DBG("[shim] eglCreateContext direct -> %p\n", c);
        }

        if (!c && ver == 3 && g_gl_mapped) {
            int tmp[64], k = 0;
            for (const int* a = at; a && a[0] != E_NONE && k < 58; a += 2) {
                tmp[k++] = a[0];
                tmp[k++] = (a[0] == 0x3098) ? 2 : a[1];
            }
            tmp[k] = E_NONE;
            c = f(d, cfg, share, tmp);
            int e_fb = c ? 0 : ctx_err();
            DBG("[shim] eglCreateContext GL->ES3 failed, ES2 fallback -> %p err=0x%x\n", c, e_fb);
            if (c) ver = 2;
        }
    }

    /* Propagate error only if context creation failed across all fallback paths */
    if (!c && T_ctx_e != E_SUCCESS) {
        T_err = T_ctx_e;
        dbg_msg(T_ctx_e, "eglCreateContext", EGL_DEBUG_MSG_ERROR_KHR, 0, NULL,
                "EGL error from driver");
    }
    T_ctx_e = 0;

    if (c) {
        void* gctx = NULL;
        if (g_gl_mapped) {
            void* (*mk)(int, int, int, int, int) =
                (void* (*)(int, int, int, int, int))emu_sym("glx_shim_egl_ctx");
            if (mk) gctx = mk(ver, major, minor, profile, flags);
        }
        pthread_mutex_lock(&G.mu);
        if (G.nctx < 64) {
            G.ctx[G.nctx] = c;
            G.ver[G.nctx] = ver;
            G.rst[G.nctx] = rst;
            G.gctx[G.nctx] = gctx;
            G.nctx++;
        } else if (gctx) {
            void (*fr)(void*) = (void (*)(void*))emu_sym("glx_shim_egl_free");
            if (fr) fr(gctx);
        }
        pthread_mutex_unlock(&G.mu);
    }
    return c;
}

EXPORT unsigned eglDestroyContext(void* d, void* ctx) {
    if (shim_init()) return 0;
    static unsigned (*f)(void*, void*);
    if (!f) *(void**)&f = rsym("eglDestroyContext");
    unsigned r = f ? f(d, ctx) : 0;
    void* dead = NULL;
    shim_surf* hit = NULL;

    pthread_mutex_lock(&G.mu);
    for (shim_surf* s = G.surfs; s; s = s->next) {
        if (s->ctx != ctx) continue;
        /* Skip surfaces already atomically claimed by eglDestroySurface */
        if (__atomic_exchange_n(&s->releasing, 1, __ATOMIC_ACQ_REL)) continue;
        s->ctx = NULL;
        s->rel_next = hit; 
        hit = s;
    }
    for (int i = 0; i < G.nctx; i++)
        if (G.ctx[i] == ctx) {
            dead = G.gctx[i];
            G.ctx[i]  = G.ctx[--G.nctx];
            G.ver[i]  = G.ver[G.nctx];
            G.rst[i]  = G.rst[G.nctx];
            G.gctx[i] = G.gctx[G.nctx];
            break;
        }
    pthread_mutex_unlock(&G.mu);

    /* Lock-free teardown with next-pointer prefetch (nx) */
    for (shim_surf* s = hit, *nx; s; s = nx) {
        nx = s->rel_next;
        if (s->is_gbm) gbm_teardown(s, 0);
        if (s->wsi) { wsi_x11_abandon(s->wsi); s->wsi = NULL; }
        if (s->dmabuf_inited) dmabuf_teardown(s, 0);
        
        __atomic_store_n(&s->releasing, 0, __ATOMIC_RELEASE);
    }

    if (dead) {
        void (*fr)(void*) = (void (*)(void*))emu_sym("glx_shim_egl_free");
        if (fr) fr(dead);
    }
    return r;
}

EXPORT unsigned eglMakeCurrent(void* d, void* draw, void* read, void* ctx) {
    if (shim_init()) return 0;
    static unsigned (*mc)(void*, void*, void*, void*);
    if (!mc) *(void**)&mc = rsym("eglMakeCurrent");
    if (!mc) return 0;
    shim_surf* s = find_surf(draw);
    if (!s) s = find_surf(read);
    if (!s || !ctx) {
        unsigned r = mc(d, draw, read, ctx);
        if (r) { T_surf = NULL; set_emu_ctx(ctx); }
        return r;
    }
    if (!mc(d, NULL, NULL, ctx)) return 0;
    if (surf_attach(s, ctx)) {
        mc(d, NULL, NULL, NULL);
        T_surf = NULL;
        set_emu_ctx(NULL);
        SET_ERR(E_BAD_ALLOC);
        return 0;
    }
    T_surf = s;
    set_emu_ctx(ctx);
    return 1;
}

EXPORT void* eglGetCurrentSurface(int which) {
    (void)which;
    shim_surf* cs = cur_surf();
    if (cs) return cs;
    if (shim_init()) return NULL;
    static void* (*f)(int);
    if (!f) *(void**)&f = rsym("eglGetCurrentSurface");
    return f ? f(which) : NULL;
}

EXPORT unsigned eglSwapInterval(void* d, int interval) {
    (void)d;
    shim_surf* s = cur_surf();
    if (s) {
        s->interval = interval;
        if (s->wsi) {
            if (interval <= 0) s->wsi->flags |= WSI_X11_ASYNC;
            else s->wsi->flags &= ~WSI_X11_ASYNC;
        }
    }
    return 1;
}

EXPORT unsigned eglSwapBuffers(void* d, void* surf) {
    shim_surf* s = find_surf(surf);
    if (!s) {
        if (shim_init()) return 0;
        static unsigned (*f)(void*, void*);
        if (!f) *(void**)&f = rsym("eglSwapBuffers");
        return f ? f(d, surf) : 0;
    }
    if (s->is_gbm) {
        if (T_surf != s) { SET_ERR(E_BAD_CURRENT_SURF); return 0; }
        gbm_swap(s);
        return 1;
    }
    if (s->dmabuf_inited) {
        if (T_surf != s) { SET_ERR(E_BAD_CURRENT_SURF); return 0; }
        dmabuf_swap(s);
        return 1;
    }
    if (s->wsi) {
        if (T_surf != s) { SET_ERR(E_BAD_CURRENT_SURF); return 0; }
        wsi_x11_swap(s->wsi);
        return 1;
    }
    return 1;
}

static unsigned swap_with_damage(void* d, void* surf, const int* rects, int n, const char* real_name) {
    shim_surf* s = find_surf(surf);
    if (!s) {
        if (shim_init()) return 0;
        unsigned (*f)(void*, void*, const int*, int) = rsym(real_name);
        if (!f) f = rsym("eglSwapBuffersWithDamageKHR");
        if (f) return f(d, surf, rects, n);
        return eglSwapBuffers(d, surf);
    }
    if (n < 0 || (n > 0 && !rects)) { SET_ERR(E_BAD_PARAMETER); return 0; }
    return eglSwapBuffers(d, surf);
}
EXPORT unsigned eglSwapBuffersWithDamageKHR(void* d, void* surf, const int* rects, int n) {
    return swap_with_damage(d, surf, rects, n, "eglSwapBuffersWithDamageKHR");
}
EXPORT unsigned eglSwapBuffersWithDamageEXT(void* d, void* surf, const int* rects, int n) {
    return swap_with_damage(d, surf, rects, n, "eglSwapBuffersWithDamageEXT");
}

/* ---------- GL: Framebuffer 0 -> Internal FBO ---------- */
static void shim_glBindFramebuffer(unsigned target, unsigned fb) {
    static void (*real)(unsigned, unsigned);
    if (!real) *(void**)&real = rsym("glBindFramebuffer");
    if (!real) *(void**)&real = glp("glBindFramebuffer");
    shim_surf* cs = cur_surf();
    if (fb == 0 && cs) fb = surf_default_fbo(cs);
    real(target, fb);
}

EXPORT unsigned shim_x11_default_fbo(void) {
    shim_surf* cs = cur_surf();
    return cs ? surf_default_fbo(cs) : 0;
}

static int is_valid_device(void* dev) { return dev == (void*)&g_device_obj; }

EXPORT unsigned eglQueryDisplayAttribKHR(void* d, int attr, intptr_t* value) {
    if (!d) { SET_ERR(E_BAD_DISPLAY); return 0; }
    if (shim_init()) { SET_ERR(E_BAD_DISPLAY); return 0; }
    static const char* (*qs)(void*, int);
    if (!qs) *(void**)&qs = rsym("eglQueryString");
    if (!qs || !qs(d, 0x3054)) { SET_ERR(E_NOT_INITIALIZED); return 0; }
    if (!value) { SET_ERR(E_BAD_ATTRIBUTE); return 0; }
    if (attr == EGL_DEVICE_EXT) { *value = (intptr_t)&g_device_obj; return 1; }
    if (attr == EGL_TRACK_REFERENCES_KHR) { *value = 1; return 1; }
    SET_ERR(E_BAD_ATTRIBUTE); return 0;
}

EXPORT unsigned eglQueryDeviceAttribEXT(void* dev, int attr, intptr_t* value) {
    (void)attr; (void)value;
    if (!is_valid_device(dev)) { SET_ERR(E_BAD_DEVICE_EXT); return 0; }
    SET_ERR(E_BAD_ATTRIBUTE);
    return 0;
}

EXPORT unsigned eglQueryDisplayAttribEXT(void* d, int attr, intptr_t* value) {
    return eglQueryDisplayAttribKHR(d, attr, value);
}

EXPORT unsigned eglQueryDevicesEXT(int max_devices, void** devices, int* num_devices) {
    if (!num_devices || (max_devices <= 0 && devices)) {
        SET_ERR(E_BAD_PARAMETER); return 0;
    }
    *num_devices = 1;
    if (devices && max_devices > 0) devices[0] = (void*)&g_device_obj;
    return 1;
}

#define GL_RENDERER_STR 0x1F01
  
static char g_glver[192];
static char g_renderer[128];
static pthread_once_t g_renderer_once = PTHREAD_ONCE_INIT;

static void* renderer_probe(void* arg) {
    (void)arg;
    void* (*gd)(void*)  = rsym("eglGetDisplay");
    unsigned (*init)(void*, int*, int*) = rsym("eglInitialize");
    unsigned (*choose)(void*, const int*, void**, int, int*) = rsym("eglChooseConfig");
    void* (*mkpb)(void*, void*, const int*) = rsym("eglCreatePbufferSurface");
    void* (*mkctx)(void*, void*, void*, const int*) = rsym("eglCreateContext");
    unsigned (*mc)(void*, void*, void*, void*) = rsym("eglMakeCurrent");
    unsigned (*dctx)(void*, void*) = rsym("eglDestroyContext");
    unsigned (*dsurf)(void*, void*) = rsym("eglDestroySurface");
    const unsigned char* (*gs)(unsigned) = glp("glGetString");
    if (!gd || !init || !choose || !mkpb || !mkctx || !mc || !dctx || !dsurf || !gs) return NULL;

    void* d = gd(NULL);
    if (!d || !init(d, NULL, NULL)) return NULL;

    static const int cattr[] = { 0x3040, 0x4, 0x3033, 0x1, E_NONE };
    void* cfg = NULL; int n = 0;
    if (!choose(d, cattr, &cfg, 1, &n) || n < 1) return NULL;

    static const int pattr[] = { 0x3057, 1, 0x3056, 1, E_NONE };
    static const int xattr[] = { 0x3098, 2, E_NONE };
    void* surf = mkpb(d, cfg, pattr);
    void* ctx = mkctx(d, cfg, NULL, xattr);
    if (surf && ctx && mc(d, surf, surf, ctx)) {
        const unsigned char* r = gs(GL_RENDERER_STR);
        const unsigned char* v = gs(0x1F02);
        if (v) snprintf(g_glver, sizeof g_glver, "%s", (const char*)v);
        if (r) snprintf(g_renderer, sizeof g_renderer, "%s", (const char*)r);
        mc(d, NULL, NULL, NULL);
    }
    if (ctx) dctx(d, ctx);
    if (surf) dsurf(d, surf);
    return NULL;
}

static void renderer_once(void) {
    pthread_t th;
    if (shim_init()) return;
    if (pthread_create(&th, NULL, renderer_probe, NULL) == 0)
        pthread_join(th, NULL);
}
  
static const char* device_renderer(void) {
    pthread_once(&g_renderer_once, renderer_once);
    return g_renderer[0] ? g_renderer : "Mali GPU";
}

static const char* drv_vendor(void) {
    static const char* v;
    if (!v) {
        const char* (*qs)(void*, int) = rsym("eglQueryString");
        void* (*gd)(void*) = rsym("eglGetDisplay");
        unsigned (*init)(void*, int*, int*) = rsym("eglInitialize");
        void* d = gd ? gd(NULL) : NULL;
        if (d && init && qs && init(d, NULL, NULL)) v = qs(d, 0x3053);
    }
    return v ? v : "";
}

static const char* drv_egl_version(void) {
    static const char* ver;
    if (!ver) {
        void* (*gd)(void*) = rsym("eglGetDisplay");
        unsigned (*init)(void*, int*, int*) = rsym("eglInitialize");
        const char* (*qs)(void*, int) = rsym("eglQueryString");
        void* d = gd ? gd(NULL) : NULL;
        if (d && init && qs && init(d, NULL, NULL)) ver = qs(d, 0x3054);
    }
    return ver ? ver : "";
}

static char g_drv_name[32];
static pthread_once_t g_drv_name_once = PTHREAD_ONCE_INIT;

static void drv_name_init(void) {
    const char* v = drv_vendor();
    size_t i = 0;
    for (; v[i] && i < sizeof g_drv_name - 1; i++)
        g_drv_name[i] = (v[i] >= 'A' && v[i] <= 'Z') ? v[i] + 32 : v[i];
    g_drv_name[i] = 0;
    if (!g_drv_name[0]) snprintf(g_drv_name, sizeof g_drv_name, "unknown");
}

static const char* drv_name(void) {
    pthread_once(&g_drv_name_once, drv_name_init);
    return g_drv_name;
}

static void drv_parse(char* arch, size_t na, char* rel, size_t nr) {
    const char* v = drv_egl_version();
    arch[0] = rel[0] = 0;
    const char* sp = strchr(v, ' ');
    if (!sp) return;
    sp++;
    size_t i = 0;
    while (sp[i] && (sp[i] == '_' || (sp[i] >= 'A' && sp[i] <= 'Z') ||
                     (sp[i] >= 'a' && sp[i] <= 'z') || (sp[i] >= '0' && sp[i] <= '9')) && i < na - 1)
        { arch[i] = sp[i]; i++; }
    arch[i] = 0;
    const char* q = strchr(sp, '"');
    if (q) {
        q++;
        const char* e = strchr(q, '"');
        size_t n = e ? (size_t)(e - q) : strlen(q);
        if (n >= nr) n = nr - 1;
        memcpy(rel, q, n); rel[n] = 0;
    }
}

static void uuid_from(const char* s, const char* salt, unsigned char out[16]) {
    uint64_t h1 = 1469598103934665603ULL;
    uint64_t h2 = 0x9E3779B97F4A7C15ULL;
    for (const char* p = salt; *p; p++) {
        h1 = (h1 ^ (unsigned char)*p) * 1099511628211ULL;
        h2 = (h2 + (unsigned char)*p) * 0x100000001B3ULL;
    }
    for (const char* p = s; *p; p++) {
        h1 = (h1 ^ (unsigned char)*p) * 1099511628211ULL;
        h2 = (h2 + (unsigned char)*p) * 0x100000001B3ULL;
    }
    memcpy(out, &h1, 8);
    memcpy(out + 8, &h2, 8);
}

static unsigned char g_vk_dev[16], g_vk_drv[16];
static int g_vk_ok;
static pthread_once_t g_vk_once = PTHREAD_ONCE_INIT;

static int uuid_nonzero(const unsigned char* u) {
    for (int i = 0; i < 16; i++) if (u[i]) return 1;
    return 0;
}

static void vk_probe(void) {
    void* h = dlopen("libvulkan.so", RTLD_NOW);
    if (!h) h = dlopen("libvulkan.so.1", RTLD_NOW);
    if (!h) return;

    PFN_vkGetInstanceProcAddr gipa = (PFN_vkGetInstanceProcAddr)dlsym(h, "vkGetInstanceProcAddr");
    PFN_vkCreateInstance ci = (PFN_vkCreateInstance)dlsym(h, "vkCreateInstance");
    if (!gipa || !ci) return;

    VkApplicationInfo ai = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                             .apiVersion = VK_API_VERSION_1_1 };
    VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
                                 .pApplicationInfo = &ai };
    VkInstance inst;
    if (ci(&ici, NULL, &inst) != VK_SUCCESS) return;

    PFN_vkEnumeratePhysicalDevices en =
        (PFN_vkEnumeratePhysicalDevices)gipa(inst, "vkEnumeratePhysicalDevices");
    PFN_vkGetPhysicalDeviceProperties2 gp2 =
        (PFN_vkGetPhysicalDeviceProperties2)gipa(inst, "vkGetPhysicalDeviceProperties2");
    PFN_vkDestroyInstance di = (PFN_vkDestroyInstance)gipa(inst, "vkDestroyInstance");

    VkPhysicalDevice pd[8];
    uint32_t n = 8;
    if (en && gp2 && en(inst, &n, pd) >= 0 && n > 0) {
        VkPhysicalDeviceIDProperties id = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES };
        VkPhysicalDeviceProperties2 p = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
                                          .pNext = &id };
        gp2(pd[0], &p);
        if (uuid_nonzero(id.deviceUUID)) {
            memcpy(g_vk_dev, id.deviceUUID, 16);
            memcpy(g_vk_drv, id.driverUUID, 16);
            g_vk_ok = 1;
            DBG("[shim] UUID from Vulkan (%s)\n", p.properties.deviceName);
        }
    }
    if (di) di(inst, NULL);
}
  
EXPORT unsigned eglQueryDeviceBinaryEXT(void* dev, int name, int max_size, void* value, int* size) {
    if (!is_valid_device(dev)) { SET_ERR(E_BAD_DEVICE_EXT); return 0; }
    if (shim_init()) { SET_ERR(E_BAD_DEVICE_EXT); return 0; }
    if (name != EGL_DEVICE_UUID_EXT && name != EGL_DRIVER_UUID_EXT) { SET_ERR(E_BAD_ATTRIBUTE); return 0; }
    if (!value) { if (!size) { SET_ERR(E_BAD_PARAMETER); return 0; } *size = 16; return 1; }

    unsigned char u[16];
    pthread_once(&g_vk_once, vk_probe);
    if (g_vk_ok) {
        memcpy(u, name == EGL_DEVICE_UUID_EXT ? g_vk_dev : g_vk_drv, 16);
    } else {
        char arch[32], rel[64], key[512];
        drv_parse(arch, sizeof arch, rel, sizeof rel);
        const char* r = device_renderer();
        if (name == EGL_DEVICE_UUID_EXT) {
            snprintf(key, sizeof key, "%s|%s", arch, r);
            uuid_from(key, "device", u);
        } else {
            snprintf(key, sizeof key, "%s|%s|%s", drv_egl_version(), g_glver, r);
            uuid_from(key, "driver", u);
        }
    }
    int n = max_size < 0 ? 0 : (max_size < 16 ? max_size : 16);
    memcpy(value, u, n);
    if (size) *size = n;
    return 1;
}

/* ---------- DRM nodes via libdrm (dlopen) ---------- */
typedef struct { char** nodes; int available_nodes; } drm_dev_prefix;
#define DRM_NODE_PRIMARY_I 0
#define DRM_NODE_RENDER_I  2

static char g_drm_primary[128], g_drm_render[128];
static pthread_once_t g_drm_once = PTHREAD_ONCE_INIT;

static void drm_probe(void) {
    const char* envp = getenv("SHIM_DRM_PRIMARY");
    const char* envr = getenv("SHIM_DRM_RENDER");
    if (envp) snprintf(g_drm_primary, sizeof g_drm_primary, "%s", envp);
    if (envr) snprintf(g_drm_render, sizeof g_drm_render, "%s", envr);
    if (envp || envr) return;

    void* h = NULL;
    const char* lib = getenv("SHIM_LIBDRM");
    if (lib && *lib) h = dlopen(lib, RTLD_NOW);
    if (!h) h = dlopen("libdrm.so.2", RTLD_NOW);
    if (!h) h = dlopen("libdrm.so", RTLD_NOW);
    if (!h) { DBG("[shim] libdrm not found\n"); return; }

    int (*get)(uint32_t, drm_dev_prefix**, int) = dlsym(h, "drmGetDevices2");
    void (*fre)(drm_dev_prefix**, int) = dlsym(h, "drmFreeDevices");
    if (!get || !fre) { DBG("[shim] libdrm: missing drmGetDevices2/drmFreeDevices\n"); return; }

    drm_dev_prefix* devs[16];
    int n = get(0, devs, 16);
    for (int i = 0; i < n; i++) {
        int av = devs[i]->available_nodes;
        if (!(av & (1 << DRM_NODE_RENDER_I))) continue;
        snprintf(g_drm_render, sizeof g_drm_render, "%s", devs[i]->nodes[DRM_NODE_RENDER_I]);
        if (av & (1 << DRM_NODE_PRIMARY_I))
            snprintf(g_drm_primary, sizeof g_drm_primary, "%s", devs[i]->nodes[DRM_NODE_PRIMARY_I]);
        break;
    }
    if (n > 0) fre(devs, n);
    DBG("[shim] DRM primary=%s render=%s\n",
        g_drm_primary[0] ? g_drm_primary : "(none)", g_drm_render[0] ? g_drm_render : "(none)");
}

EXPORT const char* eglQueryDeviceStringEXT(void* dev, int name) {
    if (!is_valid_device(dev)) { SET_ERR(E_BAD_DEVICE_EXT); return NULL; }
    if (shim_init()) { SET_ERR(E_BAD_DEVICE_EXT); return NULL; }
    switch (name) {
    case 0x3055:
        return "EGL_EXT_device_query_name EGL_EXT_device_persistent_id "
               "EGL_EXT_device_drm EGL_EXT_device_drm_render_node";
    case 0x3053: {
        const char* v = drv_vendor();
        return v[0] ? v : "ARM";
    }
    case EGL_RENDERER_EXT:
        return device_renderer();
    case EGL_DRIVER_NAME_EXT:
        return drv_name();
    case EGL_DRM_DEVICE_FILE_EXT:
        pthread_once(&g_drm_once, drm_probe);
        return g_drm_primary[0] ? g_drm_primary : NULL;
    case EGL_DRM_RENDER_NODE_FILE_EXT:
        pthread_once(&g_drm_once, drm_probe);
        return g_drm_render[0] ? g_drm_render : NULL;
    default:
        SET_ERR(E_BAD_PARAMETER); return NULL;
    }
}

EXPORT const char* eglGetDisplayDriverName(void* d) {
    if (!d) { SET_ERR(E_BAD_DISPLAY); return NULL; }
    if (shim_init()) { SET_ERR(E_BAD_DISPLAY); return NULL; }
    static const char* (*qs)(void*, int);
    if (!qs) *(void**)&qs = rsym("eglQueryString");
    if (!qs || !qs(d, 0x3054)) { SET_ERR(E_NOT_INITIALIZED); return NULL; }
    return drv_name();
}

EXPORT char* eglGetDisplayDriverConfig(void* d) {
    if (!d) { SET_ERR(E_BAD_DISPLAY); return NULL; }
    if (shim_init()) { SET_ERR(E_BAD_DISPLAY); return NULL; }
    static const char* (*qs)(void*, int);
    if (!qs) *(void**)&qs = rsym("eglQueryString");
    if (!qs || !qs(d, 0x3054)) { SET_ERR(E_NOT_INITIALIZED); return NULL; }
    char* x = strdup("<driinfo>\n</driinfo>\n");
    if (!x) SET_ERR(E_BAD_ALLOC);
    return x;
}

#define MAX_DPY 4
static struct { void* d; int refs; } g_dref[MAX_DPY];

static int dref_total(void) {          /* called with G.mu held */
    int t = 0;
    for (int i = 0; i < MAX_DPY; i++) t += g_dref[i].refs;
    return t;
}

EXPORT unsigned eglInitialize(void* d, int* maj, int* min) {
    if (shim_init()) return 0;
    static unsigned (*f)(void*, int*, int*);
    if (!f) *(void**)&f = rsym("eglInitialize");
    if (!f) return 0;
    unsigned r = f(d, maj, min);
    if (r) {
        pthread_mutex_lock(&G.mu);
        int k = -1;
        for (int i = 0; i < MAX_DPY; i++) {
            if (g_dref[i].d == d && g_dref[i].refs) { k = i; break; }
            if (k < 0 && !g_dref[i].refs) k = i;
        }
        if (k >= 0) { g_dref[k].d = d; g_dref[k].refs++; }
        else DBG("[shim] eglInitialize: MAX_DPY (%d) reached, display %p not tracked\n", MAX_DPY, d);
        pthread_mutex_unlock(&G.mu);
    }
    return r;
}

static void shim_cleanup(void* d, int last) {
    unsigned (*ds)(void*, void*) = rsym("eglDestroySyncKHR");
    unsigned (*di)(void*, void*) = rsym("eglDestroyImageKHR");
    void (*gfree)(void*) = (void (*)(void*))emu_sym("glx_shim_egl_free");

    shim_surf* surfs = NULL;
    shim_sync* syncs = NULL;
    img_rec*   imgs  = NULL;
    drm_img*   drms  = NULL;
    void* gctx[64]; int ng = 0;

    pthread_mutex_lock(&G.mu);
    for (shim_surf** p = &G.surfs; *p; ) {
        if ((*p)->dpy == d) { shim_surf* s = *p; *p = s->next; s->next = surfs; surfs = s; }
        else p = &(*p)->next;
    }
    if (last) {
        syncs = g_syncs;    g_syncs = NULL;
        imgs  = g_imgs;     g_imgs = NULL;
        drms  = g_drm_imgs; g_drm_imgs = NULL;
        for (int i = 0; i < G.nctx; i++) if (G.gctx[i]) gctx[ng++] = G.gctx[i];
        G.nctx = 0;
    }
    pthread_mutex_unlock(&G.mu);

    int ns = 0, ny = 0, ni = 0, nd = 0;
    shim_surf* cs = cur_surf();
    if (cs && cs->dpy == d) T_surf = NULL;

    for (shim_surf* s = surfs, *nx; s; s = nx) { nx = s->next; surf_release(s, 0); ns++; }
    for (shim_sync* y = syncs, *nx; y; y = nx) {
        nx = y->next;
        if (y->real && ds) ds(d, y->real);
        free(y); ny++;
    }
    for (img_rec* r = imgs, *nx; r; r = nx) { nx = r->next; free(r); ni++; }
    for (drm_img* r = drms, *nx; r; r = nx) {
        nx = r->next;
        if (r->img && di) di(d, r->img);
        if (!r->imported) drm_ahb_unreg(r->ab);
        if (r->ab && ahb_release) ahb_release(r->ab);
        free(r); nd++;
    }
    for (int i = 0; i < ng; i++) if (gfree) gfree(gctx[i]);

    if (last) {
        pthread_mutex_lock(&D.mu);
        for (dbg_lbl* l = g_lbls, *nx; l; l = nx) { nx = l->next; free(l); }
        g_lbls = NULL;
        pthread_mutex_unlock(&D.mu);
    }

    if (ns || ny || ni || nd)
        DBG("[shim] cleanup: %d surfaces, %d syncs, %d dmabuf images, %d drm images leaked by app\n",
            ns, ny, ni, nd);
}
  
EXPORT unsigned eglTerminate(void* d) {
    if (shim_init()) return 0;
    static unsigned (*f)(void*);
    if (!f) *(void**)&f = rsym("eglTerminate");
    if (!f) return 0;

    pthread_mutex_lock(&G.mu);
    int k = -1;
    for (int i = 0; i < MAX_DPY; i++)
        if (g_dref[i].d == d && g_dref[i].refs) { k = i; break; }
    if (k < 0) { pthread_mutex_unlock(&G.mu); return f(d); }
    if (g_dref[k].refs > 1) { g_dref[k].refs--; pthread_mutex_unlock(&G.mu); return 1; }
    g_dref[k].refs = 0; g_dref[k].d = NULL;
    int last = (dref_total() == 0);
    pthread_mutex_unlock(&G.mu);

    shim_cleanup(d, last);
    return f(d);
}

static void* gl_self_sym(const char* n) {
    static void* h;
    if (!h) h = dlopen("libGL.so.1", RTLD_NOW | RTLD_NOLOAD);
    return h ? dlsym(h, n) : NULL;
}

EXPORT unsigned eglExportDMABUFImageQueryMESA(void* d, void* image, int* fourcc,
                                              int* nplanes, uint64_t* mods) {
    (void)d;
    img_rec r;
    if (!img_find_copy(image, &r)) { SET_ERR(E_BAD_MATCH); return 0; }
    if (fourcc)  *fourcc = r.fourcc;
    if (nplanes) *nplanes = r.nplanes;
    if (mods)    for (int i = 0; i < r.nplanes; i++) mods[i] = r.mod;
    return 1;
}

EXPORT unsigned eglExportDMABUFImageMESA(void* d, void* image, int* fds,
                                         int* strides, int* offsets) {
    (void)d;
    img_rec r;
    if (!img_find_copy(image, &r)) { SET_ERR(E_BAD_MATCH); return 0; }
    int out[3] = { -1, -1, -1 };
    for (int i = 0; i < r.nplanes; i++) {
        out[i] = r.fd[i] >= 0 ? dup(r.fd[i]) : -1;
        if (out[i] < 0) {
            for (int j = 0; j < i; j++) close(out[j]);
            SET_ERR(E_BAD_ALLOC); return 0;
        }
    }
    for (int i = 0; i < r.nplanes; i++) {
        if (fds)     fds[i] = out[i];
        if (strides) strides[i] = r.pitch[i];
        if (offsets) offsets[i] = r.off[i];
    }
    return 1;
}

EXPORT unsigned eglPostSubBufferNV(void* d, void* surf, int x, int y, int w, int h) {
    shim_surf* s = find_surf(surf);
    if (!s) {                       /* pbuffer / Mali surface */
        if (shim_init()) return 0;
        static unsigned (*f)(void*, void*, int, int, int, int);
        if (!f) *(void**)&f = rsym("eglPostSubBufferNV");
        if (f) return f(d, surf, x, y, w, h);
        SET_ERR(E_BAD_SURFACE); return 0;
    }
    if (x < 0 || y < 0 || w < 0 || h < 0) { SET_ERR(E_BAD_PARAMETER); return 0; }
    if (T_surf != s) { SET_ERR(E_BAD_CURRENT_SURF); return 0; }
    if (w == 0 || h == 0 || x >= s->w || y >= s->h) return 1;  /* nothing to present */
    return eglSwapBuffers(d, surf);
}
  
/* ---------- eglGetProcAddress ---------- */
#define PA(n) { #n, (void*)n }
EXPORT void* eglGetProcAddress(const char* name) {
    static const struct { const char* n; void* f; } tab[] = {
        PA(eglGetError), PA(eglGetDisplay), PA(eglGetPlatformDisplayEXT), PA(eglGetPlatformDisplay),
        PA(eglInitialize), PA(eglTerminate), PA(eglQueryString), PA(eglGetConfigs),
        PA(eglChooseConfig), PA(eglGetConfigAttrib), PA(eglCreateWindowSurface),
        PA(eglCreatePlatformWindowSurfaceEXT), PA(eglCreatePlatformWindowSurface),
        PA(eglCreatePixmapSurface), PA(eglCreatePlatformPixmapSurfaceEXT),
        PA(eglCreatePlatformPixmapSurface), PA(eglCreatePbufferSurface),
        PA(eglDestroySurface), PA(eglQuerySurface), PA(eglSurfaceAttrib), 
        PA(eglDebugMessageControlKHR), PA(eglQueryDebugKHR), PA(eglLabelObjectKHR),
        PA(eglQueryDevicesEXT), PA(eglUnsignalSyncEXT), PA(eglQueryDeviceBinaryEXT), PA(eglGetDisplayDriverName), PA(eglGetDisplayDriverConfig), PA(eglExportDMABUFImageQueryMESA), PA(eglExportDMABUFImageMESA), PA(eglCreateDRMImageMESA), PA(eglExportDRMImageMESA),
        PA(eglBindAPI), PA(eglQueryAPI), PA(eglWaitClient), PA(eglWaitGL), PA(eglWaitNative),
        PA(eglReleaseThread), PA(eglCreateContext), PA(eglDestroyContext), PA(eglMakeCurrent),
        PA(eglGetCurrentContext), PA(eglGetCurrentSurface), PA(eglGetCurrentDisplay),
        PA(eglQueryContext), PA(eglSwapBuffers), PA(eglSwapInterval), PA(eglCopyBuffers),
        PA(eglCreateImageKHR), PA(eglDestroyImageKHR), PA(eglCreateImage), PA(eglDestroyImage),
        PA(eglCreateSyncKHR), PA(eglDestroySyncKHR), PA(eglClientWaitSyncKHR), PA(eglWaitSyncKHR),
        PA(eglGetSyncAttribKHR), PA(eglSetDamageRegionKHR), PA(eglDupNativeFenceFDANDROID),
        PA(eglSwapBuffersWithDamageKHR), PA(eglSwapBuffersWithDamageEXT),
        PA(eglQueryDisplayAttribEXT), PA(eglQueryDisplayAttribKHR),
        PA(eglQueryDeviceAttribEXT), PA(eglQueryDeviceStringEXT),
        PA(eglQueryDmaBufFormatsEXT), PA(eglQueryDmaBufModifiersEXT),
        PA(eglPostSubBufferNV), PA(eglSignalSyncKHR),
        { "glBindFramebuffer", (void*)shim_glBindFramebuffer },
        { "glBindFramebufferOES", (void*)shim_glBindFramebuffer },
    };
    if (!name) return NULL;
    for (unsigned i = 0; i < sizeof tab / sizeof tab[0]; i++)
        if (!strcmp(name, tab[i].n)) return tab[i].f;
    if (g_gl_mapped && name[0]=='g' && name[1]=='l' && name[2]!='X') {
        void* p = gl_self_sym(name);
        if (p) return p;
    }
    if (shim_init() || !G.gpa) return NULL;
    void* r = G.gpa(name);
    if (!r) DBG("[shim] getproc MISS %s\n", name);
    return r;
}

__attribute__((destructor))
static void shim_atexit(void) {
    if (!G.ready || !shim_debug_enabled()) return;
    int n = 0;
    for (shim_surf* s = G.surfs; s; s = s->next) n++;
    if (n || g_syncs || g_imgs || g_drm_imgs)
        fprintf(stderr, "[shim] exit: app left %d surfaces%s%s%s alive\n", n,
                g_syncs ? ", syncs" : "", g_imgs ? ", images" : "",
                g_drm_imgs ? ", drm images" : "");
}
