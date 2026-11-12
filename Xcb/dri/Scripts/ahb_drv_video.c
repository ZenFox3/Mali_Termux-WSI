// ahb_drv_video.c : VA-API driver (phase 2: AMediaCodec backend), surfaces backed by AHardwareBuffer
//
// Env:
//   AHB_VA_PROFILES=h264,vp8,vp9,hevc,av1|all|default   (default: ready profiles)
//   AHB_VA_DEBUG=1

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <android/hardware_buffer.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <va/va.h>
#include <va/va_backend.h>

#define MAX_CFG  32
#define MAX_SURF 256
#define MAX_CTX  16
#define MAX_BUF  4096
#define MAX_RENDER 128
#define MAX_PROF 16

#define LOG(...) do { if (g_dbg) fprintf(stderr, "[ahb-va] " __VA_ARGS__); } while (0)

typedef struct { int used; VAProfile prof; VAEntrypoint ep; } cfg_t;
typedef struct { int used; AHardwareBuffer* ahb; unsigned w, h; } surf_t;
typedef struct {
    int used; VAConfigID cfg; int w, h;
    VASurfaceID target; int nrender; VABufferID render[MAX_RENDER];
} ctxt_t;
typedef struct { int used; VABufferType type; unsigned size, num; void* data; } buf_t;

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static int g_dbg;
static VAProfile g_prof[MAX_PROF];
static int g_nprof;
static cfg_t cfgs[MAX_CFG];
static surf_t surfs[MAX_SURF];
static ctxt_t ctxs[MAX_CTX];
static buf_t bufs[MAX_BUF];

/* Forward declarations */
static VAStatus mc_open(unsigned cid, VAProfile prof, int w, int h);
static void mc_close(unsigned cid);
static void mc_forget(unsigned surf);

/* ---- profile selection ---- */
static const struct { const char* key; int n; VAProfile p[3]; int ready; } kmap[] = {
    { "h264", 3, { VAProfileH264ConstrainedBaseline, VAProfileH264Main, VAProfileH264High }, 1 },
    { "vp8",  1, { VAProfileVP8Version0_3 }, 1 },
    { "vp9",  1, { VAProfileVP9Profile0 },   1 },
    { "hevc", 1, { VAProfileHEVCMain },      0 },
    { "av1",  1, { VAProfileAV1Profile0 },   0 },
};

static void add_profiles(const char* key) {
    int dflt = !strcmp(key, "all") || !strcmp(key, "default");
    for (size_t i = 0; i < sizeof kmap / sizeof *kmap; i++) {
        if (dflt) { if (!kmap[i].ready) continue; }
        else if (strcmp(key, kmap[i].key)) continue;
        for (int j = 0; j < kmap[i].n && g_nprof < MAX_PROF; j++) g_prof[g_nprof++] = kmap[i].p[j];
    }
}

static void load_profiles(void) {
    g_nprof = 0;
    const char* e = getenv("AHB_VA_PROFILES");
    if (!e || !*e) e = "default";
    char* s = strdup(e);
    if (!s) return;
    char* sp = NULL;
    for (char* t = strtok_r(s, ",", &sp); t; t = strtok_r(NULL, ",", &sp)) add_profiles(t);
    free(s);
}

static int profile_ok(VAProfile p) {
    for (int i = 0; i < g_nprof; i++) if (g_prof[i] == p) return 1;
    return 0;
}

