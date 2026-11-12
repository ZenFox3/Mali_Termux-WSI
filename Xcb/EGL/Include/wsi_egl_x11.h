#pragma once
/*
 * wsi_egl_x11.h - WSI for Mali blob (sphal) on X11 without EGL X11 platform.
 *
 *   AHardwareBuffer (linear, CPU flags) -> dma-buf -> EGLImage -> FBO
 *   swap: [blit flip Y] -> native fence -> xcb_present_pixmap
 *   PresentIdleNotify -> buffer returned to pool
 *
 * Depends on: mali_sphal.h, libxcb, libxcb-dri3, libxcb-present.
 *
 * Note on colors: the RGBA8 AHB is imported with DRM fourcc AR24 (ARGB8888) on purpose.
 * That swaps R/B on the GL side so the BGRA X11 pixmap shows correct colors without a swizzle.
 *
 * Fixes vs previous revision:
 *   FIX A  dma-buf fd chosen by lseek() size (was hardcoded data[1], required numFds >= 2)
 *   FIX B  DRI3 1.0 stride is 16-bit: reject instead of truncating
 *   FIX C  failed pixmap_from_buffer left b->pix set -> later xcb_free_pixmap on a pixmap
 *          that never existed (async BadPixmap)
 *   FIX D  dup() failure was passed to the server as fd -1
 *   FIX E  acquire(): after the 200 ms poll timeout, drain events again before force-reclaiming
 *          (with a shared Xlib connection another thread may already have read IDLE_NOTIFY)
 *   FIX F  rmsync could be NULL when a sync object exists
 *   FIX G  libnativewindow fallback path, missing includes (dlfcn, stdint, errno, lseek)
 *   FIX H  check that DRI3 and Present extensions exist before doing anything
 *   FIX I  frame pacing no longer calls getenv() every frame
 */
#include "mali_sphal.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <dlfcn.h>
#include <unistd.h>
#include <sys/types.h>
#include <poll.h>
#include <time.h>
#include <xcb/xcb.h>
#include <xcb/dri3.h>
#include <xcb/present.h>

#define WSI_X11_MAX_BUF 4
#define WSI_X11_DIRECT  1   /* Direct rendering into buffer (inverted Y, no blit) */
#define WSI_X11_ASYNC   2   /* Present without vsync */
#define WSI_X11_SHARED  4   /* Shared xcb connection with Xlib (preserves normal events) */

enum { WSI_FREE = 0, WSI_ACQ, WSI_BUSY };

typedef struct { uint32_t width, height, layers, format; uint64_t usage;
                 uint32_t stride, rfu0; uint64_t rfu1; } wsi_ahb_desc;
typedef struct { int version, numFds, numInts; int data[]; } wsi_nhandle;

typedef struct {
    void* ahb; int dfd; void* img;
    unsigned tex, fbo;
    xcb_pixmap_t pix;
    uint32_t stride; size_t size;
    int state; uint32_t serial;
} wsi_x11_buf;

typedef struct wsi_x11 {
    xcb_connection_t* c;
    xcb_window_t win;
    xcb_special_event_t* se;
    uint32_t eid, serial;
    int w, h, nbuf, cur, flags, gles;
    int blit, y_inverted, forced;
    int fps; int64_t next_ns;           /* Frame pacing: fps=0 => rely only on Present vsync */
    wsi_x11_buf buf[WSI_X11_MAX_BUF];
    /* WSI_STATS=1: Present diagnostics (versions, capabilities, CompleteNotify) */
    int stats, st_msc_seen;
    int vsync, vs_timeouts;            /* WSI_VSYNC=1: max one frame in flight (CompleteNotify) */
    uint32_t last_complete, pcaps;     /* Last confirmed serial; server Present capabilities */
    int64_t st_t0;
    unsigned st_swaps, st_cmp[4];
    uint64_t st_msc0, st_msc_last;

    /* AHB */
    int  (*ahb_alloc)(const wsi_ahb_desc*, void**);
    void (*ahb_desc)(void*, wsi_ahb_desc*);
    const wsi_nhandle* (*ahb_gnh)(void*);
    void (*ahb_rel)(void*);

    /* EGL */
    void *egl, *dpy, *ctx;
    void* (*gpa)(const char*);
    void* (*mkimg)(void*, void*, unsigned, void*, const int*);
    unsigned (*rmimg)(void*, void*);
    void* (*mksync)(void*, unsigned, const int*);
    unsigned (*rmsync)(void*, void*);
    int   (*dupfence)(void*, void*);

    /* GL */
    void (*glGenTextures)(int, unsigned*);
    void (*glDeleteTextures)(int, const unsigned*);
    void (*glBindTexture)(unsigned, unsigned);
    void (*glGenFramebuffers)(int, unsigned*);
    void (*glDeleteFramebuffers)(int, const unsigned*);
    void (*glBindFramebuffer)(unsigned, unsigned);
    void (*glFramebufferTexture2D)(unsigned, unsigned, unsigned, unsigned, int);
    void (*glFramebufferRenderbuffer)(unsigned, unsigned, unsigned, unsigned);
    unsigned (*glCheckFramebufferStatus)(unsigned);
    void (*glGenRenderbuffers)(int, unsigned*);
    void (*glDeleteRenderbuffers)(int, const unsigned*);
    void (*glBindRenderbuffer)(unsigned, unsigned);
    void (*glRenderbufferStorage)(unsigned, unsigned, int, int);
    void (*glTexStorage2D)(unsigned, int, unsigned, int, int);
    void (*glBlitFramebuffer)(int, int, int, int, int, int, int, int, unsigned, unsigned);
    void (*glEGLImageTargetTexture2DOES)(unsigned, void*);
    unsigned char (*glIsEnabled)(unsigned);
    void (*glEnable)(unsigned);
    void (*glDisable)(unsigned);
    void (*glFlush)(void);
    void (*glFinish)(void);
    /* ES1 only (flip via OES_draw_texture) */
    void (*glTexImage2D)(unsigned, int, int, int, int, int, unsigned, unsigned, const void*);
    void (*glTexParameteri)(unsigned, unsigned, int);
    void (*glTexParameteriv)(unsigned, unsigned, const int*);
    void (*glDrawTexiOES)(int, int, int, int, int);
    void (*glGetIntegerv)(unsigned, int*);
    void (*glGetBooleanv)(unsigned, unsigned char*);
    void (*glActiveTexture)(unsigned);
    void (*glGetTexEnviv)(unsigned, unsigned, int*);
    void (*glTexEnvi)(unsigned, unsigned, int);
    void (*glColorMask)(unsigned char, unsigned char, unsigned char, unsigned char);
    void (*glViewport)(int, int, int, int);

    /* back buffer (flip) */
    unsigned back_tex, back_fbo, back_rb;
} wsi_x11;

