/*
 * gl_es_emul.c - included by glx_shim.c AFTER gl_compat.c.
 *   glGetString / glGetStringi / glGetIntegerv : ES1 -> "1.5 (...)", ES2 -> "2.1 (...)" + GLSL 1.20,
 *                                                ES3 -> "3.2 (Core Profile) (...)" or "4.3 (Core Profile) (...)"
 *   glShaderSource (ES2/ES3 ctx):
 *       GLSL 1.30-1.50/4.30 -> GLSL ES 3.20 (in/out, texture(), layout...)
 *       GLSL 1.10/1.20 (or no #version) -> GLSL ES 1.00 (attribute/varying/gl_FragColor/texture2D)
 *       #version ... es / #version 100 -> unchanged
 *       sampler1D / sampler2DRect / sampler1DArray -> mapped to sampler2D
 *   glCompressedTex[Sub]Image2D/3D: S3TC (DXT1/3/5, +sRGB), RGTC (BC4/BC5) and BPTC (BC6H/BC7) decoded on CPU
 *   glClearBufferData / glClearBufferSubData: emulation over ES 3.1 via glMapBufferRange.
 * Debug: GL_DEBUG=1 / MALI_DEBUG=1 / EGL_DEBUG=1, GLX_SHADER_DUMP=1 (shaders sent to Mali, on stderr), GLX_TRACE=1 (queries on stdout).
 * Extra: GLX_HIDE_EXT="GL_EXT_1,GL_EXT_2" for filtering problematic extensions.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <pthread.h>

#define BCDEC_IMPLEMENTATION
#include "bcdec.h"

typedef const unsigned char* (*F_gstr)(unsigned);
typedef void (*F_shsrc)(unsigned, int, const char* const*, const int*);

/* ---------- Debug Logging Helper ---------- */
static inline int gl_emul_debug_enabled(void) {
    const char* env_gl = getenv("GL_DEBUG");
    const char* env_mali = getenv("MALI_DEBUG");
    const char* env_egl = getenv("EGL_DEBUG");
    return (env_gl && env_gl[0] != '0') || 
           (env_mali && env_mali[0] != '0') || 
           (env_egl && env_egl[0] != '0');
}

/* 0 = no emulation: native ES context (strings and GLSL driver features remain untouched) */
static int emu_ver(void) { return (T_glx.ctx && !T_glx.ctx->es_native) ? T_glx.ctx->ver : 0; }

/* ===== A) OpenGL 4.3 Spoofing Check ===== */
static const int gl_emu_valid[] = { 11, 12, 13, 14, 15, 20, 21, 30, 32, 33, 40, 41, 42, 43 };
#define GL_EMU_COMPAT_ES1  15      /* ES1 ctx -> "1.5" */
#define GL_EMU_COMPAT_ES2  21      /* ES2 ctx -> "2.1" */
#define GL_EMU_COMPAT_ES3  32
static int gl_emu_max_core(void) { return gl_emu_valid[sizeof gl_emu_valid / sizeof gl_emu_valid[0] - 1]; }

static int gl_emu_ver(void) {
    static int envv = -2;
    if (envv == -2) { const char* e = getenv("GLX_GL"); envv = e ? atoi(e) : -1; }
    
    int want;
    if (envv > 0) {
        want = envv;
    } else if (T_glx.ctx && T_glx.ctx->want_ver) {
        want = T_glx.ctx->want_ver;
    } else {
        int det = ae_detect();
        if (det == 1) want = 15;
        else if (det == 2) want = 43;
        else want = 43;
    }
    
    if (want == 121) return 121;
    
    int best = gl_emu_valid[0];
    for (unsigned i = 0; i < sizeof gl_emu_valid / sizeof gl_emu_valid[0]; i++)
        if (gl_emu_valid[i] <= want) best = gl_emu_valid[i];
    int ev = emu_ver();
    int pm = T_glx.ctx ? T_glx.ctx->profile_mask : 0;

    if (envv <= 0) {
        /* compat: 2.0 .. 3.2 */
        if (pm == 2 && best > GL_EMU_COMPAT_ES3) best = GL_EMU_COMPAT_ES3;
        /* core: 3.3 .. 4.3 (only if the underlying context is ES3) */
        if (pm == 1 && ev >= 3 && best < 33) best = 33;
    }

    if (ev == 1 && best > GL_EMU_COMPAT_ES1) best = GL_EMU_COMPAT_ES1;
    if (ev == 2 && best > GL_EMU_COMPAT_ES2) best = GL_EMU_COMPAT_ES2;
    return best;
}

/* return: 0 = fixed-function, 1 = core, 2 = compat */
static int gl_emu_profile(int g) {
    if (g == 121 || g < 20) return 0;
    if (g <= 32) return 2;
    int pm = T_glx.ctx ? T_glx.ctx->profile_mask : 0;
    if (getenv("GLX_GL")) return 1;
    return pm == 2 ? 2 : 1;
}

static const char* gl_glsl_for(int g) {
    if (ae_detect() == 1) return "";
    
    switch (g) { 
        case 11:  return "";
        case 12:  return "";
        case 121: return "";
        case 13:  return "";
        case 14:  return "";
        case 15:  return "";
        case 20:  return "1.10"; 
        case 21:  return "1.20"; 
        case 30:  return "1.30";
        case 32:  return "1.50";
        case 33:  return "3.30"; 
        case 40:  return "4.00"; 
        case 41:  return "4.10";
        case 42:  return "4.20"; 
        case 43:  return "4.30";
        default:  return "1.20"; 
    }
}

static int gl43(void) { return gl_emu_ver() >= 43; }

/* ===== B) ARB extensions genuinely covered by ES 3.2 ===== */
static const char* const emu_arb3[] = {
    "GL_ARB_vertex_array_object", "GL_ARB_framebuffer_object", "GL_ARB_texture_storage",
    "GL_ARB_draw_elements_base_vertex", "GL_ARB_copy_image", "GL_ARB_buffer_storage", "GL_ARB_multi_bind",
    "GL_ARB_ES2_compatibility", "GL_ARB_ES3_compatibility", "GL_ARB_vertex_buffer_object",
    "GL_ARB_uniform_buffer_object", "GL_ARB_sampler_objects", "GL_ARB_instanced_arrays",
    "GL_ARB_draw_instanced", "GL_ARB_map_buffer_range", "GL_ARB_texture_rg",
    "GL_ARB_texture_non_power_of_two", "GL_ARB_depth_texture", "GL_ARB_texture_cube_map_array",
    "GL_EXT_framebuffer_object",
    "GL_ARB_transform_feedback2", "GL_ARB_ES3_2_compatibility", "GL_ARB_copy_buffer",
    "GL_ARB_texture_compression", "GL_EXT_texture_compression_s3tc", 
    "GL_ARB_sync", "GL_ARB_texture_float", "GL_ARB_half_float_pixel", "GL_EXT_texture_compression_dxt1",
    "GL_EXT_texture_compression_s3tc_srgb", "GL_EXT_texture_compression_rgtc", "GL_ARB_texture_compression_rgtc",
    "GL_ARB_texture_compression_bptc", "GL_ATI_meminfo", "GL_NVX_gpu_memory_info",
    "GL_EXT_blend_func_extended",
    "GL_ARB_compute_shader", "GL_ARB_shader_storage_buffer_object", "GL_ARB_shader_image_load_store",
    "GL_ARB_shader_atomic_counters", "GL_ARB_program_interface_query", "GL_ARB_explicit_uniform_location",
    "GL_ARB_vertex_attrib_binding", "GL_ARB_framebuffer_no_attachments", "GL_ARB_texture_storage_multisample",
    "GL_ARB_draw_indirect", "GL_ARB_ES3_1_compatibility", "GL_ARB_clear_buffer_object",
};

#define EMU_ALL  ((int)(sizeof emu_arb3 / sizeof emu_arb3[0]))
#define EMU_N3   (gl43() ? EMU_ALL : EMU_ALL - 12)

static const char emu_arb1[] =
    " GL_ARB_multitexture GL_ARB_vertex_buffer_object GL_ARB_texture_env_combine"
    " GL_ARB_texture_non_power_of_two GL_ARB_point_sprite GL_EXT_framebuffer_object"
    " GL_SGIS_generate_mipmap GL_ARB_texture_compression GL_EXT_texture_compression_s3tc"
    " GL_EXT_texture_compression_dxt1"
    " GL_ATI_meminfo";

static const char emu_arb2[] =
    " GL_ARB_shader_objects GL_ARB_vertex_shader GL_ARB_fragment_shader GL_ARB_shading_language_100"
    " GL_ARB_vertex_buffer_object GL_ARB_multitexture GL_ARB_texture_non_power_of_two"
    " GL_ARB_texture_cube_map GL_ARB_ES2_compatibility GL_EXT_framebuffer_object"
    " GL_ARB_texture_compression GL_EXT_texture_compression_s3tc GL_EXT_texture_compression_dxt1"
    " GL_EXT_texture_compression_s3tc_srgb GL_EXT_texture_compression_rgtc GL_ARB_texture_compression_rgtc"
    " GL_ARB_texture_compression_bptc"
    " GL_ATI_meminfo";

/* ---------- S3TC / RGTC / BPTC Decoder Helpers ---------- */
typedef void (*F_cti2)(unsigned, int, unsigned, int, int, int, int, const void*);
typedef void (*F_ctsi2)(unsigned, int, int, int, int, int, unsigned, int, const void*);
typedef void (*F_cti3)(unsigned, int, unsigned, int, int, int, int, int, const void*);
typedef void (*F_ctsi3)(unsigned, int, int, int, int, int, int, int, unsigned, int, const void*);
typedef void (*F_ti3)(unsigned, int, int, int, int, int, int, unsigned, unsigned, const void*);
typedef void (*F_tsi3)(unsigned, int, int, int, int, int, int, int, unsigned, unsigned, const void*);
typedef void (*F_pstore)(unsigned, int);
typedef void* (*F_mapr)(unsigned, long, long, unsigned);
typedef unsigned char (*F_unmap)(unsigned);

typedef struct { int kind, srgb, sgn, bs, bpp; } cs_fmt;

static const unsigned cs_all[] = { 0x83F0, 0x83F1, 0x83F2, 0x83F3, 0x8C4C, 0x8C4D, 0x8C4E, 0x8C4F,
                                   0x8DBB, 0x8DBC, 0x8DBD, 0x8DBE,
                                   0x8E8C, 0x8E8D, 0x8E8E, 0x8E8F };
static int cs_count(int ver) { return ver == 1 ? 4 : (int)(sizeof cs_all / sizeof cs_all[0]); }

/* ---------- ETC1 / ETC2 / EAC (native on Mali) ---------- */
static const unsigned etc_all[] = {
    0x8D64,
    0x9274, 0x9275, 0x9276, 0x9277,
    0x9278, 0x9279,
    0x9270, 0x9271, 0x9272, 0x9273
};

static int etc_is(unsigned f) {
    return f == 0x8D64 || (f >= 0x9270 && f <= 0x9279);
}

static void etc_dbg(unsigned fmt) {
    static int seen;
    if (!seen && etc_is(fmt) && gl_emul_debug_enabled()) {
        seen = 1;
        fprintf(stderr, "[gl_emul] ETC/EAC format 0x%x passed through to native driver\n", fmt);
    }
}

static int cs_merge(int ver, unsigned** out) {
    int n = 0;
    RP(glGetIntegerv, F_geti)(0x86A2, &n);
    if (n < 0) n = 0;
    int ncs = cs_count(ver);
    int ncand = ncs + (ver == 3 ? (int)(sizeof etc_all / sizeof etc_all[0]) : 0);
    unsigned* l = malloc(((size_t)n + (size_t)ncand + 1) * sizeof *l);
    *out = l;
    if (!l) return 0;
    if (n) RP(glGetIntegerv, F_geti)(0x86A3, (int*)l);
    int total = n;
    for (int i = 0; i < ncand; i++) {
        unsigned f = i < ncs ? cs_all[i] : etc_all[i - ncs];
        int dup = 0;
        for (int j = 0; j < total; j++) if (l[j] == f) { dup = 1; break; }
        if (!dup) l[total++] = f;
    }
    if (gl_emul_debug_enabled() && total > n)
        fprintf(stderr, "[gl_emul] Compressed format list: %d native + %d emulated/advertised\n", n, total - n);
    return total;
}

static int cs_lookup(unsigned f, int ver, cs_fmt* o) {
    switch (f) {
    case 0x83F0: *o = (cs_fmt){ 1, 0, 0,  8, 4 }; break;
    case 0x83F1: *o = (cs_fmt){ 2, 0, 0,  8, 4 }; break;
    case 0x83F2: *o = (cs_fmt){ 3, 0, 0, 16, 4 }; break;
    case 0x83F3: *o = (cs_fmt){ 4, 0, 0, 16, 4 }; break;
    case 0x8C4C: *o = (cs_fmt){ 1, 1, 0,  8, 4 }; break;
    case 0x8C4D: *o = (cs_fmt){ 2, 1, 0,  8, 4 }; break;
    case 0x8C4E: *o = (cs_fmt){ 3, 1, 0, 16, 4 }; break;
    case 0x8C4F: *o = (cs_fmt){ 4, 1, 0, 16, 4 }; break;
    case 0x8DBB: *o = (cs_fmt){ 5, 0, 0,  8, 1 }; break;
    case 0x8DBC: *o = (cs_fmt){ 5, 0, 1,  8, 1 }; break;
    case 0x8DBD: *o = (cs_fmt){ 6, 0, 0, 16, 2 }; break;
    case 0x8DBE: *o = (cs_fmt){ 6, 0, 1, 16, 2 }; break;
    case 0x8E8C: *o = (cs_fmt){ 7, 0, 0, 16, 4 }; break;
    case 0x8E8D: *o = (cs_fmt){ 7, 1, 0, 16, 4 }; break;
    case 0x8E8E: *o = (cs_fmt){ 8, 0, 1, 16, 12 }; break;
    case 0x8E8F: *o = (cs_fmt){ 8, 0, 0, 16, 12 }; break;
    default: return 0;
    }
    if (ver == 1 && (o->srgb || o->kind > 4)) return 0;
    return 1;
}

