/*
 * gl_atlasdump.c (v2) - prints ASCII glyphs uploaded into the WebRender atlas.
 * Does NOT define GL wrappers; does not conflict with gles_list.h / gl_desktop_ext.c.
 *
 * Include after gl_compat.c (RP) and BEFORE gl_desktop_ext.c.
 *
 * Integration (3 steps):
 *  1) in gl_desktop_ext.c, inside glTexSubImage2D, AFTER the real call, add:
 *         if (t == 0x0DE1) ad_dump(t, l, xo, yo, 0, w, h, f);
 *  2) once p_* pointers are loaded, call once:  ad_install();
 *  3) GLX_ATLAS_DUMP=20 firefox 2>&1 | head -500
 */
#include <stdio.h>
#include <stdlib.h>
#include <dlfcn.h>

typedef void (*AD_ts3)(unsigned, int, int, int, int, int, int, int, unsigned, unsigned, const void*);
typedef void (*AD_gi)(unsigned, int*);
typedef void (*AD_bi)(unsigned, unsigned);
typedef void (*AD_gf)(int, unsigned*);
typedef void (*AD_ftl)(unsigned, unsigned, unsigned, int, int);
typedef void (*AD_ft2)(unsigned, unsigned, unsigned, unsigned, int);
typedef unsigned (*AD_cfs)(unsigned);
typedef void (*AD_rp)(int, int, int, int, unsigned, unsigned, void*);
typedef void (*AD_ps)(unsigned, int);

/* glBindFramebuffer is not in gles_list.h => resolve it manually */
static AD_bi ad_bindfb(void) {
    static AD_bi f;
    if (!f) {
        f = (AD_bi)dlsym(RTLD_DEFAULT, "glBindFramebuffer");
        if (!f) { void* h = dlopen("libGLESv2.so", RTLD_NOW); if (h) f = (AD_bi)dlsym(h, "glBindFramebuffer"); }
    }
    return f;
}

static int ad_left = -1;

static void ad_dump(unsigned target, int level, int x, int y, int z, int w, int h, unsigned fmt) {
    if (ad_left < 0) { const char* e = getenv("GLX_ATLAS_DUMP"); ad_left = e ? atoi(e) : 0; }
    if (ad_left <= 0 || w < 4 || h < 4 || w > 64 || h > 64) return;
    AD_bi bindfb = ad_bindfb();
    if (!bindfb) return;

    int rfb = 0, pbo = 0, tex = 0, pack = 4;
    RP(glGetIntegerv, AD_gi)(0x8CAA, &rfb);                  /* READ_FRAMEBUFFER_BINDING */
    RP(glGetIntegerv, AD_gi)(0x88ED, &pbo);                  /* PIXEL_PACK_BUFFER_BINDING */
    RP(glGetIntegerv, AD_gi)(0x0D05, &pack);                 /* PACK_ALIGNMENT */
    RP(glGetIntegerv, AD_gi)(target == 0x8C1A ? 0x8C1D : 0x8069, &tex);

    static unsigned fbo;
    if (!fbo) RP(glGenFramebuffers, AD_gf)(1, &fbo);
    bindfb(0x8CA8, fbo);                                     /* READ_FRAMEBUFFER */
    if (target == 0x8C1A) RP(glFramebufferTextureLayer, AD_ftl)(0x8CA8, 0x8CE0, (unsigned)tex, level, z);
    else                  RP(glFramebufferTexture2D, AD_ft2)(0x8CA8, 0x8CE0, 0x0DE1, (unsigned)tex, level);

    if (RP(glCheckFramebufferStatus, AD_cfs)(0x8CA8) == 0x8CD5) {
        unsigned char* px = (unsigned char*)malloc((size_t)w * h * 4);
        RP(glBindBuffer, AD_bi)(0x88EB, 0);                  /* PIXEL_PACK_BUFFER = 0 */
        RP(glPixelStorei, AD_ps)(0x0D05, 1);
        RP(glReadPixels, AD_rp)(x, y, w, h, 0x1908, 0x1401, px);
        static const char ramp[] = " .:-=+*#%@";
        int one = (fmt == 0x1903);                           /* GL_RED => R8 */
        fprintf(stderr, "\n[atlas] target=0x%x layer=%d at(%d,%d) %dx%d fmt=0x%x (%s)\n",
                target, z, x, y, w, h, fmt, one ? "R8, R channel" : "4ch, G channel");
        for (int j = 0; j < h; j++) {
            for (int i = 0; i < w; i++) {
                unsigned char v = px[((size_t)j * w + i) * 4 + (one ? 0 : 1)];
                fputc(ramp[v * 9 / 255], stderr);
                fputc(ramp[v * 9 / 255], stderr);
            }
            fputc('\n', stderr);
        }
        if (!one) {
            fprintf(stderr, "[atlas] alpha channel:\n");
            for (int j = 0; j < h; j++) {
                for (int i = 0; i < w; i++) {
                    unsigned char v = px[((size_t)j * w + i) * 4 + 3];
                    fputc(ramp[v * 9 / 255], stderr);
                    fputc(ramp[v * 9 / 255], stderr);
                }
                fputc('\n', stderr);
            }
        }
        free(px);
        RP(glBindBuffer, AD_bi)(0x88EB, (unsigned)pbo);
        RP(glPixelStorei, AD_ps)(0x0D05, pack);
        ad_left--;
    }
    bindfb(0x8CA8, (unsigned)rfb);
}

