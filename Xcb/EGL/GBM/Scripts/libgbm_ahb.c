// libgbm_ahb.c
#define _GNU_SOURCE
#include <gbm.h>
#include <dlfcn.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdio.h>
#include <android/hardware_buffer.h>

struct gbm_device { int fd; };
struct gbm_bo {
    struct gbm_device* dev;
    AHardwareBuffer* ahb;
    void* anwb;
    uint32_t w, h, fmt, flags, stride, bpp;
    int linear;
    void* ud; void (*ud_destroy)(struct gbm_bo*, void*);
};

typedef void* (*to_anwb_fn)(AHardwareBuffer*);
typedef struct { int version, numFds, numInts; int data[]; } nh_t;
typedef const nh_t* (*gnh_fn)(const AHardwareBuffer*);
static to_anwb_fn to_anwb;
static gnh_fn get_nh;

/* DRM-AHB: hooks into libdrm_ahb (resolved via LD_PRELOAD) */
typedef int  (*drm_reg_fn)(AHardwareBuffer*, uint32_t);
typedef void (*drm_unreg_fn)(AHardwareBuffer*);
static drm_reg_fn drm_reg;
static drm_unreg_fn drm_unreg;

static void init_syms(void) {
    static int done;
    if (done) return;
    done = 1;

    /* DRM-AHB: symbols exist only if the shim's libdrm.so.2 is loaded */
    drm_reg   = (drm_reg_fn)dlsym(RTLD_DEFAULT, "drm_ahb_register");
    drm_unreg = (drm_unreg_fn)dlsym(RTLD_DEFAULT, "drm_ahb_unregister");

    void* h = dlopen("libnativewindow.so", RTLD_NOW);
    if (!h) h = dlopen("/system/lib64/libnativewindow.so", RTLD_NOW);
    if (!h) return;

    static const char* mangled =
        "_ZN7android38AHardwareBuffer_to_ANativeWindowBufferEP15AHardwareBuffer";
    
    /* 1. dlvsym for the private LIBNATIVEWINDOW_PLATFORM symbol */
    to_anwb = (to_anwb_fn)dlvsym(h, mangled, "LIBNATIVEWINDOW_PLATFORM");
    
    /* 2. Standard dlsym fallback */
    if (!to_anwb) to_anwb = (to_anwb_fn)dlsym(h, mangled);
    if (!to_anwb) to_anwb = (to_anwb_fn)dlsym(RTLD_DEFAULT, mangled);

    get_nh = (gnh_fn)dlsym(h, "AHardwareBuffer_getNativeHandle");
}

static int fmt_info(uint32_t f, uint32_t* hal, uint32_t* bpp) {
    switch (f) {
    case GBM_FORMAT_ARGB8888:
    case GBM_FORMAT_XRGB8888:    *hal = 5;    *bpp = 4; return 1;
    case GBM_FORMAT_ABGR8888:    *hal = 1;    *bpp = 4; return 1;
    case GBM_FORMAT_XBGR8888:    *hal = 2;    *bpp = 4; return 1;
    case GBM_FORMAT_BGR888:      *hal = 3;    *bpp = 3; return 1;
    case GBM_FORMAT_RGB565:      *hal = 4;    *bpp = 2; return 1;
    case GBM_FORMAT_ABGR2101010: *hal = 0x2b; *bpp = 4; return 1;
#ifdef GBM_FORMAT_ABGR16161616F
    case GBM_FORMAT_ABGR16161616F: *hal = 0x16; *bpp = 8; return 1;
#endif
    }
    return 0;
}

#define CPU_FLAGS (GBM_BO_USE_LINEAR | GBM_BO_USE_WRITE | GBM_BO_USE_CURSOR)