static void bc_565(unsigned c, int* o) {
    int r = (c >> 11) & 31, g = (c >> 5) & 63, b = c & 31;
    o[0] = (r << 3) | (r >> 2); o[1] = (g << 2) | (g >> 4); o[2] = (b << 3) | (b >> 2);
}

static void bc1_block(const uint8_t* s, int force4, uint8_t px[16][4]) {
    unsigned c0 = (unsigned)s[0] | ((unsigned)s[1] << 8), c1 = (unsigned)s[2] | ((unsigned)s[3] << 8);
    int p[4][4];
    bc_565(c0, p[0]); bc_565(c1, p[1]);
    p[0][3] = p[1][3] = 255;
    if (c0 > c1 || force4) {
        for (int k = 0; k < 3; k++) { p[2][k] = (2 * p[0][k] + p[1][k]) / 3; p[3][k] = (p[0][k] + 2 * p[1][k]) / 3; }
        p[2][3] = p[3][3] = 255;
    } else {
        for (int k = 0; k < 3; k++) { p[2][k] = (p[0][k] + p[1][k]) / 2; p[3][k] = 0; }
        p[2][3] = 255; p[3][3] = 0;
    }
    uint32_t idx = (uint32_t)s[4] | ((uint32_t)s[5] << 8) | ((uint32_t)s[6] << 16) | ((uint32_t)s[7] << 24);
    for (int i = 0; i < 16; i++) {
        const int* c = p[(idx >> (2 * i)) & 3];
        px[i][0] = (uint8_t)c[0]; px[i][1] = (uint8_t)c[1]; px[i][2] = (uint8_t)c[2]; px[i][3] = (uint8_t)c[3];
    }
}

static void bc4_block(const uint8_t* s, int sgn, uint8_t out[16]) {
    int a0, a1, v[8];
    int mn = sgn ? -127 : 0, mx = sgn ? 127 : 255;
    if (sgn) { a0 = (int8_t)s[0]; a1 = (int8_t)s[1]; if (a0 < -127) a0 = -127; if (a1 < -127) a1 = -127; }
    else { a0 = s[0]; a1 = s[1]; }
    v[0] = a0; v[1] = a1;
    if (a0 > a1) for (int i = 2; i < 8; i++) v[i] = ((8 - i) * a0 + (i - 1) * a1) / 7;
    else { for (int i = 2; i < 6; i++) v[i] = ((6 - i) * a0 + (i - 1) * a1) / 5; v[6] = mn; v[7] = mx; }
    uint64_t bits = 0;
    for (int i = 0; i < 6; i++) bits |= (uint64_t)s[2 + i] << (8 * i);
    for (int i = 0; i < 16; i++) out[i] = (uint8_t)v[(bits >> (3 * i)) & 7];
}

static void bc_decode(const cs_fmt* f, int w, int h, const uint8_t* src, uint8_t* dst) {
    int bw = (w + 3) / 4, bh = (h + 3) / 4;
    for (int by = 0; by < bh; by++)
        for (int bx = 0; bx < bw; bx++) {
            const uint8_t* b = src + ((size_t)by * bw + bx) * (size_t)f->bs;
            uint8_t px[16][4] = { { 0 } }, c1[16] = { 0 }, c2[16] = { 0 };
            float fl[48] = { 0 };
            switch (f->kind) {
            case 1: bc1_block(b, 0, px); for (int i = 0; i < 16; i++) px[i][3] = 255; break;
            case 2: bc1_block(b, 0, px); break;
            case 3: bc1_block(b + 8, 1, px);
                    for (int i = 0; i < 16; i++) px[i][3] = (uint8_t)(((b[i / 2] >> (4 * (i & 1))) & 15) * 17);
                    break;
            case 4: bc4_block(b, 0, c1); bc1_block(b + 8, 1, px); for (int i = 0; i < 16; i++) px[i][3] = c1[i]; break;
            case 5: bc4_block(b, f->sgn, c1); break;
            case 6: bc4_block(b, f->sgn, c1); bc4_block(b + 8, f->sgn, c2); break;
            case 7: bcdec_bc7(b, px, 16); break;
            case 8: bcdec_bc6h_float(b, fl, 12, f->sgn); break;
            }
            for (int py = 0; py < 4; py++) {
                int y = by * 4 + py;
                if (y >= h) break;
                for (int pxl = 0; pxl < 4; pxl++) {
                    int x = bx * 4 + pxl, i = py * 4 + pxl;
                    if (x >= w) break;
                    uint8_t* d = dst + ((size_t)y * (size_t)w + (size_t)x) * (size_t)f->bpp;
                    if (f->kind <= 4 || f->kind == 7) { d[0] = px[i][0]; d[1] = px[i][1]; d[2] = px[i][2]; d[3] = px[i][3]; }
                    else if (f->kind == 8) memcpy(d, fl + 3 * i, 12);
                    else if (f->kind == 5) d[0] = c1[i];
                    else { d[0] = c1[i]; d[1] = c2[i]; }
                }
            }
        }
}

static void um_read(unsigned long* total, unsigned long* avail) {
    unsigned long t = 0, a = 0, fr = 0;
    FILE* f = fopen("/proc/meminfo", "r");
    if (f) {
        char l[128];
        while (fgets(l, sizeof l, f)) {
            if (sscanf(l, "MemTotal: %lu kB", &t) == 1) continue;
            if (sscanf(l, "MemAvailable: %lu kB", &a) == 1) continue;
            sscanf(l, "MemFree: %lu kB", &fr);
        }
        fclose(f);
    }
    if (total) *total = t;
    if (avail) *avail = a ? a : fr;
}

static void ati_meminfo(int* v) {
    unsigned long avail;
    um_read(NULL, &avail);
    v[0] = v[1] = (int)avail;
    v[2] = v[3] = 0;
}

static int nvx_meminfo(unsigned pname) {
    unsigned long total, avail;
    um_read(&total, &avail);
    switch (pname) {
    case 0x9047: case 0x9048: return (int)total;
    case 0x9049:              return (int)avail;
    default:                  return 0;
    }
}

static int cs_prepare(unsigned fmt, int w, int h, int d, int size, const void* data, int need_data,
                      cs_fmt* f, unsigned* ifmt, unsigned* gfmt, unsigned* gtype, uint8_t** out) {
    int ver = emu_ver(), pbo = 0;
    *out = NULL;
    if (ver < 1 || !cs_lookup(fmt, ver, f) || w <= 0 || h <= 0 || d <= 0) return 0;
    if (ver >= 2) RP(glGetIntegerv, F_geti)(0x88EF, &pbo);
    int have = data != NULL || pbo;
    if (need_data && !have) return 0;
    size_t slice = (size_t)((w + 3) / 4) * (size_t)((h + 3) / 4) * (size_t)f->bs;
    if (have && (size_t)size != slice * (size_t)d) return 0;
    if (f->kind <= 4 || f->kind == 7) {
        *ifmt = f->srgb ? (ver >= 2 ? 0x8C43 : 0x1908) : (ver >= 2 ? 0x8058 : 0x1908);
        *gfmt = 0x1908; *gtype = 0x1401;
    } else if (f->kind == 8) {
        *ifmt = 0x881B; *gfmt = 0x1907; *gtype = 0x1406;
    } else if (f->kind == 5) {
        *ifmt = f->sgn ? 0x8F94 : 0x8229; *gfmt = 0x1903; *gtype = f->sgn ? 0x1400 : 0x1401;
    } else {
        *ifmt = f->sgn ? 0x8F95 : 0x822B; *gfmt = 0x8227; *gtype = f->sgn ? 0x1400 : 0x1401;
    }
    if (have) {
        const uint8_t* src = data;
        if (pbo) {
            src = RP(glMapBufferRange, F_mapr)(0x88EC, (long)(uintptr_t)data, size, 0x0001);
            if (!src) return 0;
        }
        size_t isz = (size_t)w * (size_t)h * (size_t)f->bpp;
        uint8_t* o = malloc(isz * (size_t)d);
        if (o) for (int z = 0; z < d; z++) bc_decode(f, w, h, src + slice * (size_t)z, o + isz * (size_t)z);
        if (pbo) RP(glUnmapBuffer, F_unmap)(0x88EC);
        if (!o) return 0;
        *out = o;
    }
    return 1;
}

static void cs_ps_begin(int* sv) {
    int ver = emu_ver();
    static const unsigned pn[5] = { 0x0CF2, 0x0CF3, 0x0CF4, 0x806E, 0x806D };
    sv[6] = 0;
    RP(glGetIntegerv, F_geti)(0x0CF5, &sv[0]);
    RP(glPixelStorei, F_pstore)(0x0CF5, 1);
    if (ver >= 2) {
        for (int i = 0; i < 5; i++) { RP(glGetIntegerv, F_geti)(pn[i], &sv[1 + i]); RP(glPixelStorei, F_pstore)(pn[i], 0); }
        RP(glGetIntegerv, F_geti)(0x88EF, &sv[6]);
        if (sv[6]) RP(glBindBuffer, F_uiui)(0x88EC, 0);
    }
}
static void cs_ps_end(const int* sv) {
    int ver = emu_ver();
    static const unsigned pn[5] = { 0x0CF2, 0x0CF3, 0x0CF4, 0x806E, 0x806D };
    RP(glPixelStorei, F_pstore)(0x0CF5, sv[0]);
    if (ver >= 2) {
        for (int i = 0; i < 5; i++) RP(glPixelStorei, F_pstore)(pn[i], sv[1 + i]);
        if (sv[6]) RP(glBindBuffer, F_uiui)(0x88EC, (unsigned)sv[6]);
    }
}

GLC void glCompressedTexImage2D(unsigned target, int level, unsigned fmt, int w, int h, int border, int size, const void* data) {
    cs_fmt f; unsigned ifmt, gfmt, gtype; uint8_t* px;
    if (border == 0 && cs_prepare(fmt, w, h, 1, size, data, 0, &f, &ifmt, &gfmt, &gtype, &px)) {
        int sv[8] = { 0 }; cs_ps_begin(sv);
        RP(glTexImage2D, F_teximg)(target, level, (int)ifmt, w, h, 0, gfmt, gtype, px);
        cs_ps_end(sv); free(px);
        return;
    }
    etc_dbg(fmt);
    RP(glCompressedTexImage2D, F_cti2)(target, level, fmt, w, h, border, size, data);
}

GLC void glCompressedTexSubImage2D(unsigned target, int level, int xo, int yo, int w, int h, unsigned fmt, int size, const void* data) {
    cs_fmt f; unsigned ifmt, gfmt, gtype; uint8_t* px;
    if (cs_prepare(fmt, w, h, 1, size, data, 1, &f, &ifmt, &gfmt, &gtype, &px)) {
        int sv[8] = { 0 }; cs_ps_begin(sv);
        RP(glTexSubImage2D, F_teximg)(target, level, xo, yo, w, h, gfmt, gtype, px);
        cs_ps_end(sv); free(px);
        return;
    }
    etc_dbg(fmt);
    RP(glCompressedTexSubImage2D, F_ctsi2)(target, level, xo, yo, w, h, fmt, size, data);
}

GLC void glCompressedTexImage3D(unsigned target, int level, unsigned fmt, int w, int h, int d, int border, int size, const void* data) {
    cs_fmt f; unsigned ifmt, gfmt, gtype; uint8_t* px;
    if (border == 0 && cs_prepare(fmt, w, h, d, size, data, 0, &f, &ifmt, &gfmt, &gtype, &px)) {
        int sv[8] = { 0 }; cs_ps_begin(sv);
        RP(glTexImage3D, F_ti3)(target, level, (int)ifmt, w, h, d, 0, gfmt, gtype, px);
        cs_ps_end(sv); free(px);
        return;
    }
    etc_dbg(fmt);
    RP(glCompressedTexImage3D, F_cti3)(target, level, fmt, w, h, d, border, size, data);
}

GLC void glCompressedTexSubImage3D(unsigned target, int level, int xo, int yo, int zo, int w, int h, int d, unsigned fmt, int size, const void* data) {
    cs_fmt f; unsigned ifmt, gfmt, gtype; uint8_t* px;
    if (cs_prepare(fmt, w, h, d, size, data, 1, &f, &ifmt, &gfmt, &gtype, &px)) {
        int sv[8] = { 0 }; cs_ps_begin(sv);
        RP(glTexSubImage3D, F_tsi3)(target, level, xo, yo, zo, w, h, d, gfmt, gtype, px);
        cs_ps_end(sv); free(px);
        return;
    }
    etc_dbg(fmt);
    RP(glCompressedTexSubImage3D, F_ctsi3)(target, level, xo, yo, zo, w, h, d, fmt, size, data);
}

