#include <wayland-server.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <pthread.h>
#include "wayland-drm-server-protocol.h"
#include "wl_server.h"

typedef struct wb { struct wb* next; struct wl_resource* res; struct wlsrv_info i; } wb;
static wb* g_wb;
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static struct { struct wl_display* d; struct wl_global* g; char node[128]; } B;

static void buf_destroy_req(struct wl_client* c, struct wl_resource* r) { (void)c; wl_resource_destroy(r); }
static const struct wl_buffer_interface buf_impl = { buf_destroy_req };

static void buf_free(struct wl_resource* r) {
    pthread_mutex_lock(&mu);
    for (wb** p = &g_wb; *p; p = &(*p)->next)
        if ((*p)->res == r) { wb* x = *p; *p = x->next; close(x->i.fd); free(x); break; }
    pthread_mutex_unlock(&mu);
}

static void drm_auth(struct wl_client* c, struct wl_resource* r, uint32_t id) {
    (void)c; (void)id; wl_drm_send_authenticated(r);
}
static void drm_create_buffer(struct wl_client* c, struct wl_resource* r, uint32_t id,
        uint32_t name, int32_t w, int32_t h, uint32_t stride, uint32_t fmt) {
    (void)c;(void)id;(void)name;(void)w;(void)h;(void)stride;(void)fmt;
    wl_resource_post_error(r, WL_DRM_ERROR_INVALID_NAME, "GEM names not supported, use PRIME");
}
static void drm_create_planar(struct wl_client* c, struct wl_resource* r, uint32_t id,
        uint32_t name, int32_t w, int32_t h, uint32_t fmt,
        int32_t o0, int32_t s0, int32_t o1, int32_t s1, int32_t o2, int32_t s2) {
    (void)c;(void)id;(void)name;(void)w;(void)h;(void)fmt;
    (void)o0;(void)s0;(void)o1;(void)s1;(void)o2;(void)s2;
    wl_resource_post_error(r, WL_DRM_ERROR_INVALID_NAME, "GEM names not supported, use PRIME");
}
static void drm_create_prime(struct wl_client* c, struct wl_resource* r, uint32_t id,
        int32_t fd, int32_t w, int32_t h, uint32_t fmt,
        int32_t o0, int32_t s0, int32_t o1, int32_t s1, int32_t o2, int32_t s2) {
    (void)o1;(void)s1;(void)o2;(void)s2;
    if ((fmt != WL_DRM_FORMAT_ARGB8888 && fmt != WL_DRM_FORMAT_XRGB8888) || w <= 0 || h <= 0) {
        close(fd);
        wl_resource_post_error(r, WL_DRM_ERROR_INVALID_FORMAT, "invalid format/size");
        return;
    }
    struct wl_resource* br = wl_resource_create(c, &wl_buffer_interface, 1, id);
    wb* x = calloc(1, sizeof *x);
    if (!br || !x) { close(fd); free(x); wl_client_post_no_memory(c); return; }
    x->res = br; x->i = (struct wlsrv_info){ fd, w, h, s0, o0, fmt };
    wl_resource_set_implementation(br, &buf_impl, NULL, buf_free);
    pthread_mutex_lock(&mu); x->next = g_wb; g_wb = x; pthread_mutex_unlock(&mu);
}
static const struct wl_drm_interface drm_impl = {
    drm_auth, drm_create_buffer, drm_create_planar, drm_create_prime };

static void drm_bind(struct wl_client* c, void* data, uint32_t ver, uint32_t id) {
    (void)data;
    struct wl_resource* r = wl_resource_create(c, &wl_drm_interface, ver > 2 ? 2 : (int)ver, id);
    if (!r) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(r, &drm_impl, NULL, NULL);
    wl_drm_send_device(r, B.node);
    wl_drm_send_format(r, WL_DRM_FORMAT_ARGB8888);
    wl_drm_send_format(r, WL_DRM_FORMAT_XRGB8888);
    if (ver >= 2) wl_drm_send_capabilities(r, WL_DRM_CAPABILITY_PRIME);
}

int wlsrv_bind(void* d, const char* node) {
    if (!d || B.d) return 0;
    snprintf(B.node, sizeof B.node, "%s", node && *node ? node : "/dev/dri/renderD128");
    B.g = wl_global_create(d, &wl_drm_interface, 2, NULL, drm_bind);
    if (!B.g) return 0;
    B.d = d; return 1;
}
int wlsrv_unbind(void* d) {
    if (!B.d || B.d != d) return 0;
    wl_global_destroy(B.g); B.g = NULL; B.d = NULL; return 1;
}
int wlsrv_info(void* res, struct wlsrv_info* out) {
    int ok = 0;
    pthread_mutex_lock(&mu);
    for (wb* x = g_wb; x; x = x->next) if (x->res == res) { *out = x->i; ok = 1; break; }
    pthread_mutex_unlock(&mu);
    return ok;
}