/* ---------- debug logging helper ---------- */
static inline int wsi_debug_enabled(void) {
    const char* env_egl = getenv("EGL_DEBUG");
    const char* env_mali = getenv("MALI_DEBUG");
    const char* env_gl = getenv("GL_DEBUG");
    return (env_egl && env_egl[0] != '0') ||
           (env_mali && env_mali[0] != '0') ||
           (env_gl && env_gl[0] != '0');
}

/* ---------- helpers ---------- */
static inline void* wsi_x11__sym(wsi_x11* w, const char* n) {
    void* p = dlsym(w->egl, n);
    return p ? p : (w->gpa ? w->gpa(n) : NULL);
}

static inline void* wsi_x11__glsym(wsi_x11* w, const char* n) {
    if (w->gles == 1) {                        /* ES1: glGenFramebuffers -> glGenFramebuffersOES */
        char b[96];
        snprintf(b, sizeof b, "%sOES", n);
        void* p = dlsym(w->egl, b);
        if (p) return p;
    }
    return wsi_x11__sym(w, n);
}

static inline int64_t wsi_x11_now_ns(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000000000LL + t.tv_nsec;
}

/* Frame rate cap (e.g. 90). 0 = disabled */
static inline void wsi_x11_set_fps(wsi_x11* w, int fps) { w->fps = fps; w->next_ns = 0; }

static inline void wsi_x11__pace(wsi_x11* w) {
    if (w->fps <= 0) return;
    /* FIX I: read the tuning env vars once */
    static int cfg_done = 0, spin = 0;
    static int64_t slack_ns = 1500000;
    if (!cfg_done) {
        const char* es = getenv("WSI_SLACK_US");
        if (es) slack_ns = atoll(es) * 1000;
        spin = getenv("WSI_SPIN") != NULL;
        cfg_done = 1;
    }
    int64_t step = 1000000000LL / w->fps, now = wsi_x11_now_ns();
    if (w->next_ns == 0 || now - w->next_ns > step * 4) w->next_ns = now;   /* Resync after stall */
    else if (now < w->next_ns) {
        /* Hybrid sleep: sleep until `slack` before target, remaining spin.
           WSI_SPIN=1 => pure spin loop (keeps CPU awake, tests DVFS) */
        int64_t slack = spin ? step : slack_ns;
        if (w->next_ns - now > slack) {
            int64_t t = w->next_ns - slack;
            struct timespec ts = { t / 1000000000LL, t % 1000000000LL };
            clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL);
        }
        while (wsi_x11_now_ns() < w->next_ns) { }
    }
    w->next_ns += step;
}

static inline void wsi_x11__fence(wsi_x11* w) {
    void* s = w->mksync ? w->mksync(w->dpy, 0x3144 /*NATIVE_FENCE_ANDROID*/, NULL) : NULL;
    w->glFlush();
    int fd = (s && w->dupfence) ? w->dupfence(w->dpy, s) : -1;
if (fd >= 0) {
    struct pollfd p = { fd, POLLIN, 0 };
    int r;
    do r = poll(&p, 1, 1000); while (r < 0 && errno == EINTR);
    close(fd);
    if (r <= 0) w->glFinish();   /* timeout sau eroare */
} else w->glFinish();/* FIX F */
}