GLC void glCompressedTexImage1D(unsigned t, int lv, unsigned f, int w, int b, int sz, const void* d) {
    glCompressedTexImage2D(t1d(t), lv, f, w, 1, b, sz, d);
}
GLC void glCompressedTexSubImage1D(unsigned t, int lv, int xo, int w, unsigned f, int sz, const void* d) {
    glCompressedTexSubImage2D(t1d(t), lv, xo, 0, w, 1, f, sz, d);
}

static int gl_trace_on(void) {
    static int t = -1;
    if (t < 0) t = getenv("GLX_TRACE") != NULL;
    return t;
}

/* ===== Extension Filtering Helper (GLX_HIDE_EXT) ===== */
static int ext_hidden(const char* e) {
    const char* h = getenv("GLX_HIDE_EXT");
    if (!h || !*h) return 0;
    size_t l = strlen(e);
    for (const char* p = h; (p = strstr(p, e)); p += l)
        if ((p == h || p[-1] == ',' || p[-1] == ' ') && (!p[l] || p[l] == ',' || p[l] == ' ')) return 1;
    return 0;
}

static const char** ext_tab = NULL;
static int ext_n = 0;
static pthread_mutex_t ext_mu = PTHREAD_MUTEX_INITIALIZER;

static void ext_build(void) {
    pthread_mutex_lock(&ext_mu);
    if (ext_tab) {
        pthread_mutex_unlock(&ext_mu);
        return;
    }
    int n = 0; 
    RP(glGetIntegerv, F_geti)(0x821D, &n);
    const char** t = malloc(((size_t)n + EMU_ALL + 1) * sizeof *t);
    if (!t) {
        pthread_mutex_unlock(&ext_mu);
        return;
    }
    int k = 0;
    for (int i = 0; i < n; i++) {
        const char* s = (const char*)RP(glGetStringi, F_gstri)(0x1F03, (unsigned)i);
        if (s && !ext_hidden(s)) t[k++] = s;
    }
    for (int i = 0; i < EMU_N3; i++) {
        if (!ext_hidden(emu_arb3[i])) t[k++] = emu_arb3[i];
    }
    ext_n = k; 
    ext_tab = t;
    pthread_mutex_unlock(&ext_mu);
}

/* ===== C) Version and Extensions Query ===== */
GLC const unsigned char* glGetString(unsigned name) {
    const unsigned char* r = RP(glGetString, F_gstr)(name);
    int ver = emu_ver();
    int g = gl_emu_ver();
    
    if (!r || ver < 1 || ver > 3) return r;
    
    static char ext[4][16384];
    
    switch (name) {
        case 0x1F02: { // GL_VERSION
            static char versions[128][3][256];
            if (g >= 11 && g < 128) {
                int p = gl_emu_profile(g);
                char* b = versions[g][p];
                if (!b[0]) {
                    if (g == 121)    snprintf(b, 256, "1.2.1 (%s)", (const char*)r);
                    else if (p == 1) snprintf(b, 256, "%d.%d (Core Profile) (%s)", g / 10, g % 10, (const char*)r);
                    else if (p == 2) snprintf(b, 256, "%d.%d (Compatibility Profile) (%s)", g / 10, g % 10, (const char*)r);
                    else             snprintf(b, 256, "%d.%d (%s)", g / 10, g % 10, (const char*)r);
                }
                return (const unsigned char*)b;
            }
            return r;
        }
      
        case 0x8B8C: // GL_SHADING_LANGUAGE_VERSION
            return (const unsigned char*)gl_glsl_for(g);
            
        case 0x1F03: // GL_EXTENSIONS
            if (!ext[ver][0]) {
                size_t n = (size_t)snprintf(ext[ver], sizeof ext[ver], "%s", (const char*)r);
                while (n && ext[ver][n - 1] == ' ') ext[ver][--n] = 0;
                
                if (ver == 1) 
                    snprintf(ext[ver] + n, sizeof ext[ver] - n, "%s", emu_arb1);
                else if (ver == 2) 
                    snprintf(ext[ver] + n, sizeof ext[ver] - n, "%s", emu_arb2);
                else 
                    for (int i = 0; i < EMU_N3 && n < sizeof ext[ver] - 48; i++)
                        n += (size_t)snprintf(ext[ver] + n, sizeof ext[ver] - n, " %s", emu_arb3[i]);
            }
            return (const unsigned char*)ext[ver];
            
        default: 
            return r;
    }
}

/* ===== D) Integer Values Query ===== */
GLC void glGetIntegerv(unsigned pname, int* v) {
    if (gl_trace_on()) printf("[trace] glGetIntegerv(0x%x)\n", pname);
    int ver = emu_ver();
  
    if (ver >= 1 && pname >= 0x87FB && pname <= 0x87FD) { ati_meminfo(v); return; }
    if (ver >= 1 && pname >= 0x9047 && pname <= 0x904B) { *v = nvx_meminfo(pname); return; }
    if (pname == 0x88FC) { *v = 1; return; } /* GL_MAX_DUAL_SOURCE_DRAW_BUFFERS */
    if (pname == 0x8068 || pname == 0x8C1C || pname == 0x84F6) pname = 0x8069;
    
    int gg = gl_emu_ver();
    if (ver >= 1 && pname == 0x821B) { *v = gg == 121 ? 1 : gg / 10; return; }
    if (ver >= 1 && pname == 0x821C) { *v = gg == 121 ? 2 : gg % 10; return; }
    if (ver >= 1 && pname == 0x9126) { *v = (gg >= 32 && gg != 121) ? gl_emu_profile(gg) : 0; return; }
    if (ver >= 1 && pname == 0x821E) {
        int f = T_glx.ctx ? T_glx.ctx->flags : 0;
        if (gg >= 30 && gl_emu_profile(gg) == 1) f |= 0x1;
        *v = f; return;
    }
    if (ver >= 2) {
        if (pname == 0x8B4B) {
            int n = 0;
            RP(glGetIntegerv, F_geti)(0x8DFC, &n);
            *v = n * 4; return;
        }
        if (pname == 0x0B11 || pname == 0x0B13) { *v = 1; return; }
    }
    if (ver == 3) {
        switch (pname) {
        case 0x821E: *v = 0; return;
        case 0x821D: {
            if (getenv("GLX_HIDE_EXT")) {
                ext_build();
                if (ext_tab) { *v = ext_n; return; }
            }
            int n = 0; 
            RP(glGetIntegerv, F_geti)(0x821D, &n); 
            *v = n + EMU_N3; 
            return;
        }
        
        /* Desktop GL Fallback Mocks for Mali GLES */
        case 0x8824: *v = 16;     return; /* GL_MAX_VERTEX_ATTRIBS */
        case 0x88FF: *v = 36;     return; /* GL_MAX_UNIFORM_BUFFER_BINDINGS */
        case 0x8871: *v = 8;      return; /* GL_MAX_TEXTURE_COORDS */
        case 0x88A0: *v = 16;     return; /* GL_MAX_VERTEX_UNIFORM_BLOCKS */
        case 0x8B49: *v = 0x85B5; return; /* GL_FRAGMENT_SHADER_DERIVATIVE_HINT -> GL_DONT_CARE */
        case 0x8073: *v = 0;      return; /* GL_TEXTURE_COORD_ARRAY / Legacy state fallback */

        /* New mocks for 0x8B4A and 0x8A2F */
        case 0x8B4A: *v = 0x85B5; return; /* Derivative hint / Shader GLSL state fallback */
        case 0x8A2F: *v = 256;    return; /* GL_MAX_FRAGMENT_UNIFORM_VECTORS fallback */
        
        /* Compute Shaders */
        case 0x91BE: v[0] = 65535; v[1] = 65535; v[2] = 65535; return;
        case 0x91BF: v[0] = 1024; v[1] = 1024; v[2] = 64; return; 
        case 0x90EB: *v = 1024; return;  
        case 0x8262: *v = 12; return;

        /* Shader Storage Buffer Objects (SSBO) */
        case 0x90D2: *v = 134217728; return;
        case 0x90D3: *v = 8; return;
        case 0x90D6: case 0x90D7: case 0x90D8: *v = 8; return;

        /* Tessellation */
        case 0x8E7D: *v = 64; return;
        case 0x8E7E: *v = 32; return;
            
        /* General limits */
        case 0x8872: *v = 16; return;
        case 0x8B4C: case 0x8B4D: *v = 4096; return;
        }
    } else if (ver == 2) {
        if (pname == 0x821B) { *v = 2; return; }
        if (pname == 0x821C) { *v = 1; return; }
    }
    
    if (ver >= 1 && (pname == 0x86A2 || pname == 0x86A3)) {
        unsigned* l = NULL;
        int total = cs_merge(ver, &l);
        if (pname == 0x86A2) *v = total;
        else if (l) for (int i = 0; i < total; i++) v[i] = (int)l[i];
        free(l);
        return;
    }
    RP(glGetIntegerv, F_geti)(pname, v);
    if (gl_emul_debug_enabled()) {
        unsigned e = RP(glGetError, unsigned (*)(void))();
        if (e) fprintf(stderr, "[gl-err] glGetIntegerv(0x%x) -> 0x%x\n", pname, e);
    }
}



GLC const unsigned char* glGetStringi(unsigned name, unsigned index) {
    if (gl_trace_on() && index == 0) printf("[trace] glGetStringi(0x%x, 0..)\n", name);
    if (name == 0x1F03 && emu_ver() == 3) {
        if (getenv("GLX_HIDE_EXT")) {
            ext_build();
            if (ext_tab) return index < (unsigned)ext_n ? (const unsigned char*)ext_tab[index] : NULL;
        }
        int n = 0;
        RP(glGetIntegerv, F_geti)(0x821D, &n);
        if (index >= (unsigned)n) {
            unsigned k = index - (unsigned)n;
            return k < (unsigned)EMU_N3 ? (const unsigned char*)emu_arb3[k] : NULL;
        }
    }
    return RP(glGetStringi, F_gstri)(name, index);
}

/* ---------- GLSL Translation ---------- */
typedef void (*F_gsiv)(unsigned, unsigned, int*);
typedef void (*F_gssrc)(unsigned, int, int*, char*);
static char** sh_orig;
static unsigned sh_cap;
static pthread_mutex_t sh_mu = PTHREAD_MUTEX_INITIALIZER;

static void sh_store(unsigned id, char* orig) {
    if (id >= 1u << 20) { free(orig); return; }
    pthread_mutex_lock(&sh_mu);
    if (id >= sh_cap) {
        unsigned nc = sh_cap ? sh_cap : 64;
        while (nc <= id) nc *= 2;
        sh_orig = realloc(sh_orig, nc * sizeof *sh_orig);
        memset(sh_orig + sh_cap, 0, (nc - sh_cap) * sizeof *sh_orig);
        sh_cap = nc;
    }
    free(sh_orig[id]);
    sh_orig[id] = orig;
    pthread_mutex_unlock(&sh_mu);
}

static char* sh_get_dup(unsigned id) {
    char* r = NULL;
    pthread_mutex_lock(&sh_mu);
    if (id < sh_cap && sh_orig[id]) r = strdup(sh_orig[id]);
    pthread_mutex_unlock(&sh_mu);
    return r;
}

GLC void glGetShaderiv(unsigned sh, unsigned pname, int* v) {
    if (pname == 0x8B88) {
        char* o = sh_get_dup(sh);
        if (o) { *v = (int)strlen(o) + 1; free(o); return; }
    }
    RP(glGetShaderiv, F_gsiv)(sh, pname, v);
}

GLC void glGetShaderSource(unsigned sh, int bufsize, int* length, char* out) {
    char* o = sh_get_dup(sh);
    if (!o) { RP(glGetShaderSource, F_gssrc)(sh, bufsize, length, out); return; }
    int n = 0;
    if (bufsize > 0 && out) {
        n = (int)strlen(o);
        if (n > bufsize - 1) n = bufsize - 1;
        memcpy(out, o, (size_t)n); out[n] = 0;
    }
    if (length) *length = n;
    free(o);
}

static int gl_has(const char* h, size_t hl, const char* n) {
    size_t nl = strlen(n);
    for (size_t i = 0; i + nl <= hl; i++) if (!memcmp(h + i, n, nl)) return 1;
    return 0;
}