/* ---- config ---- */
static VAStatus q_profiles(VADriverContextP c, VAProfile* l, int* n) {
    (void)c;
    for (int i = 0; i < g_nprof; i++) l[i] = g_prof[i];
    *n = g_nprof;
    return VA_STATUS_SUCCESS;
}
static VAStatus q_entrypoints(VADriverContextP c, VAProfile p, VAEntrypoint* l, int* n) {
    (void)c;
    if (!profile_ok(p)) { *n = 0; return VA_STATUS_ERROR_UNSUPPORTED_PROFILE; }
    l[0] = VAEntrypointVLD; *n = 1;
    return VA_STATUS_SUCCESS;
}
static VAStatus get_cfg_attribs(VADriverContextP c, VAProfile p, VAEntrypoint ep,
        VAConfigAttrib* a, int n) {
    (void)c;
    if (!profile_ok(p)) return VA_STATUS_ERROR_UNSUPPORTED_PROFILE;
    if (ep != VAEntrypointVLD) return VA_STATUS_ERROR_UNSUPPORTED_ENTRYPOINT;
    for (int i = 0; i < n; i++) {
        if (a[i].type == VAConfigAttribRTFormat) a[i].value = VA_RT_FORMAT_YUV420;
        else a[i].value = VA_ATTRIB_NOT_SUPPORTED;
    }
    return VA_STATUS_SUCCESS;
}
static VAStatus create_config(VADriverContextP c, VAProfile p, VAEntrypoint ep,
        VAConfigAttrib* a, int n, VAConfigID* id) {
    (void)c;
    if (!profile_ok(p)) return VA_STATUS_ERROR_UNSUPPORTED_PROFILE;
    if (ep != VAEntrypointVLD) return VA_STATUS_ERROR_UNSUPPORTED_ENTRYPOINT;
    for (int i = 0; i < n; i++)
        if (a[i].type == VAConfigAttribRTFormat && !(a[i].value & VA_RT_FORMAT_YUV420))
            return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;
    VAStatus r = VA_STATUS_ERROR_ALLOCATION_FAILED;
    pthread_mutex_lock(&mu);
    for (int i = 0; i < MAX_CFG; i++) if (!cfgs[i].used) {
        cfgs[i] = (cfg_t){ 1, p, ep }; *id = i + 1; r = VA_STATUS_SUCCESS; break;
    }
    pthread_mutex_unlock(&mu);
    LOG("CreateConfig profile=%d -> %s\n", p, r ? "FAIL" : "ok");
    return r;
}
static cfg_t* cfg_get(VAConfigID id) {
    return (id >= 1 && id <= MAX_CFG && cfgs[id - 1].used) ? &cfgs[id - 1] : NULL;
}
static VAStatus destroy_config(VADriverContextP c, VAConfigID id) {
    (void)c;
    VAStatus r = VA_STATUS_ERROR_INVALID_CONFIG;
    pthread_mutex_lock(&mu);
    if (cfg_get(id)) { cfgs[id - 1].used = 0; r = VA_STATUS_SUCCESS; }
    pthread_mutex_unlock(&mu);
    return r;
}
static VAStatus q_cfg_attribs(VADriverContextP c, VAConfigID id, VAProfile* p,
        VAEntrypoint* ep, VAConfigAttrib* a, int* n) {
    (void)c;
    VAStatus r = VA_STATUS_ERROR_INVALID_CONFIG;
    pthread_mutex_lock(&mu);
    cfg_t* g = cfg_get(id);
    if (g) {
        *p = g->prof; *ep = g->ep;
        a[0].type = VAConfigAttribRTFormat; a[0].value = VA_RT_FORMAT_YUV420; *n = 1;
        r = VA_STATUS_SUCCESS;
    }
    pthread_mutex_unlock(&mu);
    return r;
}