static inline void wsi_x11__drain(wsi_x11* w) {
    xcb_generic_event_t* e;
    while ((e = xcb_poll_for_special_event(w->c, w->se))) {
        uint16_t evt = ((xcb_present_generic_event_t*)e)->evtype;
        if (evt == XCB_PRESENT_EVENT_IDLE_NOTIFY) {
            xcb_pixmap_t px = ((xcb_present_idle_notify_event_t*)e)->pixmap;
            for (int i = 0; i < w->nbuf; i++)
                if (w->buf[i].pix == px && w->buf[i].state == WSI_BUSY) w->buf[i].state = WSI_FREE;
        } else if ((w->stats || w->vsync) && evt == XCB_PRESENT_EVENT_COMPLETE_NOTIFY) {
            xcb_present_complete_notify_event_t* cn = (xcb_present_complete_notify_event_t*)e;
            if (cn->kind == XCB_PRESENT_COMPLETE_KIND_PIXMAP) {
                if ((int32_t)(cn->serial - w->last_complete) > 0) w->last_complete = cn->serial;
                if (w->stats && cn->mode < 4) w->st_cmp[cn->mode]++;
            }
            if (w->stats) {
                if (!w->st_msc_seen) { w->st_msc0 = cn->msc; w->st_msc_seen = 1; }
                w->st_msc_last = cn->msc;
            }
        }
        free(e);
    }
    if (!(w->flags & WSI_X11_SHARED))
        while ((e = xcb_poll_for_event(w->c))) free(e);   /* Async errors */
}

/* ---------- buffers ---------- */

/* FIX A: pick the fd that is a dma-buf big enough for the pixels.
 * On the Mali gralloc tested: fd[0] -> lseek == -1 (metadata), fd[1] -> the real buffer. */
static inline int wsi_x11__pick_fd(const wsi_nhandle* h, off_t min_size) {
    for (int i = 0; i < h->numFds; i++) {
        off_t sz = lseek(h->data[i], 0, SEEK_END);
        if (sz < 0) continue;
        lseek(h->data[i], 0, SEEK_SET);
        if (sz >= min_size) return h->data[i];
    }
    return h->numFds > 1 ? h->data[1] : (h->numFds == 1 ? h->data[0] : -1);
}

static inline int wsi_x11__buf_create(wsi_x11* w, wsi_x11_buf* b) {
    memset(b, 0, sizeof *b); b->dfd = -1;
    wsi_ahb_desc d = { .width = w->w, .height = w->h, .layers = 1, .format = 1 /*RGBA8*/,
        .usage = (1ull << 9) | (1ull << 8) | (1ull << 1) | (1ull << 5) }; /* COLOR|SAMPLED|CPU_R|CPU_W => linear */
    if (w->ahb_alloc(&d, &b->ahb) || !b->ahb) {
        if (wsi_debug_enabled()) fprintf(stderr, "[wsi] AHB allocation failed\n");
        return -1;
    }
    wsi_ahb_desc o; w->ahb_desc(b->ahb, &o);
    b->stride = o.stride * 4; b->size = (size_t)b->stride * w->h;
    /* FIX B: DRI3 1.0 stride field is 16 bits */
    if (b->stride == 0 || b->stride > 65535) {
        if (wsi_debug_enabled()) fprintf(stderr, "[wsi] stride %u bytes does not fit DRI3 1.0\n", b->stride);
        return -1;
    }
    const wsi_nhandle* h = w->ahb_gnh(b->ahb);
    if (!h || h->numFds < 1) {
        if (wsi_debug_enabled()) fprintf(stderr, "[wsi] Native handle missing dma-buf\n");
        return -1;
    }
    b->dfd = wsi_x11__pick_fd(h, (off_t)b->size);          /* FIX A */
    if (b->dfd < 0) {
        if (wsi_debug_enabled()) fprintf(stderr, "[wsi] No usable dma-buf fd\n");
        return -1;
    }

    const int at[] = { 0x3057, w->w, 0x3056, w->h, 0x3271, 0x34325241 /*AR24*/,
                       0x3272, b->dfd, 0x3273, 0, 0x3274, (int)b->stride, 0x3038 };
    b->img = w->mkimg(w->dpy, NULL, 0x3270 /*LINUX_DMA_BUF_EXT*/, NULL, at);
    if (!b->img) {
        if (wsi_debug_enabled()) fprintf(stderr, "[wsi] dma-buf import failed\n");
        return -1;
    }

    w->glGenTextures(1, &b->tex);
    w->glBindTexture(0x0DE1, b->tex);
    w->glEGLImageTargetTexture2DOES(0x0DE1, b->img);
    w->glBindTexture(0x0DE1, 0);
    w->glGenFramebuffers(1, &b->fbo);
    w->glBindFramebuffer(0x8D40, b->fbo);
    w->glFramebufferTexture2D(0x8D40, 0x8CE0, 0x0DE1, b->tex, 0);
    if (w->glCheckFramebufferStatus(0x8D40) != 0x8CD5) {
        if (wsi_debug_enabled()) fprintf(stderr, "[wsi] Framebuffer incomplete\n");
        return -1;
    }

    int dfd = dup(b->dfd);                                  /* xcb takes ownership of the fd */
    if (dfd < 0) return -1;                                 /* FIX D */
    xcb_pixmap_t pix = xcb_generate_id(w->c);
    xcb_generic_error_t* err = xcb_request_check(w->c,
        xcb_dri3_pixmap_from_buffer_checked(w->c, pix, w->win, (uint32_t)b->size, w->w, w->h,
                                            (uint16_t)b->stride, 24, 32, dfd));
    if (err) {
        if (wsi_debug_enabled()) fprintf(stderr, "[wsi] pixmap_from_buffer failed, err=%u\n", err->error_code);
        free(err);
        return -1;                                          /* FIX C: b->pix stays 0 */
    }
    b->pix = pix;
    return 0;
}

