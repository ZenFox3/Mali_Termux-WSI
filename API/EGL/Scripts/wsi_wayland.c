/*
 * wsi_wayland.c - Wayland transport for egl_x11_shim.
 *
 * - private wl_event_queue (never touches the app's default queue)
 * - zwp_linux_dmabuf_v1 -> wl_buffer (async create, no fatal protocol errors)
 * - wl_buffer.release -> callback into the shim (like PRESENT_IDLE_NOTIFY)
 * - wl_surface.frame for vsync, immediate commit for interval 0
 * - reads wl_egl_window from the REAL libwayland-egl (does not export wl_egl_window_*)
 *
 * Build:
 *   wayland-scanner client-header $P linux-dmabuf-client.h
 *   wayland-scanner private-code  $P linux-dmabuf-protocol.c
 *   (see WAYLAND_PATCH.md for the clang line)
 */
#define _GNU_SOURCE
#define WL_EGL_PLATFORM 1
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>
#include <wayland-egl-backend.h>
#include "linux-dmabuf-client.h"
#include "wsi_wayland.h"

#define MAXB 8
#define MAXF 48
#define MOD_LINEAR  0ULL
#define MOD_INVALID 0x00ffffffffffffffULL
#define FRAME_WAIT_MS 100

#define WDBG(...) do { if (getenv("EGL_DEBUG")) fprintf(stderr, __VA_ARGS__); } while (0)

void wl_wsi_set_dead(struct wl_wsi* w);
struct fmt_ent { uint32_t fourcc; int linear, implicit; };
struct buf_ud  { struct wl_wsi* w; int idx; int state; /* 0 pending, 1 ok, 2 failed */ };

struct wl_wsi {
    struct wl_display* dpy;
    struct wl_event_queue* q;
    struct wl_display* dw;            /* wrapper on q */
    struct wl_surface* sw;            /* wrapper on q */
    struct wl_registry* reg;
    struct zwp_linux_dmabuf_v1* dmabuf;
    uint32_t dmabuf_ver;
    struct fmt_ent fmts[MAXF];
    int nf;
    struct wl_buffer* buf[MAXB];
    struct buf_ud ud[MAXB];
    struct wl_callback* frame;
    int frame_pending;
    pthread_mutex_t mu;
    pthread_cond_t cv;
    int dead, stop, th_started, mu_ok, threaded;
    int wake[2];
    pthread_t th;
    wl_wsi_release_fn rel;
    void* user;
};

/* ---------------- display / wl_egl_window ---------------- */

int wl_wsi_is_wl_display(void* p) {
    return p && *(const void* const*)p == (const void*)&wl_display_interface;
}

void* wl_wsi_connect(void) { return wl_display_connect(NULL); }
void  wl_wsi_disconnect(void* d) { if (d) wl_display_disconnect(d); }

static void (*g_on_resize)(void*, int, int);
static void (*g_on_destroy)(void*);

static void tramp_resize(struct wl_egl_window* win, void* priv) {
    if (g_on_resize) g_on_resize(priv, win->width, win->height);
}
static void tramp_destroy(void* priv) {
    if (g_on_destroy) g_on_destroy(priv);
}

int wl_wsi_window_info(void* p, int* w, int* h, void** surf) {
    struct wl_egl_window* win = p;
    if (!win || win->version < 3 || !win->surface) return -1;
    if (w) *w = win->width;
    if (h) *h = win->height;
    if (surf) *surf = win->surface;
    return 0;
}

void wl_wsi_window_hook(void* p, void* user,
                        void (*on_resize)(void*, int, int),
                        void (*on_destroy)(void*)) {
    struct wl_egl_window* win = p;
    if (!win) return;
    g_on_resize = on_resize;
    g_on_destroy = on_destroy;
    win->driver_private = user;
    win->resize_callback = tramp_resize;
    win->destroy_window_callback = tramp_destroy;
}

void wl_wsi_window_unhook(void* p, void* user) {
    struct wl_egl_window* win = p;
    if (!win || win->driver_private != user) return;
    win->resize_callback = NULL;
    win->destroy_window_callback = NULL;
    win->driver_private = NULL;
}

