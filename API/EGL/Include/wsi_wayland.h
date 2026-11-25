/* wsi_wayland.h - Wayland transport for egl_x11_shim (linux-dmabuf + private queue) */
#ifndef WSI_WAYLAND_H
#define WSI_WAYLAND_H
#include <stdint.h>

typedef struct wl_wsi wl_wsi;
typedef void (*wl_wsi_release_fn)(void* user, int slot);

/* true if p looks like a wl_display* (first field = &wl_display_interface) */
int   wl_wsi_is_wl_display(void* p);

/* Own connection (when the app passes native == NULL) */
void* wl_wsi_connect(void);
void  wl_wsi_disconnect(void* wl_display);

/* wl_egl_window (the app creates it with the real libwayland-egl) */
int  wl_wsi_window_info(void* win, int* w, int* h, void** wl_surface);
void wl_wsi_window_hook(void* win, void* user,
                        void (*on_resize)(void* user, int w, int h),
                        void (*on_destroy)(void* user));
void wl_wsi_window_unhook(void* win, void* user);

/* Per-surface state */
wl_wsi* wl_wsi_create(void* wl_display, void* wl_surface,
                      wl_wsi_release_fn rel, void* user);
int  wl_wsi_format_ok(wl_wsi* w, uint32_t fourcc);
/* Takes ownership of dfd (always closes it). 0 = OK */
int  wl_wsi_add_buffer(wl_wsi* w, int slot, int dfd, int width, int height,
                       uint32_t stride, uint32_t fourcc);
void wl_wsi_free_buffer(wl_wsi* w, int slot);
int  wl_wsi_start(wl_wsi* w);          /* thread only if SHIM_WL_THREAD=1 */
int  wl_wsi_pump(wl_wsi* w, int timeout_ms);       /* app thread: read+dispatch our queue */
int  wl_wsi_wait_frame(wl_wsi* w, int timeout_ms); /* block until previous frame callback */
void wl_wsi_present(wl_wsi* w, int slot, int vsync);
void wl_wsi_set_dead(wl_wsi* w);       /* wl_egl_window was destroyed */
void wl_wsi_stop(wl_wsi* w);           /* stops the thread, idempotent */
void wl_wsi_destroy(wl_wsi* w);

#endif