static struct gbm_bo* bo_new(struct gbm_device* d, uint32_t w, uint32_t h,
                             uint32_t f, uint32_t flags) {
    uint32_t hal, bpp;
    init_syms();
    if (!to_anwb || !fmt_info(f, &hal, &bpp)) { errno = EINVAL; return NULL; }

    uint64_t u = AHARDWAREBUFFER_USAGE_GPU_COLOR_OUTPUT |
                 AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE;
    /* Mali: without CPU access, gralloc selects AFBC (compressed); X11/DRI3 requires linear.
     * Scanout or GBM_FORCE_LINEAR=1 => request CPU_READ|WRITE, which disables AFBC. */
    int linear = (flags & (CPU_FLAGS | GBM_BO_USE_SCANOUT)) || getenv("GBM_FORCE_LINEAR");
    if (linear)
        u |= AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN | AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN;
    /* GBM_EXTRA_USAGE=0x...: extra usage bits (e.g., vendor "no AFBC" flag from Arm's gralloc) */
    const char* xu = getenv("GBM_EXTRA_USAGE");
    if (xu && *xu) u |= strtoull(xu, NULL, 0);
    if (getenv("GBM_DEBUG")) fprintf(stderr, "[gbm] bo %ux%u fmt=0x%x flags=0x%x linear=%d usage=0x%llx\n",
                                     w, h, f, flags, linear, (unsigned long long)u);

    AHardwareBuffer_Desc desc = { .width = w, .height = h, .layers = 1,
                                  .format = hal, .usage = u };
    struct gbm_bo* bo = calloc(1, sizeof *bo);
    if (!bo) return NULL;
    if (AHardwareBuffer_allocate(&desc, &bo->ahb)) {
        free(bo); errno = ENOMEM; return NULL;
    }
    AHardwareBuffer_describe(bo->ahb, &desc);
    bo->anwb = to_anwb(bo->ahb);
    bo->dev = d; bo->w = w; bo->h = h; bo->fmt = f; bo->flags = flags;
    bo->bpp = bpp; bo->stride = desc.stride * bpp; bo->linear = linear;

    /* DRM-AHB: key = gbm_bo_get_handle().u32 (lower 32 bits of the anwb pointer) */
    if (drm_reg) drm_reg(bo->ahb, (uint32_t)(uintptr_t)bo->anwb);
    return bo;
}

/* ---------- device ---------- */
struct gbm_device* gbm_create_device(int fd) {
    struct gbm_device* d = calloc(1, sizeof *d);
    if (d) d->fd = fd;
    return d;
}
void gbm_device_destroy(struct gbm_device* d) { free(d); }
int gbm_device_get_fd(struct gbm_device* d) { return d->fd; }
const char* gbm_device_get_backend_name(struct gbm_device* d) { (void)d; return "ahb"; }
int gbm_device_is_format_supported(struct gbm_device* d, uint32_t f, uint32_t fl) {
    uint32_t a, b; (void)d; (void)fl; return fmt_info(f, &a, &b);
}
int gbm_device_get_format_modifier_plane_count(struct gbm_device* d, uint32_t f, uint64_t m) {
    (void)d; (void)f; (void)m; return 1;
}

/* ---------- bo ---------- */
struct gbm_bo* gbm_bo_create(struct gbm_device* d, uint32_t w, uint32_t h,
                             uint32_t f, uint32_t fl) { return bo_new(d, w, h, f, fl); }
struct gbm_bo* gbm_bo_create_with_modifiers(struct gbm_device* d, uint32_t w, uint32_t h,
        uint32_t f, const uint64_t* m, const unsigned c) {
    (void)m; (void)c; return bo_new(d, w, h, f, GBM_BO_USE_RENDERING | GBM_BO_USE_SCANOUT);
}
struct gbm_bo* gbm_bo_create_with_modifiers2(struct gbm_device* d, uint32_t w, uint32_t h,
        uint32_t f, const uint64_t* m, const unsigned c, uint32_t fl) {
    (void)m; (void)c; return bo_new(d, w, h, f, fl | GBM_BO_USE_SCANOUT);
}
struct gbm_bo* gbm_bo_import(struct gbm_device* d, uint32_t t, void* b, uint32_t fl) {
    (void)d; (void)t; (void)b; (void)fl; errno = ENOSYS; return NULL;
}
void gbm_bo_destroy(struct gbm_bo* bo) {
    if (!bo) return;
    if (bo->ud_destroy) bo->ud_destroy(bo, bo->ud);
    if (drm_unreg) drm_unreg(bo->ahb);   /* DRM-AHB */
    AHardwareBuffer_release(bo->ahb);
    free(bo);
}