/* ---------------- registry / dmabuf ---------------- */

static struct fmt_ent* fmt_get(struct wl_wsi* w, uint32_t f) {
    for (int i = 0; i < w->nf; i++) if (w->fmts[i].fourcc == f) return &w->fmts[i];
    if (w->nf >= MAXF) return NULL;
    w->fmts[w->nf].fourcc = f;
    return &w->fmts[w->nf++];
}

static void dmabuf_format(void* d, struct zwp_linux_dmabuf_v1* z, uint32_t f) {
    (void)z;
    struct fmt_ent* e = fmt_get(d, f);
    if (e) e->implicit = 1;                    /* v1/v2: implicit modifier */
}
static void dmabuf_modifier(void* d, struct zwp_linux_dmabuf_v1* z, uint32_t f,
                            uint32_t hi, uint32_t lo) {
    (void)z;
    uint64_t m = ((uint64_t)hi << 32) | lo;
    struct fmt_ent* e = fmt_get(d, f);
    if (!e) return;
    if (m == MOD_LINEAR) e->linear = 1;
    else if (m == MOD_INVALID) e->implicit = 1;
}
static const struct zwp_linux_dmabuf_v1_listener dmabuf_l = { dmabuf_format, dmabuf_modifier };

static void reg_global(void* d, struct wl_registry* r, uint32_t name,
                       const char* iface, uint32_t ver) {
    struct wl_wsi* w = d;
    if (!strcmp(iface, zwp_linux_dmabuf_v1_interface.name) && !w->dmabuf) {
        uint32_t v = ver < 3 ? ver : 3;
        w->dmabuf = wl_registry_bind(r, name, &zwp_linux_dmabuf_v1_interface, v);
        w->dmabuf_ver = v;
        zwp_linux_dmabuf_v1_add_listener(w->dmabuf, &dmabuf_l, w);
    }
}
static void reg_remove(void* d, struct wl_registry* r, uint32_t n) { (void)d; (void)r; (void)n; }
static const struct wl_registry_listener reg_l = { reg_global, reg_remove };

/* ---------------- buffers ---------------- */

static void buf_release(void* data, struct wl_buffer* b) {
    (void)b;
    struct buf_ud* u = data;
    if (u->w->rel) u->w->rel(u->w->user, u->idx);
}
static const struct wl_buffer_listener buf_l = { buf_release };

static void params_created(void* data, struct zwp_linux_buffer_params_v1* p, struct wl_buffer* b) {
    (void)p;
    struct buf_ud* u = data;
    u->w->buf[u->idx] = b;
    wl_buffer_add_listener(b, &buf_l, u);
    u->state = 1;
}
static void params_failed(void* data, struct zwp_linux_buffer_params_v1* p) {
    (void)p;
    ((struct buf_ud*)data)->state = 2;
}
static const struct zwp_linux_buffer_params_v1_listener params_l = { params_created, params_failed };

int wl_wsi_format_ok(struct wl_wsi* w, uint32_t fourcc) {
    for (int i = 0; i < w->nf; i++)
        if (w->fmts[i].fourcc == fourcc) return w->fmts[i].linear || w->fmts[i].implicit;
    return 0;
}

int wl_wsi_add_buffer(struct wl_wsi* w, int slot, int dfd, int width, int height,
                      uint32_t stride, uint32_t fourcc) {
    if (slot < 0 || slot >= MAXB || !w->dmabuf) { if (dfd >= 0) close(dfd); return -1; }
    struct fmt_ent* e = NULL;
    for (int i = 0; i < w->nf; i++) if (w->fmts[i].fourcc == fourcc) e = &w->fmts[i];
    uint64_t mod = (e && e->linear) ? MOD_LINEAR : MOD_INVALID;

    if (w->buf[slot]) { wl_buffer_destroy(w->buf[slot]); w->buf[slot] = NULL; }
    w->ud[slot].w = w; w->ud[slot].idx = slot; w->ud[slot].state = 0;

    struct zwp_linux_buffer_params_v1* p = zwp_linux_dmabuf_v1_create_params(w->dmabuf);
    zwp_linux_buffer_params_v1_add(p, dfd, 0, 0, stride,
                                   (uint32_t)(mod >> 32), (uint32_t)(mod & 0xffffffffu));
    zwp_linux_buffer_params_v1_add_listener(p, &params_l, &w->ud[slot]);
    zwp_linux_buffer_params_v1_create(p, width, height, fourcc, 0);
    close(dfd);                                  /* libwayland already queued a dup */

    int rt = wl_display_roundtrip_queue(w->dpy, w->q);   /* created/failed arrive before this */
    zwp_linux_buffer_params_v1_destroy(p);
    if (rt < 0 || w->ud[slot].state != 1 || !w->buf[slot]) {
        WDBG("[shim-wl] dmabuf buffer rejected (slot %d, mod 0x%llx)\n", slot, (unsigned long long)mod);
        return -1;
    }
    return 0;
}