static inline void wsi_x11__buf_destroy(wsi_x11* w, wsi_x11_buf* b) {
    if (b->pix) xcb_free_pixmap(w->c, b->pix);
    if (b->fbo) w->glDeleteFramebuffers(1, &b->fbo);
    if (b->tex) w->glDeleteTextures(1, &b->tex);
    if (b->img) w->rmimg(w->dpy, b->img);
    if (b->ahb) w->ahb_rel(b->ahb);
    memset(b, 0, sizeof *b);
}

static inline int wsi_x11__back_create(wsi_x11* w) {
    w->glGenTextures(1, &w->back_tex);
    w->glBindTexture(0x0DE1, w->back_tex);
    if (w->gles == 1) {                                      /* ES1: no glTexStorage2D */
        w->glTexParameteri(0x0DE1, 0x2801, 0x2600);          /* MIN_FILTER NEAREST */
        w->glTexParameteri(0x0DE1, 0x2800, 0x2600);          /* MAG_FILTER NEAREST */
        w->glTexImage2D(0x0DE1, 0, 0x1908, w->w, w->h, 0, 0x1908, 0x1401, NULL);
    } else w->glTexStorage2D(0x0DE1, 1, 0x8058 /*RGBA8*/, w->w, w->h);
    w->glBindTexture(0x0DE1, 0);
    w->glGenRenderbuffers(1, &w->back_rb);
    w->glBindRenderbuffer(0x8D41, w->back_rb);
    w->glRenderbufferStorage(0x8D41, 0x88F0 /*DEPTH24_STENCIL8*/, w->w, w->h);
    w->glGenFramebuffers(1, &w->back_fbo);
    w->glBindFramebuffer(0x8D40, w->back_fbo);
    w->glFramebufferTexture2D(0x8D40, 0x8CE0, 0x0DE1, w->back_tex, 0);
    if (w->gles == 1) {
        w->glFramebufferRenderbuffer(0x8D40, 0x8D00 /*DEPTH*/, 0x8D41, w->back_rb);
        w->glFramebufferRenderbuffer(0x8D40, 0x8D20 /*STENCIL*/, 0x8D41, w->back_rb);
    } else w->glFramebufferRenderbuffer(0x8D40, 0x821A /*DEPTH_STENCIL_ATTACHMENT*/, 0x8D41, w->back_rb);
    if (w->glCheckFramebufferStatus(0x8D40) == 0x8CD5) return 0;
    if (w->gles == 1) {                                      /* Fallback: depth 16 only */
        w->glFramebufferRenderbuffer(0x8D40, 0x8D20, 0x8D41, 0);
        w->glBindRenderbuffer(0x8D41, w->back_rb);
        w->glRenderbufferStorage(0x8D41, 0x81A5 /*DEPTH_COMPONENT16*/, w->w, w->h);
        w->glFramebufferRenderbuffer(0x8D40, 0x8D00, 0x8D41, w->back_rb);
        if (w->glCheckFramebufferStatus(0x8D40) == 0x8CD5) return 0;
    }
    return -1;
}

static inline void wsi_x11__back_destroy(wsi_x11* w) {
    if (w->back_fbo) w->glDeleteFramebuffers(1, &w->back_fbo);
    if (w->back_tex) w->glDeleteTextures(1, &w->back_tex);
    if (w->back_rb)  w->glDeleteRenderbuffers(1, &w->back_rb);
    w->back_fbo = w->back_tex = w->back_rb = 0;
}

/* WSI_VSYNC=1: Waits for previous Present completion before issuing next frame. */
static inline void wsi_x11__vsync_wait(wsi_x11* w) {
    int64_t deadline = wsi_x11_now_ns() + 100000000LL;
    for (;;) {
        wsi_x11__drain(w);
        if ((int32_t)(w->serial - w->last_complete) <= 0) {   /* Last Present confirmed */
            w->vs_timeouts = 0;
            return;
        }
        int64_t left = deadline - wsi_x11_now_ns();
        if (left <= 0) {
            w->last_complete = w->serial;
            if (++w->vs_timeouts >= 5) {
                if (wsi_debug_enabled()) {
                    fprintf(stderr, "[wsi] vsync: 5 consecutive timeouts without CompleteNotify, disabling\n");
                }
                w->vsync = 0;
            } else if (w->stats && wsi_debug_enabled()) {
                fprintf(stderr, "[wsi] vsync: timeout 100 ms (%d/5)\n", w->vs_timeouts);
            }
            return;
        }
        struct pollfd pf = { xcb_get_file_descriptor(w->c), POLLIN, 0 };
        poll(&pf, 1, (int)(left / 1000000) + 1);
    }
}