uint32_t gbm_bo_get_width(struct gbm_bo* b)  { return b->w; }
uint32_t gbm_bo_get_height(struct gbm_bo* b) { return b->h; }
uint32_t gbm_bo_get_stride(struct gbm_bo* b) { return b->stride; }
uint32_t gbm_bo_get_stride_for_plane(struct gbm_bo* b, int p) { (void)p; return b->stride; }
uint32_t gbm_bo_get_format(struct gbm_bo* b) { return b->fmt; }
uint32_t gbm_bo_get_bpp(struct gbm_bo* b)    { return b->bpp * 8; }
uint32_t gbm_bo_get_offset(struct gbm_bo* b, int p) { (void)b; (void)p; return 0; }
struct gbm_device* gbm_bo_get_device(struct gbm_bo* b) { return b->dev; }
int gbm_bo_get_plane_count(struct gbm_bo* b) { (void)b; return 1; }
uint64_t gbm_bo_get_modifier(struct gbm_bo* b) {
    return b->linear ? 0 /* LINEAR */ : 0x00ffffffffffffffULL /* INVALID */;
}

union gbm_bo_handle gbm_bo_get_handle(struct gbm_bo* b) {
    union gbm_bo_handle h; memset(&h, 0, sizeof h);
    h.ptr = b->anwb;               /* for the EGL shim; .u32 = key registered in drm_ahb_register */
    return h;
}
union gbm_bo_handle gbm_bo_get_handle_for_plane(struct gbm_bo* b, int p) {
    (void)p; return gbm_bo_get_handle(b);
}
int gbm_bo_get_fd(struct gbm_bo* b) {
    init_syms();
    if (!get_nh) return -1;
    const nh_t* h = get_nh(b->ahb);
    if (!h || h->numFds <= 0) return -1;

    if (getenv("GBM_DEBUG")) {
        fprintf(stderr, "[gbm] native_handle: numFds=%d numInts=%d\n",
                h->numFds, h->numInts);
        for (int i = 0; i < h->numFds; i++) {
            char p[64], l[256] = "?";
            snprintf(p, sizeof p, "/proc/self/fd/%d", h->data[i]);
            ssize_t n = readlink(p, l, sizeof l - 1);
            if (n > 0) l[n] = 0;
            fprintf(stderr, "[gbm]   fd[%d]=%d -> %s\n", i, h->data[i], l);
        }
    }

    int idx = -1;
    const char* e = getenv("GBM_FD_INDEX");
    if (e) {
        idx = atoi(e);
    } else {
        for (int i = 0; i < h->numFds; i++) {
            char p[64], l[256] = "";
            snprintf(p, sizeof p, "/proc/self/fd/%d", h->data[i]);
            ssize_t n = readlink(p, l, sizeof l - 1);
            if (n > 0) {
                l[n] = 0;
                if (!strncmp(l, "/dmabuf", 7)) { idx = i; break; }
            }
        }
    }
    if (idx < 0 || idx >= h->numFds) return -1;
    return dup(h->data[idx]);
}
int gbm_bo_get_fd_for_plane(struct gbm_bo* b, int p) { (void)p; return gbm_bo_get_fd(b); }

void* gbm_bo_map(struct gbm_bo* bo, uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                 uint32_t flags, uint32_t* stride, void** map_data) {
    uint64_t u = 0; void* p = NULL; (void)w; (void)h;
    if (flags & GBM_BO_TRANSFER_READ)  u |= AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN;
    if (flags & GBM_BO_TRANSFER_WRITE) u |= AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN;
    if (!u || AHardwareBuffer_lock(bo->ahb, u, -1, NULL, &p) || !p) {
        errno = EINVAL; return NULL;
    }
    *stride = bo->stride; *map_data = bo;
    return (uint8_t*)p + (size_t)y * bo->stride + (size_t)x * bo->bpp;
}
void gbm_bo_unmap(struct gbm_bo* bo, void* map_data) {
    (void)map_data; AHardwareBuffer_unlock(bo->ahb, NULL);
}
int gbm_bo_write(struct gbm_bo* bo, const void* buf, size_t n) {
    void* p = NULL;
    if (AHardwareBuffer_lock(bo->ahb, AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN, -1, NULL, &p) || !p)
        return -1;
    memcpy(p, buf, n);
    AHardwareBuffer_unlock(bo->ahb, NULL);
    return 0;
}
void gbm_bo_set_user_data(struct gbm_bo* b, void* d, void (*fn)(struct gbm_bo*, void*)) {
    b->ud = d; b->ud_destroy = fn;
}
void* gbm_bo_get_user_data(struct gbm_bo* b) { return b->ud; }