void wl_wsi_free_buffer(struct wl_wsi* w, int slot) {
    if (!w || slot < 0 || slot >= MAXB) return;
    struct wl_buffer* b = w->buf[slot];
    w->buf[slot] = NULL;
    if (b) wl_buffer_destroy(b);
}

/* ---------------- present ---------------- */

static void frame_done(void* d, struct wl_callback* cb, uint32_t t) {
    (void)t;
    struct wl_wsi* w = d;
    pthread_mutex_lock(&w->mu);
    w->frame_pending = 0;
    if (w->frame == cb) w->frame = NULL;
    pthread_cond_broadcast(&w->cv);
    pthread_mutex_unlock(&w->mu);
    wl_callback_destroy(cb);
}
static const struct wl_callback_listener frame_l = { frame_done };

void wl_wsi_present(struct wl_wsi* w, int slot, int vsync) {
    pthread_mutex_lock(&w->mu);
    if (w->stop) { pthread_mutex_unlock(&w->mu); return; }
    if (w->dead || slot < 0 || slot >= MAXB || !w->buf[slot]) {
        pthread_mutex_unlock(&w->mu);
        if (w->rel) w->rel(w->user, slot);        /* do not leave the slot busy */
        return;
    }
    if (vsync && w->frame_pending) {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        ts.tv_nsec += FRAME_WAIT_MS * 1000000L;
        ts.tv_sec += ts.tv_nsec / 1000000000L;
        ts.tv_nsec %= 1000000000L;
        while (w->frame_pending && !w->dead && !w->stop)
            if (pthread_cond_timedwait(&w->cv, &w->mu, &ts) == ETIMEDOUT) break;
        if (w->stop) { pthread_mutex_unlock(&w->mu); return; }
        if (w->dead) {
            pthread_mutex_unlock(&w->mu);
            if (w->rel) w->rel(w->user, slot);
            return;
        }
    }
    wl_surface_attach(w->sw, w->buf[slot], 0, 0);
    if (wl_proxy_get_version((struct wl_proxy*)w->sw) >= 4)
        wl_surface_damage_buffer(w->sw, 0, 0, INT32_MAX, INT32_MAX);
    else
        wl_surface_damage(w->sw, 0, 0, INT32_MAX, INT32_MAX);
    if (vsync && !w->frame_pending) {
        struct wl_callback* cb = wl_surface_frame(w->sw);
        wl_callback_add_listener(cb, &frame_l, w);
        w->frame = cb;
        w->frame_pending = 1;
    }
    wl_surface_commit(w->sw);
    pthread_mutex_unlock(&w->mu);

    if (wl_display_flush(w->dpy) < 0 && errno == EAGAIN) {
        char c = 1; (void)!write(w->wake[1], &c, 1);   /* thread will poll POLLOUT */
    }
}

/* ---------------- event thread ---------------- */

