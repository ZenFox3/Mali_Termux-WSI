#pragma once
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <dlfcn.h>
#include <sys/types.h>
#include <android/dlext.h>

#define MALI_SPHAL_DEFAULT "/vendor/lib64/egl/libGLES_mali.so"

typedef void* (*mali_get_ns_fn)(const char*);
typedef void* (*mali_sphal_fn)(const char*, int);

static inline int mali_debug_enabled(void) {
    const char* env_gl = getenv("GL_DEBUG");
    const char* env_mali = getenv("MALI_DEBUG");
    return (env_gl && env_gl[0] != '0') || (env_mali && env_mali[0] != '0');
}

static inline void* mali_sphal_load(const char* path) {
    const int debug = mali_debug_enabled();

    
    void* l = dlopen("libdl_android.so", RTLD_NOW);
    if (l) {
        mali_get_ns_fn get_ns = (mali_get_ns_fn)dlsym(l, "android_get_exported_namespace");
        void* ns = get_ns ? get_ns("sphal") : NULL;

        if (ns) {
            android_dlextinfo info = {
                .flags = ANDROID_DLEXT_USE_NAMESPACE,
                .library_namespace = (struct android_namespace_t*)ns
            };
            void* h = android_dlopen_ext(path, RTLD_NOW, &info);
            if (h) {
                if (debug) fprintf(stderr, "[shim] Dlopen via sphal namespace OK: %p\n", h);
                
                return h;
            }
            if (debug) fprintf(stderr, "[shim] dlopen_ext failed: %s\n", dlerror());
        }
      
    }

    
    void* v = dlopen("libvndksupport.so", RTLD_NOW);
    if (v) {
        mali_sphal_fn f = (mali_sphal_fn)dlsym(v, "android_load_sphal_library");
        if (f) {
            void* h = f(path, RTLD_NOW);
            if (h) {
                if (debug) fprintf(stderr, "[shim] VNDK load OK: %p\n", h);
                
                return h;
            }
        }
        if (debug) fprintf(stderr, "[shim] VNDK fallback failed\n");
    }

    if (debug) fprintf(stderr, "[shim] SPHAL completely unavailable\n");
    return NULL;
}