/* extra helpers for the EGL shim */
AHardwareBuffer* gbm_bo_get_ahb(struct gbm_bo* b)  { return b->ahb; }
void*            gbm_bo_get_anwb(struct gbm_bo* b) { return b->anwb; }

/* ---------- surface: 3-BO pool ---------- */
#define POOL 3
enum { S_FREE, S_RENDER, S_QUEUED, S_LOCKED };

struct gbm_surface {
    struct gbm_device* dev;
    uint32_t w, h, fmt, flags;
    struct gbm_bo* bo[POOL];
    int st[POOL];
    unsigned seq[POOL], counter;
};

static struct gbm_surface* surf_new(struct gbm_device* d, uint32_t w, uint32_t h,
                                    uint32_t f, uint32_t fl) {
    struct gbm_surface* s = calloc(1, sizeof *s);
    if (!s) return NULL;
    s->dev = d; s->w = w; s->h = h; s->fmt = f; s->flags = fl;
    for (int i = 0; i < POOL; i++) {
        s->bo[i] = bo_new(d, w, h, f, fl);
        if (!s->bo[i]) {
            for (int j = 0; j < i; j++) gbm_bo_destroy(s->bo[j]);
            free(s); return NULL;
        }
    }
    return s;
}
struct gbm_surface* gbm_surface_create(struct gbm_device* d, uint32_t w, uint32_t h,
                                       uint32_t f, uint32_t fl) { return surf_new(d, w, h, f, fl); }
struct gbm_surface* gbm_surface_create_with_modifiers(struct gbm_device* d, uint32_t w,
        uint32_t h, uint32_t f, const uint64_t* m, const unsigned c) {
    (void)m; (void)c; return surf_new(d, w, h, f, GBM_BO_USE_RENDERING | GBM_BO_USE_SCANOUT);
}
struct gbm_surface* gbm_surface_create_with_modifiers2(struct gbm_device* d, uint32_t w,
        uint32_t h, uint32_t f, const uint64_t* m, const unsigned c, uint32_t fl) {
    (void)m; (void)c; return surf_new(d, w, h, f, fl | GBM_BO_USE_SCANOUT);
}
void gbm_surface_destroy(struct gbm_surface* s) {
    if (!s) return;
    for (int i = 0; i < POOL; i++) gbm_bo_destroy(s->bo[i]);
    free(s);
}
int gbm_surface_has_free_buffers(struct gbm_surface* s) {
    if (!s) return 0;
    for (int i = 0; i < POOL; i++) if (s->st[i] == S_FREE) return 1;
    return 0;
}
struct gbm_bo* gbm_surface_ahb_dequeue(struct gbm_surface* s) {
    if (!s) return NULL;
    for (int i = 0; i < POOL; i++)
        if (s->st[i] == S_FREE) { s->st[i] = S_RENDER; return s->bo[i]; }
    int old = -1;
    for (int i = 0; i < POOL; i++)
        if (s->st[i] == S_QUEUED && (old < 0 || s->seq[i] < s->seq[old])) old = i;
    if (old < 0) return NULL;
    s->st[old] = S_RENDER;
    return s->bo[old];
}
void gbm_surface_ahb_queue(struct gbm_surface* s, struct gbm_bo* bo) {
    if (!s || !bo) return;
    for (int i = 0; i < POOL; i++)
        if (s->bo[i] == bo) { s->st[i] = S_QUEUED; s->seq[i] = ++s->counter; break; }
}
int gbm_surface_ahb_size(struct gbm_surface* s, uint32_t* w, uint32_t* h) {
    if (!s || !w || !h) return -1;
    *w = s->w; *h = s->h; return 0;
}
struct gbm_bo* gbm_surface_lock_front_buffer(struct gbm_surface* s) {
    if (!s) return NULL;
    int best = -1;
    for (int i = 0; i < POOL; i++)
        if (s->st[i] == S_QUEUED && (best < 0 || s->seq[i] < s->seq[best])) best = i;
    if (best < 0) return NULL;
    s->st[best] = S_LOCKED;
    return s->bo[best];
}
void gbm_surface_release_buffer(struct gbm_surface* s, struct gbm_bo* bo) {
    if (!s || !bo) return;
    for (int i = 0; i < POOL; i++)
        if (s->bo[i] == bo) s->st[i] = S_FREE;
}
