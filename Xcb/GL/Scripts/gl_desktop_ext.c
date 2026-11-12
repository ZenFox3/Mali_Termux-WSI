/* gl_desktop_ext.c - missing desktop functions in ES (included by glx_shim.c after gl_es_emul.c) */
#include <stdio.h>

typedef void (*F_mda)(unsigned, const int*, const int*, int);
static void ad_dump(unsigned, int, int, int, int, int, int, unsigned);
static void ad_install(void);

static void dx_warn(const char* m) {
    if (gl_compat_debug_enabled()) fprintf(stderr, "[gl_desktop] %s\n", m);
}

GLC void glMultiDrawArrays(unsigned mode, const int* first, const int* count, int n) {
    for (int i = 0; i < n; i++) if (count[i] > 0) RP(glDrawArrays, F_da)(mode, first[i], count[i]);
}

GLC void glMultiDrawElements(unsigned mode, const int* count, unsigned type, const void* const* idx, int n) {
    for (int i = 0; i < n; i++) if (count[i] > 0) RP(glDrawElements, F_de)(mode, count[i], type, idx[i]);
}

/* ES3 only supports primitive restart on the maximum index of the type (0xFF/0xFFFF/0xFFFFFFFF) */
GLC void glPrimitiveRestartIndex(unsigned idx) {
    if (idx != 0xFFu && idx != 0xFFFFu && idx != 0xFFFFFFFFu)
        dx_warn("glPrimitiveRestartIndex: only the maximum index of the type is supported");
}

GLC void glProvokingVertex(unsigned mode) {
    if (mode == 0x8E4D) dx_warn("glProvokingVertex(FIRST) is not supported in ES (remains LAST)");
}

GLC void glClampColor(unsigned target, unsigned clamp) { (void)target; (void)clamp; }

static int dx_emulated(void) { return T_glx.ctx && T_glx.ctx->ver >= 2 && !T_glx.ctx->es_native; }

extern void* G_mali;   /* from libGLESv2_shim.c */

GLC void glLogicOp(unsigned op) {
    static void (*real)(unsigned);
    if (!real && G_mali)
        real = (void (*)(unsigned))dlsym(G_mali, "glLogicOp");
    if (real) { real(op); return; }

    if (dx_emulated()) {
        if (op != 0x1503) dx_warn("glLogicOp != GL_COPY ignored");
        return;
    }
    RP(glLogicOp, F_ui)(op);
}

GLC unsigned char glIsEnabled(unsigned cap) {
    if (gl_cap_ignored(cap)) return 0;
    return RP(glIsEnabled, F_isen)(gl_cap_xlat(cap));
}

/* ---------- glPolygonMode: LINE/POINT emulated via custom index buffer ---------- */
typedef void (*F_bdata)(unsigned, long, const void*, unsigned);

static unsigned pm_mode = 0x1B02;       /* GL_FILL */
static unsigned pm_ebo;

GLC void glPolygonMode(unsigned face, unsigned mode) {
    (void)face;
    if (mode == 0x1B00 || mode == 0x1B01 || mode == 0x1B02) pm_mode = mode;
    if (!dx_emulated() && mode != 0x1B02) dx_warn("glPolygonMode(LINE/POINT) unimplemented in native ES/ES1 context");
}

/* v[n] = vertex indices for TRIANGLES(4)/STRIP(5)/FAN(6) -> lines or points */
static unsigned* pm_expand(unsigned mode, const unsigned* v, int n, int* outn, unsigned* outmode) {
    int tris = mode == 4 ? n / 3 : n - 2, pts = (pm_mode == 0x1B00);
    if (tris <= 0) return NULL;
    unsigned* o = malloc((size_t)tris * (pts ? 3 : 6) * sizeof *o);
    if (!o) return NULL;
    int k = 0;
    for (int t = 0; t < tris; t++) {
        unsigned a, b, c;
        if (mode == 4)      { a = v[3 * t]; b = v[3 * t + 1]; c = v[3 * t + 2]; }
        else if (mode == 5) { a = v[t];     b = v[t + 1];     c = v[t + 2]; }
        else                { a = v[0];     b = v[t + 1];     c = v[t + 2]; }
        if (pts) { o[k++] = a; o[k++] = b; o[k++] = c; }
        else { o[k++] = a; o[k++] = b; o[k++] = b; o[k++] = c; o[k++] = c; o[k++] = a; }
    }
    *outn = k; *outmode = pts ? 0 : 1;
    return o;
}