/* ---- Log GL errors during texture upload/allocation (GLX_ERRLOG=1) ---- */
typedef unsigned (*AD_ge)(void);
typedef void (*AD_tst3)(unsigned, int, unsigned, int, int, int);
typedef void (*AD_tst2)(unsigned, int, unsigned, int, int);
typedef void (*AD_ti3)(unsigned, int, int, int, int, int, int, unsigned, unsigned, const void*);

static int ad_errlog(void) { static int v = -1; if (v < 0) v = getenv("GLX_ERRLOG") != NULL; return v; }
static void ad_pre(void) { if (ad_errlog()) RP(glGetError, AD_ge)(); }   /* clear stale errors */
static void ad_err(const char* fn, unsigned t, unsigned ifmt, unsigned fmt, unsigned ty, int w, int h, int d) {
    if (!ad_errlog()) return;
    static unsigned n;
    ++n;
    if (n <= 40 || !(n % 100))
        fprintf(stderr, "[errlog] #%u %s target=0x%x internal=0x%x format=0x%x type=0x%x %dx%dx%d\n",
                n, fn, t, ifmt, fmt, ty, w, h, d);
    unsigned e = RP(glGetError, AD_ge)();
    if (e) fprintf(stderr, "[errlog] %s target=0x%x internal=0x%x format=0x%x type=0x%x %dx%dx%d -> GL error 0x%x\n",
                   fn, t, ifmt, fmt, ty, w, h, d, e);
}

static AD_ts3  ad_real3;
static AD_tst3 ad_r_tst3;
static AD_tst2 ad_r_tst2;
static AD_ti3  ad_r_ti3;

static void ad_hook3(unsigned t, int l, int x, int y, int z, int w, int h, int d,
                     unsigned f, unsigned ty, const void* p) {
    ad_pre();
    ad_real3(t, l, x, y, z, w, h, d, f, ty, p);
    ad_err("glTexSubImage3D", t, 0, f, ty, w, h, d);
    if (t == 0x8C1A && d == 1) ad_dump(t, l, x, y, z, w, h, f);
}
static void ad_h_tst3(unsigned t, int lv, unsigned i, int w, int h, int d) {
    ad_pre(); ad_r_tst3(t, lv, i, w, h, d); ad_err("glTexStorage3D", t, i, 0, 0, w, h, d);
}
static void ad_h_tst2(unsigned t, int lv, unsigned i, int w, int h) {
    ad_pre(); ad_r_tst2(t, lv, i, w, h); ad_err("glTexStorage2D", t, i, 0, 0, w, h, 1);
}
static void ad_h_ti3(unsigned t, int lv, int i, int w, int h, int d, int b, unsigned f, unsigned ty, const void* p) {
    ad_pre(); ad_r_ti3(t, lv, i, w, h, d, b, f, ty, p); ad_err("glTexImage3D", t, (unsigned)i, f, ty, w, h, d);
}

typedef void (*AD_t2)(unsigned, int, int, int, int, int, unsigned, unsigned, const void*);
static AD_t2 ad_r_ts2, ad_r_ti2;
static void ad_h_ts2(unsigned t, int l, int x, int y, int w, int h, unsigned f, unsigned ty, const void* p) {
    ad_pre(); ad_r_ts2(t, l, x, y, w, h, f, ty, p); ad_err("glTexSubImage2D", t, 0, f, ty, w, h, 1);
    if (t == 0x0DE1) ad_dump(t, l, x, y, 0, w, h, f);
}
static void ad_h_ti2(unsigned t, int l, int i, int w, int h, int b, unsigned f, unsigned ty, const void* p) {
    ad_pre(); ad_r_ti2(t, l, i, w, h, b, f, ty, p); ad_err("glTexImage2D", t, (unsigned)i, f, ty, w, h, 1);
}