/* ---- surfaces ---- */
static VAStatus q_surf_attribs(VADriverContextP c, VAConfigID id, VASurfaceAttrib* l, unsigned* n) {
    (void)c;
    if (!cfg_get(id)) return VA_STATUS_ERROR_INVALID_CONFIG;
    VASurfaceAttrib a[5];
    memset(a, 0, sizeof a);
    int vals[5] = { VA_FOURCC_NV12, 16, 8192, 16, 8192 };
    VASurfaceAttribType t[5] = { VASurfaceAttribPixelFormat, VASurfaceAttribMinWidth,
        VASurfaceAttribMaxWidth, VASurfaceAttribMinHeight, VASurfaceAttribMaxHeight };
    for (int i = 0; i < 5; i++) {
        a[i].type = t[i]; a[i].flags = VA_SURFACE_ATTRIB_GETTABLE;
        a[i].value.type = VAGenericValueTypeInteger; a[i].value.value.i = vals[i];
    }
    if (!l) { *n = 5; return VA_STATUS_SUCCESS; }
    if (*n < 5) { *n = 5; return VA_STATUS_ERROR_MAX_NUM_EXCEEDED; }
    memcpy(l, a, sizeof a); *n = 5;
    return VA_STATUS_SUCCESS;
}
static VAStatus create_surfaces2(VADriverContextP c, unsigned fmt, unsigned w, unsigned h,
        VASurfaceID* out, unsigned n, VASurfaceAttrib* at, unsigned nat) {
    (void)c; (void)at; (void)nat;
    if (!(fmt & VA_RT_FORMAT_YUV420)) return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;
    if (!w || !h || w > 8192 || h > 8192) return VA_STATUS_ERROR_INVALID_PARAMETER;
    unsigned done = 0;
    VAStatus r = VA_STATUS_SUCCESS;
    for (; done < n; done++) {
        AHardwareBuffer_Desc d = { .width = w, .height = h, .layers = 1,
            .format = AHARDWAREBUFFER_FORMAT_Y8Cb8Cr8_420,
            .usage = AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN | AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN |
                     AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE };
        AHardwareBuffer* ahb = NULL;
        if (AHardwareBuffer_allocate(&d, &ahb) || !ahb) { r = VA_STATUS_ERROR_ALLOCATION_FAILED; break; }
        int slot = -1;
        pthread_mutex_lock(&mu);
        for (int i = 0; i < MAX_SURF; i++) if (!surfs[i].used) {
            surfs[i] = (surf_t){ 1, ahb, w, h }; slot = i; break;
        }
        pthread_mutex_unlock(&mu);
        if (slot < 0) { AHardwareBuffer_release(ahb); r = VA_STATUS_ERROR_ALLOCATION_FAILED; break; }
        out[done] = slot + 1;
    }
    if (r) {   /* rollback */
        for (unsigned i = 0; i < done; i++) {
            pthread_mutex_lock(&mu);
            surf_t* s = &surfs[out[i] - 1];
            AHardwareBuffer_release(s->ahb); memset(s, 0, sizeof *s);
            pthread_mutex_unlock(&mu);
            out[i] = VA_INVALID_ID;
        }
    }
    LOG("CreateSurfaces %ux%u x%u -> %s\n", w, h, n, r ? "FAIL" : "ok");
    return r;
}
static VAStatus create_surfaces(VADriverContextP c, int w, int h, int fmt, int n, VASurfaceID* out) {
    return create_surfaces2(c, (unsigned)fmt, (unsigned)w, (unsigned)h, out, (unsigned)n, NULL, 0);
}
static VAStatus destroy_surfaces(VADriverContextP c, VASurfaceID* l, int n) {
    (void)c;
    pthread_mutex_lock(&mu);
    for (int i = 0; i < n; i++) {
        if (l[i] < 1 || l[i] > MAX_SURF || !surfs[l[i] - 1].used) continue;
        mc_forget(l[i]);
        AHardwareBuffer_release(surfs[l[i] - 1].ahb);
        memset(&surfs[l[i] - 1], 0, sizeof(surf_t));
    }
    pthread_mutex_unlock(&mu);
    return VA_STATUS_SUCCESS;
}
static VAStatus q_surface_error(VADriverContextP c, VASurfaceID s, VAStatus e, void** info) {
    (void)c; (void)s; (void)e; (void)info; return VA_STATUS_ERROR_UNIMPLEMENTED;
}

/* ---- images ---- */
static VAStatus q_image_formats(VADriverContextP c, VAImageFormat* l, int* n) {
    (void)c;
    memset(&l[0], 0, sizeof l[0]);
    l[0].fourcc = VA_FOURCC_NV12; l[0].byte_order = VA_LSB_FIRST; l[0].bits_per_pixel = 12;
    *n = 1;
    return VA_STATUS_SUCCESS;
}