static void* ev_thread(void* arg) {
    struct wl_wsi* w = arg;
    struct pollfd pf[2];
    pf[0].fd = wl_display_get_fd(w->dpy);
    pf[1].fd = w->wake[0]; pf[1].events = POLLIN;

    while (!__atomic_load_n(&w->stop, __ATOMIC_ACQUIRE)) {
        while (wl_display_prepare_read_queue(w->dpy, w->q) != 0)
            if (wl_display_dispatch_queue_pending(w->dpy, w->q) < 0) goto dead;

        int fl = wl_display_flush(w->dpy);
        if (fl < 0 && errno != EAGAIN) { wl_display_cancel_read(w->dpy); goto dead; }
        pf[0].events = POLLIN | (fl < 0 ? POLLOUT : 0);
        pf[0].revents = pf[1].revents = 0;

        int r = poll(pf, 2, 500);
        if (r < 0 && errno != EINTR) { wl_display_cancel_read(w->dpy); goto dead; }

        if (pf[1].revents & POLLIN) { char b[16]; while (read(w->wake[0], b, sizeof b) > 0) {} }

        if (r > 0 && (pf[0].revents & POLLIN)) {
            if (wl_display_read_events(w->dpy) < 0) goto dead;
        } else {
            wl_display_cancel_read(w->dpy);
        }
        if (wl_display_dispatch_queue_pending(w->dpy, w->q) < 0) goto dead;
    }
    return NULL;
dead:
    WDBG("[shim-wl] connection error, presenting disabled\n");
    pthread_mutex_lock(&w->mu);
    w->dead = 1;
    pthread_cond_broadcast(&w->cv);
    pthread_mutex_unlock(&w->mu);
    return NULL;
}

/* Default: NO background thread. The socket is read only by the app thread, inside
 * eglSwapBuffers (like Mesa), so we never steal wake-ups from the app's own poll().
 * SHIM_WL_THREAD=1 restores the old event-thread behaviour (for A/B testing). */
int wl_wsi_start(struct wl_wsi* w) {
    const char* e = getenv("SHIM_WL_THREAD");
    if (!(e && atoi(e))) return 0;
    if (w->th_started) return 0;
    if (pthread_create(&w->th, NULL, ev_thread, w)) return -1;
    w->th_started = 1;
    w->threaded = 1;
    return 0;
}

static long mono_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

/* Reads/dispatches events of OUR queue on the calling thread (prepare_read protocol).
 * Runs the release / frame-done callbacks. Returns events dispatched, <0 on error. */
int wl_wsi_pump(struct wl_wsi* w, int timeout_ms) {
    if (!w || __atomic_load_n(&w->stop, __ATOMIC_ACQUIRE)) return -1;
    if (w->threaded) { if (timeout_ms > 0) usleep(1000); return 0; }

    int n = wl_display_dispatch_queue_pending(w->dpy, w->q);
    if (n < 0) goto err;
    if (n > 0) return n;

    while (wl_display_prepare_read_queue(w->dpy, w->q) != 0)
        if (wl_display_dispatch_queue_pending(w->dpy, w->q) < 0) goto err;

    int fl = wl_display_flush(w->dpy);
    if (fl < 0 && errno != EAGAIN) { wl_display_cancel_read(w->dpy); goto err; }

    struct pollfd pf = { .fd = wl_display_get_fd(w->dpy),
                         .events = POLLIN | (fl < 0 ? POLLOUT : 0) };
    int r;
    do { r = poll(&pf, 1, timeout_ms); } while (r < 0 && errno == EINTR);

    if (r > 0 && (pf.revents & POLLIN)) {
        if (wl_display_read_events(w->dpy) < 0) goto err;
    } else {
        wl_display_cancel_read(w->dpy);
    }
    n = wl_display_dispatch_queue_pending(w->dpy, w->q);
    if (n < 0) goto err;
    return n;
err:
    WDBG("[shim-wl] connection error, presenting disabled\n");
    wl_wsi_set_dead(w);
    return -1;
}

/* Blocks (pumping) until the previous wl_surface.frame is done, up to timeout_ms.
 * 0 = done / nothing pending, -1 = timeout. */
int wl_wsi_wait_frame(struct wl_wsi* w, int timeout_ms) {
    long end = mono_ms() + timeout_ms;
    for (;;) {
        pthread_mutex_lock(&w->mu);
        int pend = w->frame_pending && !w->dead && !w->stop;
        pthread_mutex_unlock(&w->mu);
        if (!pend) return 0;
        long left = end - mono_ms();
        if (left <= 0) return -1;
        wl_wsi_pump(w, left < 10 ? (int)left : 10);
    }
}

