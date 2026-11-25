/* shim_wayland.inc - included in egl_x11_shim.c right before present_worker().
 * Uses: shim_surf, G, SET_ERR, DBG, now_us, surf_use_async, KIND_WL (all defined above). */

static uint32_t swl_fourcc(shim_surf* s) {
    /* Content is ALWAYS B,G,R,A in memory (native BGRA, or RGBA + swizzle in the shader) */
    return (s->depth == 32 && !getenv("SHIM_OPAQUE")) ? 0x34325241u   /* ARGB8888 */
                                                     : 0x34325258u;   /* XRGB8888 */
}

/* wl_buffer.release: same job as drain_events() for PRESENT_IDLE_NOTIFY */
static void swl_on_release(void* user, int slot) {
    shim_surf* s = user;
    pthread_mutex_lock(&s->mu);
    if (slot >= 0 && slot < s->nbuf && s->slots[slot].busy) {
        s->st_idle += now_us() - s->t_pres[slot];
        s->st_idle_n++;
        s->slots[slot].busy = 0;
        if (s->inflight > 0) s->inflight--;
    }
    pthread_cond_broadcast(&s->cv_free);
    pthread_mutex_unlock(&s->mu);
}

/* wl_egl_window_resize() -> resize_callback */
static void swl_on_resize(void* user, int w, int h) {
    shim_surf* s = user;
    __atomic_store_n(&s->pend_w, w, __ATOMIC_RELAXED);
    __atomic_store_n(&s->pend_h, h, __ATOMIC_RELAXED);
}

/* wl_egl_window_destroy(): the wl_surface may die right after, stop touching it */
static void swl_on_destroy(void* user) {
    shim_surf* s = user;
    s->wlwin = NULL;
    if (s->wl) wl_wsi_set_dead(s->wl);
}

static int swl_setup(shim_surf* s) {
    int w = 0, h = 0;
    void* surf = NULL;
    if (!s->wlwin || wl_wsi_window_info(s->wlwin, &w, &h, &surf)) {
        DBG("[shim] wayland: invalid wl_egl_window (version < 3 or no surface)\n");
        return -1;
    }
    if (w > 0 && h > 0) { s->w = w; s->h = h; }     /* the app may have resized since creation */
    s->pend_w = s->pend_h = 0;
    s->wl = wl_wsi_create(G.wl_native, surf, swl_on_release, s);
    if (!s->wl) { DBG("[shim] wayland: wl_wsi_create failed (no linux-dmabuf?)\n"); return -1; }
    if (!wl_wsi_format_ok(s->wl, swl_fourcc(s))) {
        DBG("[shim] wayland: compositor does not accept fourcc 0x%x\n", swl_fourcc(s));
        wl_wsi_destroy(s->wl); s->wl = NULL;
        return -1;
    }
    wl_wsi_window_hook(s->wlwin, s, swl_on_resize, swl_on_destroy);
    DBG("[shim] Present: wl_buffer.release via private queue + event thread\n");
    return 0;
}

static void swl_teardown(shim_surf* s) {
    if (s->wlwin && s->wl) wl_wsi_window_unhook(s->wlwin, s);
    if (s->wl) { wl_wsi_destroy(s->wl); s->wl = NULL; }
}

static int swl_create_buf(shim_surf* s, int idx, int dfd, int w, int h, uint32_t stride) {
    return wl_wsi_add_buffer(s->wl, idx, dfd, w, h, stride, swl_fourcc(s));
}

/* Like acquire_slot(), but never holds s->mu while pumping:
 * the release callback takes s->mu on this same thread. */
static int swl_acquire_slot(shim_surf* s) {
    long t0 = now_us();
    for (;;) {
        pthread_mutex_lock(&s->mu);
        int k = first_free(s);
        pthread_mutex_unlock(&s->mu);
        if (k >= 0) return k;
        if (s->no_idle || now_us() - t0 > 300000) {
            DBG("[shim] wayland: no wl_buffer.release for 300 ms, continuing without waiting\n");
            pthread_mutex_lock(&s->mu);
            s->no_idle = 1;
            s->inflight = 0;
            for (int i = 0; i < s->nbuf; i++) s->slots[i].busy = 0;
            k = s->current_slot;
            pthread_mutex_unlock(&s->mu);
            return k;
        }
        wl_wsi_pump(s->wl, 10);
    }
}

static void swl_present(shim_surf* s, int slot) {
    wl_wsi_present(s->wl, slot, !surf_use_async(s));
}

/* ---------- Display + surface ---------- */

static void* shim_wl_display(void* native) {
    if (shim_init()) { SET_ERR(E_BAD_DISPLAY); return NULL; }
    pthread_mutex_lock(&G.mu);
    if (!G.wdpy) {
        void* wd = native;
        int own = 0;
        if (!wd) { wd = wl_wsi_connect(); own = 1; }
        void* (*gd)(void*) = (void* (*)(void*))rsym("eglGetDisplay");
        void* d = (wd && gd) ? gd(NULL) : NULL;
        if (!d) {
            if (own && wd) wl_wsi_disconnect(wd);
            pthread_mutex_unlock(&G.mu);
            SET_ERR(E_BAD_DISPLAY);
            return NULL;
        }
        G.wl_native = wd; G.own_wl = own; G.wdpy = d; G.plat_wl = 1;
        if (!G.dpy) G.dpy = d;          /* the dmabuf/fence code uses G.dpy for driver calls */
        DBG("[shim] Wayland %s, display %p\n", own ? "own connection" : "app connection", d);
    }
    void* d = G.wdpy;
    pthread_mutex_unlock(&G.mu);
    return d;
}