/* ---- context ---- */
static VAStatus create_context(VADriverContextP c, VAConfigID cfg, int w, int h, int flag,
        VASurfaceID* rt, int nrt, VAContextID* out) {
    (void)c; (void)flag; (void)rt; (void)nrt;
    VAStatus r = VA_STATUS_ERROR_ALLOCATION_FAILED;
    VAProfile prof = VAProfileNone;
    pthread_mutex_lock(&mu);
    cfg_t* g = cfg_get(cfg);
    if (!g) r = VA_STATUS_ERROR_INVALID_CONFIG;
    else for (int i = 0; i < MAX_CTX; i++) if (!ctxs[i].used) {
        memset(&ctxs[i], 0, sizeof ctxs[i]);
        ctxs[i].used = 1; ctxs[i].cfg = cfg; ctxs[i].w = w; ctxs[i].h = h;
        ctxs[i].target = VA_INVALID_ID;
        prof = g->prof; *out = i + 1; r = VA_STATUS_SUCCESS; break;
    }
    pthread_mutex_unlock(&mu);
    if (r == VA_STATUS_SUCCESS && (r = mc_open(*out, prof, w, h))) {
        pthread_mutex_lock(&mu); ctxs[*out - 1].used = 0; pthread_mutex_unlock(&mu);
        *out = VA_INVALID_ID;
    }
    LOG("CreateContext %dx%d -> %s\n", w, h, r ? "FAIL" : "ok");
    return r;
}
static ctxt_t* ctx_get(VAContextID id) {
    return (id >= 1 && id <= MAX_CTX && ctxs[id - 1].used) ? &ctxs[id - 1] : NULL;
}
static VAStatus destroy_context(VADriverContextP c, VAContextID id) {
    (void)c;
    VAStatus r = VA_STATUS_ERROR_INVALID_CONTEXT;
    pthread_mutex_lock(&mu);
    if (ctx_get(id)) { ctxs[id - 1].used = 0; r = VA_STATUS_SUCCESS; }
    pthread_mutex_unlock(&mu);
    if (!r) mc_close(id);
    return r;
}

/* ---- buffers ---- */
static buf_t* buf_get(VABufferID id) {
    return (id >= 1 && id <= MAX_BUF && bufs[id - 1].used) ? &bufs[id - 1] : NULL;
}
static void buf_free_locked(VABufferID id) {
    free(bufs[id - 1].data);
    memset(&bufs[id - 1], 0, sizeof(buf_t));
}
static VAStatus create_buffer(VADriverContextP c, VAContextID ctx, VABufferType type,
        unsigned size, unsigned num, void* data, VABufferID* out) {
    (void)c; (void)ctx;
    size_t total = (size_t)size * num;
    void* mem = calloc(1, total ? total : 1);
    if (!mem) return VA_STATUS_ERROR_ALLOCATION_FAILED;
    if (data && total) memcpy(mem, data, total);
    VAStatus r = VA_STATUS_ERROR_ALLOCATION_FAILED;
    pthread_mutex_lock(&mu);
    for (int i = 0; i < MAX_BUF; i++) if (!bufs[i].used) {
        bufs[i] = (buf_t){ 1, type, size, num, mem };
        *out = i + 1; r = VA_STATUS_SUCCESS; break;
    }
    pthread_mutex_unlock(&mu);
    if (r) free(mem);
    return r;
}
static VAStatus buffer_set_num(VADriverContextP c, VABufferID id, unsigned num) {
    (void)c;
    VAStatus r = VA_STATUS_ERROR_INVALID_BUFFER;
    pthread_mutex_lock(&mu);
    buf_t* b = buf_get(id);
    if (b) { b->num = num; r = VA_STATUS_SUCCESS; }
    pthread_mutex_unlock(&mu);
    return r;
}
static VAStatus map_buffer(VADriverContextP c, VABufferID id, void** p) {
    (void)c;
    VAStatus r = VA_STATUS_ERROR_INVALID_BUFFER;
    pthread_mutex_lock(&mu);
    buf_t* b = buf_get(id);
    if (b) { *p = b->data; r = VA_STATUS_SUCCESS; }
    pthread_mutex_unlock(&mu);
    return r;
}
static VAStatus unmap_buffer(VADriverContextP c, VABufferID id) {
    (void)c;
    pthread_mutex_lock(&mu);
    VAStatus r = buf_get(id) ? VA_STATUS_SUCCESS : VA_STATUS_ERROR_INVALID_BUFFER;
    pthread_mutex_unlock(&mu);
    return r;
}
static VAStatus destroy_buffer(VADriverContextP c, VABufferID id) {
    (void)c;
    VAStatus r = VA_STATUS_ERROR_INVALID_BUFFER;
    pthread_mutex_lock(&mu);
    if (buf_get(id)) { buf_free_locked(id); r = VA_STATUS_SUCCESS; }
    pthread_mutex_unlock(&mu);
    return r;
}