/* WSI_STATS=1: Print diagnostics once per second. */
static inline void wsi_x11__stats_tick(wsi_x11* w) {
    int64_t now = wsi_x11_now_ns();
    w->st_swaps++;
    if (!w->st_t0) { w->st_t0 = now; return; }
    if (now - w->st_t0 < 1000000000LL) return;
    double dt = (now - w->st_t0) / 1e9;
    if (wsi_debug_enabled()) {
        fprintf(stderr, "[wsi] %.0f swaps/s | complete: copy=%u flip=%u skip=%u subopt=%u | msc %.1f/s | forced %d\n",
                w->st_swaps / dt, w->st_cmp[0], w->st_cmp[1], w->st_cmp[2], w->st_cmp[3],
                w->st_msc_seen ? (double)(w->st_msc_last - w->st_msc0) / dt : 0.0, w->forced);
    }
    w->st_swaps = 0; memset(w->st_cmp, 0, sizeof w->st_cmp);
    w->st_msc0 = w->st_msc_last; w->st_t0 = now;
}

/* ES1: copy back_tex -> pool buffer with Y flip via OES_draw_texture */
static inline void wsi_x11__flip_es1(wsi_x11* w, wsi_x11_buf* b) {
    static const unsigned caps[8] = { 0x0C11 /*SCISSOR*/, 0x0B71 /*DEPTH*/, 0x0BE2 /*BLEND*/,
        0x0BC0 /*ALPHA_TEST*/, 0x0B90 /*STENCIL*/, 0x0B60 /*FOG*/, 0x0B50 /*LIGHTING*/, 0x0BF2 /*LOGIC_OP*/ };
    unsigned char was[8], en_tex[8] = { 0 }, cm[4];
    int vp[4], act = 0x84C0, tex = 0, env = 0x1E01, nu = 1, i;

    w->glGetIntegerv(0x0BA2, vp);                    /* VIEWPORT */
    w->glGetIntegerv(0x84E0, &act);                  /* ACTIVE_TEXTURE */
    w->glGetIntegerv(0x84E2, &nu);                   /* MAX_TEXTURE_UNITS */
    if (nu > 8) nu = 8;
    if (nu < 1) nu = 1;
    w->glGetBooleanv(0x0C23, cm);                    /* COLOR_WRITEMASK */
    for (i = 0; i < 8; i++) { was[i] = w->glIsEnabled(caps[i]); if (was[i]) w->glDisable(caps[i]); }
    w->glColorMask(1, 1, 1, 1);
    for (i = nu - 1; i >= 0; i--) {                  /* Unit 0 only */
        w->glActiveTexture(0x84C0 + i);
        en_tex[i] = w->glIsEnabled(0x0DE1);
        if (i && en_tex[i]) w->glDisable(0x0DE1);
    }
    w->glGetIntegerv(0x8069, &tex);                  /* TEXTURE_BINDING_2D (unit 0) */
    w->glGetTexEnviv(0x2300, 0x2200, &env);          /* TEXTURE_ENV_MODE */
    w->glTexEnvi(0x2300, 0x2200, 0x1E01);            /* REPLACE */
    w->glEnable(0x0DE1);
    w->glBindTexture(0x0DE1, w->back_tex);
    { const int crop[4] = { 0, w->h, w->w, -w->h };
      w->glTexParameteriv(0x0DE1, 0x8B9D /*TEXTURE_CROP_RECT_OES*/, crop); }
    w->glBindFramebuffer(0x8D40, b->fbo);
    w->glViewport(0, 0, w->w, w->h);
    w->glDrawTexiOES(0, 0, 0, w->w, w->h);

    w->glBindTexture(0x0DE1, tex);
    if (!en_tex[0]) w->glDisable(0x0DE1);
    w->glTexEnvi(0x2300, 0x2200, env);
    for (i = 1; i < nu; i++) if (en_tex[i]) { w->glActiveTexture(0x84C0 + i); w->glEnable(0x0DE1); }
    w->glActiveTexture(act);
    w->glColorMask(cm[0], cm[1], cm[2], cm[3]);
    for (i = 0; i < 8; i++) if (was[i]) w->glEnable(caps[i]);
    w->glViewport(vp[0], vp[1], vp[2], vp[3]);
}

/* ---------- API ---------- */

static inline int wsi_x11_acquire(wsi_x11* w) {
    int pick = -1;
    for (;;) {
        wsi_x11__drain(w);
        for (int k = 1; k <= w->nbuf && pick < 0; k++) {
            int i = (w->cur + k) % w->nbuf;
            if (w->buf[i].state == WSI_FREE) pick = i;
        }
        if (pick >= 0) break;
        struct pollfd p = { xcb_get_file_descriptor(w->c), POLLIN, 0 };
        if (poll(&p, 1, 200) == 0) {
            /* FIX E: with a shared Xlib connection, another thread may already have read the
             * IDLE_NOTIFY into xcb's queue; look again before reclaiming anything by force. */
            wsi_x11__drain(w);
            int any_free = 0;
            for (int i = 0; i < w->nbuf; i++) if (w->buf[i].state == WSI_FREE) any_free = 1;
            if (!any_free) {                         /* No idle buffer: reclaim oldest */
                int old = -1;
                for (int i = 0; i < w->nbuf; i++)
                    if (w->buf[i].state == WSI_BUSY && (old < 0 || w->buf[i].serial < w->buf[old].serial)) old = i;
                if (old >= 0) { w->buf[old].state = WSI_FREE; w->forced++; }
            }
        }
    }
    w->cur = pick;
    w->buf[pick].state = WSI_ACQ;
    w->glBindFramebuffer(0x8D40, w->blit ? w->back_fbo : w->buf[pick].fbo);
    return 0;
}