void wl_wsi_set_dead(struct wl_wsi* w) {
    if (!w) return;
    pthread_mutex_lock(&w->mu);
    w->dead = 1;
    pthread_cond_broadcast(&w->cv);
    pthread_mutex_unlock(&w->mu);
}

void wl_wsi_stop(struct wl_wsi* w) {
    if (!w) return;
    pthread_mutex_lock(&w->mu);
    __atomic_store_n(&w->stop, 1, __ATOMIC_RELEASE);
    pthread_cond_broadcast(&w->cv);
    pthread_mutex_unlock(&w->mu);
    if (w->th_started) {
        char c = 1; (void)!write(w->wake[1], &c, 1);
        pthread_join(w->th, NULL);
        w->th_started = 0;
    }
}

/* ---------------- create / destroy ---------------- */

static void wsi_free(struct wl_wsi* w) {
    for (int i = 0; i < MAXB; i++) if (w->buf[i]) wl_buffer_destroy(w->buf[i]);
    if (w->frame) wl_callback_destroy(w->frame);
    if (w->sw) wl_proxy_wrapper_destroy(w->sw);
    if (w->dmabuf) zwp_linux_dmabuf_v1_destroy(w->dmabuf);
    if (w->reg) wl_registry_destroy(w->reg);
    if (w->dw) wl_proxy_wrapper_destroy(w->dw);
    if (w->q) { wl_display_flush(w->dpy); wl_event_queue_destroy(w->q); }
    if (w->wake[0] > 0) close(w->wake[0]);
    if (w->wake[1] > 0) close(w->wake[1]);
    if (w->mu_ok) { pthread_mutex_destroy(&w->mu); pthread_cond_destroy(&w->cv); }
    free(w);
}

void wl_wsi_destroy(struct wl_wsi* w) {
    if (!w) return;
    wl_wsi_stop(w);                    /* thread first: no events while destroying proxies */
    wsi_free(w);
}

struct wl_wsi* wl_wsi_create(void* dpy, void* surface, wl_wsi_release_fn rel, void* user) {
    if (!dpy || !surface) return NULL;
    struct wl_wsi* w = calloc(1, sizeof *w);
    if (!w) return NULL;
    w->dpy = dpy; w->rel = rel; w->user = user;
    w->wake[0] = w->wake[1] = -1;

    pthread_condattr_t ca;
    pthread_condattr_init(&ca);
    pthread_condattr_setclock(&ca, CLOCK_MONOTONIC);
    pthread_mutex_init(&w->mu, NULL);
    pthread_cond_init(&w->cv, &ca);
    pthread_condattr_destroy(&ca);
    w->mu_ok = 1;

    if (pipe2(w->wake, O_CLOEXEC | O_NONBLOCK)) { w->wake[0] = w->wake[1] = -1; goto fail; }

    w->q = wl_display_create_queue(w->dpy);
    if (!w->q) goto fail;
    w->dw = wl_proxy_create_wrapper(w->dpy);
    if (!w->dw) goto fail;
    wl_proxy_set_queue((struct wl_proxy*)w->dw, w->q);

    w->reg = wl_display_get_registry(w->dw);
    wl_registry_add_listener(w->reg, &reg_l, w);
    if (wl_display_roundtrip_queue(w->dpy, w->q) < 0) goto fail;   /* globals */
    if (!w->dmabuf) { WDBG("[shim-wl] zwp_linux_dmabuf_v1 missing\n"); goto fail; }
    if (wl_display_roundtrip_queue(w->dpy, w->q) < 0) goto fail;   /* format/modifier */

    w->sw = wl_proxy_create_wrapper(surface);
    if (!w->sw) goto fail;
    wl_proxy_set_queue((struct wl_proxy*)w->sw, w->q);

    WDBG("[shim-wl] dmabuf v%u, %d formats\n", w->dmabuf_ver, w->nf);
    return w;
fail:
    wsi_free(w);
    return NULL;
}
