/* Dynamic-loader indirection for the Mali shims.
 *  - bionic (Termux native): plain dlopen/dlsym/dlerror.
 *  - glibc guest (-DSHIM_GLIBC): libhybris' hybris_dl* loads the Android HAL.
 *    libhybris-common must already be in the process (LD_PRELOAD from
 *    proot-sphal-run), so it is resolved at run time, with no link dependency. */
#ifndef SHIM_DL_H
#define SHIM_DL_H
#include <dlfcn.h>
#include <stdio.h>

#ifdef SHIM_GLIBC
#define SHIM_UNUSED __attribute__((unused))
static void* (*shim_h_open)(const char*, int) SHIM_UNUSED;
static void* (*shim_h_sym)(void*, const char*) SHIM_UNUSED;
static char* (*shim_h_err)(void) SHIM_UNUSED;

static SHIM_UNUSED int shim_dl_ready(void) {
    if (shim_h_open && shim_h_sym && shim_h_err) return 1;
    shim_h_open = (void* (*)(const char*, int))dlsym(RTLD_DEFAULT, "hybris_dlopen");
    shim_h_sym  = (void* (*)(void*, const char*))dlsym(RTLD_DEFAULT, "hybris_dlsym");
    shim_h_err  = (char* (*)(void))dlsym(RTLD_DEFAULT, "hybris_dlerror");
    if (!shim_h_open || !shim_h_sym || !shim_h_err) {
        fprintf(stderr, "shim: libhybris-common not loaded; start through "
                        "/opt/proot-sphal/bin/proot-sphal-run (see run_glibc.sh)\n");
        return 0;
    }
    return 1;
}
static SHIM_UNUSED void* shim_dlopen(const char* p, int f) { return shim_dl_ready() ? shim_h_open(p, f) : NULL; }
static SHIM_UNUSED void* shim_dlsym(void* h, const char* s) { return shim_dl_ready() ? shim_h_sym(h, s) : NULL; }
static SHIM_UNUSED const char* shim_dlerror(void) {
    const char* e = shim_dl_ready() ? shim_h_err() : "libhybris unavailable";
    return e ? e : "(no error)";
}
#else
#define shim_dlopen  dlopen
#define shim_dlsym   dlsym
#define shim_dlerror dlerror
#endif
#endif