static inline int wsi_x11_swap(wsi_x11* w) {
    wsi_x11_buf* b = &w->buf[w->cur];
    if (w->blit && w->gles == 1) {
        wsi_x11__flip_es1(w, b);
    } else if (w->blit) {
        unsigned char sc = w->glIsEnabled(0x0C11);           /* SCISSOR_TEST */
        if (sc) w->glDisable(0x0C11);
        w->glBindFramebuffer(0x8CA8, w->back_fbo);            /* READ */
        w->glBindFramebuffer(0x8CA9, b->fbo);                 /* DRAW */
        w->glBlitFramebuffer(0, 0, w->w, w->h, 0, w->h, w->w, 0, 0x4000, 0x2600 /*NEAREST*/);
        if (sc) w->glEnable(0x0C11);
    }
    wsi_x11__fence(w);
    wsi_x11__pace(w);
    if (w->vsync) wsi_x11__vsync_wait(w);
    b->serial = ++w->serial;
    b->state = WSI_BUSY;
    xcb_present_pixmap(w->c, w->win, b->pix, b->serial, 0, 0, 0, 0, 0, 0, 0,
                       ((w->flags & WSI_X11_ASYNC) && (w->pcaps & XCB_PRESENT_CAPABILITY_ASYNC))
                           ? XCB_PRESENT_OPTION_ASYNC : XCB_PRESENT_OPTION_NONE,
                       0, 0, 0, 0, NULL);
    xcb_flush(w->c);
    if (w->stats) wsi_x11__stats_tick(w);
    return wsi_x11_acquire(w);
}

static inline void* wsi_x11_getproc(wsi_x11* w, const char* n) { return wsi_x11__sym(w, n); }

static inline void wsi_x11_destroy(wsi_x11* w) {
    if (!w) return;
    if (w->glFinish) w->glFinish();
    for (int i = 0; i < w->nbuf; i++) wsi_x11__buf_destroy(w, &w->buf[i]);
    wsi_x11__back_destroy(w);
    if (w->se) xcb_unregister_for_special_event(w->c, w->se);
    xcb_flush(w->c);
    free(w);
}

static inline int wsi_x11_resize(wsi_x11* w, int width, int height) {
    w->glFinish();
    for (int i = 0; i < w->nbuf; i++) wsi_x11__buf_destroy(w, &w->buf[i]);
    wsi_x11__back_destroy(w);
    w->w = width; w->h = height;
    if (w->blit && wsi_x11__back_create(w)) return -1;
    for (int i = 0; i < w->nbuf; i++) if (wsi_x11__buf_create(w, &w->buf[i])) return -1;
    w->cur = w->nbuf - 1;
    return wsi_x11_acquire(w);
}