#include "ahb_images.inc"
#include "ahb_mc.inc"

/* ---- decode pipeline ---- */
static VAStatus begin_picture(VADriverContextP c, VAContextID id, VASurfaceID target) {
    (void)c;
    VAStatus r = VA_STATUS_ERROR_INVALID_CONTEXT;
    pthread_mutex_lock(&mu);
    ctxt_t* x = ctx_get(id);
    if (x) {
        if (target < 1 || target > MAX_SURF || !surfs[target - 1].used) r = VA_STATUS_ERROR_INVALID_SURFACE;
        else { x->target = target; x->nrender = 0; r = VA_STATUS_SUCCESS; }
    }
    pthread_mutex_unlock(&mu);
    return r;
}
static VAStatus render_picture(VADriverContextP c, VAContextID id, VABufferID* l, int n) {
    (void)c;
    VAStatus r = VA_STATUS_ERROR_INVALID_CONTEXT;
    pthread_mutex_lock(&mu);
    ctxt_t* x = ctx_get(id);
    if (x) {
        r = VA_STATUS_SUCCESS;
        for (int i = 0; i < n; i++) {
            if (!buf_get(l[i])) { r = VA_STATUS_ERROR_INVALID_BUFFER; break; }
            if (x->nrender >= MAX_RENDER) { r = VA_STATUS_ERROR_MAX_NUM_EXCEEDED; break; }
            x->render[x->nrender++] = l[i];
            LOG("Render buf type=%d size=%u num=%u\n", bufs[l[i] - 1].type, bufs[l[i] - 1].size, bufs[l[i] - 1].num);
        }
    }
    pthread_mutex_unlock(&mu);
    return r;
}
static VAStatus end_picture(VADriverContextP c, VAContextID id) {
    (void)c;
    vbuf bs = {0}; int shown = 1; unsigned target = VA_INVALID_ID;
    VAStatus r = VA_STATUS_ERROR_INVALID_CONTEXT;
    pthread_mutex_lock(&mu);
    ctxt_t* x = ctx_get(id);
    if (x) {
        cfg_t* g = cfg_get(x->cfg);
        target = x->target;
        if (!g) r = VA_STATUS_ERROR_INVALID_CONFIG;
        else if (target == VA_INVALID_ID) r = VA_STATUS_ERROR_INVALID_SURFACE;
        else r = mc_packetize(id, g->prof, x->render, x->nrender, &bs, &shown);
        for (int i = 0; i < x->nrender; i++)
            if (buf_get(x->render[i])) buf_free_locked(x->render[i]);
        x->nrender = 0; x->target = VA_INVALID_ID;
    }
    pthread_mutex_unlock(&mu);
    if (!r) r = mc_submit(id, target, bs.p, bs.n, shown);
    vb_free(&bs);
    return r;
}

