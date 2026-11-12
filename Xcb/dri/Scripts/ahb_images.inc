/* ahb_images.inc : included by ahb_drv_video.c AFTER the buffer functions.
 * NV12 images; surface chroma read/written through lockPlanes with real
 * pixelStride/rowStride, so it works whether gralloc gives NV12 or NV21. */

#include <stdint.h>

#define MAX_IMG 64
typedef struct { int used; VABufferID buf; unsigned w, h, pitch; } img_t;
static img_t imgs[MAX_IMG];

static img_t* img_get(VAImageID id) {
    return (id >= 1 && id <= MAX_IMG && imgs[id - 1].used) ? &imgs[id - 1] : NULL;
}
static surf_t* surf_get(VASurfaceID s) {
    return (s >= 1 && s <= MAX_SURF && surfs[s - 1].used) ? &surfs[s - 1] : NULL;
}

static VAStatus create_image(VADriverContextP c, VAImageFormat* f, int w, int h, VAImage* im) {
    if (!f || f->fourcc != VA_FOURCC_NV12) return VA_STATUS_ERROR_INVALID_IMAGE_FORMAT;
    if (w <= 0 || h <= 0 || w > 8192 || h > 8192) return VA_STATUS_ERROR_INVALID_PARAMETER;
    unsigned pitch = ((unsigned)w + 15) & ~15u;
    unsigned size = pitch * (unsigned)h + pitch * (((unsigned)h + 1) / 2);
    VABufferID buf;
    VAStatus r = create_buffer(c, 0, VAImageBufferType, size, 1, NULL, &buf);
    if (r) return r;
    int slot = -1;
    pthread_mutex_lock(&mu);
    for (int i = 0; i < MAX_IMG; i++) if (!imgs[i].used) {
        imgs[i] = (img_t){ 1, buf, (unsigned)w, (unsigned)h, pitch }; slot = i; break;
    }
    pthread_mutex_unlock(&mu);
    if (slot < 0) { destroy_buffer(c, buf); return VA_STATUS_ERROR_ALLOCATION_FAILED; }
    memset(im, 0, sizeof *im);
    im->image_id = slot + 1; im->buf = buf; im->format = *f;
    im->format.byte_order = VA_LSB_FIRST; im->format.bits_per_pixel = 12;
    im->width = (uint16_t)w; im->height = (uint16_t)h; im->data_size = size;
    im->num_planes = 2;
    im->pitches[0] = pitch; im->pitches[1] = pitch;
    im->offsets[0] = 0;     im->offsets[1] = pitch * (unsigned)h;
    LOG("CreateImage %dx%d pitch=%u\n", w, h, pitch);
    return VA_STATUS_SUCCESS;
}

static VAStatus destroy_image(VADriverContextP c, VAImageID id) {
    (void)c;
    VAStatus r = VA_STATUS_ERROR_INVALID_IMAGE;
    pthread_mutex_lock(&mu);
    img_t* im = img_get(id);
    if (im) {
        if (buf_get(im->buf)) buf_free_locked(im->buf);
        memset(im, 0, sizeof *im);
        r = VA_STATUS_SUCCESS;
    }
    pthread_mutex_unlock(&mu);
    return r;
}

/* dir 0 = surface -> image (GetImage), dir 1 = image -> surface (PutImage) */
static VAStatus copy_image(VASurfaceID s, VAImageID id, unsigned w, unsigned h, int dir) {
    VAStatus r = VA_STATUS_SUCCESS;
    pthread_mutex_lock(&mu);
    img_t* im = img_get(id);
    surf_t* sf = surf_get(s);
    if (!im || !buf_get(im->buf)) r = VA_STATUS_ERROR_INVALID_IMAGE;
    else if (!sf) r = VA_STATUS_ERROR_INVALID_SURFACE;
    else {
        if (w > sf->w) w = sf->w;
        if (w > im->w) w = im->w;
        if (h > sf->h) h = sf->h;
        if (h > im->h) h = im->h;
        uint8_t* img = bufs[im->buf - 1].data;
        uint8_t* ic = img + (size_t)im->pitch * im->h;
        AHardwareBuffer_Planes p;
        uint64_t usage = dir ? AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN : AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN;
        int rc = AHardwareBuffer_lockPlanes(sf->ahb, usage, -1, NULL, &p);
        if (rc) r = VA_STATUS_ERROR_OPERATION_FAILED;
        else if (p.planeCount < 3) { AHardwareBuffer_unlock(sf->ahb, NULL); r = VA_STATUS_ERROR_OPERATION_FAILED; }
        else {
            for (unsigned y = 0; y < h; y++) {
                uint8_t* sy = (uint8_t*)p.planes[0].data + (size_t)y * p.planes[0].rowStride;
                uint8_t* iy = img + (size_t)y * im->pitch;
                if (dir) memcpy(sy, iy, w); else memcpy(iy, sy, w);
            }
            for (unsigned cy = 0; cy < (h + 1) / 2; cy++) {
                for (unsigned cx = 0; cx < (w + 1) / 2; cx++) {
                    uint8_t* su = (uint8_t*)p.planes[1].data + (size_t)cy * p.planes[1].rowStride + (size_t)cx * p.planes[1].pixelStride;
                    uint8_t* sv = (uint8_t*)p.planes[2].data + (size_t)cy * p.planes[2].rowStride + (size_t)cx * p.planes[2].pixelStride;
                    uint8_t* iu = ic + (size_t)cy * im->pitch + 2 * cx;
                    if (dir) { *su = iu[0]; *sv = iu[1]; }
                    else     { iu[0] = *su; iu[1] = *sv; }
                }
            }
            AHardwareBuffer_unlock(sf->ahb, NULL);
        }
    }
    pthread_mutex_unlock(&mu);
    return r;
}

static VAStatus get_image(VADriverContextP c, VASurfaceID s, int x, int y,
        unsigned w, unsigned h, VAImageID id) {
    (void)c;
    if (x || y) return VA_STATUS_ERROR_INVALID_PARAMETER;
    return copy_image(s, id, w, h, 0);
}
static VAStatus put_image(VADriverContextP c, VASurfaceID s, VAImageID id,
        int sx, int sy, unsigned sw, unsigned sh, int dx, int dy, unsigned dw, unsigned dh) {
    (void)c;
    if (sx || sy || dx || dy || sw != dw || sh != dh) return VA_STATUS_ERROR_INVALID_PARAMETER;
    return copy_image(s, id, sw, sh, 1);
}

/* debug helper for tests: dlsym this from the driver .so */
__attribute__((visibility("default")))
AHardwareBuffer* ahb_va_surface_ahb(unsigned id) {
    AHardwareBuffer* a = NULL;
    pthread_mutex_lock(&mu);
    surf_t* s = surf_get(id);
    if (s) a = s->ahb;
    pthread_mutex_unlock(&mu);
    return a;
}
