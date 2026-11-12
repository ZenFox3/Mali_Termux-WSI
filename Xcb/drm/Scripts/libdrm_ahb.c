// libdrm_ahb.c : kernel-less libdrm, buffers are backed by AHardwareBuffer
//
// Env: DRM_AHB_MODE=1080x2400@60 (default)
//      DRM_AHB_DRIVER=ahb (default driver name)
// mmap() on dumb buffers requires LD_PRELOAD=./libdrm.so.2 (otherwise libc wins symbol resolution)

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <android/hardware_buffer.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <stdarg.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <drm.h>
#include <drm_mode.h>
#include <drm_fourcc.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

/* ---- Helper macro pentru dlsym ---- */
#define REAL(ret, name, args) \
    static ret (*real) args; \
    if (!real) real = (ret (*) args)dlsym(RTLD_NEXT, #name)

/* ---- AHB native handle (not exposed in NDK) ---- */
typedef struct { int version, numFds, numInts; int data[]; } nh_t;
const void* AHardwareBuffer_getNativeHandle(const AHardwareBuffer* b);

static int ahb_dmabuf_fd(AHardwareBuffer* ahb) {
    const nh_t* h = AHardwareBuffer_getNativeHandle(ahb);
    if (!h || h->numFds <= 1) return -1;
    
    return (int)syscall(SYS_fcntl, h->data[1], F_DUPFD_CLOEXEC, 0);   /* data[1] = dmabuf on mobile GPUs */
}

/* ---- global state ---- */
#define MAX_BO  1024
#define MAX_FB  64
#define MAX_DEV 64
#define CRTC_ID 1
#define ENC_ID  2
#define CONN_ID 3
#define FB_BASE 100
#define MAP_SHIFT 24

typedef struct {
    AHardwareBuffer* ahb;
    uint32_t w, h, bpp, pitch;
    int dmafd; dev_t dev; ino_t ino;
    void* map;
    uint32_t key; int foreign;   /* foreign: registered from libgbm, key = gbm_bo_get_handle().u32 */
} bo_t;
typedef struct { uint32_t id, w, h, fmt, pitch, handle; } fb_t;
typedef struct { uint64_t user; uint32_t crtc, seq; uint64_t ns; } ev_t;

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static bo_t bos[MAX_BO + 1];
static fb_t fbs[MAX_FB];
static int devs[MAX_DEV];
static uint8_t devtype[MAX_DEV];   /* DRM_NODE_PRIMARY / DRM_NODE_RENDER */

/* true while inside AHardwareBuffer_lock/unlock/release: internal mmap/munmap go directly to libc */
static __thread int in_ahb;
static ev_t evq[32]; static int ev_head, ev_cnt;
static uint32_t crtc_fb, vseq;
static drmModeModeInfo g_mode; static int mode_ok;

typedef void (*drm_ahb_sink_fn)(AHardwareBuffer*, uint32_t w, uint32_t h, void*);
static drm_ahb_sink_fn g_sink; static void* g_sink_ud;
static uint32_t g_last_fmt;

/* Forward declarations */
static int node_for(const char* prefix, const char* dflt, char* out, size_t n);
static int fake_stat(const char* p, struct stat* st);
int open(const char* p, int fl, ...);
int openat(int dfd, const char* p, int fl, ...);
int stat(const char* p, struct stat* st);
int lstat(const char* p, struct stat* st);
int fstatat(int dfd, const char* p, struct stat* st, int fl);

/* Who actually displays the framebuffer: set your sink callback here */
void drm_ahb_set_sink(drm_ahb_sink_fn fn, void* ud) { g_sink = fn; g_sink_ud = ud; }

/* DRM_AHB_SINK=x11 => loads libdrm_ahb_x11.so (or DRM_AHB_SINK_LIB=/full/path.so) */
static void sink_autoload(void) {
    static int tried;
    if (tried || g_sink) return;
    tried = 1;
    const char* n = getenv("DRM_AHB_SINK");
    if (!n || !*n) return;
    char lib[512]; const char* l = getenv("DRM_AHB_SINK_LIB");
    if (l && *l) snprintf(lib, sizeof lib, "%s", l); else snprintf(lib, sizeof lib, "libdrm_ahb_%s.so", n);
    void* h = dlopen(lib, RTLD_NOW);
    if (!h) { fprintf(stderr, "[drm-ahb] sink %s: %s\n", lib, dlerror()); return; }
    drm_ahb_sink_fn fn = (drm_ahb_sink_fn)dlsym(h, "drm_ahb_sink");
    if (fn) g_sink = fn; else fprintf(stderr, "[drm-ahb] %s: missing drm_ahb_sink\n", lib);
}
/* helpers for sinks: DRM format of the last fb and CPU pointer if bo is mapped by application */
uint32_t drm_ahb_cur_format(void) { return g_last_fmt ? g_last_fmt : DRM_FORMAT_XRGB8888; }
void* drm_ahb_cpu_ptr(AHardwareBuffer* ahb) {
    void* r = NULL;
    pthread_mutex_lock(&mu);
    for (int i = 1; i <= MAX_BO; i++) if (bos[i].ahb == ahb) { r = bos[i].map; break; }
    pthread_mutex_unlock(&mu);
    return r;
}

/* size + refresh from X server (Termux:X11). Without X headers: dlopen. */
static int detect_x_mode(unsigned* w, unsigned* h, unsigned* r) {
    if (!getenv("DISPLAY")) return 0;
    void* x = dlopen("libX11.so.6", RTLD_NOW | RTLD_LOCAL);
    if (!x) return 0;
    void* (*XOpenDisplay_)(const char*) = dlsym(x, "XOpenDisplay");
    int   (*XCloseDisplay_)(void*)      = dlsym(x, "XCloseDisplay");
    int   (*XDefaultScreen_)(void*)     = dlsym(x, "XDefaultScreen");
    unsigned long (*XRootWindow_)(void*, int) = dlsym(x, "XRootWindow");
    int   (*XDisplayWidth_)(void*, int)  = dlsym(x, "XDisplayWidth");
    int   (*XDisplayHeight_)(void*, int) = dlsym(x, "XDisplayHeight");
    if (!XOpenDisplay_ || !XCloseDisplay_ || !XDefaultScreen_ || !XRootWindow_ ||
        !XDisplayWidth_ || !XDisplayHeight_) return 0;

    void* d = XOpenDisplay_(NULL);
    if (!d) return 0;
    int s = XDefaultScreen_(d);
    int W = XDisplayWidth_(d, s), H = XDisplayHeight_(d, s);
    int R = 0;

    void* xr = dlopen("libXrandr.so.2", RTLD_NOW | RTLD_LOCAL);
    if (xr) {
        void* (*GetCfg)(void*, unsigned long) = dlsym(xr, "XRRGetScreenInfo");
        short (*CurRate)(void*)               = dlsym(xr, "XRRConfigCurrentRate");
        void  (*FreeCfg)(void*)               = dlsym(xr, "XRRFreeScreenConfigInfo");
        if (GetCfg && CurRate) {
            void* cfg = GetCfg(d, XRootWindow_(d, s));
            if (cfg) { R = CurRate(cfg); if (FreeCfg) FreeCfg(cfg); }
        }
    }
    XCloseDisplay_(d);
    if (W <= 0 || H <= 0) return 0;
    *w = (unsigned)W; *h = (unsigned)H;
    if (R > 0) *r = (unsigned)R;
    return 1;
}

static void mode_init(void) {
    if (mode_ok) return;
    unsigned w = 1080, h = 2400, r = 60;
    detect_x_mode(&w, &h, &r);                      /* automatic */
    const char* e = getenv("DRM_AHB_MODE");          /* manual override */
    if (e && *e) sscanf(e, "%ux%u@%u", &w, &h, &r);
    drmModeModeInfo* m = &g_mode; memset(m, 0, sizeof *m);
    m->hdisplay = w; m->hsync_start = w + 16; m->hsync_end = w + 32; m->htotal = w + 48;
    m->vdisplay = h; m->vsync_start = h + 8;  m->vsync_end = h + 16; m->vtotal = h + 24;
    m->vrefresh = r;
    m->clock = (uint32_t)((uint64_t)m->htotal * m->vtotal * r / 1000);
    m->type = DRM_MODE_TYPE_PREFERRED | DRM_MODE_TYPE_DRIVER;
    snprintf(m->name, sizeof m->name, "%ux%u", w, h);
    if (getenv("DRM_AHB_DEBUG")) fprintf(stderr, "[drm-ahb] mode %ux%u@%u\n", w, h, r);
    mode_ok = 1;
}

static int node_type(int fd) {
    int r = -1;
    if (fd < 0) return -1;
    pthread_mutex_lock(&mu);
    for (int i = 0; i < MAX_DEV; i++) if (devs[i] == fd) { r = devtype[i]; break; }
    pthread_mutex_unlock(&mu);
    return r;
}

static int is_ours(int fd) {
    return node_type(fd) >= 0;
}

static int is_kms(int fd) {
    return node_type(fd) == DRM_NODE_PRIMARY;
}

/* ---- A. Macro + registrare fd ---- */
static void dev_register(int fd, int type) {
    if (fd < 0) return;
    int done = 0;
    pthread_mutex_lock(&mu);
    for (int i = 0; i < MAX_DEV; i++)          /* întâi o intrare existentă */
        if (devs[i] == fd) { devtype[i] = (uint8_t)type; done = 1; break; }
    for (int i = 0; i < MAX_DEV && !done; i++) /* apoi un slot gol */
        if (!devs[i]) { devs[i] = fd; devtype[i] = (uint8_t)type; done = 1; }
    pthread_mutex_unlock(&mu);
    if (!done && getenv("DRM_AHB_DEBUG"))
        fprintf(stderr, "[drm-ahb] dev table full, fd %d not tracked\n", fd);
}

static void dev_unregister(int fd) {
    if (fd < 0) return;
    pthread_mutex_lock(&mu);
    for (int i = 0; i < MAX_DEV; i++) if (devs[i] == fd) { devs[i] = 0; devtype[i] = 0; }
    pthread_mutex_unlock(&mu);
}

/* nume de nod sigur, fără buffer neinițializat */
static void node_name(int type, char* out, size_t n) {
    const char* pre = type == DRM_NODE_RENDER ? "renderD" : "card";
    const char* def = type == DRM_NODE_RENDER ? "renderD128" : "card0";
    out[0] = 0;
    if (!node_for(pre, def, out, n) || !out[0]) snprintf(out, n, "%s", def);
}

static char* dev_name_t(int t) {
    char nm[64], *r = malloc(80);
    if (!r) return NULL;
    node_name(t, nm, sizeof nm);
    snprintf(r, 80, "/dev/dri/%s", nm);
    return r;
}

static bo_t* bo_get(uint32_t h) { return (h >= 1 && h <= MAX_BO && bos[h].ahb) ? &bos[h] : NULL; }
/* own GEM handle or GBM key; called under mutex */
static bo_t* bo_lookup(uint32_t h) {
    bo_t* b = bo_get(h);
    if (b) return b;
    for (int i = 1; i <= MAX_BO; i++) if (bos[i].ahb && bos[i].foreign && bos[i].key == h) return &bos[i];
    return NULL;
}
static fb_t* fb_get(uint32_t id) {
    for (int i = 0; i < MAX_FB; i++) if (fbs[i].id == id) return &fbs[i];
    return NULL;
}
static uint64_t now_ns(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + t.tv_nsec;
}

/* ---- GEM / bo ---- */
static int bo_create(uint32_t w, uint32_t h, uint32_t bpp, uint32_t* out, uint64_t* size) {
    uint32_t fmt;
    switch (bpp) {
    case 32: fmt = AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM; break;
    case 16: fmt = AHARDWAREBUFFER_FORMAT_R5G6B5_UNORM; break;
    case 8:  fmt = AHARDWAREBUFFER_FORMAT_R8_UNORM; break;
    default: errno = EINVAL; return -1;
    }
    AHardwareBuffer_Desc d = { .width = w, .height = h, .layers = 1, .format = fmt,
        .usage = AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN | AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN |
                 AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE | AHARDWAREBUFFER_USAGE_GPU_COLOR_OUTPUT };
    AHardwareBuffer* ahb = NULL;
    if (AHardwareBuffer_allocate(&d, &ahb) || !ahb) { errno = ENOMEM; return -1; }
    AHardwareBuffer_describe(ahb, &d);
    int fd = ahb_dmabuf_fd(ahb);
    struct stat st; memset(&st, 0, sizeof st);
    if (fd >= 0) fstat(fd, &st);

    pthread_mutex_lock(&mu);
    uint32_t h_ = 0;
    for (uint32_t i = 1; i <= MAX_BO; i++) if (!bos[i].ahb) { h_ = i; break; }
    if (!h_) {
        pthread_mutex_unlock(&mu);
        if (fd >= 0) close(fd);
        AHardwareBuffer_release(ahb); errno = ENOMEM; return -1;
    }
    bo_t* b = &bos[h_];
    b->ahb = ahb; b->w = w; b->h = h; b->bpp = bpp; b->pitch = d.stride * (bpp / 8);
    b->dmafd = fd; b->dev = st.st_dev; b->ino = st.st_ino; b->map = NULL;
    if (size) *size = (uint64_t)b->pitch * h;
    pthread_mutex_unlock(&mu);
    *out = h_;
    return 0;
}
static int bo_close(uint32_t h) {
    pthread_mutex_lock(&mu);
    bo_t* b = bo_get(h);
    if (!b) { pthread_mutex_unlock(&mu); errno = EINVAL; return -1; }
    bo_t c = *b;
    memset(b, 0, sizeof *b);
    pthread_mutex_unlock(&mu);
    in_ahb = 1;
    if (c.map) AHardwareBuffer_unlock(c.ahb, NULL);
    AHardwareBuffer_release(c.ahb);
    in_ahb = 0;
    if (c.dmafd >= 0) close(c.dmafd);
    return 0;
}
/* libgbm_ahb calls these (dlsym RTLD_DEFAULT) so AddFB finds the bo by gbm_bo_get_handle().u32 */
int drm_ahb_register(AHardwareBuffer* ahb, uint32_t key) {
    AHardwareBuffer_Desc d; AHardwareBuffer_describe(ahb, &d);
    AHardwareBuffer_acquire(ahb);
    pthread_mutex_lock(&mu);
    for (uint32_t i = 1; i <= MAX_BO; i++) if (!bos[i].ahb) {
        bos[i].ahb = ahb; bos[i].w = d.width; bos[i].h = d.height; bos[i].bpp = 32;
        bos[i].pitch = d.stride * 4; bos[i].dmafd = -1; bos[i].map = NULL;
        bos[i].key = key; bos[i].foreign = 1;
        pthread_mutex_unlock(&mu); return (int)i;
    }
    pthread_mutex_unlock(&mu);
    AHardwareBuffer_release(ahb); errno = ENOMEM; return -1;
}
void drm_ahb_unregister(AHardwareBuffer* ahb) {
    uint32_t h = 0;
    pthread_mutex_lock(&mu);
    for (uint32_t i = 1; i <= MAX_BO; i++) if (bos[i].ahb == ahb && bos[i].foreign) { h = i; break; }
    pthread_mutex_unlock(&mu);
    if (h) bo_close(h);
}

static int prime_to_fd(uint32_t h, uint32_t flags, int* out) {
    pthread_mutex_lock(&mu);
    bo_t* b = bo_get(h);
    int src = b ? b->dmafd : -1;
    int r = -1;
    if (src >= 0) r = (int)syscall(SYS_fcntl, src, (flags & O_CLOEXEC) ? F_DUPFD_CLOEXEC : F_DUPFD, 0);
    pthread_mutex_unlock(&mu);
    if (r < 0) { errno = EINVAL; return -1; }
    *out = r; return 0;
}
static int prime_to_handle(int fd, uint32_t* out) {
    struct stat st;
    if (fstat(fd, &st)) return -1;
    int r = -1;
    pthread_mutex_lock(&mu);
    for (uint32_t i = 1; i <= MAX_BO; i++)
        if (bos[i].ahb && bos[i].ino == st.st_ino && bos[i].dev == st.st_dev) { *out = i; r = 0; break; }
    pthread_mutex_unlock(&mu);
    if (r) errno = EINVAL;   /* foreign dmabuf: would require AHB import from fd (not available in NDK) */
    return r;
}

/* ---- caps ---- */
static int get_cap(uint64_t cap, uint64_t* v) {
    switch (cap) {
    case DRM_CAP_DUMB_BUFFER: case DRM_CAP_TIMESTAMP_MONOTONIC:
    case DRM_CAP_ADDFB2_MODIFIERS:
#ifdef DRM_CAP_CRTC_IN_VBLANK_EVENT
    case DRM_CAP_CRTC_IN_VBLANK_EVENT:
#endif
        *v = 1; return 0;
    case DRM_CAP_PRIME: *v = DRM_PRIME_CAP_IMPORT | DRM_PRIME_CAP_EXPORT; return 0;
    case DRM_CAP_DUMB_PREFERRED_DEPTH: *v = 24; return 0;
    case DRM_CAP_CURSOR_WIDTH: case DRM_CAP_CURSOR_HEIGHT: *v = 64; return 0;
    default: *v = 0; return 0;
    }
}

/* ---- device / fd ---- */
static int dev_open(int type) {
    int fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (fd < 0) return -1;
    dev_register(fd, type);
    return fd;
}

int drmOpen(const char* n, const char* b) { (void)n; (void)b; return dev_open(DRM_NODE_PRIMARY); }
int drmOpenWithType(const char* n, const char* b, int t) {
    (void)n; (void)b; return dev_open(t == DRM_NODE_RENDER ? DRM_NODE_RENDER : DRM_NODE_PRIMARY);
}
int drmOpenControl(int m) { (void)m; return dev_open(DRM_NODE_PRIMARY); }
int drmOpenRender(int m)  { (void)m; return dev_open(DRM_NODE_RENDER); }

int drmClose(int fd) {
    return close(fd);
}
int drmAvailable(void) { return 1; }
int drmSetMaster(int fd)  { if (!is_kms(fd)) { errno = EACCES; return -1; } return 0; }
int drmDropMaster(int fd) { if (!is_kms(fd)) { errno = EACCES; return -1; } return 0; }
int drmIsMaster(int fd)   { return is_kms(fd); }
void drmFree(void* p) { free(p); }

char* drmGetDeviceNameFromFd2(int fd) {
    int t = node_type(fd);
    return dev_name_t(t < 0 ? DRM_NODE_PRIMARY : t);
}
char* drmGetDeviceNameFromFd(int fd) { return drmGetDeviceNameFromFd2(fd); }

drmVersionPtr drmGetVersion(int fd) {
    (void)fd;
    drmVersionPtr v = calloc(1, sizeof *v);
    if (!v) return NULL;
    v->version_major = 1; v->version_minor = 0; v->version_patchlevel = 0;
    const char* dname = getenv("DRM_AHB_DRIVER");
    if (!dname || !*dname) dname = "ahb";
    v->name = strdup(dname);  v->name_len = (int)strlen(v->name);
    v->date = strdup("Nov 8, 2026"); v->date_len = (int)strlen(v->date);
    v->desc = strdup("AHardwareBuffer DRM (userspace)"); v->desc_len = (int)strlen(v->desc);
    return v;
}
void drmFreeVersion(drmVersionPtr v) {
    if (!v) return;
    free(v->name); free(v->date); free(v->desc); free(v);
}
int drmGetCap(int fd, uint64_t cap, uint64_t* v) { (void)fd; return get_cap(cap, v); }
int drmSetClientCap(int fd, uint64_t cap, uint64_t v) { (void)fd; (void)cap; (void)v; return 0; }

/* ---- PRIME ---- */
int drmPrimeHandleToFD(int fd, uint32_t h, uint32_t flags, int* out) { (void)fd; return prime_to_fd(h, flags, out); }
int drmPrimeFDToHandle(int fd, int pfd, uint32_t* h) { (void)fd; return prime_to_handle(pfd, h); }
int drmCloseBufferHandle(int fd, uint32_t h) { (void)fd; return bo_close(h); }

/* ---- ioctl ---- */
int drmIoctl(int fd, unsigned long req, void* arg) {
    if (!is_ours(fd)) {
        int r;
        do { r = ioctl(fd, req, arg); } while (r == -1 && (errno == EINTR || errno == EAGAIN));
        return r;
    }
    switch (req) {
    case DRM_IOCTL_MODE_CREATE_DUMB: {
        struct drm_mode_create_dumb* a = arg;
        uint64_t size;
        if (bo_create(a->width, a->height, a->bpp, &a->handle, &size)) return -1;
        a->pitch = bos[a->handle].pitch; a->size = size; return 0;
    }
    case DRM_IOCTL_MODE_MAP_DUMB: {
        struct drm_mode_map_dumb* a = arg;
        if (!bo_get(a->handle)) { errno = EINVAL; return -1; }
        a->offset = (uint64_t)a->handle << MAP_SHIFT; return 0;
    }
    case DRM_IOCTL_MODE_DESTROY_DUMB: return bo_close(((struct drm_mode_destroy_dumb*)arg)->handle);
    case DRM_IOCTL_GEM_CLOSE:         return bo_close(((struct drm_gem_close*)arg)->handle);
    case DRM_IOCTL_PRIME_HANDLE_TO_FD: {
        struct drm_prime_handle* a = arg; return prime_to_fd(a->handle, a->flags, &a->fd);
    }
    case DRM_IOCTL_PRIME_FD_TO_HANDLE: {
        struct drm_prime_handle* a = arg; return prime_to_handle(a->fd, &a->handle);
    }
    case DRM_IOCTL_GET_CAP: {
        struct drm_get_cap* a = arg; uint64_t v = 0;
        int r = get_cap(a->capability, &v); a->value = v; return r;
    }
    case DRM_IOCTL_GET_MAGIC:  ((struct drm_auth*)arg)->magic = 1; return 0;
    case DRM_IOCTL_AUTH_MAGIC: return 0;
    case DRM_IOCTL_SET_CLIENT_CAP: case DRM_IOCTL_SET_MASTER: case DRM_IOCTL_DROP_MASTER: return 0;
    }
    errno = ENOTTY; return -1;
}

/* ---- mmap for dumb buffers (requires LD_PRELOAD) ---- */
void* mmap(void* a, size_t l, int p, int f, int fd, off_t o) {
    static void* (*real)(void*, size_t, int, int, int, off_t);
    if (!in_ahb && fd >= 0 && is_ours(fd)) {
        uint32_t hd = (uint32_t)((uint64_t)o >> MAP_SHIFT);
        AHardwareBuffer* ahb = NULL; void* m = NULL;
        pthread_mutex_lock(&mu);
        bo_t* b = bo_get(hd);
        if (b) { ahb = b->ahb; m = b->map; }
        pthread_mutex_unlock(&mu);
        if (!ahb) { errno = EINVAL; return MAP_FAILED; }
        if (!m) {
            void* np = NULL;
            in_ahb = 1;   /* mmap performed by gralloc in lock() goes directly to libc */
            int rc = AHardwareBuffer_lock(ahb, AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN |
                         AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN, -1, NULL, &np);
            in_ahb = 0;
            if (rc || !np) { errno = EINVAL; return MAP_FAILED; }
            pthread_mutex_lock(&mu);
            b = bo_get(hd);
            if (b && b->ahb == ahb) b->map = np;
            pthread_mutex_unlock(&mu);
            m = np;
        }
        return m;
    }
    if (!real) real = dlsym(RTLD_NEXT, "mmap");
    return real(a, l, p, f, fd, o);
}
void* mmap64(void* a, size_t l, int p, int f, int fd, off64_t o) { return mmap(a, l, p, f, fd, (off_t)o); }
int munmap(void* a, size_t l) {
    static int (*real)(void*, size_t);
    if (!in_ahb) {
        AHardwareBuffer* ahb = NULL;
        pthread_mutex_lock(&mu);
        for (int i = 1; i <= MAX_BO; i++)
            if (bos[i].ahb && bos[i].map == a) { ahb = bos[i].ahb; bos[i].map = NULL; break; }
        pthread_mutex_unlock(&mu);
        if (ahb) { in_ahb = 1; AHardwareBuffer_unlock(ahb, NULL); in_ahb = 0; return 0; }
    }
    if (!real) real = dlsym(RTLD_NEXT, "munmap");
    return real(a, l);
}

/* ---- fake KMS ---- */
static uint32_t* one_id(uint32_t id) { uint32_t* p = malloc(4); if (p) *p = id; return p; }

drmModeResPtr drmModeGetResources(int fd) {
    if (!is_kms(fd)) { errno = EINVAL; return NULL; }
    mode_init();
    drmModeResPtr r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->count_crtcs = r->count_connectors = r->count_encoders = 1;
    r->crtcs = one_id(CRTC_ID); r->connectors = one_id(CONN_ID); r->encoders = one_id(ENC_ID);
    r->min_width = r->min_height = 1; r->max_width = r->max_height = 16384;
    return r;
}
void drmModeFreeResources(drmModeResPtr r) {
    if (!r) return;
    free(r->fbs); free(r->crtcs); free(r->connectors); free(r->encoders); free(r);
}
drmModeConnectorPtr drmModeGetConnector(int fd, uint32_t id) {
    if (!is_kms(fd) || id != CONN_ID) { errno = ENOENT; return NULL; }
    mode_init();
    drmModeConnectorPtr c = calloc(1, sizeof *c);
    if (!c) return NULL;
    c->connector_id = CONN_ID; c->encoder_id = ENC_ID;
    c->connector_type = DRM_MODE_CONNECTOR_VIRTUAL; c->connector_type_id = 1;
    c->connection = DRM_MODE_CONNECTED; c->subpixel = DRM_MODE_SUBPIXEL_UNKNOWN;
    c->mmWidth = g_mode.hdisplay * 254 / 4200; c->mmHeight = g_mode.vdisplay * 254 / 4200;
    c->count_modes = 1; c->modes = malloc(sizeof g_mode);
    if (c->modes) memcpy(c->modes, &g_mode, sizeof g_mode);
    c->count_encoders = 1; c->encoders = one_id(ENC_ID);
    return c;
}
drmModeConnectorPtr drmModeGetConnectorCurrent(int fd, uint32_t id) { return drmModeGetConnector(fd, id); }
void drmModeFreeConnector(drmModeConnectorPtr c) {
    if (!c) return;
    free(c->modes); free(c->props); free(c->prop_values); free(c->encoders); free(c);
}
drmModeEncoderPtr drmModeGetEncoder(int fd, uint32_t id) {
    if (!is_kms(fd) || id != ENC_ID) { errno = ENOENT; return NULL; }
    drmModeEncoderPtr e = calloc(1, sizeof *e);
    if (!e) return NULL;
    e->encoder_id = ENC_ID; e->encoder_type = DRM_MODE_ENCODER_VIRTUAL;
    e->crtc_id = CRTC_ID; e->possible_crtcs = 1;
    return e;
}
void drmModeFreeEncoder(drmModeEncoderPtr e) { free(e); }

drmModeCrtcPtr drmModeGetCrtc(int fd, uint32_t id) {
    if (!is_kms(fd) || id != CRTC_ID) { errno = ENOENT; return NULL; }
    mode_init();
    drmModeCrtcPtr c = calloc(1, sizeof *c);
    if (!c) return NULL;
    c->crtc_id = CRTC_ID; c->buffer_id = crtc_fb; c->mode_valid = 1; c->mode = g_mode;
    c->width = g_mode.hdisplay; c->height = g_mode.vdisplay;
    return c;
}
void drmModeFreeCrtc(drmModeCrtcPtr c) { free(c); }


/* ---- framebuffers ---- */
int drmModeAddFB2WithModifiers(int fd, uint32_t w, uint32_t h, uint32_t fmt, const uint32_t hs[4],
        const uint32_t pitches[4], const uint32_t offs[4], const uint64_t mods[4],
        uint32_t* id, uint32_t flags) {
    (void)fd; (void)offs; (void)mods; (void)flags;
    pthread_mutex_lock(&mu);
    int r = -1;
    bo_t* bb = bo_lookup(hs[0]);
    if (!bb) {
        if (getenv("DRM_AHB_DEBUG")) fprintf(stderr, "[drm-ahb] AddFB: unknown handle 0x%x\n", hs[0]);
        errno = EINVAL;
    } else {
        static uint32_t next = FB_BASE;
        for (int i = 0; i < MAX_FB; i++) if (!fbs[i].id) {
            fbs[i] = (fb_t){ next, w, h, fmt, pitches[0], (uint32_t)(bb - bos) };
            *id = next++; r = 0; break;
        }
        if (r) errno = ENOMEM;
    }
    pthread_mutex_unlock(&mu);
    return r;
}
int drmModeAddFB2(int fd, uint32_t w, uint32_t h, uint32_t fmt, const uint32_t hs[4],
        const uint32_t p[4], const uint32_t o[4], uint32_t* id, uint32_t flags) {
    return drmModeAddFB2WithModifiers(fd, w, h, fmt, hs, p, o, NULL, id, flags);
}
int drmModeAddFB(int fd, uint32_t w, uint32_t h, uint8_t depth, uint8_t bpp, uint32_t pitch,
        uint32_t handle, uint32_t* id) {
    (void)depth; (void)bpp;
    uint32_t hs[4] = { handle }, p[4] = { pitch }, o[4] = { 0 };
    return drmModeAddFB2WithModifiers(fd, w, h, 0, hs, p, o, NULL, id, 0);
}
int drmModeRmFB(int fd, uint32_t id) {
    (void)fd;
    pthread_mutex_lock(&mu);
    fb_t* f = fb_get(id);
    if (f) memset(f, 0, sizeof *f);
    pthread_mutex_unlock(&mu);
    return f ? 0 : (errno = ENOENT, -1);
}
drmModeFBPtr drmModeGetFB(int fd, uint32_t id) {
    (void)fd;
    drmModeFBPtr o = NULL;
    pthread_mutex_lock(&mu);
    fb_t* f = fb_get(id);
    if (f && (o = calloc(1, sizeof *o))) {
        o->fb_id = f->id; o->width = f->w; o->height = f->h;
        o->pitch = f->pitch; o->bpp = 32; o->depth = 24; o->handle = f->handle;
    }
    pthread_mutex_unlock(&mu);
    return o;
}
void drmModeFreeFB(drmModeFBPtr f) { free(f); }

/* ---- flip: where the frame lands ---- */
static int present(uint32_t fb_id) {
    AHardwareBuffer* ahb = NULL; uint32_t w = 0, h = 0;
    pthread_mutex_lock(&mu);
    fb_t* f = fb_get(fb_id);
    bo_t* b = f ? bo_get(f->handle) : NULL;
    if (b) { ahb = b->ahb; w = b->w; h = b->h; crtc_fb = fb_id; g_last_fmt = f->fmt; }
    pthread_mutex_unlock(&mu);
    if (!ahb) { errno = ENOENT; return -1; }
    sink_autoload();
    if (g_sink) g_sink(ahb, w, h, g_sink_ud);
    return 0;
}
int drmModeSetCrtc(int fd, uint32_t crtc, uint32_t fb, uint32_t x, uint32_t y,
        uint32_t* conns, int n, drmModeModeInfoPtr mode) {
    (void)x; (void)y; (void)conns; (void)n; (void)mode;
    if (!is_kms(fd) || crtc != CRTC_ID) { errno = ENOENT; return -1; }
    return fb ? present(fb) : 0;
}
int drmModePageFlip(int fd, uint32_t crtc, uint32_t fb, uint32_t flags, void* user) {
    if (!is_kms(fd) || crtc != CRTC_ID) { errno = ENOENT; return -1; }
    if (present(fb)) return -1;
    if (flags & DRM_MODE_PAGE_FLIP_EVENT) {
        pthread_mutex_lock(&mu);
        if (ev_cnt == 32) { ev_head = (ev_head + 1) % 32; ev_cnt--; }
        evq[(ev_head + ev_cnt++) % 32] = (ev_t){ (uint64_t)(uintptr_t)user, crtc, ++vseq, now_ns() };
        pthread_mutex_unlock(&mu);
        uint64_t one = 1; (void)!write(fd, &one, 8);
    }
    return 0;
}
int drmHandleEvent(int fd, drmEventContextPtr ctx) {
    uint64_t c; (void)!read(fd, &c, 8);
    for (;;) {
        ev_t e;
        pthread_mutex_lock(&mu);
        if (!ev_cnt) { pthread_mutex_unlock(&mu); break; }
        e = evq[ev_head]; ev_head = (ev_head + 1) % 32; ev_cnt--;
        pthread_mutex_unlock(&mu);
        unsigned s = (unsigned)(e.ns / 1000000000ull), us = (unsigned)(e.ns % 1000000000ull / 1000);
        void* u = (void*)(uintptr_t)e.user;
        if (ctx->version >= 3 && ctx->page_flip_handler2) ctx->page_flip_handler2(fd, e.seq, s, us, e.crtc, u);
        else if (ctx->page_flip_handler) ctx->page_flip_handler(fd, e.seq, s, us, u);
    }
    return 0;
}

/* ---- replaces /dev/dri nodes: intercepted open/close/dup (requires LD_PRELOAD) ---- */
/* DRM_AHB_NODE=card9,renderD199 : only these nodes are fake (the rest of /dev/dri remains real).
 * Unset: any card or renderD node is replaced. */
static int node_for(const char* prefix, const char* dflt, char* out, size_t n) {
    const char* e = getenv("DRM_AHB_NODE");
    size_t pl = strlen(prefix);
    if (!e || !*e) { snprintf(out, n, "%s", dflt); return 1; }
    for (const char* t = e; *t;) {
        const char* c = strchr(t, ',');
        size_t tl = c ? (size_t)(c - t) : strlen(t);
        if (tl >= pl && tl < n && !strncmp(t, prefix, pl)) { memcpy(out, t, tl); out[tl] = 0; return 1; }
        t += tl; if (*t == ',') t++;
    }
    return 0;
}
static int is_drm_path(const char* p) {
    if (!p || strncmp(p, "/dev/dri/", 9)) return 0;
    p += 9;
    const char* e = getenv("DRM_AHB_NODE");
    if (!e || !*e) return !strncmp(p, "card", 4) || !strncmp(p, "renderD", 7);
    size_t l = strlen(p);
    for (const char* t = e; *t;) {
        const char* c = strchr(t, ',');
        size_t tl = c ? (size_t)(c - t) : strlen(t);
        if (tl == l && !strncmp(t, p, l)) return 1;
        t += tl; if (*t == ',') t++;
    }
    return 0;
}
static int path_type(const char* p) {
    return !strncmp(p + 9, "renderD", 7) ? DRM_NODE_RENDER : DRM_NODE_PRIMARY;
}

/* ---- B. close / dup / fcntl ---- */
int close(int fd) {
    REAL(int, close, (int));
    dev_unregister(fd);          /* înainte de close, ca să nu ștergi un fd refolosit de alt thread */
    return real(fd);
}
int dup(int oldfd) {
    REAL(int, dup, (int));
    int t = node_type(oldfd), r = real(oldfd);
    if (r >= 0 && t >= 0) dev_register(r, t);
    return r;
}
int dup2(int oldfd, int newfd) {
    REAL(int, dup2, (int, int));
    int t = node_type(oldfd), r = real(oldfd, newfd);
    if (r >= 0 && oldfd != newfd) {
        dev_unregister(r);                       /* newfd a fost înlocuit */
        if (t >= 0) dev_register(r, t);
    }
    return r;
}
int dup3(int oldfd, int newfd, int flags) {
    REAL(int, dup3, (int, int, int));
    int t = node_type(oldfd), r = real(oldfd, newfd, flags);
    if (r >= 0) {
        dev_unregister(r);
        if (t >= 0) dev_register(r, t);
    }
    return r;
}
int fcntl(int fd, int cmd, ...) {
    REAL(int, fcntl, (int, int, ...));
    va_list ap; va_start(ap, cmd);
    long a = 0;
    if (cmd == F_DUPFD || cmd == F_DUPFD_CLOEXEC || cmd == F_SETFD || cmd == F_SETFL ||
        cmd == F_GETLK || cmd == F_SETLK || cmd == F_SETLKW) {
        a = va_arg(ap, long);
    }
    va_end(ap);
    int r = real(fd, cmd, a);
    if (r >= 0 && (cmd == F_DUPFD || cmd == F_DUPFD_CLOEXEC)) {
        int t = node_type(fd);
        if (t >= 0) dev_register(r, t);
    }
    return r;
}

/* ---- C. open / access ---- */
static int real_open(const char* p, int fl, mode_t m) {
    REAL(int, open, (const char*, int, ...));
    return real(p, fl, m);
}
static int real_open64(const char* p, int fl, mode_t m) {
    REAL(int, open64, (const char*, int, ...));
    return real(p, fl, m);
}
int open(const char* p, int fl, ...) {
    mode_t m = 0;
    if (fl & (O_CREAT | O_TMPFILE)) { va_list ap; va_start(ap, fl); m = va_arg(ap, mode_t); va_end(ap); }
    if (is_drm_path(p)) return dev_open(path_type(p));
    return real_open(p, fl, m);
}
int open64(const char* p, int fl, ...) {
    mode_t m = 0;
    if (fl & (O_CREAT | O_TMPFILE)) { va_list ap; va_start(ap, fl); m = va_arg(ap, mode_t); va_end(ap); }
    if (is_drm_path(p)) return dev_open(path_type(p));
    return real_open64(p, fl, m);
}
int __open_2(const char* p, int fl) {
    REAL(int, __open_2, (const char*, int));
    if (is_drm_path(p)) return dev_open(path_type(p));
    return real(p, fl);
}
int openat(int dfd, const char* p, int fl, ...) {
    REAL(int, openat, (int, const char*, int, ...));
    mode_t m = 0;
    if (fl & (O_CREAT | O_TMPFILE)) { va_list ap; va_start(ap, fl); m = va_arg(ap, mode_t); va_end(ap); }
    if (is_drm_path(p)) return dev_open(path_type(p));
    return real(dfd, p, fl, m);
}
int __openat_2(int dfd, const char* p, int fl) {
    REAL(int, __openat_2, (int, const char*, int));
    if (is_drm_path(p)) return dev_open(path_type(p));
    return real(dfd, p, fl);
}
int access(const char* p, int mode) {
    REAL(int, access, (const char*, int));
    if (is_drm_path(p)) return 0;
    return real(p, mode);
}

/* ---- device enumeration (wlroots / Mesa / weston) ---- */
struct dev_blob {
    drmDevice d;
    char* nodes[DRM_NODE_MAX];
    char names[DRM_NODE_MAX][64];
    drmPlatformBusInfo bus;
    drmPlatformDeviceInfo info;
    char* compat[2];
};
static drmDevicePtr dev_new(void) {
    struct dev_blob* b = calloc(1, sizeof *b);
    if (!b) return NULL;
    int avail = 0; char nm[64];
    if (node_for("card", "card0", nm, sizeof nm)) {
        snprintf(b->names[DRM_NODE_PRIMARY], 64, "/dev/dri/%s", nm);
        b->nodes[DRM_NODE_PRIMARY] = b->names[DRM_NODE_PRIMARY]; avail |= 1 << DRM_NODE_PRIMARY;
    }
    if (node_for("renderD", "renderD128", nm, sizeof nm)) {
        snprintf(b->names[DRM_NODE_RENDER], 64, "/dev/dri/%s", nm);
        b->nodes[DRM_NODE_RENDER] = b->names[DRM_NODE_RENDER]; avail |= 1 << DRM_NODE_RENDER;
    }
    snprintf(b->bus.fullname, sizeof b->bus.fullname, "ahb");
    b->compat[0] = "generic,ahb";
    b->info.compatible = b->compat;
    b->d.nodes = b->nodes;
    b->d.available_nodes = avail;
    b->d.bustype = DRM_BUS_PLATFORM;
    b->d.businfo.platform = &b->bus;
    b->d.deviceinfo.platform = &b->info;
    return &b->d;
}
int drmGetDevices2(uint32_t flags, drmDevicePtr devices[], int max) {
    (void)flags;
    if (!devices || max < 1) return 1;

    drmDevicePtr dev = dev_new();
    if (!dev) return -ENOMEM;

    devices[0] = dev;
    return 1;
}

int drmGetDevices(drmDevicePtr devices[], int max) {
    return drmGetDevices2(0, devices, max);
}
int drmGetDevice2(int fd, uint32_t flags, drmDevicePtr* out) {
    (void)flags;
    if (!is_ours(fd)) return -ENODEV;
    *out = dev_new();
    return *out ? 0 : -ENOMEM;
}
int drmGetDevice(int fd, drmDevicePtr* out) { return drmGetDevice2(fd, 0, out); }
void drmFreeDevice(drmDevicePtr* d) { if (d) { free(*d); *d = NULL; } }
void drmFreeDevices(drmDevicePtr devs[], int n) { for (int i = 0; i < n; i++) drmFreeDevice(&devs[i]); }

int drmDevicesEqual(drmDevicePtr a, drmDevicePtr b) {
    if (a == b) return 1;
    if (!a || !b || a->bustype != b->bustype) return 0;
    for (int i = 0; i < DRM_NODE_MAX; i++)
        if ((a->available_nodes & (1 << i)) && (b->available_nodes & (1 << i)) &&
            !strcmp(a->nodes[i], b->nodes[i])) return 1;
    return 0;
}

/* ---- D. stat* ---- */
#include <sys/sysmacros.h>
static int fake_stat(const char* p, struct stat* st) {
    const char* r_ = strstr(p, "renderD"); const char* c_ = strstr(p, "card");
    unsigned minor = r_ ? (unsigned)atoi(r_ + 7) : c_ ? (unsigned)atoi(c_ + 4) : 0;
    memset(st, 0, sizeof *st);
    st->st_mode = S_IFCHR | 0666; st->st_rdev = makedev(226, minor);
    st->st_nlink = 1; st->st_ino = 1000 + minor; st->st_blksize = 4096;
    return 0;
}

int stat(const char* p, struct stat* st) {
    REAL(int, stat, (const char*, struct stat*));
    if (is_drm_path(p)) return fake_stat(p, st);
    return real(p, st);
}
int lstat(const char* p, struct stat* st) {
    REAL(int, lstat, (const char*, struct stat*));
    if (is_drm_path(p)) return fake_stat(p, st);
    return real(p, st);
}
int fstatat(int dfd, const char* p, struct stat* st, int fl) {
    REAL(int, fstatat, (int, const char*, struct stat*, int));
    if (is_drm_path(p)) return fake_stat(p, st);
    return real(dfd, p, st, fl);
}
int fstat(int fd, struct stat* st) {
    REAL(int, fstat, (int, struct stat*));
    int t = node_type(fd);
    if (t >= 0) {
        char nm[64], path[96];
        node_name(t, nm, sizeof nm);
        snprintf(path, sizeof path, "/dev/dri/%s", nm);
        return fake_stat(path, st);
    }
    return real(fd, st);
}

/* ================= KMS atomic: planes, properties, blobs, commit ================= */
#include <drm_fourcc.h>
#define PLANE_ID 4

enum { PL_TYPE = 10, PL_FB, PL_CRTC, PL_SX, PL_SY, PL_SW, PL_SH, PL_CX, PL_CY, PL_CW, PL_CH,
       CR_ACTIVE, CR_MODE, CN_CRTC };
typedef struct { uint32_t id; const char* name; uint32_t flags; uint64_t lo, hi; } pdef_t;
static const pdef_t pdefs[] = {
    { PL_TYPE, "type",   DRM_MODE_PROP_ENUM | DRM_MODE_PROP_IMMUTABLE, 0, 0 },
    { PL_FB,   "FB_ID",  DRM_MODE_PROP_OBJECT, DRM_MODE_OBJECT_FB, 0 },
    { PL_CRTC, "CRTC_ID", DRM_MODE_PROP_OBJECT, DRM_MODE_OBJECT_CRTC, 0 },
    { PL_SX,   "SRC_X",  DRM_MODE_PROP_RANGE, 0, UINT32_MAX },
    { PL_SY,   "SRC_Y",  DRM_MODE_PROP_RANGE, 0, UINT32_MAX },
    { PL_SW,   "SRC_W",  DRM_MODE_PROP_RANGE, 0, UINT32_MAX },
    { PL_SH,   "SRC_H",  DRM_MODE_PROP_RANGE, 0, UINT32_MAX },
    { PL_CX,   "CRTC_X", DRM_MODE_PROP_SIGNED_RANGE, (uint64_t)(int64_t)INT32_MIN, INT32_MAX },
    { PL_CY,   "CRTC_Y", DRM_MODE_PROP_SIGNED_RANGE, (uint64_t)(int64_t)INT32_MIN, INT32_MAX },
    { PL_CW,   "CRTC_W", DRM_MODE_PROP_RANGE, 0, INT32_MAX },
    { PL_CH,   "CRTC_H", DRM_MODE_PROP_RANGE, 0, INT32_MAX },
    { CR_ACTIVE, "ACTIVE",  DRM_MODE_PROP_RANGE, 0, 1 },
    { CR_MODE,   "MODE_ID", DRM_MODE_PROP_BLOB, 0, 0 },
    { CN_CRTC,   "CRTC_ID", DRM_MODE_PROP_OBJECT, DRM_MODE_OBJECT_CRTC, 0 },
};

typedef struct {
    uint32_t fb, crtc, sx, sy, sw, sh, cw, ch; int32_t cx, cy;
    uint32_t active, mode, conn_crtc;
} kms_t;
static kms_t kms;

static uint64_t kget(const kms_t* k, uint32_t id) {
    switch (id) {
    case PL_TYPE: return 1; /* primary */
    case PL_FB: return k->fb;   case PL_CRTC: return k->crtc;
    case PL_SX: return k->sx;   case PL_SY: return k->sy;
    case PL_SW: return k->sw;   case PL_SH: return k->sh;
    case PL_CX: return (uint64_t)(int64_t)k->cx; case PL_CY: return (uint64_t)(int64_t)k->cy;
    case PL_CW: return k->cw;   case PL_CH: return k->ch;
    case CR_ACTIVE: return k->active; case CR_MODE: return k->mode;
    case CN_CRTC: return k->conn_crtc;
    }
    return 0;
}
static int kset(kms_t* k, uint32_t id, uint64_t v) {
    switch (id) {
    case PL_FB: k->fb = (uint32_t)v; return 0;   case PL_CRTC: k->crtc = (uint32_t)v; return 0;
    case PL_SX: k->sx = (uint32_t)v; return 0;   case PL_SY: k->sy = (uint32_t)v; return 0;
    case PL_SW: k->sw = (uint32_t)v; return 0;   case PL_SH: k->sh = (uint32_t)v; return 0;
    case PL_CX: k->cx = (int32_t)(int64_t)v; return 0; case PL_CY: k->cy = (int32_t)(int64_t)v; return 0;
    case PL_CW: k->cw = (uint32_t)v; return 0;   case PL_CH: k->ch = (uint32_t)v; return 0;
    case CR_ACTIVE: k->active = (uint32_t)v; return 0; case CR_MODE: k->mode = (uint32_t)v; return 0;
    case CN_CRTC: k->conn_crtc = (uint32_t)v; return 0;
    }
    return -1; /* including "type" (immutable) */
}
static int obj_props(uint32_t obj, uint32_t* ids) {
    int n = 0;
    if (obj == PLANE_ID) for (uint32_t p = PL_TYPE; p <= PL_CH; p++) ids[n++] = p;
    else if (obj == CRTC_ID) { ids[n++] = CR_ACTIVE; ids[n++] = CR_MODE; }
    else if (obj == CONN_ID) ids[n++] = CN_CRTC;
    else if (obj != ENC_ID) return -1;
    return n;
}
static int prop_belongs(uint32_t obj, uint32_t prop) {
    uint32_t ids[16]; int n = obj_props(obj, ids);
    for (int i = 0; i < n; i++) if (ids[i] == prop) return 1;
    return 0;
}

/* ---- blobs ---- */
static struct { uint32_t id; void* data; uint32_t len; } blobs[16];
static uint32_t blob_next = 1000;
int drmModeCreatePropertyBlob(int fd, const void* data, size_t size, uint32_t* id) {
    (void)fd;
    void* c = malloc(size ? size : 1);
    if (!c) return -1;
    memcpy(c, data, size);
    pthread_mutex_lock(&mu);
    for (int i = 0; i < 16; i++) if (!blobs[i].id) {
        blobs[i].id = blob_next++; blobs[i].data = c; blobs[i].len = (uint32_t)size;
        *id = blobs[i].id; pthread_mutex_unlock(&mu); return 0;
    }
    pthread_mutex_unlock(&mu);
    free(c); errno = ENOMEM; return -1;
}
int drmModeDestroyPropertyBlob(int fd, uint32_t id) {
    (void)fd; int r = -1;
    pthread_mutex_lock(&mu);
    for (int i = 0; i < 16; i++) if (blobs[i].id == id) { free(blobs[i].data); memset(&blobs[i], 0, sizeof blobs[i]); r = 0; }
    pthread_mutex_unlock(&mu);
    if (r) errno = ENOENT;
    return r;
}
drmModePropertyBlobPtr drmModeGetPropertyBlob(int fd, uint32_t id) {
    (void)fd;
    drmModePropertyBlobPtr b = NULL;
    pthread_mutex_lock(&mu);
    for (int i = 0; i < 16 && !b; i++) if (blobs[i].id == id && (b = calloc(1, sizeof *b))) {
        b->id = id; b->length = blobs[i].len; b->data = malloc(blobs[i].len ? blobs[i].len : 1);
        if (b->data) memcpy(b->data, blobs[i].data, blobs[i].len);
    }
    pthread_mutex_unlock(&mu);
    if (!b) errno = ENOENT;
    return b;
}
void drmModeFreePropertyBlob(drmModePropertyBlobPtr b) { if (b) { free(b->data); free(b); } }

/* ---- properties ---- */
drmModeObjectPropertiesPtr drmModeObjectGetProperties(int fd, uint32_t obj, uint32_t type) {
    (void)type;
    uint32_t ids[16];
    int n = is_kms(fd) ? obj_props(obj, ids) : -1;
    if (n < 0) { errno = ENOENT; return NULL; }
    drmModeObjectPropertiesPtr r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->count_props = n;
    r->props = calloc(n ? n : 1, sizeof(uint32_t));
    r->prop_values = calloc(n ? n : 1, sizeof(uint64_t));
    pthread_mutex_lock(&mu);
    for (int i = 0; i < n; i++) { r->props[i] = ids[i]; r->prop_values[i] = kget(&kms, ids[i]); }
    pthread_mutex_unlock(&mu);
    return r;
}
void drmModeFreeObjectProperties(drmModeObjectPropertiesPtr p) { if (p) { free(p->props); free(p->prop_values); free(p); } }

drmModePropertyPtr drmModeGetProperty(int fd, uint32_t id) {
    (void)fd;
    const pdef_t* d = NULL;
    for (size_t i = 0; i < sizeof pdefs / sizeof *pdefs; i++) if (pdefs[i].id == id) d = &pdefs[i];
    if (!d) { errno = ENOENT; return NULL; }
    drmModePropertyPtr p = calloc(1, sizeof *p);
    if (!p) return NULL;
    p->prop_id = id; p->flags = d->flags;
    snprintf(p->name, sizeof p->name, "%s", d->name);
    if (d->flags & DRM_MODE_PROP_ENUM) {
        static const char* en[3] = { "Overlay", "Primary", "Cursor" };
        p->count_enums = 3; p->enums = calloc(3, sizeof *p->enums);
        for (int i = 0; i < 3 && p->enums; i++) { p->enums[i].value = i; snprintf(p->enums[i].name, sizeof p->enums[i].name, "%s", en[i]); }
    } else if (!(d->flags & DRM_MODE_PROP_BLOB)) {
        p->count_values = (d->flags & DRM_MODE_PROP_OBJECT) ? 1 : 2;
        p->values = calloc(2, sizeof(uint64_t));
        if (p->values) { p->values[0] = d->lo; p->values[1] = d->hi; }
    }
    return p;
}
void drmModeFreeProperty(drmModePropertyPtr p) {
    if (!p) return;
    free(p->values); free(p->enums); free(p);
}

/* ---- planes ---- */
drmModePlaneResPtr drmModeGetPlaneResources(int fd) {
    if (!is_kms(fd)) { errno = EINVAL; return NULL; }
    drmModePlaneResPtr r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->count_planes = 1; r->planes = one_id(PLANE_ID);
    return r;
}
void drmModeFreePlaneResources(drmModePlaneResPtr p) { if (p) { free(p->planes); free(p); } }
drmModePlanePtr drmModeGetPlane(int fd, uint32_t id) {
    static const uint32_t fm[] = { DRM_FORMAT_XRGB8888, DRM_FORMAT_ARGB8888, DRM_FORMAT_XBGR8888, DRM_FORMAT_ABGR8888 };
    if (!is_kms(fd) || id != PLANE_ID) { errno = ENOENT; return NULL; }
    drmModePlanePtr p = calloc(1, sizeof *p);
    if (!p) return NULL;
    p->count_formats = 4; p->formats = malloc(sizeof fm);
    if (p->formats) memcpy(p->formats, fm, sizeof fm);
    p->plane_id = PLANE_ID; p->possible_crtcs = 1;
    pthread_mutex_lock(&mu);
    p->crtc_id = kms.crtc; p->fb_id = kms.fb; p->crtc_x = kms.cx; p->crtc_y = kms.cy;
    pthread_mutex_unlock(&mu);
    return p;
}
void drmModeFreePlane(drmModePlanePtr p) { if (p) { free(p->formats); free(p); } }

/* ---- atomic ---- */
struct _drmModeAtomicReq { int n, cap; struct { uint32_t obj, prop; uint64_t val; }* e; };
drmModeAtomicReqPtr drmModeAtomicAlloc(void) { return calloc(1, sizeof(struct _drmModeAtomicReq)); }
void drmModeAtomicFree(drmModeAtomicReqPtr r) { if (r) { free(r->e); free(r); } }
int drmModeAtomicAddProperty(drmModeAtomicReqPtr r, uint32_t obj, uint32_t prop, uint64_t val) {
    if (!r) return -EINVAL;
    if (r->n == r->cap) {
        int nc = r->cap ? r->cap * 2 : 16;
        void* e = realloc(r->e, (size_t)nc * sizeof *r->e);
        if (!e) return -ENOMEM;
        r->e = e; r->cap = nc;
    }
    r->e[r->n].obj = obj; r->e[r->n].prop = prop; r->e[r->n].val = val; r->n++;
    return r->n;
}
int drmModeAtomicCommit(int fd, drmModeAtomicReqPtr req, uint32_t flags, void* user) {
    if (!is_kms(fd) || !req) { errno = EINVAL; return -1; }
    int bad = 0;
    pthread_mutex_lock(&mu);
    kms_t s = kms;
    for (int i = 0; i < req->n && !bad; i++)
        if (!prop_belongs(req->e[i].obj, req->e[i].prop) || kset(&s, req->e[i].prop, req->e[i].val)) bad = 1;
    if (!bad && s.fb && (!fb_get(s.fb) || s.crtc != CRTC_ID)) bad = 1;
    if (!bad && s.crtc && s.crtc != CRTC_ID) bad = 1;
    if (!bad && s.conn_crtc && s.conn_crtc != CRTC_ID) bad = 1;
    if (!bad && s.mode) {
        bad = 1;
        for (int i = 0; i < 16; i++) if (blobs[i].id == s.mode) bad = 0;
    }
    if (!bad && !(flags & DRM_MODE_ATOMIC_TEST_ONLY)) kms = s;
    pthread_mutex_unlock(&mu);
    if (bad) { errno = EINVAL; return -1; }
    if (flags & DRM_MODE_ATOMIC_TEST_ONLY) return 0;

    if (s.fb && s.active) present(s.fb);
    if (flags & DRM_MODE_PAGE_FLIP_EVENT) {
        pthread_mutex_lock(&mu);
        if (ev_cnt == 32) { ev_head = (ev_head + 1) % 32; ev_cnt--; }
        evq[(ev_head + ev_cnt++) % 32] = (ev_t){ (uint64_t)(uintptr_t)user, CRTC_ID, ++vseq, now_ns() };
        pthread_mutex_unlock(&mu);
        uint64_t one = 1; (void)!write(fd, &one, 8);
    }
    return 0;
}
int drmIsKMS(int fd) { return is_kms(fd); }

/* ---- symbols requested by Mesa / GBM / Xwayland ---- */
static char* node_path(const char* prefix, const char* dflt) {
    char nm[64] = "";
    if (!node_for(prefix, dflt, nm, sizeof nm)) return NULL;
    char* r = malloc(80);
    if (r) snprintf(r, 80, "/dev/dri/%s", nm);
    return r;
}
/* render node: Mesa no longer requires authentication (drmGetMagic/AuthMagic) */
int drmGetNodeTypeFromFd(int fd) {
    int t = node_type(fd);
    return t < 0 ? -EINVAL : t;
}
char* drmGetPrimaryDeviceNameFromFd(int fd) { (void)fd; return node_path("card", "card0"); }
char* drmGetRenderDeviceNameFromFd(int fd) { (void)fd; return node_path("renderD", "renderD128"); }

int drmGetDeviceFromDevId(dev_t id, uint32_t flags, drmDevicePtr* out) {
    (void)id; (void)flags;
    *out = dev_new();
    return *out ? 0 : -ENOMEM;
}

char* drmGetBusid(int fd) { (void)fd; return strdup("platform:ahb"); }
void drmFreeBusid(const char* b) { free((void*)b); }
int drmGetMagic(int fd, drm_magic_t* m) { (void)fd; if (m) *m = 1; return 0; }
int drmAuthMagic(int fd, drm_magic_t m) { (void)fd; (void)m; return 0; }

drmVersionPtr drmGetLibVersion(int fd) {
    (void)fd;
    drmVersionPtr v = calloc(1, sizeof *v);
    if (!v) return NULL;
    v->version_major = 1; v->version_minor = 4; v->version_patchlevel = 0;
    return v;
}
int drmModeDirtyFB(int fd, uint32_t id, drmModeClipPtr c, uint32_t n) {
    (void)fd; (void)id; (void)c; (void)n; return 0;
}

/* syncobj: DRM_CAP_SYNCOBJ cap returns 0, so Mesa doesn't use them. Stubs to satisfy loading. */
int drmSyncobjCreate(int fd, uint32_t flags, uint32_t* h) { (void)fd; (void)flags; (void)h; return -ENOSYS; }
int drmSyncobjDestroy(int fd, uint32_t h) { (void)fd; (void)h; return -ENOSYS; }
int drmSyncobjHandleToFD(int fd, uint32_t h, int* o) { (void)fd; (void)h; (void)o; return -ENOSYS; }
int drmSyncobjFDToHandle(int fd, int f, uint32_t* h) { (void)fd; (void)f; (void)h; return -ENOSYS; }

int drmSyncobjImportSyncFile(int fd, uint32_t h, int sf) { (void)fd; (void)h; (void)sf; return -ENOSYS; }
int drmSyncobjExportSyncFile(int fd, uint32_t h, int* sf) { (void)fd; (void)h; (void)sf; return -ENOSYS; }
int drmSyncobjWait(int fd, uint32_t* hs, unsigned n, int64_t timeout, unsigned flags, uint32_t* first) {
    (void)fd; (void)hs; (void)n; (void)timeout; (void)flags; (void)first; return -ENOSYS;
}
int drmSyncobjReset(int fd, const uint32_t* hs, uint32_t n) { (void)fd; (void)hs; (void)n; return -ENOSYS; }
int drmSyncobjSignal(int fd, const uint32_t* hs, uint32_t n) { (void)fd; (void)hs; (void)n; return -ENOSYS; }
int drmSyncobjTransfer(int fd, uint32_t dst, uint64_t dst_pt, uint32_t src, uint64_t src_pt, uint32_t flags) {
    (void)fd; (void)dst; (void)dst_pt; (void)src; (void)src_pt; (void)flags; return -ENOSYS;
}
int drmSyncobjTimelineSignal(int fd, const uint32_t* hs, uint64_t* pts, uint32_t n) {
    (void)fd; (void)hs; (void)pts; (void)n; return -ENOSYS;
}
int drmSyncobjTimelineWait(int fd, uint32_t* hs, uint64_t* pts, unsigned n, int64_t timeout,
        unsigned flags, uint32_t* first) {
    (void)fd; (void)hs; (void)pts; (void)n; (void)timeout; (void)flags; (void)first; return -ENOSYS;
}
int drmSyncobjQuery(int fd, uint32_t* hs, uint64_t* pts, uint32_t n) {
    (void)fd; (void)hs; (void)pts; (void)n; return -ENOSYS;
}
int drmSyncobjQuery2(int fd, uint32_t* hs, uint64_t* pts, uint32_t n, uint32_t flags) {
    (void)fd; (void)hs; (void)pts; (void)n; (void)flags; return -ENOSYS;
}
int drmSyncobjEventfd(int fd, uint32_t h, uint64_t pt, int ev_fd, uint32_t flags) {
    (void)fd; (void)h; (void)pt; (void)ev_fd; (void)flags; return -ENOSYS;
}

int drmError(int err, const char* label) {
    switch (err) {
    case DRM_ERR_NO_DEVICE: fprintf(stderr, "%s: no device\n", label); break;
    case DRM_ERR_NO_ACCESS: fprintf(stderr, "%s: no access\n", label); break;
    case DRM_ERR_NOT_ROOT:  fprintf(stderr, "%s: not root\n", label); break;
    case DRM_ERR_INVALID:   fprintf(stderr, "%s: invalid args\n", label); break;
    default:
        if (err < 0) err = -err;
        fprintf(stderr, "%s: error %d (%s)\n", label, err, strerror(err));
        break;
    }
    return 1;
}
static int cmd_ioctl(int fd, unsigned dir, unsigned long idx, void* data, unsigned long size) {
    if (is_ours(fd)) return -ENOTTY;   
    unsigned long req = _IOC(dir, DRM_IOCTL_BASE, DRM_COMMAND_BASE + idx, size);
    return drmIoctl(fd, req, data) ? -errno : 0;
}
int drmCommandNone(int fd, unsigned long idx) {
    return cmd_ioctl(fd, _IOC_NONE, idx, NULL, 0);
}
int drmCommandRead(int fd, unsigned long idx, void* data, unsigned long size) {
    return cmd_ioctl(fd, _IOC_READ, idx, data, size);
}
int drmCommandWrite(int fd, unsigned long idx, void* data, unsigned long size) {
    return cmd_ioctl(fd, _IOC_WRITE, idx, data, size);
}
int drmCommandWriteRead(int fd, unsigned long idx, void* data, unsigned long size) {
    return cmd_ioctl(fd, _IOC_READ | _IOC_WRITE, idx, data, size);
}