/* ---- terminate ---- */
static VAStatus terminate(VADriverContextP c) {
    for (unsigned i = 1; i <= MAX_CTX; i++) mc_close(i);
    pthread_mutex_lock(&mu);
    for (int i = 0; i < MAX_SURF; i++) if (surfs[i].used) AHardwareBuffer_release(surfs[i].ahb);
    for (int i = 0; i < MAX_BUF; i++) if (bufs[i].used) free(bufs[i].data);
    memset(surfs, 0, sizeof surfs); memset(bufs, 0, sizeof bufs);
    memset(ctxs, 0, sizeof ctxs);   memset(cfgs, 0, sizeof cfgs);
    pthread_mutex_unlock(&mu);
    c->pDriverData = NULL;
    return VA_STATUS_SUCCESS;
}

/* generic stub for vtable entries libva requires but we don't implement */
static VAStatus stub_unimpl(void) { return VA_STATUS_ERROR_UNIMPLEMENTED; }
#define STUB(f) v->f = (void*)stub_unimpl

/* ---- entry point: libva looks for __vaDriverInit_1_<minor> ---- */
#define INIT_NAME_(m) __vaDriverInit_1_##m
#define INIT_NAME(m) INIT_NAME_(m)

__attribute__((visibility("default")))
VAStatus INIT_NAME(VA_MINOR_VERSION)(VADriverContextP ctx) {
    g_dbg = getenv("AHB_VA_DEBUG") != NULL;
    load_profiles();
    LOG("init, %d profile(s)\n", g_nprof);

    ctx->version_major = VA_MAJOR_VERSION;
    ctx->version_minor = VA_MINOR_VERSION;
    ctx->max_profiles = MAX_PROF;
    ctx->max_entrypoints = 1;
    ctx->max_attributes = 1;
    ctx->max_image_formats = 1;
    ctx->max_subpic_formats = 1;
    ctx->max_display_attributes = 1;
    ctx->str_vendor = "AHardwareBuffer VA-API driver";

    struct VADriverVTable* v = ctx->vtable;
    v->vaTerminate = terminate;
    v->vaQueryConfigProfiles = q_profiles;
    v->vaQueryConfigEntrypoints = q_entrypoints;
    v->vaGetConfigAttributes = get_cfg_attribs;
    v->vaCreateConfig = create_config;
    v->vaDestroyConfig = destroy_config;
    v->vaQueryConfigAttributes = q_cfg_attribs;
    v->vaCreateSurfaces = create_surfaces;
    v->vaCreateSurfaces2 = create_surfaces2;
    v->vaQuerySurfaceAttributes = q_surf_attribs;
    v->vaDestroySurfaces = destroy_surfaces;
    v->vaCreateContext = create_context;
    v->vaDestroyContext = destroy_context;
    v->vaCreateBuffer = create_buffer;
    v->vaBufferSetNumElements = buffer_set_num;
    v->vaMapBuffer = map_buffer;
    v->vaUnmapBuffer = unmap_buffer;
    v->vaDestroyBuffer = destroy_buffer;
    v->vaBeginPicture = begin_picture;
    v->vaRenderPicture = render_picture;
    v->vaEndPicture = end_picture;
    v->vaSyncSurface = sync_surface;
    v->vaQuerySurfaceStatus = q_surface_status;
    v->vaQuerySurfaceError = q_surface_error;
    v->vaQueryImageFormats = q_image_formats;

    v->vaCreateImage = create_image;
    v->vaDestroyImage = destroy_image;
    v->vaGetImage = get_image;
    v->vaPutImage = put_image;

    STUB(vaDeriveImage);
    STUB(vaSetImagePalette);
    STUB(vaQuerySubpictureFormats);
    STUB(vaCreateSubpicture);
    STUB(vaDestroySubpicture);
    STUB(vaSetSubpictureImage);
    STUB(vaSetSubpictureChromakey);
    STUB(vaSetSubpictureGlobalAlpha);
    STUB(vaAssociateSubpicture);
    STUB(vaDeassociateSubpicture);
    STUB(vaQueryDisplayAttributes);
    STUB(vaGetDisplayAttributes);
    STUB(vaSetDisplayAttributes);

    return VA_STATUS_SUCCESS;
}
