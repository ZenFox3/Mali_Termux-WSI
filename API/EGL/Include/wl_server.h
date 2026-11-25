#pragma once
#include <stdint.h>
struct wlsrv_info { int fd, w, h, stride, off; uint32_t fourcc; };
int  wlsrv_bind(void* wl_display, const char* node);
int  wlsrv_unbind(void* wl_display);
int  wlsrv_info(void* wl_resource, struct wlsrv_info* out);