static void pm_draw_idx(unsigned mode, const unsigned* v, int n) {
    int cnt = 0, prev = 0; unsigned gm = 0;
    unsigned* o = pm_expand(mode, v, n, &cnt, &gm);
    if (!o) return;
    RP(glGetIntegerv, F_geti)(0x8895, &prev);                    /* ELEMENT_ARRAY_BUFFER_BINDING (part of VAO) */
    if (!pm_ebo) RP(glGenBuffers, F_gentex)(1, &pm_ebo);
    RP(glBindBuffer, F_uiui)(0x8893, pm_ebo);
    RP(glBufferData, F_bdata)(0x8893, (long)cnt * 4, o, 0x88E0); /* STREAM_DRAW */
    RP(glDrawElements, F_de)(gm, cnt, 0x1405, (const void*)0);
    RP(glBindBuffer, F_uiui)(0x8893, (unsigned)prev);            /* restores VAO's EBO */
    free(o);
}

GLC void glDrawArrays(unsigned mode, int first, int count) {
    if (pm_mode != 0x1B02 && mode >= 4 && mode <= 6 && count >= 3 && dx_emulated()) {
        unsigned* v = malloc((size_t)count * sizeof *v);
        if (v) {
            for (int i = 0; i < count; i++) v[i] = (unsigned)(first + i);
            pm_draw_idx(mode, v, count); free(v); return;
        }
    }
    RP(glDrawArrays, F_da)(mode, first, count);
}

GLC void glDrawElements(unsigned mode, int count, unsigned type, const void* ind) {
    int tb = type == 0x1401 ? 1 : type == 0x1403 ? 2 : type == 0x1405 ? 4 : 0;
    if (pm_mode == 0x1B02 || mode < 4 || mode > 6 || count < 3 || !tb || !dx_emulated()) {
        RP(glDrawElements, F_de)(mode, count, type, ind); return;
    }
    int eb = 0;
    RP(glGetIntegerv, F_geti)(0x8895, &eb);
    const uint8_t* src = ind;
    if (eb) {                                                    /* "ind" is an offset in buffer: read via mapping */
        src = RP(glMapBufferRange, F_mapr)(0x8893, (long)(uintptr_t)ind, (long)count * tb, 0x0001);
        if (!src) { RP(glDrawElements, F_de)(mode, count, type, ind); return; }
    }
    unsigned* v = malloc((size_t)count * sizeof *v);
    if (v) {
        for (int i = 0; i < count; i++)
            v[i] = tb == 1 ? src[i] : tb == 2 ? ((const uint16_t*)src)[i] : ((const uint32_t*)src)[i];
    }
    if (eb) RP(glUnmapBuffer, F_unmap)(0x8893);
    if (!v) { RP(glDrawElements, F_de)(mode, count, type, ind); return; }
    pm_draw_idx(mode, v, count); free(v);
}

GLC void glTexImage2D(unsigned t, int l, int i, int w, int h, int b, unsigned f, unsigned ty, const void* p) {
    if (f == 0x80E1) {                                   /* GL_BGRA */
        if (ty == 0x8367) ty = 0x1401;                   /* UNSIGNED_INT_8_8_8_8_REV -> UNSIGNED_BYTE */
        if (i == 0x1908 || i == 0x8058) i = 0x80E1;      /* internal = BGRA_EXT (ES requires format == internal) */
    }
    RP(glTexImage2D, F_teximg)(t1d(t), l, i, w, h, b, f, ty, p);
    if (t == 0x84F5 && l == 0 && dx_emulated()) {
        RP(glTexParameteri, F_da)(0x0DE1, 0x2801, 0x2601);
        RP(glTexParameteri, F_da)(0x0DE1, 0x2802, 0x812F);
        RP(glTexParameteri, F_da)(0x0DE1, 0x2803, 0x812F);
    }
}

GLC void glTexSubImage2D(unsigned t, int l, int xo, int yo, int w, int h, unsigned f, unsigned ty, const void* p) {
    if (f == 0x80E1 && ty == 0x8367) ty = 0x1401;
    RP(glTexSubImage2D, F_teximg)(t1d(t), l, xo, yo, w, h, f, ty, p);
    if (t == 0x0DE1) ad_dump(0x0DE1, l, xo, yo, 0, w, h, f);
}