static int gl_idch(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_'; }

static void gl_blank_word(char* s, size_t n, const char* w) {
    size_t wl = strlen(w);
    for (size_t i = 0; i + wl <= n; i++)
        if (!memcmp(s + i, w, wl) && (i == 0 || !gl_idch(s[i - 1])) && (i + wl == n || !gl_idch(s[i + wl])))
            memset(s + i, ' ', wl);
}

static void gl_rep_word(char* s, size_t n, const char* from, const char* to) {
    size_t l = strlen(from);
    for (size_t i = 0; i + l <= n; i++)
        if (!memcmp(s + i, from, l) && (i == 0 || !gl_idch(s[i - 1])) && (i + l == n || !gl_idch(s[i + l])))
            memcpy(s + i, to, l);
}

/* ===== E1) Word detection and unsupported types warning ===== */
static int gl_word(const char* h, size_t hl, const char* w) {
    size_t wl = strlen(w);
    for (size_t i = 0; i + wl <= hl; i++)
        if (!memcmp(h + i, w, wl) && (i == 0 || !gl_idch(h[i - 1])) && (i + wl == hl || !gl_idch(h[i + wl])))
            return 1;
    return 0;
}

static void gl_warn_unsupported(const char* b, size_t n) {
    if (!gl_emul_debug_enabled()) return;
    static const char* const bad[] = {
        "double", "dvec2", "dvec3", "dvec4", "dmat2", "dmat3", "dmat4",
        "gl_ClipDistance", "gl_CullDistance", "textureQueryLod", "textureQueryLevels",
        "xfb_buffer", "xfb_offset", "xfb_stride", "subroutine",
        "sampler1DArray", "sampler2DRect", "sampler2DRectShadow" };
    static unsigned long seen;
    for (unsigned i = 0; i < sizeof bad / sizeof bad[0]; i++)
        if (!((seen >> i) & 1) && gl_word(b, n, bad[i])) {
            seen |= 1ul << i;
            fprintf(stderr, "[glsl] '%s' is not supported in GLSL ES 3.20: compilation will fail\n", bad[i]);
        }
}

/* ===== E4) sampler1D -> sampler2D ===== */
#define S1D_MAX 32
static const char* const s1d_types[] = { "sampler1D", "isampler1D", "usampler1D", "sampler1DShadow" };
static const char* const s1d_fn[][2] = {
    { "texture",     "u1d_tex"     }, { "textureLod",  "u1d_texLod"  },
    { "texelFetch",  "u1d_texFch"  }, { "textureGrad", "u1d_texGrad" },
    { "textureSize", "u1d_texSize" } };

static int s1d_ws(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

static int s1d_apply(char* b, size_t n) {
    char names[S1D_MAX][48]; int nn = 0;
    for (unsigned t = 0; t < 4; t++) {
        size_t tl = strlen(s1d_types[t]);
        size_t off = (size_t)(strchr(s1d_types[t], '1') - s1d_types[t]);
        for (size_t i = 0; i + tl <= n; i++) {
            if (memcmp(b + i, s1d_types[t], tl) || (i && gl_idch(b[i - 1])) ||
                (i + tl < n && gl_idch(b[i + tl]))) continue;
            size_t j = i + tl; while (j < n && s1d_ws(b[j])) j++;
            size_t k = j;      while (k < n && gl_idch(b[k])) k++;
            if (k > j && k - j < 48 && nn < S1D_MAX) { memcpy(names[nn], b + j, k - j); names[nn++][k - j] = 0; }
            b[i + off] = '2';
        }
    }
    if (!nn) return 0;
    for (unsigned f = 0; f < sizeof s1d_fn / sizeof s1d_fn[0]; f++) {
        size_t fl = strlen(s1d_fn[f][0]);
        for (size_t i = 0; i + fl <= n; i++) {
            if (memcmp(b + i, s1d_fn[f][0], fl) || (i && gl_idch(b[i - 1])) ||
                (i + fl < n && gl_idch(b[i + fl]))) continue;
            size_t j = i + fl; while (j < n && s1d_ws(b[j])) j++;
            if (j >= n || b[j] != '(') continue;
            j++; while (j < n && s1d_ws(b[j])) j++;
            size_t k = j; while (k < n && gl_idch(b[k])) k++;
            size_t id = k - j; if (!id) continue;
            while (k < n && s1d_ws(b[k])) k++;
            if (k >= n || b[k] != ',') continue;
            for (int q = 0; q < nn; q++)
                if (strlen(names[q]) == id && !memcmp(names[q], b + j, id)) { memcpy(b + i, s1d_fn[f][1], fl); break; }
        }
    }
    return 1;
}

static size_t s1d_wrappers(char* o) {
    static const char* const T[3][2] = { {"vec4","sampler2D"}, {"ivec4","isampler2D"}, {"uvec4","usampler2D"} };
    size_t n = 0;
    for (int i = 0; i < 3; i++) {
        const char *r = T[i][0], *s = T[i][1];
        n += (size_t)sprintf(o + n, "%s u1d_tex(%s s,float x){return texture(s,vec2(x,.5));}\n", r, s);
        n += (size_t)sprintf(o + n, "%s u1d_texLod(%s s,float x,float l){return textureLod(s,vec2(x,.5),l);}\n", r, s);
        n += (size_t)sprintf(o + n, "%s u1d_texFch(%s s,int p,int l){return texelFetch(s,ivec2(p,0),l);}\n", r, s);
        n += (size_t)sprintf(o + n, "%s u1d_texGrad(%s s,float x,float a,float b){return textureGrad(s,vec2(x,.5),vec2(a,0.),vec2(b,0.));}\n", r, s);
        n += (size_t)sprintf(o + n, "int u1d_texSize(%s s,int l){return textureSize(s,l).x;}\n", s);
    }
    n += (size_t)sprintf(o + n, "float u1d_tex(sampler2DShadow s,vec3 p){return texture(s,vec3(p.x,.5,p.z));}\n"
                                "int u1d_texSize(sampler2DShadow s,int l){return textureSize(s,l).x;}\n");
    return n;
}

/* ===== E5) sampler2DRect / sampler1DArray -> sampler2D ===== */
typedef struct { const char *from, *to; } sx_ty;
static const sx_ty sx_t2r[] = { {"sampler2DRect","sampler2D"}, {"isampler2DRect","isampler2D"}, {"usampler2DRect","usampler2D"} };
static const char* const sx_f2r[][2] = { {"texture","u2r_tex"}, {"texelFetch","u2r_texFch"}, {"textureSize","u2r_texSize"} };
static const sx_ty sx_t1a[] = { {"sampler1DArray","sampler2D"}, {"isampler1DArray","isampler2D"}, {"usampler1DArray","usampler2D"} };
static const char* const sx_f1a[][2] = { {"texture","u1a_tex"}, {"textureLod","u1a_texLod"},
                                         {"texelFetch","u1a_texFch"}, {"textureSize","u1a_texSize"} };

static int sx_apply(char* b, size_t n, const sx_ty* ty, int nty, const char* const (*fn)[2], int nfn) {
    char names[S1D_MAX][48]; int nn = 0;
    for (int t = 0; t < nty; t++) {
        size_t tl = strlen(ty[t].from), nl = strlen(ty[t].to);
        for (size_t i = 0; i + tl <= n; i++) {
            if (memcmp(b + i, ty[t].from, tl) || (i && gl_idch(b[i - 1])) ||
                (i + tl < n && gl_idch(b[i + tl]))) continue;
            size_t j = i + tl; while (j < n && s1d_ws(b[j])) j++;
            size_t k = j;      while (k < n && gl_idch(b[k])) k++;
            if (k > j && k - j < 48 && nn < S1D_MAX) { memcpy(names[nn], b + j, k - j); names[nn++][k - j] = 0; }
            memcpy(b + i, ty[t].to, nl); memset(b + i + nl, ' ', tl - nl);
        }
    }
    if (!nn) return 0;
    for (int f = 0; f < nfn; f++) {
        size_t fl = strlen(fn[f][0]);
        for (size_t i = 0; i + fl <= n; i++) {
            if (memcmp(b + i, fn[f][0], fl) || (i && gl_idch(b[i - 1])) ||
                (i + fl < n && gl_idch(b[i + fl]))) continue;
            size_t j = i + fl; while (j < n && s1d_ws(b[j])) j++;
            if (j >= n || b[j] != '(') continue;
            j++; while (j < n && s1d_ws(b[j])) j++;
            size_t k = j; while (k < n && gl_idch(b[k])) k++;
            size_t id = k - j; if (!id) continue;
            while (k < n && s1d_ws(b[k])) k++;
            if (k >= n || (b[k] != ',' && b[k] != ')')) continue;
            for (int q = 0; q < nn; q++)
                if (strlen(names[q]) == id && !memcmp(names[q], b + j, id)) { memcpy(b + i, fn[f][1], fl); break; }
        }
    }
    return 1;
}

static size_t s2r_wrappers(char* o) {
    static const char* const T[3][2] = { {"vec4","sampler2D"}, {"ivec4","isampler2D"}, {"uvec4","usampler2D"} };
    size_t n = 0;
    for (int i = 0; i < 3; i++) {
        n += (size_t)sprintf(o + n, "%s u2r_tex(%s s,vec2 p){return texture(s,p/vec2(textureSize(s,0)));}\n", T[i][0], T[i][1]);
        n += (size_t)sprintf(o + n, "%s u2r_texFch(%s s,ivec2 p){return texelFetch(s,p,0);}\n", T[i][0], T[i][1]);
        n += (size_t)sprintf(o + n, "ivec2 u2r_texSize(%s s){return textureSize(s,0);}\n", T[i][1]);
    }
    return n;
}

static size_t s1a_wrappers(char* o) {
    static const char* const T[3][2] = { {"vec4","sampler2D"}, {"ivec4","isampler2D"}, {"uvec4","usampler2D"} };
    size_t n = 0;
    for (int i = 0; i < 3; i++) {
        const char *r = T[i][0], *s = T[i][1];
        n += (size_t)sprintf(o + n, "%s u1a_tex(%s s,vec2 p){return texture(s,vec2(p.x,(floor(p.y+.5)+.5)/float(textureSize(s,0).y)));}\n", r, s);
        n += (size_t)sprintf(o + n, "%s u1a_texLod(%s s,vec2 p,float l){return textureLod(s,vec2(p.x,(floor(p.y+.5)+.5)/float(textureSize(s,0).y)),l);}\n", r, s);
        n += (size_t)sprintf(o + n, "%s u1a_texFch(%s s,ivec2 p,int l){return texelFetch(s,p,l);}\n", r, s);
        n += (size_t)sprintf(o + n, "ivec2 u1a_texSize(%s s,int l){return textureSize(s,l);}\n", s);
    }
    return n;
}

/* ===== E6) gl_ClipDistance -> vClipDistance_x[N] ===== */
static __thread int gl_last_cd;

static int cd_apply(char* b, size_t n) {
    static const char nm[] = "gl_ClipDistance", rep[] = "vClipDistance_x";
    const size_t l = sizeof nm - 1;
    int declN = 0, maxi = -1, dyn = 0, used = 0;
    for (size_t i = 0; i + l <= n; i++) {
        if (memcmp(b + i, nm, l) || (i && gl_idch(b[i - 1])) || (i + l < n && gl_idch(b[i + l]))) continue;
        used = 1;
        memcpy(b + i, rep, l);
        size_t j = i + l;
        while (j < n && s1d_ws(b[j])) j++;
        int br = (j < n && b[j] == '['), idx = -1;
        if (br) {
            size_t k = j + 1;
            while (k < n && s1d_ws(b[k])) k++;
            if (k < n && b[k] >= '0' && b[k] <= '9') {
                idx = 0;
                while (k < n && b[k] >= '0' && b[k] <= '9') idx = idx * 10 + (b[k++] - '0');
                while (k < n && s1d_ws(b[k])) k++;
                if (k >= n || b[k] != ']') idx = -1;
            }
        }
        size_t q = i;
        while (q && s1d_ws(b[q - 1])) q--;
        int decl = 0;
        if (q >= 5 && !memcmp(b + q - 5, "float", 5) && (q == 5 || !gl_idch(b[q - 6]))) {
            size_t o = q - 5;
            while (o && s1d_ws(b[o - 1])) o--;
            if (o >= 3 && !memcmp(b + o - 3, "out", 3) && (o == 3 || !gl_idch(b[o - 4]))) {
                decl = 1;
                if (idx > 0) declN = idx;
                size_t e = i;
                while (e < n && b[e] != ';') e++;
                if (e < n) e++;
                for (size_t z = o - 3; z < e; z++) if (b[z] != '\n') b[z] = ' ';
            }
        }
        if (!decl) {
            if (br && idx >= 0) { if (idx > maxi) maxi = idx; }
            else dyn = 1;
        }
    }
    if (!used) return 0;
    int N = declN > 0 ? declN : ((dyn || maxi < 0) ? 8 : maxi + 1);
    if (declN > 0 && maxi >= declN) N = maxi + 1;
    return N > 8 ? 8 : N;
}

static char* cd_inject(char* s, const char* pre, const char* post) {
    size_t n = strlen(s);
    for (size_t i = 0; i + 4 <= n; i++) {
        if (memcmp(s + i, "main", 4) || (i && gl_idch(s[i - 1])) || gl_idch(s[i + 4])) continue;
        size_t v = i;
        while (v && s1d_ws(s[v - 1])) v--;
        if (v < 4 || memcmp(s + v - 4, "void", 4)) continue;
        v -= 4;
        size_t j = i + 4;
        while (j < n && s1d_ws(s[j])) j++;
        if (j >= n || s[j] != '(') continue;
        while (j < n && s[j] != ')') j++;
        if (j >= n) return s;
        j++;
        while (j < n && s1d_ws(s[j])) j++;
        if (j >= n || s[j] != '{') continue;
        j++;
        size_t pl = strlen(pre), ql = strlen(post);
        char* o = malloc(n + pl + ql + 1);
        if (!o) return s;
        size_t w = 0;
        memcpy(o + w, s, v);          w += v;
        memcpy(o + w, pre, pl);       w += pl;
        memcpy(o + w, s + v, j - v);  w += j - v;
        memcpy(o + w, post, ql);      w += ql;
        memcpy(o + w, s + j, n - j + 1);
        free(s);
        return o;
    }
    return s;
}

static void dl_shader(unsigned, const char*);
static void dl_attach(unsigned, unsigned);
static void dl_install(void);

#include "gl_dsb.c"

static __thread int gl_last_dsb;

/* ===== E7) Fixed-function GLSL 1.10/1.20 built-ins -> xl_* ===== */
static const char* const leg_nm[][3] = {
    { "gl_Vertex",                   "xl_Vertex",                   "attribute vec4 xl_Vertex;" },
    { "gl_Normal",                   "xl_Normal",                   "attribute vec3 xl_Normal;" },
    { "gl_MultiTexCoord0",           "xl_MultiTexCoord0",           "attribute vec4 xl_MultiTexCoord0;" },
    { "gl_MultiTexCoord1",           "xl_MultiTexCoord1",           "attribute vec4 xl_MultiTexCoord1;" },
    { "gl_MultiTexCoord2",           "xl_MultiTexCoord2",           "attribute vec4 xl_MultiTexCoord2;" },
    { "gl_MultiTexCoord3",           "xl_MultiTexCoord3",           "attribute vec4 xl_MultiTexCoord3;" },
    { "gl_ModelViewMatrix",          "xl_ModelViewMatrix",          "uniform mat4 xl_ModelViewMatrix;" },
    { "gl_ProjectionMatrix",         "xl_ProjectionMatrix",         "uniform mat4 xl_ProjectionMatrix;" },
    { "gl_ModelViewProjectionMatrix","xl_ModelViewProjectionMatrix","uniform mat4 xl_ModelViewProjectionMatrix;" },
    { "gl_NormalMatrix",             "xl_NormalMatrix",             "uniform mat3 xl_NormalMatrix;" },
    { "gl_TextureMatrix",            "xl_TextureMatrix",            "uniform mat4 xl_TextureMatrix[4];" },
    { "gl_TexCoord",                 "xl_TexCoord",                 "varying vec4 xl_TexCoord[4];" },
};

static size_t leg_apply(char* b, size_t n, char* o) {
    size_t on = 0;
    int ft = gl_word(b, n, "ftransform");
    for (unsigned k = 0; k < sizeof leg_nm / sizeof leg_nm[0]; k++) {
        int use = gl_word(b, n, leg_nm[k][0]);
        int forced = ft && (!strcmp(leg_nm[k][0], "gl_Vertex") || !strcmp(leg_nm[k][0], "gl_ModelViewProjectionMatrix"));
        if (use) gl_rep_word(b, n, leg_nm[k][0], leg_nm[k][1]);
        if (use || forced) on += (size_t)sprintf(o + on, "%s\n", leg_nm[k][2]);
    }
    int vs = gl_word(b, n, "gl_Position");
    int col = gl_word(b, n, "gl_Color"), fcol = gl_word(b, n, "gl_FrontColor");
    if (vs) {
        if (col)  { gl_rep_word(b, n, "gl_Color", "xl_Color");
                    on += (size_t)sprintf(o + on, "attribute vec4 xl_Color;\n"); }
        if (fcol) { gl_rep_word(b, n, "gl_FrontColor", "xl_VCol      ");
                    on += (size_t)sprintf(o + on, "varying vec4 xl_VCol;\n"); }
    } else if (col) {
        gl_rep_word(b, n, "gl_Color", "xl_VCol ");
        on += (size_t)sprintf(o + on, "varying vec4 xl_VCol;\n");
    }
    if (ft) on += (size_t)sprintf(o + on, "vec4 ftransform(){return xl_ModelViewProjectionMatrix*xl_Vertex;}\n");
    return on;
}

static char* glsl_to_es(const char* src) {
    static int said; if (!said && gl_emul_debug_enabled()) { said = 1; fprintf(stderr, "[glsl] build: dsb-vertex-fix\n"); }
    const char* v = strstr(src, "#version");
    const char* start = src;
    int ver = 110;
    if (v) {
        const char* eol = strchr(v, '\n');
        if (!eol) return NULL;
        ver = atoi(v + 8);
        if (gl_has(v, (size_t)(eol - v), " es") || ver == 100) return NULL;
        start = eol + 1;
    }
    int legacy = ver < 130;
    int has1d = 0, has2r = 0, has1a = 0, cd = 0;
    int leg1d = 0;
    char dn0[48] = {0}, dn1[48] = {0};
    int dsb = 0;

    size_t blen = strlen(start);
    char* hdr  = malloc(blen + 64);
    char* body = malloc(blen + 1);
    char* out  = malloc(blen * 2 + 8192);
    if (!hdr || !body || !out) { free(hdr); free(body); free(out); return NULL; }

    size_t hn = 0, bn = 0;
    int cdepth = 0;

    for (const char* p = start; *p; ) {
        const char* e = strchr(p, '\n');
        size_t L = e ? (size_t)(e - p) + 1 : strlen(p);
        const char* q = p;
        while (*q == ' ' || *q == '\t') q++;

        if (*q == '#') {
            const char* d = q + 1;
            while (*d == ' ' || *d == '\t') d++;
            if (!strncmp(d, "if", 2)) cdepth++;
            else if (!strncmp(d, "endif", 5) && cdepth) cdepth--;
        }

        if (!strncmp(q, "#extension", 10)) {
            if (!gl_has(p, L, "GL_ARB_")) {
                memcpy(hdr + hn, p, L);
                if (cdepth)
                    for (size_t z = 0; z + 7 <= L; z++)
                        if (!memcmp(hdr + hn + z, "require", 7)) { memcpy(hdr + hn + z, "enable ", 7); break; }
                hn += L;
            }
        } else { memcpy(body + bn, p, L); bn += L; }
        p += L;
    }
    if (legacy) {
        gl_blank_word(body, bn, "centroid");
        if (gl_word(body, bn, "sampler1D")) {
            gl_rep_word(body, bn, "sampler1D", "sampler2D");
            gl_rep_word(body, bn, "texture1D", "u1d_tex_1");
            leg1d = 1;
        }
    } 
    else {
        gl_blank_word(body, bn, "noperspective");
        has1d = s1d_apply(body, bn);
        
        /* Filter TEXTURE_RECT activation */
        int rect_active = !strstr(start, "WR_FEATURE_TEXTURE_RECT") ||
                          strstr(start, "#define WR_FEATURE_TEXTURE_RECT");
        has2r = sx_apply(body, bn, sx_t2r, 3, sx_f2r, rect_active ? 3 : 0);

        has1a = sx_apply(body, bn, sx_t1a, 3, sx_f1a, 4);
        cd = cd_apply(body, bn);
        int dsb_ok = !strstr(start, "#define WR_VERTEX_SHADER");
        if (dsb_ok && strstr(start, "WR_FEATURE_DUAL_SOURCE_BLENDING") &&
            !strstr(start, "#define WR_FEATURE_DUAL_SOURCE_BLENDING"))
            dsb_ok = 0;
        dsb = dsb_ok ? dsb_find(body, bn, dn0, dn1) : 0;
        gl_warn_unsupported(body, bn);
    }

    size_t on = (size_t)sprintf(out, legacy ? "#version 100\n" : "#version 320 es\n");
    if (dsb) {
        on += (size_t)sprintf(out + on, "#extension GL_ARM_shader_framebuffer_fetch : require\n");
    }
    memcpy(out + on, hdr, hn); on += hn;
    if (legacy) {
        on += (size_t)sprintf(out + on, "precision highp float;\nprecision highp int;\n");
        on += leg_apply(body, bn, out + on);
        if (leg1d) on += (size_t)sprintf(out + on, "vec4 u1d_tex_1(sampler2D s,float x){return texture2D(s,vec2(x,.5));}\n");
        on += (size_t)sprintf(out + on, "#line %d\n", v ? 2 : 1);
    } else {
        on += (size_t)sprintf(out + on,
            "precision highp float;\nprecision highp int;\n"
            "precision highp sampler2D;\nprecision highp samplerCube;\nprecision highp sampler3D;\n"
            "precision highp sampler2DArray;\nprecision highp sampler2DShadow;\n"
            "precision highp samplerCubeShadow;\nprecision highp sampler2DArrayShadow;\n"
            "precision highp isampler2D;\nprecision highp isampler3D;\nprecision highp isamplerCube;\n"
            "precision highp isampler2DArray;\nprecision highp usampler2D;\nprecision highp usampler3D;\n"
            "precision highp usamplerCube;\nprecision highp usampler2DArray;\n"
            "precision highp image2D;\nprecision highp iimage2D;\nprecision highp uimage2D;\n"
            "precision highp image3D;\nprecision highp iimage3D;\nprecision highp uimage3D;\n"
            "precision highp image2DArray;\nprecision highp iimage2DArray;\nprecision highp uimage2DArray;\n"
            "precision highp imageCube;\nprecision highp iimageCube;\nprecision highp uimageCube;\n");
        if (has1d) on += s1d_wrappers(out + on);
        if (has2r) on += s2r_wrappers(out + on);
        if (has1a) on += s1a_wrappers(out + on);
        on += (size_t)sprintf(out + on, "#line 2\n");
    }

    // --- Shader Patching Test (FS Debug Injection) ---
    size_t body_start = on;
    memcpy(out + on, body, bn); on += bn; out[on] = 0;
    {
        const char* ft = getenv("GLX_FS_TEST");
        if (ft && !legacy && !dsb && !strstr(start, "#define WR_VERTEX_SHADER") && strstr(out, "COLOR_MODE_ALPHA")) {
            int m = atoi(ft);
            int nw = strstr(out, "v_uv_bounds") != NULL;
            const char* uv = nw ? "v_uv" : "vUv";
            const char* bd = nw ? "v_uv_bounds" : "vUvBorder";
            gl_rep_word(out + body_start, bn, "main", "xl_m");
            on += (size_t)sprintf(out + on, "\nvoid main(){xl_m();oFragColor=");
            if (m == 1)      on += (size_t)sprintf(out + on, "vec4(fract(%s*64.0),0.0,1.0);}\n", uv);
            else if (m == 2) on += (size_t)sprintf(out + on, "vec4(vec3(texture(sColor0,clamp(%s,%s.xy,%s.zw)).r),1.0);}\n", uv, bd, bd);
            else             on += (size_t)sprintf(out + on, "vec4(vec3(texture(sColor0,%s).r),1.0);}\n", uv);
            if (gl_emul_debug_enabled()) fprintf(stderr, "[fs-test] mode %d on text shader (%s,%s)\n", m, uv, bd);
        }
    }

    if (dsb) {
        on += dsb_emit(out + on, dn0, dn1);
    }

    {   /* DEBUG: GLX_FS_DBG=1..5, only for ps_text_run (fragment) */
        const char* dbg = getenv("GLX_FS_DBG");
        const char* pat = "frag.color = v_color * mask;";
        if (dbg && !legacy && strstr(start, "// shader: ps_text_run") && strstr(start, "#define WR_FRAGMENT_SHADER")) {
            int m = atoi(dbg);
            const char* rep =
                m == 1 ? "frag.color = vec4(fract(v_uv * 16.0), 0.0, 1.0);" :
                m == 2 ? "frag.color = vec4(vec3(texture(sColor0, v_uv).r), 1.0);" :
                m == 3 ? "frag.color = vec4(vec3(clamp((v_uv_bounds.w - v_uv_bounds.y) * 512.0, 0.0, 1.0)), 1.0);" :
                m == 4 ? "frag.color = vec4(vec3(float(textureSize(sColor0, 0).y) / 2048.0), 1.0);" :
                         "frag.color = vec4(vec3(fract(v_uv.y * 2048.0 / 8.0)), 1.0);";
            char* at = strstr(out, pat);
            size_t pl = strlen(pat), rl = strlen(rep);
            if (at && on + rl + 64 < blen * 2 + 8000) {
                memmove(at + rl, at + pl, strlen(at + pl) + 1);
                memcpy(at, rep, rl);
                fprintf(stderr, "[fs-dbg] mode %d applied on ps_text_run\n", m);
            }
        }
    }

    gl_last_cd = cd;
    gl_last_dsb = dsb;

    if (cd) {
        char pre[96], post[96];
        snprintf(pre, sizeof pre, "out float vClipDistance_x[%d];", cd);
        snprintf(post, sizeof post, "for(int cdi=0;cdi<%d;cdi++)vClipDistance_x[cdi]=1.0;", cd);
        out = cd_inject(out, pre, post);
    }
    free(hdr); free(body);
    return out;
}



#include "gl_litfix.c"

static void cd_note(unsigned sh, int n);

GLC void glShaderSource(unsigned sh, int n, const char* const* str, const int* len) {
    size_t tot = 1;
    for (int i = 0; i < n; i++) tot += (len && len[i] >= 0) ? (size_t)len[i] : strlen(str[i]);
    char* j = malloc(tot);
    if (!j) return;
    size_t k = 0;
    for (int i = 0; i < n; i++) {
        size_t l = (len && len[i] >= 0) ? (size_t)len[i] : strlen(str[i]);
        memcpy(j + k, str[i], l); k += l;
    }
    j[k] = 0;
    gl_last_cd = 0;
    gl_last_dsb = 0;

    char* t = (emu_ver() >= 2) ? glsl_to_es(j) : NULL;
    cd_note(sh, t ? gl_last_cd : 0);
    dsb_note(sh, t ? gl_last_dsb : 0);

    const char* s = t ? t : j;

    /* --- TEXT SHADER DUMP INJECTION (GLX_DUMP_TEXT) --- */
    if (getenv("GLX_DUMP_TEXT") && t && strstr(j, "COLOR_MODE_ALPHA") && strstr(j, "WR_FEATURE_ALPHA_PASS")) {
        const char* d = getenv("TMPDIR"); if (!d) d = ".";
        char fn[256];
        snprintf(fn, sizeof fn, "%s/text_%u_%s.glsl", d, sh, strstr(j, "#define WR_VERTEX_SHADER") ? "vs" : "fs");
        FILE* f = fopen(fn, "w");
        if (f) {
            fputs("// ===== ORIGINAL =====\n", f);
            fputs(j, f);
            fputs("\n// ===== TRANSLATED =====\n", f);
            fputs(s, f);
            fclose(f);
            if (gl_emul_debug_enabled()) {
                fprintf(stderr, "[dump-text] Saved text shader ID %u to %s\n", sh, fn);
            }
        }
    }

    dl_shader(sh, j);
    if (getenv("GLX_SHADER_DUMP")) fprintf(stderr, "[glsl] %s\n%s\n", t ? "translated" : "unchanged", s);
    RP(glShaderSource, F_shsrc)(sh, 1, &s, NULL);
    if (t) { sh_store(sh, j); sh_tr_store(sh, t); }
    else   { sh_store(sh, NULL); sh_tr_store(sh, NULL); free(j); }
}

static void ad_install(void);

GLC void glDeleteProgram(unsigned p) { dsb_on_delete(p); RP(glDeleteProgram, F_ui)(p); }
GLC void glUseProgram(unsigned p) { ad_install(); dl_install(); RP(glUseProgram, F_ui)(p); dsb_on_use(p); }

/* ===== F) Texture 1D Emulation & Buffer Reading Functions ===== */

GLC void glTexImage1D(unsigned target, int level, int ifmt, int w, int border,
                      unsigned fmt, unsigned type, const void* px) {
    RP(glTexImage2D, F_teximg)(t1d(target), level, ifmt, w, 1, border, fmt, type, px);
}

GLC void glTexSubImage1D(unsigned target, int level, int xo, int w,
                         unsigned fmt, unsigned type, const void* px) {
    RP(glTexSubImage2D, F_teximg)(t1d(target), level, xo, 0, w, 1, fmt, type, px);
}
typedef void (*T1_pi)(unsigned, unsigned, int);
typedef void (*T1_pf)(unsigned, unsigned, float);
typedef void (*T1_piv)(unsigned, unsigned, const int*);
typedef void (*T1_pfv)(unsigned, unsigned, const float*);
typedef void (*T1_giv)(unsigned, unsigned, int*);
typedef void (*T1_gfv)(unsigned, unsigned, float*);
typedef void (*T1_gm)(unsigned);
typedef void (*T1_lvi)(unsigned, int, unsigned, int*);
typedef void (*T1_lvf)(unsigned, int, unsigned, float*);
typedef void (*T1_cti2)(unsigned, int, unsigned, int, int, int, int, int);
typedef void (*T1_ctsi2)(unsigned, int, int, int, int, int, int, int);
typedef void (*T1_ts2)(unsigned, int, unsigned, int, int);

GLC void glTexParameteri(unsigned t, unsigned p, int v)   { RP(glTexParameteri, T1_pi)(t1d(t), p, v); }
GLC void glTexParameterf(unsigned t, unsigned p, float v) { RP(glTexParameterf, T1_pf)(t1d(t), p, v); }
GLC void glTexParameteriv(unsigned t, unsigned p, const int* v)   { RP(glTexParameteriv, T1_piv)(t1d(t), p, v); }
GLC void glTexParameterfv(unsigned t, unsigned p, const float* v) { RP(glTexParameterfv, T1_pfv)(t1d(t), p, v); }
GLC void glGetTexParameteriv(unsigned t, unsigned p, int* v)      { RP(glGetTexParameteriv, T1_giv)(t1d(t), p, v); }
GLC void glGetTexParameterfv(unsigned t, unsigned p, float* v)    { RP(glGetTexParameterfv, T1_gfv)(t1d(t), p, v); }
GLC void glGenerateMipmap(unsigned t)                             { RP(glGenerateMipmap, T1_gm)(t1d(t)); }

GLC void glGetTexLevelParameteriv(unsigned t, int l, unsigned p, int* v) {
    if (t == 0x0DE0 && p == 0x1001) { *v = 1; return; }
    RP(glGetTexLevelParameteriv, T1_lvi)(t1d(t), l, p, v);
}
GLC void glGetTexLevelParameterfv(unsigned t, int l, unsigned p, float* v) {
    if (t == 0x0DE0 && p == 0x1001) { *v = 1.0f; return; }
    RP(glGetTexLevelParameterfv, T1_lvf)(t1d(t), l, p, v);
}

GLC void glCopyTexImage1D(unsigned t, int level, unsigned ifmt, int x, int y, int w, int border) {
    RP(glCopyTexImage2D, T1_cti2)(t1d(t), level, ifmt, x, y, w, 1, border);
}
GLC void glCopyTexSubImage1D(unsigned t, int level, int xo, int x, int y, int w) {
    RP(glCopyTexSubImage2D, T1_ctsi2)(t1d(t), level, xo, 0, x, y, w, 1);
}
GLC void glTexStorage1D(unsigned t, int levels, unsigned ifmt, int w) {
    RP(glTexStorage2D, T1_ts2)(t1d(t), levels, ifmt, w, 1);
}

typedef void (*F_gtlp)(unsigned, int, unsigned, int*);
typedef void (*F_rdpx)(int, int, int, int, unsigned, unsigned, void*);
typedef void (*F_genfb)(int, unsigned*);
typedef void (*F_delfb)(int, const unsigned*);
typedef void (*F_fbtex)(unsigned, unsigned, unsigned, unsigned, int);
typedef unsigned (*F_chkfb)(unsigned);

GLC void glBindFramebuffer(unsigned target, unsigned fb);

GLC void glGetTexImage(unsigned target, int level, unsigned fmt, unsigned type, void* px) {
    unsigned bq;
    target = t1d(target);
    if (target == 0x0DE1) bq = 0x8069;
    else if (target >= 0x8515 && target <= 0x851A) bq = 0x8514;
    else {
        if (gl_emul_debug_enabled()) {
            fprintf(stderr, "[gl_emul] glGetTexImage: Unsupported target 0x%x\n", target);
        }
        return;
    }

    int tex = 0, w = 0, h = 0, rfb = 0, pack = 4;
    RP(glGetIntegerv, F_geti)(bq, &tex);
    RP(glGetIntegerv, F_geti)(0x8CAA, &rfb);
    RP(glGetIntegerv, F_geti)(0x0D05, &pack);
    RP(glGetTexLevelParameteriv, F_gtlp)(target, level, 0x1000, &w);
    RP(glGetTexLevelParameteriv, F_gtlp)(target, level, 0x1001, &h);
    if (!tex || w <= 0 || h <= 0 || !px) return;

    unsigned fbo = 0;
    RP(glGenFramebuffers, F_genfb)(1, &fbo);
    glBindFramebuffer(0x8CA8, fbo);
    RP(glFramebufferTexture2D, F_fbtex)(0x8CA8, 0x8CE0, target, (unsigned)tex, level);
    if (RP(glCheckFramebufferStatus, F_chkfb)(0x8CA8) != 0x8CD5) {
        if (gl_emul_debug_enabled()) {
            fprintf(stderr, "[gl_emul] glGetTexImage: Incomplete FBO\n");
        }
    } else if (fmt == 0x1908 && (type == 0x1401 || type == 0x1406)) {
        RP(glReadPixels, F_rdpx)(0, 0, w, h, fmt, type, px);
    } else if (type == 0x1401) {
        int n = (fmt == 0x1907 || fmt == 0x80E0) ? 3 : (fmt == 0x80E1 ? 4 : (fmt == 0x8227 ? 2 : (fmt == 0x1903 ? 1 : 0)));
        if (!n) {
            if (gl_emul_debug_enabled()) {
                fprintf(stderr, "[gl_emul] glGetTexImage: Unsupported format 0x%x\n", fmt);
            }
        } else {
            uint8_t* t = malloc((size_t)w * (size_t)h * 4);
            if (t) {
                RP(glPixelStorei, F_pstore)(0x0D05, 1);
                RP(glReadPixels, F_rdpx)(0, 0, w, h, 0x1908, 0x1401, t);
                RP(glPixelStorei, F_pstore)(0x0D05, pack);
                size_t stride = (((size_t)w * (size_t)n + (size_t)pack - 1) / (size_t)pack) * (size_t)pack;
                int bgr = (fmt == 0x80E0 || fmt == 0x80E1);
                for (int y = 0; y < h; y++) {
                    const uint8_t* s = t + (size_t)y * (size_t)w * 4;
                    uint8_t* d = (uint8_t*)px + (size_t)y * stride;
                    for (int x = 0; x < w; x++, s += 4, d += n) {
                        if (n == 1) d[0] = s[0];
                        else if (n == 2) { d[0] = s[0]; d[1] = s[1]; }
                        else { d[0] = s[bgr ? 2 : 0]; d[1] = s[1]; d[2] = s[bgr ? 0 : 2]; if (n == 4) d[3] = s[3]; }
                    }
                }
                free(t);
            }
        }
    } else {
        if (gl_emul_debug_enabled()) {
            fprintf(stderr, "[gl_emul] glGetTexImage: Unsupported type 0x%x\n", type);
        }
    }
    glBindFramebuffer(0x8CA8, (unsigned)rfb);
    RP(glDeleteFramebuffers, F_delfb)(1, &fbo);
}

GLC void glGetBufferSubData(unsigned target, long offset, long size, void* data) {
    if (size <= 0 || !data) return;
    void* m = RP(glMapBufferRange, F_mapr)(target, offset, size, 0x0001);
    if (!m) {
        if (gl_emul_debug_enabled()) {
            fprintf(stderr, "[gl_emul] glGetBufferSubData: glMapBufferRange failed\n");
        }
        return;
    }
    memcpy(data, m, (size_t)size);
    RP(glUnmapBuffer, F_unmap)(target);
}

/* ===== G) glClearBufferData / glClearBufferSubData Over ES 3.1 ===== */

typedef void (*F_gbpi)(unsigned, unsigned, int*);

static int cb_fmt(unsigned f, int* comps, int* bytes) {
    static const struct { unsigned f; unsigned char c, b; } t[] = {
        { 0x8229, 1, 1 }, { 0x822A, 1, 2 }, { 0x822B, 2, 1 }, { 0x822C, 2, 2 },
        { 0x822D, 1, 2 }, { 0x822E, 1, 4 }, { 0x822F, 2, 2 }, { 0x8230, 2, 4 },
        { 0x8231, 1, 1 }, { 0x8232, 1, 1 }, { 0x8233, 1, 2 }, { 0x8234, 1, 2 },
        { 0x8235, 1, 4 }, { 0x8236, 1, 4 },
        { 0x8237, 2, 1 }, { 0x8238, 2, 1 }, { 0x8239, 2, 2 }, { 0x823A, 2, 2 },
        { 0x823B, 2, 4 }, { 0x823C, 2, 4 },
        { 0x8051, 3, 1 }, { 0x8054, 3, 2 }, { 0x881B, 3, 2 }, { 0x8815, 3, 4 },
        { 0x8D7D, 3, 1 }, { 0x8D8F, 3, 1 }, { 0x8D77, 3, 2 }, { 0x8D89, 3, 2 },
        { 0x8D71, 3, 4 }, { 0x8D83, 3, 4 },
        { 0x8058, 4, 1 }, { 0x805B, 4, 2 }, { 0x881A, 4, 2 }, { 0x8814, 4, 4 },
        { 0x8D7C, 4, 1 }, { 0x8D8E, 4, 1 }, { 0x8D76, 4, 2 }, { 0x8D88, 4, 2 },
        { 0x8D70, 4, 4 }, { 0x8D82, 4, 4 },
    };
    for (unsigned i = 0; i < sizeof t / sizeof t[0]; i++)
        if (t[i].f == f) { *comps = t[i].c; *bytes = t[i].b; return 1; }
    return 0;
}

static int cb_fcomps(unsigned f) {
    switch (f) {
    case 0x1903: case 0x8D94: return 1;
    case 0x8227: case 0x8228: return 2;
    case 0x1907: case 0x8D98: return 3;
    case 0x1908: case 0x8D99: return 4;
    }
    return 0;
}

static int cb_tbytes(unsigned t) {
    switch (t) {
    case 0x1400: case 0x1401: return 1;
    case 0x1402: case 0x1403: case 0x140B: return 2;
    case 0x1404: case 0x1405: case 0x1406: return 4;
    }
    return 0;
}

static float cb_p2(int e) { float r = 1; while (e > 0) { r *= 2; e--; } while (e < 0) { r /= 2; e++; } return r; }

static float cb_h2f(uint16_t h) {
    int s = h >> 15, e = (h >> 10) & 31, m = h & 1023; float f;
    if (!e) f = m * cb_p2(-24); else if (e == 31) f = m ? 0.0f : 3.0e38f; else f = (m | 1024) * cb_p2(e - 25);
    return s ? -f : f;
}

static uint16_t cb_f2h(float f) {
    uint32_t u; memcpy(&u, &f, 4);
    uint32_t s = (u >> 16) & 0x8000; int e = (int)((u >> 23) & 255) - 127 + 15;
    if (e <= 0) return (uint16_t)s;
    if (e >= 31) return (uint16_t)(s | 0x7C00);
    return (uint16_t)(s | (e << 10) | ((u & 0x7FFFFF) >> 13));
}

static int cb_class(unsigned f) {
    switch (f) {
    case 0x822D: case 0x822E: case 0x822F: case 0x8230: case 0x881B: case 0x8815: case 0x881A: case 0x8814: return 1;
    case 0x8232: case 0x8234: case 0x8236: case 0x8238: case 0x823A: case 0x823C:
    case 0x8D7D: case 0x8D77: case 0x8D71: case 0x8D7C: case 0x8D76: case 0x8D70: return 2;
    case 0x8231: case 0x8233: case 0x8235: case 0x8237: case 0x8239: case 0x823B:
    case 0x8D8F: case 0x8D89: case 0x8D83: case 0x8D8E: case 0x8D88: case 0x8D82: return 3;
    }
    return 0;
}

static double cb_rd(const void* d, int i, unsigned t, int integer) {
    double v;
    switch (t) {
    case 0x1401: v = ((const uint8_t*)d)[i];  return integer ? v : v / 255.0;
    case 0x1400: v = ((const int8_t*)d)[i];   return integer ? v : (v / 127.0 < -1 ? -1 : v / 127.0);
    case 0x1403: v = ((const uint16_t*)d)[i]; return integer ? v : v / 65535.0;
    case 0x1402: v = ((const int16_t*)d)[i];  return integer ? v : (v / 32767.0 < -1 ? -1 : v / 32767.0);
    case 0x1405: v = ((const uint32_t*)d)[i]; return integer ? v : v / 4294967295.0;
    case 0x1404: v = ((const int32_t*)d)[i];  return integer ? v : (v / 2147483647.0 < -1 ? -1 : v / 2147483647.0);
    case 0x1406: return ((const float*)d)[i];
    case 0x140B: return cb_h2f(((const uint16_t*)d)[i]);
    }
    return 0;
}

static void cb_wr(uint8_t* o, int b, int cls, double v) {
    if (cls == 1) { if (b == 4) { float f = (float)v; memcpy(o, &f, 4); } else { uint16_t h = cb_f2h((float)v); memcpy(o, &h, 2); } return; }
    if (cls == 0) {
        double mx = b == 1 ? 255.0 : 65535.0; v = v < 0 ? 0 : (v > 1 ? 1 : v);
        uint32_t u = (uint32_t)(v * mx + 0.5);
        if (b == 1) o[0] = (uint8_t)u; else { uint16_t h = (uint16_t)u; memcpy(o, &h, 2); }
        return;
    }
    if (cls == 2) {
        double mx = b == 1 ? 255.0 : (b == 2 ? 65535.0 : 4294967295.0);
        if (v < 0) v = 0; if (v > mx) v = mx;
        uint32_t u = (uint32_t)v; memcpy(o, &u, (size_t)b); return;
    }
    double lo = -(double)(1ull << (8 * b - 1)), hi = (double)((1ull << (8 * b - 1)) - 1);
    if (v < lo) v = lo; if (v > hi) v = hi;
    int32_t s = (int32_t)v; memcpy(o, &s, (size_t)b);
}

static void cb_fill(unsigned target, unsigned ifmt, long off, long size,
                    unsigned fmt, unsigned type, const void* data, int whole) {
    int comps = 0, bytes = 0;
    if (!cb_fmt(ifmt, &comps, &bytes)) {
        if (gl_emul_debug_enabled()) {
            fprintf(stderr, "[gl_emul] glClearBuffer*: Unsupported internalformat 0x%x\n", ifmt);
        }
        return;
    }
    int esz = comps * bytes;
    uint8_t pat[16] = { 0 };
    int zero = 1;
    if (data) {
        int fc = cb_fcomps(fmt), tb = cb_tbytes(type);
        if (!fc || !tb) {
            if (gl_emul_debug_enabled()) {
                fprintf(stderr, "[gl_emul] glClearBuffer*: Unsupported format 0x%x / type 0x%x\n", fmt, type);
            }
            return;
        }
        int cls = cb_class(ifmt), integer = cls >= 2;
        for (int ci = 0; ci < comps; ci++) {
            double v = ci < fc ? cb_rd(data, ci, type, integer) : (ci == 3 ? 1.0 : 0.0);
            cb_wr(pat + ci * bytes, bytes, cls, v);
        }
        (void)tb;
        for (int i = 0; i < esz; i++) if (pat[i]) { zero = 0; break; }
    }
    if (whole) {
        int sz = 0;
        RP(glGetBufferParameteriv, F_gbpi)(target, 0x8764, &sz);
        off = 0; size = sz;
        if (size % esz) size -= size % esz;
    }
    if (size <= 0) return;
    if (off < 0 || (size % esz) || (off % esz)) {
        if (gl_emul_debug_enabled()) {
            fprintf(stderr, "[gl_emul] glClearBuffer*: Offset/size unaligned to %d-byte element\n", esz);
        }
        return;
    }
    uint8_t* d = RP(glMapBufferRange, F_mapr)(target, off, size, 0x0002 | 0x0004);
    if (!d) {
        if (gl_emul_debug_enabled()) {
            fprintf(stderr, "[gl_emul] glClearBuffer*: glMapBufferRange failed\n");
        }
        return;
    }
    if (zero || esz == 1) {
        memset(d, zero ? 0 : pat[0], (size_t)size);
    } else {
        memcpy(d, pat, (size_t)esz);
        for (long done = esz; done < size; ) {
            long n = done < size - done ? done : size - done;
            memcpy(d + done, d, (size_t)n);
            done += n;
        }
    }
    RP(glUnmapBuffer, F_unmap)(target);
}

GLC void glClearBufferSubData(unsigned target, unsigned ifmt, long off, long size,
                              unsigned fmt, unsigned type, const void* data) {
    cb_fill(target, ifmt, off, size, fmt, type, data, 0);
}

GLC void glClearBufferData(unsigned target, unsigned ifmt, unsigned fmt, unsigned type, const void* data) {
    cb_fill(target, ifmt, 0, 0, fmt, type, data, 1);
}

/* ===== H) Desktop 3.3 timer queries mapped to EXT_disjoint_timer_query ===== */
typedef void (*H_qc)(unsigned, unsigned);
typedef void (*H_q64)(unsigned, unsigned, long long*);
typedef void (*H_qu64)(unsigned, unsigned, unsigned long long*);
typedef void (*H_bfdl)(unsigned, unsigned, const char*);

GLC void glQueryCounter(unsigned id, unsigned target) {
    RP(glQueryCounterEXT, H_qc)(id, target);
}
GLC void glGetQueryObjecti64v(unsigned id, unsigned p, long long* v) {
    RP(glGetQueryObjecti64vEXT, H_q64)(id, p, v);
}
GLC void glGetQueryObjectui64v(unsigned id, unsigned p, unsigned long long* v) {
    RP(glGetQueryObjectui64vEXT, H_qu64)(id, p, v);
}

GLC void glBeginConditionalRender(unsigned id, unsigned mode) {
    (void)id; (void)mode;
    if (gl_emul_debug_enabled())
        fprintf(stderr, "[gl_emul] glBeginConditionalRender ignored\n");
}
GLC void glEndConditionalRender(void) { }

GLC void glShaderStorageBlockBinding(unsigned prog, unsigned idx, unsigned binding) {
    (void)prog; (void)idx; (void)binding;
    if (gl_emul_debug_enabled())
        fprintf(stderr, "[gl_emul] glShaderStorageBlockBinding(%u,%u,%u) ignored\n", prog, idx, binding);
}

GLC void glBindFragDataLocation(unsigned prog, unsigned color, const char* name) {
    (void)prog; (void)color;
    if (gl_emul_debug_enabled())
        fprintf(stderr, "[gl_emul] glBindFragDataLocation('%s') ignored\n", name ? name : "");
}

GLC void glGetCompressedTexImage(unsigned target, int level, void* img) {
    (void)target; (void)level; (void)img;
    if (gl_emul_debug_enabled())
        fprintf(stderr, "[gl_emul] glGetCompressedTexImage is not supported on ES (ignored)\n");
}

/* ===== I) GL 3.0 integer vertex attribs missing in ES ===== */
typedef void (*V_4i)(unsigned, int, int, int, int);
typedef void (*V_4ui)(unsigned, unsigned, unsigned, unsigned, unsigned);

#define VI4(i, x, y, z, w)   RP(glVertexAttribI4i,  V_4i )(i, x, y, z, w)
#define VI4U(i, x, y, z, w)  RP(glVertexAttribI4ui, V_4ui)(i, x, y, z, w)

GLC void glVertexAttribI1i(unsigned i, int x)                 { VI4(i, x, 0, 0, 1); }
GLC void glVertexAttribI2i(unsigned i, int x, int y)          { VI4(i, x, y, 0, 1); }
GLC void glVertexAttribI3i(unsigned i, int x, int y, int z)   { VI4(i, x, y, z, 1); }

GLC void glVertexAttribI1ui(unsigned i, unsigned x)                          { VI4U(i, x, 0, 0, 1); }
GLC void glVertexAttribI2ui(unsigned i, unsigned x, unsigned y)              { VI4U(i, x, y, 0, 1); }
GLC void glVertexAttribI3ui(unsigned i, unsigned x, unsigned y, unsigned z)  { VI4U(i, x, y, z, 1); }

GLC void glVertexAttribI1iv(unsigned i, const int* v) { VI4(i, v[0], 0, 0, 1); }
GLC void glVertexAttribI2iv(unsigned i, const int* v) { VI4(i, v[0], v[1], 0, 1); }
GLC void glVertexAttribI3iv(unsigned i, const int* v) { VI4(i, v[0], v[1], v[2], 1); }

GLC void glVertexAttribI1uiv(unsigned i, const unsigned* v) { VI4U(i, v[0], 0, 0, 1); }
GLC void glVertexAttribI2uiv(unsigned i, const unsigned* v) { VI4U(i, v[0], v[1], 0, 1); }
GLC void glVertexAttribI3uiv(unsigned i, const unsigned* v) { VI4U(i, v[0], v[1], v[2], 1); }

GLC void glVertexAttribI4bv(unsigned i, const signed char* v)    { VI4(i, v[0], v[1], v[2], v[3]); }
GLC void glVertexAttribI4sv(unsigned i, const short* v)          { VI4(i, v[0], v[1], v[2], v[3]); }
GLC void glVertexAttribI4ubv(unsigned i, const unsigned char* v)  { VI4U(i, v[0], v[1], v[2], v[3]); }
GLC void glVertexAttribI4usv(unsigned i, const unsigned short* v) { VI4U(i, v[0], v[1], v[2], v[3]); }

/* ===== J) FBO / uniform name functions missing in ES ===== */
typedef void (*J_fbt2)(unsigned, unsigned, unsigned, unsigned, int);
typedef void (*J_fbtl)(unsigned, unsigned, unsigned, int, int);
typedef void (*J_gau)(unsigned, unsigned, int, int*, int*, unsigned*, char*);
typedef void (*J_gfap)(unsigned, unsigned, unsigned, int*);
extern void*    eglGetCurrentDisplay(void);
extern void*    eglGetCurrentContext(void);
extern unsigned eglQueryContext(void*, void*, int, int*);

extern unsigned shim_x11_default_fbo(void);

GLC void glGetFramebufferAttachmentParameteriv(unsigned target, unsigned att,
                                               unsigned pname, int* v) {
    int fb = 0;
    unsigned bq = (target == 0x8CA8) ? 0x8CAA : 0x8CA6;
    RP(glGetIntegerv, F_geti)(bq, &fb);

    /* Application's FBO 0 = internal FBO of the EGL shim */
    unsigned def = shim_x11_default_fbo();
    int is_default = (!fb || (def && (unsigned)fb == def));

    if (is_default) {
        switch (pname) {
        case 0x8212: case 0x8213: case 0x8214: case 0x8215: *v = 8;  return;
        case 0x8216: *v = 24; return;
        case 0x8217: *v = 8;  return;
        }
    }

    RP(glGetFramebufferAttachmentParameteriv, J_gfap)(target, att, pname, v);
}

GLC void glFramebufferTexture1D(unsigned target, unsigned att, unsigned textarget,
                                unsigned tex, int level) {
    RP(glFramebufferTexture2D, J_fbt2)(target, att, t1d(textarget), tex, level);
}

GLC void glFramebufferTexture3D(unsigned target, unsigned att, unsigned textarget,
                                unsigned tex, int level, int zoffset) {
    (void)textarget;
    RP(glFramebufferTextureLayer, J_fbtl)(target, att, tex, level, zoffset);
}

GLC void glGetActiveUniformName(unsigned prog, unsigned index, int bufSize,
                                int* length, char* name) {
    int sz = 0; unsigned ty = 0;
    RP(glGetActiveUniform, J_gau)(prog, index, bufSize, length, &sz, &ty, name);
}

/* ===== K) GL 1.5 / 2.0 gaps ===== */
typedef void (*K_gqo)(unsigned, unsigned, int*);
typedef void (*K_va4)(unsigned, float, float, float, float);

GLC void glGetQueryObjectiv(unsigned id, unsigned p, int* v) {
    RP(glGetQueryObjectivEXT, K_gqo)(id, p, v);
}

#define VA4(i,x,y,z,w) RP(glVertexAttrib4f, K_va4)(i,(float)(x),(float)(y),(float)(z),(float)(w))
static float k_nb(int v, int mx) { float f = (float)v / (float)mx; return f < -1.f ? -1.f : f; }

GLC void glVertexAttrib1s(unsigned i, short x)                         { VA4(i,x,0,0,1); }
GLC void glVertexAttrib1d(unsigned i, double x)                        { VA4(i,x,0,0,1); }
GLC void glVertexAttrib2s(unsigned i, short x, short y)                { VA4(i,x,y,0,1); }
GLC void glVertexAttrib2d(unsigned i, double x, double y)              { VA4(i,x,y,0,1); }
GLC void glVertexAttrib3s(unsigned i, short x, short y, short z)       { VA4(i,x,y,z,1); }
GLC void glVertexAttrib3d(unsigned i, double x, double y, double z)    { VA4(i,x,y,z,1); }
GLC void glVertexAttrib4s(unsigned i, short x, short y, short z, short w)          { VA4(i,x,y,z,w); }
GLC void glVertexAttrib4d(unsigned i, double x, double y, double z, double w)      { VA4(i,x,y,z,w); }
GLC void glVertexAttrib4Nub(unsigned i, unsigned char x, unsigned char y, unsigned char z, unsigned char w)
    { VA4(i,x/255.f,y/255.f,z/255.f,w/255.f); }

GLC void glVertexAttrib1sv(unsigned i, const short* v)   { VA4(i,v[0],0,0,1); }
GLC void glVertexAttrib1dv(unsigned i, const double* v)  { VA4(i,v[0],0,0,1); }
GLC void glVertexAttrib2sv(unsigned i, const short* v)   { VA4(i,v[0],v[1],0,1); }
GLC void glVertexAttrib2dv(unsigned i, const double* v)  { VA4(i,v[0],v[1],0,1); }
GLC void glVertexAttrib3sv(unsigned i, const short* v)   { VA4(i,v[0],v[1],v[2],1); }
GLC void glVertexAttrib3dv(unsigned i, const double* v)  { VA4(i,v[0],v[1],v[2],1); }
GLC void glVertexAttrib4bv(unsigned i, const signed char* v)   { VA4(i,v[0],v[1],v[2],v[3]); }
GLC void glVertexAttrib4sv(unsigned i, const short* v)         { VA4(i,v[0],v[1],v[2],v[3]); }
GLC void glVertexAttrib4iv(unsigned i, const int* v)           { VA4(i,v[0],v[1],v[2],v[3]); }
GLC void glVertexAttrib4dv(unsigned i, const double* v)        { VA4(i,v[0],v[1],v[2],v[3]); }
GLC void glVertexAttrib4ubv(unsigned i, const unsigned char* v){ VA4(i,v[0],v[1],v[2],v[3]); }
GLC void glVertexAttrib4usv(unsigned i, const unsigned short* v){ VA4(i,v[0],v[1],v[2],v[3]); }
GLC void glVertexAttrib4uiv(unsigned i, const unsigned* v)     { VA4(i,v[0],v[1],v[2],v[3]); }

GLC void glVertexAttrib4Nubv(unsigned i, const unsigned char* v)
    { VA4(i,v[0]/255.f,v[1]/255.f,v[2]/255.f,v[3]/255.f); }
GLC void glVertexAttrib4Nusv(unsigned i, const unsigned short* v)
    { VA4(i,v[0]/65535.f,v[1]/65535.f,v[2]/65535.f,v[3]/65535.f); }
GLC void glVertexAttrib4Nuiv(unsigned i, const unsigned* v)
    { VA4(i,v[0]/4294967295.0,v[1]/4294967295.0,v[2]/4294967295.0,v[3]/4294967295.0); }
GLC void glVertexAttrib4Nbv(unsigned i, const signed char* v)
    { VA4(i,k_nb(v[0],127),k_nb(v[1],127),k_nb(v[2],127),k_nb(v[3],127)); }
GLC void glVertexAttrib4Nsv(unsigned i, const short* v)
    { VA4(i,k_nb(v[0],32767),k_nb(v[1],32767),k_nb(v[2],32767),k_nb(v[3],32767)); }
GLC void glVertexAttrib4Niv(unsigned i, const int* v)
    { VA4(i,k_nb(v[0],2147483647),k_nb(v[1],2147483647),k_nb(v[2],2147483647),k_nb(v[3],2147483647)); }

GLC void glGetVertexAttribdv(unsigned i, unsigned p, double* v) {
    float f[4] = {0}; int n = (p == 0x8626) ? 4 : 1;
    RP(glGetVertexAttribfv, void (*)(unsigned, unsigned, float*))(i, p, f);
    for (int k = 0; k < n; k++) v[k] = f[k];
}

/* ===== M) ARB_multi_bind: buffers, samplers, vertex buffers ===== */
typedef void (*M_bbb)(unsigned, unsigned, unsigned);
typedef void (*M_bbr)(unsigned, unsigned, unsigned, long, long);
typedef void (*M_bs)(unsigned, unsigned);
typedef void (*M_bvb)(unsigned, unsigned, long, int);
typedef void (*M_at)(unsigned);
typedef void (*M_bt)(unsigned, unsigned);
typedef void (*M_bit)(unsigned, unsigned, int, unsigned char, int, unsigned, unsigned);
  
GLC void glBindBuffersBase(unsigned target, unsigned first, int count, const unsigned* b) {
    for (int i = 0; i < count; i++)
        RP(glBindBufferBase, M_bbb)(target, first + (unsigned)i, b ? b[i] : 0);
}

GLC void glBindBuffersRange(unsigned target, unsigned first, int count, const unsigned* b,
                            const long* off, const long* size) {
    for (int i = 0; i < count; i++) {
        if (b && b[i]) RP(glBindBufferRange, M_bbr)(target, first + (unsigned)i, b[i], off[i], size[i]);
        else           RP(glBindBufferBase, M_bbb)(target, first + (unsigned)i, 0);
    }
}

GLC void glBindSamplers(unsigned first, int count, const unsigned* s) {
    for (int i = 0; i < count; i++)
        RP(glBindSampler, M_bs)(first + (unsigned)i, s ? s[i] : 0);
}

GLC void glBindVertexBuffers(unsigned first, int count, const unsigned* b,
                             const long* off, const int* stride) {
    for (int i = 0; i < count; i++)
        RP(glBindVertexBuffer, M_bvb)(first + (unsigned)i, b ? b[i] : 0, b ? off[i] : 0, b ? stride[i] : 0);
}

GLC void glBindTextures(unsigned first, int count, const unsigned* textures) {
    for (int i = 0; i < count; i++) {
        unsigned tex = textures ? textures[i] : 0;
        RP(glActiveTexture, M_at)(0x84C0 + first + i);
        RP(glBindTexture, M_bt)(0x0DE1, tex);
    }
    RP(glActiveTexture, M_at)(0x84C0);
}

GLC void glBindImageTextures(unsigned first, int count, const unsigned* textures) {
    for (int i = 0; i < count; i++) {
        unsigned tex = textures ? textures[i] : 0;
        RP(glBindImageTexture, M_bit)(first + i, tex, 0, 0, 0, 0x8B8C, 0x8814);
    }
}

/* ===== Wrappers for glTexGen ===== */
typedef void (*F_tgfv)(unsigned, unsigned, const float*);
typedef void (*F_tgiv)(unsigned, unsigned, const int*);

GLC void glTexGenfv(unsigned coord, unsigned pname, const float* params) {
    RP(glTexGenfvOES, F_tgfv)(coord, pname, params);
}

GLC void glTexGeniv(unsigned coord, unsigned pname, const int* params) {
    RP(glTexGenivOES, F_tgiv)(coord, pname, params);
}

GLC void glTexGenf(unsigned coord, unsigned pname, float param) {
    float p[4] = { param, 0.0f, 0.0f, 0.0f };
    RP(glTexGenfvOES, F_tgfv)(coord, pname, p);
}

GLC void glTexGeni(unsigned coord, unsigned pname, int param) {
    int p[4] = { param, 0, 0, 0 };
    RP(glTexGenivOES, F_tgiv)(coord, pname, p);
}

GLC void glTexGenx(unsigned coord, unsigned pname, int param) {
    float p[4] = { (float)param, 0.0f, 0.0f, 0.0f };
    RP(glTexGenfvOES, F_tgfv)(coord, pname, p);
}

GLC void glTexGenxv(unsigned coord, unsigned pname, const int* params) {
    float p[4] = { (float)params[0], (float)params[1], (float)params[2], (float)params[3] };
    RP(glTexGenfvOES, F_tgfv)(coord, pname, p);
}
  
/* ===== L) GL 3.2 multisample textures ===== */
typedef void (*L_tsm2)(unsigned, int, unsigned, int, int, unsigned char);
typedef void (*L_tsm3)(unsigned, int, unsigned, int, int, int, unsigned char);

GLC void glTexImage2DMultisample(unsigned target, int samples, unsigned ifmt,
                                 int w, int h, unsigned char fixed) {
    RP(glTexStorage2DMultisample, L_tsm2)(target, samples, ifmt, w, h, fixed);
}

GLC void glTexImage3DMultisample(unsigned target, int samples, unsigned ifmt,
                                 int w, int h, int d, unsigned char fixed) {
    RP(glTexStorage3DMultisampleOES, L_tsm3)(target, samples, ifmt, w, h, d, fixed);
}

/* ===== N) glDrawTransformFeedback over ES3 ===== */
typedef void (*N_gq)(int, unsigned*);
typedef void (*N_bq)(unsigned, unsigned);
typedef void (*N_eq)(unsigned);
typedef void (*N_gqo)(unsigned, unsigned, unsigned*);
typedef void (*N_btf)(unsigned);
typedef void (*N_btf2)(unsigned, unsigned);
typedef void (*N_da)(unsigned, int, int);

#define TFO_MAX 4096
static unsigned tfo_q[TFO_MAX];
static int      tfo_vpp[TFO_MAX];
static unsigned tfo_cur;

GLC void glBindTransformFeedback(unsigned target, unsigned id) {
    tfo_cur = id;
    RP(glBindTransformFeedback, N_btf2)(target, id);
}

GLC void glBeginTransformFeedback(unsigned mode) {
    unsigned id = tfo_cur;
    if (id < TFO_MAX) {
        if (!tfo_q[id]) RP(glGenQueries, N_gq)(1, &tfo_q[id]);
        tfo_vpp[id] = mode == 0x0000 ? 1 : (mode == 0x0001 ? 2 : 3);
        RP(glBeginQuery, N_bq)(0x8C88, tfo_q[id]);
    }
    RP(glBeginTransformFeedback, N_btf)(mode);
}

GLC void glEndTransformFeedback(void) {
    RP(glEndTransformFeedback, void (*)(void))();
    if (tfo_cur < TFO_MAX && tfo_q[tfo_cur]) RP(glEndQuery, N_eq)(0x8C88);
}

GLC void glDrawTransformFeedback(unsigned mode, unsigned id) {
    unsigned n = 0;
    if (id < TFO_MAX && tfo_q[id]) {
        RP(glGetQueryObjectuiv, N_gqo)(tfo_q[id], 0x8866, &n);
        n *= (unsigned)tfo_vpp[id];
    }
    RP(glDrawArrays, N_da)(mode, 0, (int)n);
}

GLC void glDrawTransformFeedbackStream(unsigned mode, unsigned id, unsigned stream) {
    (void)stream; glDrawTransformFeedback(mode, id);
}

GLC void glDrawTransformFeedbackInstanced(unsigned mode, unsigned id, int inst) {
    unsigned n = 0;
    if (id < TFO_MAX && tfo_q[id]) {
        RP(glGetQueryObjectuiv, N_gqo)(tfo_q[id], 0x8866, &n);
        n *= (unsigned)tfo_vpp[id];
    }
    RP(glDrawArraysInstanced, void (*)(unsigned,int,int,int))(mode, 0, (int)n, inst);
}

/* ===== O) GL_ARB_ES3_2_compatibility ===== */
typedef void (*O_pbb)(float, float, float, float, float, float, float, float);

GLC void glPrimitiveBoundingBoxARB(float minX, float minY, float minZ, float minW,
                                   float maxX, float maxY, float maxZ, float maxW) {
    void* p = p_glPrimitiveBoundingBox;
    if (!p) p = p_glPrimitiveBoundingBoxEXT;
    if (!p) p = p_glPrimitiveBoundingBoxOES;
    if (p) ((O_pbb)p)(minX, minY, minZ, minW, maxX, maxY, maxZ, maxW);
}
      
#include "gl_es_alias.c"