static void ad_scan(int tex) {
    AD_bi bindfb = ad_bindfb(); if (!bindfb || tex <= 0) return;
    int rfb = 0, pbo = 0, pack = 4, W = 2048, H = 2048;
    const char* s = getenv("GLX_ATLAS_WH"); if (s) sscanf(s, "%d,%d", &W, &H);
    RP(glGetIntegerv, AD_gi)(0x8CAA, &rfb);
    RP(glGetIntegerv, AD_gi)(0x88ED, &pbo);
    RP(glGetIntegerv, AD_gi)(0x0D05, &pack);
    static unsigned fbo; if (!fbo) RP(glGenFramebuffers, AD_gf)(1, &fbo);
    bindfb(0x8CA8, fbo);
    RP(glFramebufferTexture2D, AD_ft2)(0x8CA8, 0x8CE0, 0x0DE1, (unsigned)tex, 0);
    if (RP(glCheckFramebufferStatus, AD_cfs)(0x8CA8) == 0x8CD5) {
        unsigned char* px = (unsigned char*)malloc((size_t)W * H * 4);
        RP(glBindBuffer, AD_bi)(0x88EB, 0);
        RP(glPixelStorei, AD_ps)(0x0D05, 1);
        RP(glReadPixels, AD_rp)(0, 0, W, H, 0x1908, 0x1401, px);
        long nz = 0; int fx = -1, fy = -1;
        for (int y = 0; y < H; y++) for (int x = 0; x < W; x++)
            if (px[((size_t)y * W + x) * 4]) { nz++; if (fy < 0) { fy = y; fx = x; } }
        fprintf(stderr, "[scan] tex=%d %dx%d nonzero=%ld first=(%d,%d)\n", tex, W, H, nz, fx, fy);
        if (fy >= 0) {
            int x0 = fx > 8 ? fx - 8 : 0;
            static const char ramp[] = " .:-=+*#%@";
            for (int y = fy; y < fy + 24 && y < H; y++) {
                for (int x = x0; x < x0 + 70 && x < W; x++) {
                    char c = ramp[px[((size_t)y * W + x) * 4] * 9 / 255];
                    fputc(c, stderr); fputc(c, stderr);
                }
                fputc('\n', stderr);
            }
        }
        free(px);
        RP(glBindBuffer, AD_bi)(0x88EB, (unsigned)pbo);
        RP(glPixelStorei, AD_ps)(0x0D05, pack);
    }
    bindfb(0x8CA8, (unsigned)rfb);
}

static void ad_install(void) {
    static int tried;
    if (tried) return;
    tried = 1;
    if (!getenv("GLX_ATLAS_DUMP") && !getenv("GLX_ERRLOG")) return;
    ad_real3 = (AD_ts3)p_glTexSubImage3D;
    if (ad_real3) p_glTexSubImage3D = (__typeof__(p_glTexSubImage3D))ad_hook3;
    fprintf(stderr, "[errlog] hooks installed: TexSubImage3D=%s\n", ad_real3 ? "yes" : "NO (p_glTexSubImage3D null)");
    if (getenv("GLX_ERRLOG")) {
        ad_r_tst3 = (AD_tst3)p_glTexStorage3D;
        if (ad_r_tst3) p_glTexStorage3D = (__typeof__(p_glTexStorage3D))ad_h_tst3;
        ad_r_tst2 = (AD_tst2)p_glTexStorage2D;
        if (ad_r_tst2) p_glTexStorage2D = (__typeof__(p_glTexStorage2D))ad_h_tst2;
        ad_r_ts2 = (AD_t2)p_glTexSubImage2D;
        if (ad_r_ts2) p_glTexSubImage2D = (__typeof__(p_glTexSubImage2D))ad_h_ts2;
        ad_r_ti2 = (AD_t2)p_glTexImage2D;
        if (ad_r_ti2) p_glTexImage2D = (__typeof__(p_glTexImage2D))ad_h_ti2;
        ad_r_ti3 = (AD_ti3)p_glTexImage3D;
        if (ad_r_ti3) p_glTexImage3D = (__typeof__(p_glTexImage3D))ad_h_ti3;
    }
}