static inline wsi_x11* wsi_x11_create_ex(xcb_connection_t* c, xcb_window_t win,
                                         int width, int height, int nbuf, int flags,
                                         void* egl_in, void* dpy_in, int gles_in) {
    const int debug = wsi_debug_enabled();
    wsi_x11* w = calloc(1, sizeof *w);
    if (!w) return NULL;
    w->c = c; w->win = win; w->w = width; w->h = height; w->flags = flags;
    w->nbuf = nbuf < 2 ? 2 : (nbuf > WSI_X11_MAX_BUF ? WSI_X11_MAX_BUF : nbuf);

    void* nw = dlopen("libnativewindow.so", RTLD_NOW);
    if (!nw) nw = dlopen("/system/lib64/libnativewindow.so", RTLD_NOW);     /* FIX G */
    if (!nw) {
        if (debug) fprintf(stderr, "[wsi] Failed to open libnativewindow.so\n");
        goto fail;
    }
    *(void**)&w->ahb_alloc = dlsym(nw, "AHardwareBuffer_allocate");
    *(void**)&w->ahb_desc  = dlsym(nw, "AHardwareBuffer_describe");
    *(void**)&w->ahb_gnh   = dlsym(nw, "AHardwareBuffer_getNativeHandle");
    *(void**)&w->ahb_rel   = dlsym(nw, "AHardwareBuffer_release");
    if (!w->ahb_alloc || !w->ahb_desc || !w->ahb_gnh || !w->ahb_rel) goto fail;

    if (egl_in) {
        w->egl = egl_in; w->dpy = dpy_in; w->gles = gles_in;
        *(void**)&w->gpa = dlsym(w->egl, "eglGetProcAddress");
        goto have_ctx;
    }
    w->egl = mali_sphal_load(MALI_SPHAL_DEFAULT);
    if (!w->egl) goto fail;
    if (debug) fprintf(stderr, "[wsi] Mali driver loaded via SPHAL\n");

    void* (*gd)(void*)            = dlsym(w->egl, "eglGetDisplay");
    unsigned (*ini)(void*, int*, int*) = dlsym(w->egl, "eglInitialize");
    unsigned (*bapi)(unsigned)    = dlsym(w->egl, "eglBindAPI");
    unsigned (*chs)(void*, const int*, void**, int, int*) = dlsym(w->egl, "eglChooseConfig");
    void* (*mkc)(void*, void*, void*, const int*) = dlsym(w->egl, "eglCreateContext");
    unsigned (*cur)(void*, void*, void*, void*)   = dlsym(w->egl, "eglMakeCurrent");
    *(void**)&w->gpa = dlsym(w->egl, "eglGetProcAddress");
    if (!gd || !ini || !bapi || !chs || !mkc || !cur) goto fail;

    w->dpy = gd(NULL);
    int a, b;
    if (!ini(w->dpy, &a, &b)) goto fail;
    bapi(0x30A0 /*OPENGL_ES_API*/);

    void* cfg = NULL; int ncfg = 0;
    for (int ver = 3; ver >= 2 && !w->ctx; ver--) {
        const int ca[] = { 0x3040, ver == 3 ? 0x40 : 0x4, 0x3038 };
        const int cx[] = { 0x3098, ver, 0x3038 };
        if (chs(w->dpy, ca, &cfg, 1, &ncfg) && ncfg) {
            w->ctx = mkc(w->dpy, cfg, NULL, cx);
            if (w->ctx) w->gles = ver;
        }
    }
    if (!w->ctx) {
        if (debug) fprintf(stderr, "[wsi] eglCreateContext failed\n");
        goto fail;
    }
    if (!cur(w->dpy, NULL, NULL, w->ctx)) {
        if (debug) fprintf(stderr, "[wsi] MakeCurrent surfaceless failed\n");
        goto fail;
    }

have_ctx:
    *(void**)&w->mkimg    = wsi_x11__sym(w, "eglCreateImageKHR");
    *(void**)&w->rmimg    = wsi_x11__sym(w, "eglDestroyImageKHR");
    *(void**)&w->mksync   = wsi_x11__sym(w, "eglCreateSyncKHR");
    *(void**)&w->rmsync   = wsi_x11__sym(w, "eglDestroySyncKHR");
    *(void**)&w->dupfence = wsi_x11__sym(w, "eglDupNativeFenceFDANDROID");

#define WSI_GL(n) *(void**)&w->n = wsi_x11__glsym(w, #n); if (!w->n) { if (debug) fprintf(stderr, "[wsi] Missing symbol: " #n "\n"); goto fail; }
    WSI_GL(glGenTextures) WSI_GL(glDeleteTextures) WSI_GL(glBindTexture)
    WSI_GL(glGenFramebuffers) WSI_GL(glDeleteFramebuffers) WSI_GL(glBindFramebuffer)
    WSI_GL(glFramebufferTexture2D) WSI_GL(glCheckFramebufferStatus)
    WSI_GL(glEGLImageTargetTexture2DOES)
    WSI_GL(glIsEnabled) WSI_GL(glEnable) WSI_GL(glDisable) WSI_GL(glFlush) WSI_GL(glFinish)
#undef WSI_GL
    if (!w->mkimg || !w->rmimg) goto fail;

    w->blit = 0;                       /* "blit" = back buffer + flip Y */
    if (!(flags & WSI_X11_DIRECT)) {
        w->glGenRenderbuffers    = wsi_x11__glsym(w, "glGenRenderbuffers");
        w->glDeleteRenderbuffers = wsi_x11__glsym(w, "glDeleteRenderbuffers");
        w->glBindRenderbuffer    = wsi_x11__glsym(w, "glBindRenderbuffer");
        w->glRenderbufferStorage = wsi_x11__glsym(w, "glRenderbufferStorage");
        w->glFramebufferRenderbuffer = wsi_x11__glsym(w, "glFramebufferRenderbuffer");
        int have_rb = w->glGenRenderbuffers && w->glDeleteRenderbuffers && w->glBindRenderbuffer &&
                      w->glRenderbufferStorage && w->glFramebufferRenderbuffer;
        if (w->gles >= 3) {
            w->glTexStorage2D   = wsi_x11__glsym(w, "glTexStorage2D");
            w->glBlitFramebuffer = wsi_x11__glsym(w, "glBlitFramebuffer");
            w->blit = have_rb && w->glTexStorage2D && w->glBlitFramebuffer;
        } else if (w->gles == 1) {
            w->glTexImage2D     = wsi_x11__glsym(w, "glTexImage2D");
            w->glTexParameteri  = wsi_x11__glsym(w, "glTexParameteri");
            w->glTexParameteriv = wsi_x11__glsym(w, "glTexParameteriv");
            w->glDrawTexiOES    = wsi_x11__glsym(w, "glDrawTexiOES");
            w->glGetIntegerv    = wsi_x11__glsym(w, "glGetIntegerv");
            w->glGetBooleanv    = wsi_x11__glsym(w, "glGetBooleanv");
            w->glActiveTexture  = wsi_x11__glsym(w, "glActiveTexture");
            w->glGetTexEnviv    = wsi_x11__glsym(w, "glGetTexEnviv");
            w->glTexEnvi        = wsi_x11__glsym(w, "glTexEnvi");
            w->glColorMask      = wsi_x11__glsym(w, "glColorMask");
            w->glViewport       = wsi_x11__glsym(w, "glViewport");
            w->blit = have_rb && w->glTexImage2D && w->glTexParameteri && w->glTexParameteriv &&
                      w->glDrawTexiOES && w->glGetIntegerv && w->glGetBooleanv && w->glActiveTexture &&
                      w->glGetTexEnviv && w->glTexEnvi && w->glColorMask && w->glViewport;
        }
        /* NOTE: GLES 2 has no flip path here -> falls to direct mode with y_inverted=1
         * (image appears upside-down). The shim upgrades ES2 contexts to ES3 to avoid this. */
    }
    w->y_inverted = !w->blit;
    if (debug) {
        fprintf(stderr, "[wsi] GLES %d, %s\n", w->gles,
                w->blit ? (w->gles == 1 ? "flip Y via OES_draw_texture" : "blit flip Y")
                        : "direct (y_inverted=1)");
    }

    /* FIX H: X server must have DRI3 and Present */
    {
        const xcb_query_extension_reply_t* e3 = xcb_get_extension_data(c, &xcb_dri3_id);
        const xcb_query_extension_reply_t* ep = xcb_get_extension_data(c, &xcb_present_id);
        if (!e3 || !e3->present || !ep || !ep->present) {
            if (debug) fprintf(stderr, "[wsi] DRI3/Present extension missing on the X server\n");
            goto fail;
        }
    }

    /* X: DRI3 + Present */
    if (getenv("WSI_STATS")) {
        w->stats = 1;
        xcb_dri3_query_version_reply_t* dv =
            xcb_dri3_query_version_reply(c, xcb_dri3_query_version(c, 1, 4), NULL);
        xcb_present_query_version_reply_t* pv =
            xcb_present_query_version_reply(c, xcb_present_query_version(c, 1, 4), NULL);
        if (debug) {
            fprintf(stderr, "[wsi] Server: DRI3 %u.%u, Present %u.%u (requested 1.4)\n",
                    dv ? dv->major_version : 0, dv ? dv->minor_version : 0,
                    pv ? pv->major_version : 0, pv ? pv->minor_version : 0);
        }
        free(dv); free(pv);
        xcb_present_query_capabilities_reply_t* cp =
            xcb_present_query_capabilities_reply(c, xcb_present_query_capabilities(c, win), NULL);
        if (cp) {
            w->pcaps = cp->capabilities;
            if (debug) {
                fprintf(stderr, "[wsi] Present capabilities 0x%x: async=%d fence=%d ust=%d may_tear=%d syncobj=%d\n",
                        cp->capabilities,
                        !!(cp->capabilities & XCB_PRESENT_CAPABILITY_ASYNC),
                        !!(cp->capabilities & XCB_PRESENT_CAPABILITY_FENCE),
                        !!(cp->capabilities & XCB_PRESENT_CAPABILITY_UST),
                        !!(cp->capabilities & XCB_PRESENT_CAPABILITY_ASYNC_MAY_TEAR),
                        !!(cp->capabilities & XCB_PRESENT_CAPABILITY_SYNCOBJ));
            }
            free(cp);
        }
    } else {
        free(xcb_dri3_query_version_reply(c, xcb_dri3_query_version(c, 1, 0), NULL));
        free(xcb_present_query_version_reply(c, xcb_present_query_version(c, 1, 2), NULL));
        xcb_present_query_capabilities_reply_t* cp =
            xcb_present_query_capabilities_reply(c, xcb_present_query_capabilities(c, win), NULL);
        if (cp) { w->pcaps = cp->capabilities; free(cp); }
    }
    w->eid = xcb_generate_id(c);
    w->se = xcb_register_for_special_xge(c, &xcb_present_id, w->eid, NULL);
    w->vsync = getenv("WSI_VSYNC") != NULL;
    xcb_present_select_input(c, w->eid, win, XCB_PRESENT_EVENT_MASK_IDLE_NOTIFY |
                             ((w->stats || w->vsync) ? XCB_PRESENT_EVENT_MASK_COMPLETE_NOTIFY : 0));

    if (w->blit && wsi_x11__back_create(w)) {
        if (debug) fprintf(stderr, "[wsi] Back FBO creation failed\n");
        goto fail;
    }
    for (int i = 0; i < w->nbuf; i++) {
        if (wsi_x11__buf_create(w, &w->buf[i])) goto fail;
    }
    w->cur = w->nbuf - 1;
    wsi_x11_acquire(w);
    return w;
fail:
    if (debug) fprintf(stderr, "[wsi] WSI creation failed\n");
    wsi_x11_destroy(w);
    return NULL;
}

static inline wsi_x11* wsi_x11_create(xcb_connection_t* c, xcb_window_t win,
                                      int width, int height, int nbuf, int flags) {
    return wsi_x11_create_ex(c, win, width, height, nbuf, flags, NULL, NULL, 0);
}

static inline unsigned wsi_x11_default_fbo(wsi_x11* w) {
    return w->blit ? w->back_fbo : w->buf[w->cur].fbo;
}
static inline void wsi_x11_bind(wsi_x11* w) {
    w->glBindFramebuffer(0x8D40, wsi_x11_default_fbo(w));
}

static inline void wsi_x11_abandon(wsi_x11* w) {
    if (!w) return;
    for (int i = 0; i < w->nbuf; i++) {
        wsi_x11_buf* b = &w->buf[i];
        if (b->pix) xcb_free_pixmap(w->c, b->pix);
        if (b->img) w->rmimg(w->dpy, b->img);
        if (b->ahb) w->ahb_rel(b->ahb);
    }
    if (w->se) xcb_unregister_for_special_event(w->c, w->se);
    xcb_flush(w->c);
    free(w);
}