static int is_wl_display(void* d) { return G.plat_wl && G.wdpy && d == G.wdpy; }

/* win = struct wl_egl_window* (both eglCreateWindowSurface and the platform variants) */
static void* make_wl_surface(void* d, void* win) {
    if (shim_init()) return NULL;
    if (!is_wl_display(d)) { SET_ERR(E_BAD_DISPLAY); return NULL; }
    int w = 0, h = 0;
    void* surf = NULL;
    if (!win || wl_wsi_window_info(win, &w, &h, &surf)) { SET_ERR(E_BAD_NATIVE_WINDOW); return NULL; }
    shim_surf* s = calloc(1, sizeof *s);
    if (!s) { SET_ERR(E_BAD_ALLOC); return NULL; }
    s->magic = SURF_MAGIC; s->kind = KIND_WL; s->wlwin = win;
    s->w = w; s->h = h;
    s->depth = getenv("SHIM_WL_ALPHA") ? 32 : 24;
    s->interval = 1;
    s->dpy = d;
    pthread_mutex_lock(&G.mu);
    s->next = G.surfs; G.surfs = s;
    pthread_mutex_unlock(&G.mu);
    return s;
}

#include <wayland-client.h>
#include "linux-dmabuf-client.h"
#include "linux-dmabuf-client.h"

static struct {
    pthread_mutex_t mu;
    void* wd;                          /* wl_display pentru care e legat */
    struct zwp_linux_dmabuf_v1* dmabuf; /* proxy pe coada implicită */
    int tried;
} SD = { .mu = PTHREAD_MUTEX_INITIALIZER };

static struct zwp_linux_dmabuf_v1* swl_bound;   /* completat de callback */

static void swl_reg_global(void* data, struct wl_registry* reg, uint32_t name,
                           const char* iface, uint32_t ver) {
    (void)data;
    if (!strcmp(iface, zwp_linux_dmabuf_v1_interface.name) && ver >= 2)
        swl_bound = wl_registry_bind(reg, name, &zwp_linux_dmabuf_v1_interface,
                                     ver < 3 ? ver : 3);
}
static void swl_reg_remove(void* d, struct wl_registry* r, uint32_t n) { (void)d;(void)r;(void)n; }
static const struct wl_registry_listener swl_reg_l = { swl_reg_global, swl_reg_remove };

/* Leagă linux-dmabuf pe conexiunea aplicației, printr-o coadă privată,
 * ca să nu consumăm evenimentele aplicației. */
static struct zwp_linux_dmabuf_v1* swl_dmabuf_get(void) {
    struct wl_display* wd = G.wl_native;
    if (!wd) return NULL;
    pthread_mutex_lock(&SD.mu);
    if (SD.wd == wd && SD.dmabuf) { void* r = SD.dmabuf; pthread_mutex_unlock(&SD.mu); return r; }
    if (SD.wd == wd && SD.tried) { pthread_mutex_unlock(&SD.mu); return NULL; }
    SD.wd = wd; SD.tried = 1; SD.dmabuf = NULL; swl_bound = NULL;

    struct wl_event_queue* q = wl_display_create_queue(wd);
    struct wl_display* w = wl_proxy_create_wrapper(wd);
    if (q && w) {
        wl_proxy_set_queue((struct wl_proxy*)w, q);
        struct wl_registry* reg = wl_display_get_registry(w);
        if (reg) {
            wl_registry_add_listener(reg, &swl_reg_l, NULL);
            wl_display_roundtrip_queue(wd, q);
            wl_registry_destroy(reg);
        }
    }
    if (w) wl_proxy_wrapper_destroy(w);
    if (swl_bound) {
        /* Proxy-ul îl mutăm pe coada implicită, ca buffer-ele create din el
         * să livreze evenimente (release) pe coada aplicației. */
        wl_proxy_set_queue((struct wl_proxy*)swl_bound, NULL);
        SD.dmabuf = swl_bound;
    }
    if (q) wl_event_queue_destroy(q);
    struct zwp_linux_dmabuf_v1* r = SD.dmabuf;
    pthread_mutex_unlock(&SD.mu);
    if (!r) DBG("[shim] wayland: zwp_linux_dmabuf_v1 (v>=2) unavailable\n");
    return r;
}

static void* swl_buffer_from_dmabuf(void* d, uint32_t fourcc, int w, int h,
                                    int fd, uint32_t stride, uint32_t off,
                                    uint64_t mod) {
    (void)d;
    struct zwp_linux_dmabuf_v1* dm = swl_dmabuf_get();
    if (!dm) return NULL;
    struct zwp_linux_buffer_params_v1* p = zwp_linux_dmabuf_v1_create_params(dm);
    if (!p) return NULL;
    /* add() trimite fd-ul prin socket (kernelul îl duplică); nu îl închidem aici */
    zwp_linux_buffer_params_v1_add(p, fd, 0, off, stride,
                                   (uint32_t)(mod >> 32), (uint32_t)(mod & 0xffffffffu));
    struct wl_buffer* b = zwp_linux_buffer_params_v1_create_immed(p, w, h, fourcc, 0);
    zwp_linux_buffer_params_v1_destroy(p);
    wl_display_flush((struct wl_display*)G.wl_native);
    return b;
}