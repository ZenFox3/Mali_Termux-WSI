/*
 * gl_compat.c - subset of desktop OpenGL 1.x over GLES 1.1 (included by glx_shim.c).
 *
 *   glBegin/glEnd/glVertex*  -> client-side vertex arrays (QUADS -> triangles, QUAD_STRIP -> TRIANGLE_STRIP,
 *                               POLYGON -> TRIANGLE_FAN)
 *   glNewList/glCallList     -> recorded and replayed commands (Begin..End blocks bake into vertex arrays)
 *   glMaterialfv(GL_FRONT)   -> GL_FRONT_AND_BACK (the only one accepted by ES 1.1)
 *   glFrustum/glOrtho/...d   -> float variants from Mali
 *
 * Works only in a GLES 1.x context (GLX_ES=1). ES2/3 lacks fixed-functionality.
 * Recorded in lists: Begin/End, Normal, Color, TexCoord, Material, Light, ShadeModel, FrontFace,
 * Enable/Disable, Push/PopMatrix, Translate/Rotate/Scale, MultMatrix, LoadIdentity, BindTexture, CallList.
 * Any other call made between glNewList and glEndList executes immediately (unrecorded).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

#define GLC __attribute__((visibility("default")))
#define RP(n, T) ((T)p_##n)

/* Convert TEXTURE_1D (0x0DE0) -> TEXTURE_2D (0x0DE1) */
#ifndef T1D_DEFINED
#define T1D_DEFINED
static inline unsigned t1d(unsigned t) { 
    return (t == 0x0DE0 || t == 0x8C18 || t == 0x84F5) ? 0x0DE1 : t; 
}
#endif

typedef void (*F_v)(void);
typedef void (*F_ui)(unsigned);
typedef void (*F_uiui)(unsigned, unsigned);
typedef void (*F_fff)(float, float, float);
typedef void (*F_ffff)(float, float, float, float);
typedef void (*F_utex)(unsigned, float, float, float, float);
typedef void (*F_mat)(unsigned, unsigned, const float*);
typedef void (*F_pf)(const float*);
typedef unsigned char (*F_isen)(unsigned);
typedef void (*F_vptr)(int, unsigned, int, const void*);
typedef void (*F_nptr)(unsigned, int, const void*);
typedef void (*F_da)(unsigned, int, int);
typedef void (*F_de)(unsigned, int, unsigned, const void*);
typedef void (*F_6f)(float, float, float, float, float, float);
typedef void (*F_f)(float);
typedef void (*F_ff)(float, float);
typedef void (*F_uif)(unsigned, float);
typedef void (*F_clip)(unsigned, const float*);
typedef void (*F_getf)(unsigned, float*);
typedef void (*F_geti)(unsigned, int*);
typedef const unsigned char* (*F_gstri)(unsigned, unsigned);
typedef void (*F_gentex)(int, unsigned*);
typedef void (*F_teximg)(unsigned, int, int, int, int, int, unsigned, unsigned, const void*);   /* and TexSubImage2D */
typedef void (*F_texenv)(unsigned, unsigned, float);
typedef void (*F_texgeti)(unsigned, unsigned, int*);

static void cc_note_front(unsigned m);
static void cc_setfront(void);

enum { OP_DRAW = 1, OP_NORMAL, OP_COLOR, OP_TEXCOORD, OP_MATERIAL, OP_SHADE, OP_FRONTFACE, OP_ENABLE,
       OP_DISABLE, OP_PUSH, OP_POP, OP_TRANSLATE, OP_ROTATE, OP_SCALE, OP_MULT, OP_IDENT, OP_LIGHT,
       OP_BINDTEX, OP_CALL, OP_WINDOWPOS, OP_RASTERPOS };

typedef struct { int op; unsigned u[2]; float f[16]; void* p; } gl_cmd;
typedef struct { int mode, n; float *pos, *nrm, *col, *tex; } gl_block;
typedef struct { gl_cmd* c; int n, cap, used; } gl_list;

static gl_list* gl_lists;
static unsigned gl_lists_cap, gl_next_list = 1;
static pthread_mutex_t gl_lmu = PTHREAD_MUTEX_INITIALIZER;

static __thread struct {
    int init, in_begin, mode, n, cap, has_n, has_c, has_t;
    float *pos, *nrm, *col, *tex;
    float cn[3], cc[4], ct[2];
    unsigned rec_id;
    int rec_mode;                      /* 0x1300 GL_COMPILE, 0x1301 GL_COMPILE_AND_EXECUTE */
} IM;

/* raster position (window coordinates) + zoom, per thread/context */
static __thread float g_rasterx, g_rastery, g_zoomx = 1.0f, g_zoomy = 1.0f;
static __thread int   g_rvalid = 1;

/* persistent texture for glDrawPixels */
static __thread unsigned g_dp_tex, g_dp_pw, g_dp_ph, g_dp_ifmt, g_dp_type;

static inline int gl_compat_debug_enabled(void) {
    const char* env_gl = getenv("GL_DEBUG");
    const char* env_mali = getenv("MALI_DEBUG");
    const char* env_egl = getenv("EGL_DEBUG");
    return (env_gl && env_gl[0] != '0') || 
           (env_mali && env_mali[0] != '0') || 
           (env_egl && env_egl[0] != '0');
}

static void im_init(void) {
    if (IM.init) return;
    IM.init = 1;
    IM.cn[0] = 0.0f; IM.cn[1] = 0.0f; IM.cn[2] = 1.0f;
    IM.cc[0] = IM.cc[1] = IM.cc[2] = IM.cc[3] = 1.0f;
    IM.ct[0] = IM.ct[1] = 0.0f;
}

static void gl_warn_es(const char* fn) {
    static int warned;
    if (!warned && T_glx.ctx && T_glx.ctx->ver != 1) {
        warned = 1;
        fprintf(stderr, "libGL.so: warning: %s called in an ES%d context: fixed-function pipeline only exists in ES1. "
                        "Run with GLX_ES=1\n", fn, T_glx.ctx->ver);
    }
}

GLC void glWindowPos3f(float x, float y, float z);
GLC void glRasterPos4f(float x, float y, float z, float w);

/* ---------- Display Lists ---------- */
static void lists_ensure(unsigned n) {
    if (n <= gl_lists_cap) return;
    unsigned nc = gl_lists_cap ? gl_lists_cap : 64;
    while (nc < n) nc *= 2;
    gl_lists = realloc(gl_lists, nc * sizeof *gl_lists);
    memset(gl_lists + gl_lists_cap, 0, (nc - gl_lists_cap) * sizeof *gl_lists);
    gl_lists_cap = nc;
}

static void list_clear(gl_list* L) {
    for (int i = 0; i < L->n; i++)
        if (L->c[i].op == OP_DRAW && L->c[i].p) {
            gl_block* b = L->c[i].p;
            free(b->pos); free(b->nrm); free(b->col); free(b->tex); free(b);
        }
    free(L->c);
    memset(L, 0, sizeof *L);
}

static gl_cmd* rec_cmd(int op) {
    gl_list* L = &gl_lists[IM.rec_id];
    if (L->n == L->cap) {
        L->cap = L->cap ? L->cap * 2 : 64;
        L->c = realloc(L->c, (size_t)L->cap * sizeof(gl_cmd));
    }
    gl_cmd* c = &L->c[L->n++];
    memset(c, 0, sizeof *c);
    c->op = op;
    return c;
}
#define REC_SKIP() (IM.rec_mode == 0x1300)      /* GL_COMPILE: do not execute */

/* ---------- Drawing (Immediate Mode -> Arrays) ---------- */
static void im_draw(int mode, int n, const float* pos, const float* nrm, const float* col, const float* tex) {
    unsigned gm = 0;
    unsigned short* ix = NULL;
    int cnt = n;
    switch (mode) {
    case 0: case 1: case 2: case 3: case 4: case 5: case 6: gm = (unsigned)mode; break;
    case 7: {                                                   /* GL_QUADS -> 2 triangles / quad */
        if (n > 65536) n = 65536;
        int q = n / 4;
        if (q <= 0) return;
        ix = malloc((size_t)q * 6 * sizeof *ix);
        if (!ix) return;
        for (int i = 0; i < q; i++) {
            unsigned short b = (unsigned short)(i * 4);
            ix[i * 6 + 0] = b;     ix[i * 6 + 1] = b + 1; ix[i * 6 + 2] = b + 2;
            ix[i * 6 + 3] = b;     ix[i * 6 + 4] = b + 2; ix[i * 6 + 5] = b + 3;
        }
        cnt = q * 6; gm = 4;
        break; }
    case 8: gm = 5; break;                                      /* GL_QUAD_STRIP -> TRIANGLE_STRIP */
    case 9: gm = 6; break;                                      /* GL_POLYGON -> TRIANGLE_FAN */
    default: return;
    }
    static const unsigned caps[4] = { 0x8074, 0x8075, 0x8076, 0x8078 };   /* VERTEX, NORMAL, COLOR, TEXCOORD array */
    int want[4] = { 1, nrm != NULL, col != NULL, tex != NULL }, prev[4], i;
    for (i = 0; i < 4; i++) {
        prev[i] = RP(glIsEnabled, F_isen)(caps[i]);
        if (want[i] != prev[i])
            (want[i] ? RP(glEnableClientState, F_ui) : RP(glDisableClientState, F_ui))(caps[i]);
    }
    RP(glVertexPointer, F_vptr)(3, 0x1406, 0, pos);
    if (nrm) RP(glNormalPointer, F_nptr)(0x1406, 0, nrm);
    if (col) RP(glColorPointer, F_vptr)(4, 0x1406, 0, col);
    if (tex) RP(glTexCoordPointer, F_vptr)(2, 0x1406, 0, tex);
    if (ix) RP(glDrawElements, F_de)(gm, cnt, 0x1403, ix);
    else    RP(glDrawArrays, F_da)(gm, 0, cnt);
    for (i = 0; i < 4; i++)
        if (want[i] != prev[i])
            (want[i] ? RP(glDisableClientState, F_ui) : RP(glEnableClientState, F_ui))(caps[i]);
    free(ix);
    /* After block, "current" values are those of the last vertex */
    int l = n - 1;
    if (nrm) RP(glNormal3f, F_fff)(nrm[3 * l], nrm[3 * l + 1], nrm[3 * l + 2]);
    if (col) RP(glColor4f, F_ffff)(col[4 * l], col[4 * l + 1], col[4 * l + 2], col[4 * l + 3]);
    if (tex) RP(glMultiTexCoord4f, F_utex)(0x84C0, tex[2 * l], tex[2 * l + 1], 0.0f, 1.0f);
}

static float* fdup(const float* s, size_t n) {
    float* d = malloc(n * sizeof(float));
    if (d) memcpy(d, s, n * sizeof(float));
    return d;
}

static void im_vertex(float x, float y, float z) {
    if (!IM.in_begin) return;
    if (IM.n == IM.cap) {
        IM.cap = IM.cap ? IM.cap * 2 : 256;
        IM.pos = realloc(IM.pos, (size_t)IM.cap * 3 * sizeof(float));
        IM.nrm = realloc(IM.nrm, (size_t)IM.cap * 3 * sizeof(float));
        IM.col = realloc(IM.col, (size_t)IM.cap * 4 * sizeof(float));
        IM.tex = realloc(IM.tex, (size_t)IM.cap * 2 * sizeof(float));
    }
    float* p = IM.pos + 3 * IM.n; p[0] = x; p[1] = y; p[2] = z;
    memcpy(IM.nrm + 3 * IM.n, IM.cn, 3 * sizeof(float));
    memcpy(IM.col + 4 * IM.n, IM.cc, 4 * sizeof(float));
    memcpy(IM.tex + 2 * IM.n, IM.ct, 2 * sizeof(float));
    IM.n++;
}

GLC void glBegin(unsigned mode) {
    im_init();
    gl_warn_es("glBegin");
    IM.in_begin = 1; IM.mode = (int)mode; IM.n = 0;
    IM.has_n = IM.has_c = IM.has_t = 0;
}

GLC void glEnd(void) {
    if (!IM.in_begin) return;
    IM.in_begin = 0;
    int n = IM.n;
    if (n <= 0) return;
    const float *nr = IM.has_n ? IM.nrm : NULL, *co = IM.has_c ? IM.col : NULL, *te = IM.has_t ? IM.tex : NULL;
    if (IM.rec_id) {
        gl_block* b = calloc(1, sizeof *b);
        if (b) {
            b->mode = IM.mode; b->n = n;
            b->pos = fdup(IM.pos, (size_t)n * 3);
            if (nr) b->nrm = fdup(nr, (size_t)n * 3);
            if (co) b->col = fdup(co, (size_t)n * 4);
            if (te) b->tex = fdup(te, (size_t)n * 2);
            rec_cmd(OP_DRAW)->p = b;
        }
        if (REC_SKIP()) return;
    }
    im_draw(IM.mode, n, IM.pos, nr, co, te);
}

GLC void glVertex2f(float x, float y)                { im_vertex(x, y, 0.0f); }
GLC void glVertex3f(float x, float y, float z)       { im_vertex(x, y, z); }
GLC void glVertex4f(float x, float y, float z, float w) { (void)w; im_vertex(x, y, z); }
GLC void glVertex2fv(const float* v)                 { im_vertex(v[0], v[1], 0.0f); }
GLC void glVertex3fv(const float* v)                 { im_vertex(v[0], v[1], v[2]); }
GLC void glVertex4fv(const float* v)                 { im_vertex(v[0], v[1], v[2]); }
GLC void glVertex2i(int x, int y)                    { im_vertex((float)x, (float)y, 0.0f); }
GLC void glVertex3i(int x, int y, int z)             { im_vertex((float)x, (float)y, (float)z); }
GLC void glVertex4i(int x, int y, int z, int w)      { (void)w; im_vertex((float)x, (float)y, (float)z); }
GLC void glVertex2iv(const int* v)                   { im_vertex((float)v[0], (float)v[1], 0.0f); }
GLC void glVertex3iv(const int* v)                   { im_vertex((float)v[0], (float)v[1], (float)v[2]); }
GLC void glVertex4iv(const int* v)                   { im_vertex((float)v[0], (float)v[1], (float)v[2]); }
GLC void glVertex2s(short x, short y)                { im_vertex((float)x, (float)y, 0.0f); }
GLC void glVertex3s(short x, short y, short z)       { im_vertex((float)x, (float)y, (float)z); }
GLC void glVertex4s(short x, short y, short z, short w) { (void)w; im_vertex((float)x, (float)y, (float)z); }
GLC void glVertex2sv(const short* v)                 { im_vertex((float)v[0], (float)v[1], 0.0f); }
GLC void glVertex3sv(const short* v)                 { im_vertex((float)v[0], (float)v[1], (float)v[2]); }
GLC void glVertex4sv(const short* v)                 { im_vertex((float)v[0], (float)v[1], (float)v[2]); }
GLC void glVertex2d(double x, double y)              { im_vertex((float)x, (float)y, 0.0f); }
GLC void glVertex3d(double x, double y, double z)    { im_vertex((float)x, (float)y, (float)z); }
GLC void glVertex4d(double x, double y, double z, double w) { (void)w; im_vertex((float)x, (float)y, (float)z); }
GLC void glVertex2dv(const double* v)                { im_vertex((float)v[0], (float)v[1], 0.0f); }
GLC void glVertex3dv(const double* v)                { im_vertex((float)v[0], (float)v[1], (float)v[2]); }
GLC void glVertex4dv(const double* v)                { im_vertex((float)v[0], (float)v[1], (float)v[2]); }

/* Rectangles (desktop): a quad in the z=0 plane */
GLC void glRectf(float x1, float y1, float x2, float y2) {
    glBegin(7 /*GL_QUADS*/);
    im_vertex(x1, y1, 0.0f); im_vertex(x2, y1, 0.0f); im_vertex(x2, y2, 0.0f); im_vertex(x1, y2, 0.0f);
    glEnd();
}
GLC void glRectd(double x1, double y1, double x2, double y2) { glRectf((float)x1, (float)y1, (float)x2, (float)y2); }
GLC void glRecti(int x1, int y1, int x2, int y2)             { glRectf((float)x1, (float)y1, (float)x2, (float)y2); }
GLC void glRects(short x1, short y1, short x2, short y2)     { glRectf((float)x1, (float)y1, (float)x2, (float)y2); }
GLC void glRectfv(const float* a, const float* b)            { glRectf(a[0], a[1], b[0], b[1]); }
GLC void glRectdv(const double* a, const double* b)          { glRectf((float)a[0], (float)a[1], (float)b[0], (float)b[1]); }
GLC void glRectiv(const int* a, const int* b)                { glRectf((float)a[0], (float)a[1], (float)b[0], (float)b[1]); }
GLC void glRectsv(const short* a, const short* b)            { glRectf((float)a[0], (float)a[1], (float)b[0], (float)b[1]); }

/* ---------- Current Attributes (recordable) ---------- */
GLC void glNormal3f(float x, float y, float z) {
    im_init();
    IM.cn[0] = x; IM.cn[1] = y; IM.cn[2] = z;
    if (IM.in_begin) { IM.has_n = 1; return; }
    if (IM.rec_id) {
        gl_cmd* c = rec_cmd(OP_NORMAL); c->f[0] = x; c->f[1] = y; c->f[2] = z;
        if (REC_SKIP()) return;
    }
    RP(glNormal3f, F_fff)(x, y, z);
}
GLC void glNormal3fv(const float* v) { glNormal3f(v[0], v[1], v[2]); }
GLC void glNormal3d(double x, double y, double z) { glNormal3f((float)x, (float)y, (float)z); }
GLC void glNormal3dv(const double* v) { glNormal3f((float)v[0], (float)v[1], (float)v[2]); }
GLC void glNormal3i(int x, int y, int z) { glNormal3f((float)x, (float)y, (float)z); }
GLC void glNormal3iv(const int* v) { glNormal3f((float)v[0], (float)v[1], (float)v[2]); }
GLC void glNormal3s(short x, short y, short z) { glNormal3f((float)x, (float)y, (float)z); }
GLC void glNormal3sv(const short* v) { glNormal3f((float)v[0], (float)v[1], (float)v[2]); }
GLC void glNormal3b(signed char x, signed char y, signed char z) { glNormal3f(x / 127.0f, y / 127.0f, z / 127.0f); }
GLC void glNormal3bv(const signed char* v) { glNormal3f(v[0] / 127.0f, v[1] / 127.0f, v[2] / 127.0f); }

GLC void glColor4f(float r, float g, float b, float a) {
    im_init();
    IM.cc[0] = r; IM.cc[1] = g; IM.cc[2] = b; IM.cc[3] = a;
    if (IM.in_begin) { IM.has_c = 1; return; }
    if (IM.rec_id) {
        gl_cmd* c = rec_cmd(OP_COLOR); c->f[0] = r; c->f[1] = g; c->f[2] = b; c->f[3] = a;
        if (REC_SKIP()) return;
    }
    RP(glColor4f, F_ffff)(r, g, b, a);
}
GLC void glColor3f(float r, float g, float b)  { glColor4f(r, g, b, 1.0f); }
GLC void glColor3fv(const float* v)            { glColor4f(v[0], v[1], v[2], 1.0f); }
GLC void glColor4fv(const float* v)            { glColor4f(v[0], v[1], v[2], v[3]); }
GLC void glColor3d(double r, double g, double b) { glColor4f((float)r, (float)g, (float)b, 1.0f); }
GLC void glColor4d(double r, double g, double b, double a) { glColor4f((float)r, (float)g, (float)b, (float)a); }
GLC void glColor3dv(const double* v)           { glColor4f((float)v[0], (float)v[1], (float)v[2], 1.0f); }
GLC void glColor4dv(const double* v)           { glColor4f((float)v[0], (float)v[1], (float)v[2], (float)v[3]); }
GLC void glColor3ub(unsigned char r, unsigned char g, unsigned char b) { glColor4f(r / 255.0f, g / 255.0f, b / 255.0f, 1.0f); }
GLC void glColor4ub(unsigned char r, unsigned char g, unsigned char b, unsigned char a) { glColor4f(r / 255.0f, g / 255.0f, b / 255.0f, a / 255.0f); }
GLC void glColor3ubv(const unsigned char* v)   { glColor4f(v[0] / 255.0f, v[1] / 255.0f, v[2] / 255.0f, 1.0f); }
GLC void glColor4ubv(const unsigned char* v)   { glColor4f(v[0] / 255.0f, v[1] / 255.0f, v[2] / 255.0f, v[3] / 255.0f); }
GLC void glColor3ui(unsigned r, unsigned g, unsigned b) { glColor4f(r / 4294967295.0f, g / 4294967295.0f, b / 4294967295.0f, 1.0f); }
GLC void glColor4ui(unsigned r, unsigned g, unsigned b, unsigned a) { glColor4f(r / 4294967295.0f, g / 4294967295.0f, b / 4294967295.0f, a / 4294967295.0f); }
GLC void glColor3uiv(const unsigned* v)         { glColor3ui(v[0], v[1], v[2]); }
GLC void glColor4uiv(const unsigned* v)         { glColor4ui(v[0], v[1], v[2], v[3]); }
GLC void glColor3us(unsigned short r, unsigned short g, unsigned short b) { glColor4f(r / 65535.0f, g / 65535.0f, b / 65535.0f, 1.0f); }
GLC void glColor4us(unsigned short r, unsigned short g, unsigned short b, unsigned short a) { glColor4f(r / 65535.0f, g / 65535.0f, b / 65535.0f, a / 65535.0f); }
GLC void glColor3usv(const unsigned short* v)   { glColor3us(v[0], v[1], v[2]); }
GLC void glColor4usv(const unsigned short* v)   { glColor4us(v[0], v[1], v[2], v[3]); }

/* ---------- TexCoord ---------- */
GLC void glTexCoord2f(float s, float t) {
    im_init();
    IM.ct[0] = s; IM.ct[1] = t;
    if (IM.in_begin) { IM.has_t = 1; return; }
    if (IM.rec_id) {
        gl_cmd* c = rec_cmd(OP_TEXCOORD); c->f[0] = s; c->f[1] = t;
        if (REC_SKIP()) return;
    }
    RP(glMultiTexCoord4f, F_utex)(0x84C0, s, t, 0.0f, 1.0f);
}
GLC void glTexCoord2fv(const float* v)          { glTexCoord2f(v[0], v[1]); }
GLC void glTexCoord1f(float s)                   { glTexCoord2f(s, 0.0f); }
GLC void glTexCoord1fv(const float* v)          { glTexCoord2f(v[0], 0.0f); }
GLC void glTexCoord3f(float s, float t, float r) { (void)r; glTexCoord2f(s, t); }
GLC void glTexCoord3fv(const float* v)          { glTexCoord2f(v[0], v[1]); }
GLC void glTexCoord4f(float s, float t, float r, float q) { (void)r; (void)q; glTexCoord2f(s, t); }
GLC void glTexCoord4fv(const float* v)          { glTexCoord2f(v[0], v[1]); }

GLC void glTexCoord1d(double s)                 { glTexCoord2f((float)s, 0.0f); }
GLC void glTexCoord2d(double s, double t)       { glTexCoord2f((float)s, (float)t); }
GLC void glTexCoord3d(double s, double t, double r) { (void)r; glTexCoord2f((float)s, (float)t); }
GLC void glTexCoord4d(double s, double t, double r, double q) { (void)r; (void)q; glTexCoord2f((float)s, (float)t); }
GLC void glTexCoord1dv(const double* v)         { glTexCoord2f((float)v[0], 0.0f); }
GLC void glTexCoord2dv(const double* v)         { glTexCoord2f((float)v[0], (float)v[1]); }
GLC void glTexCoord3dv(const double* v)         { glTexCoord2f((float)v[0], (float)v[1]); }
GLC void glTexCoord4dv(const double* v)         { glTexCoord2f((float)v[0], (float)v[1]); }

GLC void glTexCoord1i(int s)                    { glTexCoord2f((float)s, 0.0f); }
GLC void glTexCoord2i(int s, int t)            { glTexCoord2f((float)s, (float)t); }
GLC void glTexCoord3i(int s, int t, int r)      { (void)r; glTexCoord2f((float)s, (float)t); }
GLC void glTexCoord4i(int s, int t, int r, int q) { (void)r; (void)q; glTexCoord2f((float)s, (float)t); }
GLC void glTexCoord1iv(const int* v)            { glTexCoord2f((float)v[0], 0.0f); }
GLC void glTexCoord2iv(const int* v)            { glTexCoord2f((float)v[0], (float)v[1]); }
GLC void glTexCoord3iv(const int* v)            { glTexCoord2f((float)v[0], (float)v[1]); }
GLC void glTexCoord4iv(const int* v)            { glTexCoord2f((float)v[0], (float)v[1]); }

GLC void glTexCoord1s(short s)                  { glTexCoord2f((float)s, 0.0f); }
GLC void glTexCoord2s(short s, short t)          { glTexCoord2f((float)s, (float)t); }
GLC void glTexCoord3s(short s, short t, short r) { (void)r; glTexCoord2f((float)s, (float)t); }
GLC void glTexCoord4s(short s, short t, short r, short q) { (void)r; (void)q; glTexCoord2f((float)s, (float)t); }
GLC void glTexCoord1sv(const short* v)          { glTexCoord2f((float)v[0], 0.0f); }
GLC void glTexCoord2sv(const short* v)          { glTexCoord2f((float)v[0], (float)v[1]); }
GLC void glTexCoord3sv(const short* v)          { glTexCoord2f((float)v[0], (float)v[1]); }
GLC void glTexCoord4sv(const short* v)          { glTexCoord2f((float)v[0], (float)v[1]); }

/* ---------- Fixed-function Recordable Calls ---------- */
static int light_count(unsigned pname) {
    if (pname >= 0x1205 && pname <= 0x1209) return 1;      /* SPOT_EXPONENT .. QUADRATIC_ATTENUATION */
    if (pname == 0x1204) return 3;                          /* SPOT_DIRECTION */
    return 4;
}

GLC void glMaterialfv(unsigned face, unsigned pname, const float* v) {
    (void)face;                                             /* ES 1.1 accepts GL_FRONT_AND_BACK only */
    if (IM.rec_id) {
        gl_cmd* c = rec_cmd(OP_MATERIAL);
        c->u[0] = 0x0408; c->u[1] = pname;
        memcpy(c->f, v, (pname == 0x1601 ? 1 : 4) * sizeof(float));
        if (REC_SKIP()) return;
    }
    RP(glMaterialfv, F_mat)(0x0408, pname, v);
}

GLC void glMaterialf(unsigned face, unsigned pname, float v) {
    glMaterialfv(face, pname, &v);
}

GLC void glLightfv(unsigned light, unsigned pname, const float* v) {
    if (IM.rec_id) {
        gl_cmd* c = rec_cmd(OP_LIGHT);
        c->u[0] = light; c->u[1] = pname;
        memcpy(c->f, v, (size_t)light_count(pname) * sizeof(float));
        if (REC_SKIP()) return;
    }
    RP(glLightfv, F_mat)(light, pname, v);
}

GLC void glLightf(unsigned light, unsigned pname, float v) {
    glLightfv(light, pname, &v);
}

#define REC1(OPC, FN, T, VAL)                                    \
    if (IM.rec_id) { gl_cmd* c = rec_cmd(OPC); c->u[0] = (VAL); if (REC_SKIP()) return; } \
    RP(FN, T)(VAL);

GLC void glShadeModel(unsigned m) { REC1(OP_SHADE, glShadeModel, F_ui, m) }
GLC void glFrontFace(unsigned m) {
    if (IM.rec_id) { gl_cmd* c = rec_cmd(OP_FRONTFACE); c->u[0] = m; if (REC_SKIP()) return; }
    cc_note_front(m);
    cc_setfront();
}
/* GL_TEXTURE_GEN_S/T/R (0x0C60..0x0C62) -> GL_TEXTURE_GEN_STR_OES (ES1 only; OES binds them together) */
static unsigned gl_cap_xlat(unsigned cap) {
    return (cap >= 0x0C60 && cap <= 0x0C62 && T_glx.ctx && T_glx.ctx->ver == 1) ? 0x8D60 : cap;
}

/* Capabilitati desktop inexistente in ES2/3: ignorate (altfel Mali da GL_INVALID_ENUM).
   Doar in contexte emulate (nu ES1, nu ES nativ), unde unele dintre ele sunt valide. */
static int gl_cap_ignored(unsigned cap) {
    if (cap == 0x0DE0) return 1;                                   /* TEXTURE_1D (mereu) */
    if (!T_glx.ctx || T_glx.ctx->ver < 2 || T_glx.ctx->es_native) return 0;
    switch (cap) {
    case 0x8F9D: case 0x8D69:                                      /* PRIMITIVE_RESTART (ES3: index fix, mereu activ) */
    case 0x8861: case 0x8642: case 0x864F: case 0x884F:            /* POINT_SPRITE, PROGRAM_POINT_SIZE, DEPTH_CLAMP, CUBE_MAP_SEAMLESS */
    case 0x809D: case 0x0B20: case 0x0B41: case 0x0BF2:            /* MULTISAMPLE, LINE_SMOOTH, POLYGON_SMOOTH, COLOR_LOGIC_OP */
    case 0x0DE1: case 0x8513:                                      /* TEXTURE_2D, TEXTURE_CUBE_MAP (legacy compat) */
    case 0x0B50: case 0x0BC0: case 0x0B60: case 0x0B57: case 0x0BA1: /* LIGHTING, ALPHA_TEST, FOG, COLOR_MATERIAL, NORMALIZE */
    case 0x0C60: case 0x0C61: case 0x0C62:                         /* TEXTURE_GEN_S/T/R */
      case 0x8DB9:   /* FRAMEBUFFER_SRGB */
case 0x0B10:   /* POINT_SMOOTH */
        return 1;
    }
    if ((cap >= 0x3000 && cap <= 0x3007) ||                        /* CLIP_DISTANCE0..7 */
        (cap >= 0x4000 && cap <= 0x4007)) return 1;                /* LIGHT0..7 */
    return 0;
}

/* Forward declaration pentru notificarea modulului Dual-Source Blending */
static void dsb_on_enable(unsigned cap, int on);

GLC void glEnable(unsigned cap) {
    if (gl_cap_ignored(cap)) return;
    cap = gl_cap_xlat(cap);
    REC1(OP_ENABLE, glEnable, F_ui, cap)
    dsb_on_enable(cap, 1);
}

GLC void glDisable(unsigned cap) {
    if (gl_cap_ignored(cap)) return;
    cap = gl_cap_xlat(cap);
    REC1(OP_DISABLE, glDisable, F_ui, cap)
    dsb_on_enable(cap, 0);
}

GLC void glBindTexture(unsigned target, unsigned tex) {
    target = t1d(target);                                  /* Convert GL_TEXTURE_1D -> GL_TEXTURE_2D */
    if (IM.rec_id) {
        gl_cmd* c = rec_cmd(OP_BINDTEX); c->u[0] = target; c->u[1] = tex;
        if (REC_SKIP()) return;
    }
    RP(glBindTexture, F_uiui)(target, tex);
}

GLC void glPushMatrix(void) {
    if (IM.rec_id) { rec_cmd(OP_PUSH); if (REC_SKIP()) return; }
    RP(glPushMatrix, F_v)();
}
GLC void glPopMatrix(void) {
    if (IM.rec_id) { rec_cmd(OP_POP); if (REC_SKIP()) return; }
    RP(glPopMatrix, F_v)();
}
GLC void glLoadIdentity(void) {
    if (IM.rec_id) { rec_cmd(OP_IDENT); if (REC_SKIP()) return; }
    RP(glLoadIdentity, F_v)();
}
GLC void glTranslatef(float x, float y, float z) {
    if (IM.rec_id) { gl_cmd* c = rec_cmd(OP_TRANSLATE); c->f[0] = x; c->f[1] = y; c->f[2] = z; if (REC_SKIP()) return; }
    RP(glTranslatef, F_fff)(x, y, z);
}
GLC void glScalef(float x, float y, float z) {
    if (IM.rec_id) { gl_cmd* c = rec_cmd(OP_SCALE); c->f[0] = x; c->f[1] = y; c->f[2] = z; if (REC_SKIP()) return; }
    RP(glScalef, F_fff)(x, y, z);
}
GLC void glRotatef(float a, float x, float y, float z) {
    if (IM.rec_id) {
        gl_cmd* c = rec_cmd(OP_ROTATE); c->f[0] = a; c->f[1] = x; c->f[2] = y; c->f[3] = z;
        if (REC_SKIP()) return;
    }
    RP(glRotatef, F_ffff)(a, x, y, z);
}
GLC void glMultMatrixf(const float* m) {
    if (IM.rec_id) { gl_cmd* c = rec_cmd(OP_MULT); memcpy(c->f, m, 16 * sizeof(float)); if (REC_SKIP()) return; }
    RP(glMultMatrixf, F_pf)(m);
}

/* ---------- Raster Position: ARB_window_pos + glRasterPos ---------- */
/* GLES lacks a raster position; we manage it manually (in window coordinates) and glDrawPixels consumes it. */
GLC void glWindowPos3f(float x, float y, float z) {
    im_init();
    gl_warn_es("glWindowPos3f");
    if (IM.rec_id) {
        gl_cmd* c = rec_cmd(OP_WINDOWPOS);
        c->f[0] = x; c->f[1] = y; c->f[2] = z;
        if (REC_SKIP()) return;
    }
    g_rasterx = x; g_rastery = y; g_rvalid = 1;
}

GLC void glWindowPos2f(float x, float y)             { glWindowPos3f(x, y, 0.0f); }
GLC void glWindowPos2i(int x, int y)                 { glWindowPos3f((float)x, (float)y, 0.0f); }
GLC void glWindowPos3i(int x, int y, int z)          { glWindowPos3f((float)x, (float)y, (float)z); }
GLC void glWindowPos2d(double x, double y)           { glWindowPos3f((float)x, (float)y, 0.0f); }
GLC void glWindowPos3d(double x, double y, double z) { glWindowPos3f((float)x, (float)y, (float)z); }
GLC void glWindowPos2s(short x, short y)             { glWindowPos3f((float)x, (float)y, 0.0f); }
GLC void glWindowPos3s(short x, short y, short z)    { glWindowPos3f((float)x, (float)y, (float)z); }

GLC void glWindowPos2fv(const float* v)              { glWindowPos3f(v[0], v[1], 0.0f); }
GLC void glWindowPos3fv(const float* v)              { glWindowPos3f(v[0], v[1], v[2]); }
GLC void glWindowPos2iv(const int* v)                { glWindowPos3f((float)v[0], (float)v[1], 0.0f); }
GLC void glWindowPos3iv(const int* v)                { glWindowPos3f((float)v[0], (float)v[1], (float)v[2]); }
GLC void glWindowPos2dv(const double* v)             { glWindowPos3f((float)v[0], (float)v[1], 0.0f); }
GLC void glWindowPos3dv(const double* v)             { glWindowPos3f((float)v[0], (float)v[1], (float)v[2]); }
GLC void glWindowPos2sv(const short* v)              { glWindowPos3f((float)v[0], (float)v[1], 0.0f); }
GLC void glWindowPos3sv(const short* v)              { glWindowPos3f((float)v[0], (float)v[1], (float)v[2]); }

GLC void glWindowPos2dARB(double x, double y)           { glWindowPos2d(x, y); }
GLC void glWindowPos2fARB(float x, float y)            { glWindowPos2f(x, y); }
GLC void glWindowPos2iARB(int x, int y)                { glWindowPos2i(x, y); }
GLC void glWindowPos2sARB(short x, short y)            { glWindowPos2s(x, y); }
GLC void glWindowPos2dvARB(const double *p)            { glWindowPos2dv(p); }
GLC void glWindowPos2fvARB(const float *p)             { glWindowPos2fv(p); }
GLC void glWindowPos2ivARB(const int *p)               { glWindowPos2iv(p); }
GLC void glWindowPos2svARB(const short *p)             { glWindowPos2sv(p); }
GLC void glWindowPos3dARB(double x, double y, double z) { glWindowPos3d(x, y, z); }
GLC void glWindowPos3fARB(float x, float y, float z)   { glWindowPos3f(x, y, z); }
GLC void glWindowPos3iARB(int x, int y, int z)         { glWindowPos3i(x, y, z); }
GLC void glWindowPos3sARB(short x, short y, short z)   { glWindowPos3s(x, y, z); }
GLC void glWindowPos3dvARB(const double *p)            { glWindowPos3dv(p); }
GLC void glWindowPos3fvARB(const float *p)             { glWindowPos3fv(p); }
GLC void glWindowPos3ivARB(const int *p)               { glWindowPos3iv(p); }
GLC void glWindowPos3svARB(const short *p)             { glWindowPos3sv(p); }

/* glRasterPos: CPU projection using current modelview/projection matrices (column-major) */
GLC void glRasterPos4f(float x, float y, float z, float w) {
    im_init();
    gl_warn_es("glRasterPos");
    if (IM.rec_id) {
        gl_cmd* c = rec_cmd(OP_RASTERPOS);
        c->f[0] = x; c->f[1] = y; c->f[2] = z; c->f[3] = w;
        if (REC_SKIP()) return;
    }
    float mv[16], pr[16];
    int vp[4];
    RP(glGetFloatv, F_getf)(0x0BA6, mv);        /* GL_MODELVIEW_MATRIX */
    RP(glGetFloatv, F_getf)(0x0BA7, pr);        /* GL_PROJECTION_MATRIX */
    RP(glGetIntegerv, F_geti)(0x0BA2, vp);      /* GL_VIEWPORT */
    float e[4] = { x, y, z, w }, t[4], c[4];
    for (int r = 0; r < 4; r++) t[r] = mv[r] * e[0] + mv[4 + r] * e[1] + mv[8 + r] * e[2] + mv[12 + r] * e[3];
    for (int r = 0; r < 4; r++) c[r] = pr[r] * t[0] + pr[4 + r] * t[1] + pr[8 + r] * t[2] + pr[12 + r] * t[3];
    if (c[3] <= 0.0f) { g_rvalid = 0; return; } /* Behind camera: invalid raster position */
    g_rasterx = vp[0] + (c[0] / c[3] * 0.5f + 0.5f) * vp[2];
    g_rastery = vp[1] + (c[1] / c[3] * 0.5f + 0.5f) * vp[3];
    g_rvalid = 1;
}
GLC void glRasterPos3f(float x, float y, float z)     { glRasterPos4f(x, y, z, 1.0f); }
GLC void glRasterPos2f(float x, float y)              { glRasterPos4f(x, y, 0.0f, 1.0f); }
GLC void glRasterPos2i(int x, int y)                  { glRasterPos4f((float)x, (float)y, 0.0f, 1.0f); }
GLC void glRasterPos3i(int x, int y, int z)           { glRasterPos4f((float)x, (float)y, (float)z, 1.0f); }
GLC void glRasterPos4i(int x, int y, int z, int w)    { glRasterPos4f((float)x, (float)y, (float)z, (float)w); }
GLC void glRasterPos2s(short x, short y)              { glRasterPos4f((float)x, (float)y, 0.0f, 1.0f); }
GLC void glRasterPos3s(short x, short y, short z)     { glRasterPos4f((float)x, (float)y, (float)z, 1.0f); }
GLC void glRasterPos4s(short x, short y, short z, short w) { glRasterPos4f((float)x, (float)y, (float)z, (float)w); }
GLC void glRasterPos2d(double x, double y)            { glRasterPos4f((float)x, (float)y, 0.0f, 1.0f); }
GLC void glRasterPos3d(double x, double y, double z)  { glRasterPos4f((float)x, (float)y, (float)z, 1.0f); }
GLC void glRasterPos4d(double x, double y, double z, double w) { glRasterPos4f((float)x, (float)y, (float)z, (float)w); }
GLC void glRasterPos2fv(const float* v)               { glRasterPos4f(v[0], v[1], 0.0f, 1.0f); }
GLC void glRasterPos3fv(const float* v)               { glRasterPos4f(v[0], v[1], v[2], 1.0f); }
GLC void glRasterPos4fv(const float* v)               { glRasterPos4f(v[0], v[1], v[2], v[3]); }
GLC void glRasterPos2iv(const int* v)                 { glRasterPos4f((float)v[0], (float)v[1], 0.0f, 1.0f); }
GLC void glRasterPos3iv(const int* v)                 { glRasterPos4f((float)v[0], (float)v[1], (float)v[2], 1.0f); }
GLC void glRasterPos4iv(const int* v)                 { glRasterPos4f((float)v[0], (float)v[1], (float)v[2], (float)v[3]); }
GLC void glRasterPos2sv(const short* v)               { glRasterPos4f((float)v[0], (float)v[1], 0.0f, 1.0f); }
GLC void glRasterPos3sv(const short* v)               { glRasterPos4f((float)v[0], (float)v[1], (float)v[2], 1.0f); }
GLC void glRasterPos4sv(const short* v)               { glRasterPos4f((float)v[0], (float)v[1], (float)v[2], (float)v[3]); }
GLC void glRasterPos2dv(const double* v)              { glRasterPos4f((float)v[0], (float)v[1], 0.0f, 1.0f); }
GLC void glRasterPos3dv(const double* v)              { glRasterPos4f((float)v[0], (float)v[1], (float)v[2], 1.0f); }
GLC void glRasterPos4dv(const double* v)              { glRasterPos4f((float)v[0], (float)v[1], (float)v[2], (float)v[3]); }

/* ---------- glDrawPixels (Emulation) ---------- */
GLC void glPixelZoom(float xfactor, float yfactor) { g_zoomx = xfactor; g_zoomy = yfactor; }

static unsigned dp_pot(unsigned v) { unsigned p = 1; while (p < v) p <<= 1; return p; }

/*
 * glDrawPixels emulation on GLES 1.1 (Mali):
 *  - Persistent POT texture (no NPOT, no per-frame gen/delete), loaded via glTexSubImage2D
 *  - internalformat == format, UNSIGNED_BYTE only (+ RGB 5_6_5); BGRA converted on CPU to RGBA
 *  - Ortho quad on viewport at raster position (glRasterPos / glWindowPos), respecting glPixelZoom
 *  - Preserves application's scissor/blend (matters on desktop); depth/lighting/fog/cull are disabled
 *  - All modified state is restored; uses direct RP pointers (bypasses display lists)
 * Limitations: not recorded in display lists, no pixel transfer (scale/bias/map), no
 *              GL_UNPACK_ROW_LENGTH (unavailable in ES 1.x).
 */
GLC void glDrawPixels(int w, int h, unsigned fmt, unsigned type, const void *px) {
    im_init();
    gl_warn_es("glDrawPixels");
    if (IM.in_begin) return;
    if (w <= 0 || h <= 0 || !px) return;
    if (IM.rec_id) {                                   /* Unsupported in display lists */
        if (gl_compat_debug_enabled()) {
            static int warned;
            if (!warned) { warned = 1; fprintf(stderr, "[gl_compat] glDrawPixels in display list: ignored\n"); }
        }
        return;
    }
    if (!g_rvalid) return;                             /* Invalid raster position -> draw nothing */

    int is565 = (type == 0x8363 && fmt == 0x1907);     /* UNSIGNED_SHORT_5_6_5 with RGB */
    if (type != 0x1401 && !is565) {
        if (gl_compat_debug_enabled()) {
            fprintf(stderr, "[gl_compat] glDrawPixels: Unsupported type 0x%x (UNSIGNED_BYTE only)\n", type);
        }
        return;
    }
    if (fmt != 0x1906 && fmt != 0x1907 && fmt != 0x1908 && fmt != 0x1909 && fmt != 0x190A && fmt != 0x80E1) {
        if (gl_compat_debug_enabled()) {
            fprintf(stderr, "[gl_compat] glDrawPixels: Unsupported format 0x%x\n", fmt);
        }
        return;
    }

    /* BGRA -> RGBA on CPU (safe across all Mali drivers) */
    unsigned char* conv = NULL;
    unsigned ifmt = fmt;
    if (fmt == 0x80E1) {
        const unsigned char* s = px;
        conv = malloc((size_t)w * h * 4);
        if (!conv) return;
        for (size_t i = 0, n = (size_t)w * h; i < n; i++) {
            conv[4 * i + 0] = s[4 * i + 2];
            conv[4 * i + 1] = s[4 * i + 1];
            conv[4 * i + 2] = s[4 * i + 0];
            conv[4 * i + 3] = s[4 * i + 3];
        }
        px = conv; ifmt = 0x1908;
    }


    /* --- Save State --- */
    int old_tex = 0, old_mm = 0x1700, old_env = 0x2100, vp[4];
    float old_col[4] = { 1, 1, 1, 1 }, old_tc[4] = { 0, 0, 0, 1 };
    RP(glGetIntegerv, F_geti)(0x8069, &old_tex);                 /* TEXTURE_BINDING_2D */
    RP(glGetIntegerv, F_geti)(0x0BA0, &old_mm);                  /* MATRIX_MODE */
    RP(glGetIntegerv, F_geti)(0x0BA2, vp);                       /* VIEWPORT */
    RP(glGetFloatv,   F_getf)(0x0B00, old_col);                  /* GL_CURRENT_COLOR */
    RP(glGetFloatv,   F_getf)(0x0B03, old_tc);                   /* GL_CURRENT_TEXTURE_COORDS */
    RP(glGetTexEnviv, F_texgeti)(0x2300, 0x2200, &old_env);      /* TEXTURE_ENV_MODE */
    int was_tex   = RP(glIsEnabled, F_isen)(0x0DE1);
    int was_depth = RP(glIsEnabled, F_isen)(0x0B71);
    int was_light = RP(glIsEnabled, F_isen)(0x0B50);
    int was_cull  = RP(glIsEnabled, F_isen)(0x0B44);
    int was_fog   = RP(glIsEnabled, F_isen)(0x0B60);

    if (was_depth) RP(glDisable, F_ui)(0x0B71);
    if (was_light) RP(glDisable, F_ui)(0x0B50);
    if (was_cull)  RP(glDisable, F_ui)(0x0B44);
    if (was_fog)   RP(glDisable, F_ui)(0x0B60);

    /* --- Persistent POT Texture --- */
    if (!g_dp_tex) RP(glGenTextures, F_gentex)(1, &g_dp_tex);
    RP(glBindTexture, F_uiui)(0x0DE1, g_dp_tex);

    unsigned pw = dp_pot((unsigned)w), ph = dp_pot((unsigned)h);
    if (pw < g_dp_pw) pw = g_dp_pw;
    if (ph < g_dp_ph) ph = g_dp_ph;
    if (pw != g_dp_pw || ph != g_dp_ph || ifmt != g_dp_ifmt || type != g_dp_type) {
        g_dp_pw = pw; g_dp_ph = ph; g_dp_ifmt = ifmt; g_dp_type = type;
        RP(glTexParameteri, F_da)(0x0DE1, 0x2801, 0x2600);       /* MIN_FILTER NEAREST */
        RP(glTexParameteri, F_da)(0x0DE1, 0x2800, 0x2600);       /* MAG_FILTER NEAREST */
        RP(glTexParameteri, F_da)(0x0DE1, 0x2802, 0x812F);       /* WRAP_S CLAMP_TO_EDGE */
        RP(glTexParameteri, F_da)(0x0DE1, 0x2803, 0x812F);       /* WRAP_T CLAMP_TO_EDGE */
        RP(glTexImage2D, F_teximg)(0x0DE1, 0, (int)ifmt, (int)pw, (int)ph, 0, ifmt, type, NULL);
    }
    /* Unpack alignment remains as set by application (default 4) */
    RP(glTexSubImage2D, F_teximg)(0x0DE1, 0, 0, 0, w, h, ifmt, type, px);
    free(conv);

    RP(glTexEnvf, F_texenv)(0x2300, 0x2200, (float)0x1E01);      /* GL_REPLACE: ignore current color */
    RP(glEnable, F_ui)(0x0DE1);
    RP(glColor4f, F_ffff)(1.0f, 1.0f, 1.0f, 1.0f);

    /* --- Ortho projection over viewport (texture matrix = identity) --- */
    RP(glMatrixMode, F_ui)(0x1702);                              /* GL_TEXTURE */
    RP(glPushMatrix, F_v)(); RP(glLoadIdentity, F_v)();
    RP(glMatrixMode, F_ui)(0x1701);                              /* GL_PROJECTION */
    RP(glPushMatrix, F_v)(); RP(glLoadIdentity, F_v)();
    RP(glOrthof, F_6f)((float)vp[0], (float)(vp[0] + vp[2]), (float)vp[1], (float)(vp[1] + vp[3]), -1.0f, 1.0f);
    RP(glMatrixMode, F_ui)(0x1700);                              /* GL_MODELVIEW */
    RP(glPushMatrix, F_v)(); RP(glLoadIdentity, F_v)();

    /* Rows in glDrawPixels are bottom-to-top: t=0 at y (raster), increasing upwards.
       Negative PixelZoom on Y naturally flips the image (coordinates become decreasing). */
    float x0 = g_rasterx, y0 = g_rastery;
    float x1 = x0 + w * g_zoomx, y1 = y0 + h * g_zoomy;
    float s = (float)w / (float)g_dp_pw, t = (float)h / (float)g_dp_ph;
    float v[12] = { x0, y0, 0,   x1, y0, 0,   x0, y1, 0,   x1, y1, 0 };
    float c[8]  = { 0, 0,        s, 0,         0, t,        s, t };
    im_draw(8, 4, v, NULL, NULL, c);                             /* QUAD_STRIP -> TRIANGLE_STRIP */

    /* --- Restore State --- */
    RP(glPopMatrix, F_v)();                                      /* Modelview */
    RP(glMatrixMode, F_ui)(0x1701); RP(glPopMatrix, F_v)();      /* Projection */
    RP(glMatrixMode, F_ui)(0x1702); RP(glPopMatrix, F_v)();      /* Texture */
    RP(glMatrixMode, F_ui)((unsigned)old_mm);

    RP(glTexEnvf, F_texenv)(0x2300, 0x2200, (float)old_env);
    RP(glBindTexture, F_uiui)(0x0DE1, (unsigned)old_tex);
    RP(glColor4f, F_ffff)(old_col[0], old_col[1], old_col[2], old_col[3]);
    RP(glMultiTexCoord4f, F_utex)(0x84C0, old_tc[0], old_tc[1], old_tc[2], old_tc[3]);
    if (!was_tex)  RP(glDisable, F_ui)(0x0DE1);
    if (was_depth) RP(glEnable, F_ui)(0x0B71);
    if (was_light) RP(glEnable, F_ui)(0x0B50);
    if (was_cull)  RP(glEnable, F_ui)(0x0B44);
    if (was_fog)   RP(glEnable, F_ui)(0x0B60);
}

/* ---------- glBitmap (emulat): cate un quad per serie orizontala de pixeli setati ---------- */
static float bm_floor(float f) { int i = (int)f; return (float)(f < (float)i ? i - 1 : i); }

GLC void glBitmap(int w, int h, float xorig, float yorig, float xmove, float ymove, const unsigned char* bm) {
    im_init();
    gl_warn_es("glBitmap");
    if (IM.in_begin || IM.rec_id) return;              /* nu se inregistreaza in display lists */
    if (!g_rvalid) return;

    if (w > 0 && h > 0 && bm) {
        int al = 4; RP(glGetIntegerv, F_geti)(0x0CF5, &al);          /* GL_UNPACK_ALIGNMENT */
        if (al < 1) al = 4;
        int stride = ((((w + 7) / 8) + al - 1) / al) * al;
        size_t maxq = (size_t)h * (size_t)((w + 1) / 2);
        if (maxq > 16384) maxq = 16384;                               /* limita im_draw: 65536 varfuri */
        float* v = malloc(maxq * 12 * sizeof(float));
        size_t nq = 0;
        float bx = bm_floor(g_rasterx - xorig), by = bm_floor(g_rastery - yorig);

        for (int r = 0; r < h && v; r++) {
            const unsigned char* row = bm + (size_t)r * stride;       /* randurile sunt de jos in sus */
            int x = 0;
            while (x < w) {
                if (!((row[x >> 3] >> (7 - (x & 7))) & 1)) { x++; continue; }
                int x0 = x;
                while (x < w && ((row[x >> 3] >> (7 - (x & 7))) & 1)) x++;
                if (nq >= maxq) break;
                float* q = v + nq * 12;
                float xa = bx + x0, xb = bx + x, ya = by + r, yb = ya + 1.0f;
                q[0] = xa; q[1] = ya; q[2] = 0;   q[3] = xb; q[4] = ya; q[5] = 0;
                q[6] = xb; q[7] = yb; q[8] = 0;   q[9] = xa; q[10] = yb; q[11] = 0;
                nq++;
            }
        }

        if (v && nq) {
            int old_mm = 0x1700, vp[4];
            RP(glGetIntegerv, F_geti)(0x0BA0, &old_mm);               /* MATRIX_MODE */
            RP(glGetIntegerv, F_geti)(0x0BA2, vp);                    /* VIEWPORT */
            int was_tex   = RP(glIsEnabled, F_isen)(0x0DE1);
            int was_depth = RP(glIsEnabled, F_isen)(0x0B71);
            int was_light = RP(glIsEnabled, F_isen)(0x0B50);
            int was_cull  = RP(glIsEnabled, F_isen)(0x0B44);
            int was_fog   = RP(glIsEnabled, F_isen)(0x0B60);
            if (was_tex)   RP(glDisable, F_ui)(0x0DE1);
            if (was_depth) RP(glDisable, F_ui)(0x0B71);
            if (was_light) RP(glDisable, F_ui)(0x0B50);
            if (was_cull)  RP(glDisable, F_ui)(0x0B44);
            if (was_fog)   RP(glDisable, F_ui)(0x0B60);

            RP(glMatrixMode, F_ui)(0x1701);                           /* PROJECTION */
            RP(glPushMatrix, F_v)(); RP(glLoadIdentity, F_v)();
            RP(glOrthof, F_6f)((float)vp[0], (float)(vp[0] + vp[2]), (float)vp[1], (float)(vp[1] + vp[3]), -1.0f, 1.0f);
            RP(glMatrixMode, F_ui)(0x1700);                           /* MODELVIEW */
            RP(glPushMatrix, F_v)(); RP(glLoadIdentity, F_v)();

            im_draw(7, (int)(nq * 4), v, NULL, NULL, NULL);           /* GL_QUADS -> triunghiuri */

            RP(glPopMatrix, F_v)();
            RP(glMatrixMode, F_ui)(0x1701); RP(glPopMatrix, F_v)();
            RP(glMatrixMode, F_ui)((unsigned)old_mm);
            if (was_tex)   RP(glEnable, F_ui)(0x0DE1);
            if (was_depth) RP(glEnable, F_ui)(0x0B71);
            if (was_light) RP(glEnable, F_ui)(0x0B50);
            if (was_cull)  RP(glEnable, F_ui)(0x0B44);
            if (was_fog)   RP(glEnable, F_ui)(0x0B60);
        }
        free(v);
    }
    g_rasterx += xmove; g_rastery += ymove;                           /* avans raster, ca in GL */
}
/* ---------- Display Lists Execution ---------- */
static void list_exec(unsigned id, int depth) {
    if (id == 0 || id >= gl_lists_cap || depth > 32) return;
    gl_list* L = &gl_lists[id];
    for (int i = 0; i < L->n; i++) {
        gl_cmd* c = &L->c[i];
        switch (c->op) {
        case OP_DRAW: { gl_block* b = c->p; im_draw(b->mode, b->n, b->pos, b->nrm, b->col, b->tex); break; }
        case OP_NORMAL:    RP(glNormal3f, F_fff)(c->f[0], c->f[1], c->f[2]); break;
        case OP_COLOR:     RP(glColor4f, F_ffff)(c->f[0], c->f[1], c->f[2], c->f[3]); break;
        case OP_TEXCOORD:  RP(glMultiTexCoord4f, F_utex)(0x84C0, c->f[0], c->f[1], 0.0f, 1.0f); break;
        case OP_MATERIAL:  RP(glMaterialfv, F_mat)(c->u[0], c->u[1], c->f); break;
        case OP_LIGHT:     RP(glLightfv, F_mat)(c->u[0], c->u[1], c->f); break;
        case OP_SHADE:     RP(glShadeModel, F_ui)(c->u[0]); break;
        case OP_FRONTFACE: cc_note_front(c->u[0]); cc_setfront(); break;
        case OP_ENABLE:    RP(glEnable, F_ui)(c->u[0]);  dsb_on_enable(c->u[0], 1); break;
        case OP_DISABLE:   RP(glDisable, F_ui)(c->u[0]); dsb_on_enable(c->u[0], 0); break;
        case OP_BINDTEX:   RP(glBindTexture, F_uiui)(c->u[0], c->u[1]); break;
        case OP_PUSH:      RP(glPushMatrix, F_v)(); break;
        case OP_POP:       RP(glPopMatrix, F_v)(); break;
        case OP_IDENT:     RP(glLoadIdentity, F_v)(); break;
        case OP_TRANSLATE: RP(glTranslatef, F_fff)(c->f[0], c->f[1], c->f[2]); break;
        case OP_SCALE:     RP(glScalef, F_fff)(c->f[0], c->f[1], c->f[2]); break;
        case OP_ROTATE:    RP(glRotatef, F_ffff)(c->f[0], c->f[1], c->f[2], c->f[3]); break;
        case OP_MULT:      RP(glMultMatrixf, F_pf)(c->f); break;
        case OP_WINDOWPOS: glWindowPos3f(c->f[0], c->f[1], c->f[2]); break;
        case OP_RASTERPOS: glRasterPos4f(c->f[0], c->f[1], c->f[2], c->f[3]); break;
        case OP_CALL:      list_exec(c->u[0], depth + 1); break;
        default: break;
        }
    }
}

GLC unsigned glGenLists(int range) {
    if (range <= 0) return 0;
    pthread_mutex_lock(&gl_lmu);
    unsigned first = gl_next_list;
    gl_next_list += (unsigned)range;
    lists_ensure(gl_next_list);
    pthread_mutex_unlock(&gl_lmu);
    return first;
}

GLC void glDeleteLists(unsigned first, int range) {
    pthread_mutex_lock(&gl_lmu);
    for (unsigned i = first; range > 0 && i < first + (unsigned)range && i < gl_lists_cap; i++) list_clear(&gl_lists[i]);
    pthread_mutex_unlock(&gl_lmu);
}

GLC unsigned char glIsList(unsigned id) { return id && id < gl_lists_cap && gl_lists[id].used; }

GLC void glNewList(unsigned id, unsigned mode) {
    im_init();
    gl_warn_es("glNewList");
    if (!id) return;
    pthread_mutex_lock(&gl_lmu);
    if (id >= gl_next_list) gl_next_list = id + 1;
    lists_ensure(gl_next_list);
    list_clear(&gl_lists[id]);
    gl_lists[id].used = 1;
    pthread_mutex_unlock(&gl_lmu);
    IM.rec_id = id; IM.rec_mode = (int)mode;
}

GLC void glEndList(void) { IM.rec_id = 0; IM.rec_mode = 0; }

GLC void glCallList(unsigned id) {
    if (IM.rec_id) { rec_cmd(OP_CALL)->u[0] = id; if (REC_SKIP()) return; }
    list_exec(id, 0);
}

GLC void glCallLists(int n, unsigned type, const void* lists) {
    for (int i = 0; i < n; i++) {
        unsigned id = 0;
        switch (type) {
        case 0x1401: id = ((const unsigned char*)lists)[i]; break;     /* UNSIGNED_BYTE */
        case 0x1400: id = ((const signed char*)lists)[i]; break;       /* BYTE */
        case 0x1403: id = ((const unsigned short*)lists)[i]; break;    /* UNSIGNED_SHORT */
        case 0x1402: id = ((const short*)lists)[i]; break;             /* SHORT */
        case 0x1404: case 0x1405: id = ((const unsigned*)lists)[i]; break;   /* INT / UNSIGNED_INT */
        case 0x1406: id = (unsigned)((const float*)lists)[i]; break;   /* FLOAT */
        default: return;
        }
        glCallList(id);
    }
}

/* ---------- Double Variants / Desktop Enums ---------- */
GLC void glFrustum(double l, double r, double b, double t, double n, double f) {
    RP(glFrustumf, F_6f)((float)l, (float)r, (float)b, (float)t, (float)n, (float)f);
}
GLC void glOrtho(double l, double r, double b, double t, double n, double f) {
    RP(glOrthof, F_6f)((float)l, (float)r, (float)b, (float)t, (float)n, (float)f);
}
GLC void glTranslated(double x, double y, double z) { glTranslatef((float)x, (float)y, (float)z); }
GLC void glScaled(double x, double y, double z)     { glScalef((float)x, (float)y, (float)z); }
GLC void glRotated(double a, double x, double y, double z) { glRotatef((float)a, (float)x, (float)y, (float)z); }
GLC void glLoadMatrixd(const double* m) {
    float f[16];
    for (int i = 0; i < 16; i++) f[i] = (float)m[i];
    RP(glLoadMatrixf, F_pf)(f);
}
GLC void glMultMatrixd(const double* m) {
    float f[16];
    for (int i = 0; i < 16; i++) f[i] = (float)m[i];
    glMultMatrixf(f);
}
GLC void glClearDepth(double d)               { RP(glClearDepthf, F_f)((float)d); }
GLC void glDepthRange(double n, double f)     { RP(glDepthRangef, F_ff)((float)n, (float)f); }
GLC void glClipPlane(unsigned plane, const double* e) {
    float f[4] = { (float)e[0], (float)e[1], (float)e[2], (float)e[3] };
    RP(glClipPlanef, F_clip)(plane, f);
}
GLC void glLightModeli(unsigned pname, int v) { RP(glLightModelf, F_uif)(pname, (float)v); }
GLC void glGetDoublev(unsigned pname, double* out) {
    if (pname == 0x8068) pname = 0x8069;                           /* GL_TEXTURE_BINDING_1D -> GL_TEXTURE_BINDING_2D */
    float f[16] = { 0 };
    int n = 1;
    if (pname == 0x0BA6 || pname == 0x0BA7 || pname == 0x0BA8) n = 16;     /* MODELVIEW/PROJECTION/TEXTURE matrix */
    else if (pname == 0x0BA2 || pname == 0x0C22) n = 4;                    /* VIEWPORT / CURRENT_COLOR-like */
    RP(glGetFloatv, F_getf)(pname, f);
    for (int i = 0; i < n; i++) out[i] = f[i];
}

/* ===== GL 1.3 / 1.4 fixed-function gaps ===== */
/* MultiTexCoord: doar unitatea 0 se înregistrează în glBegin/End; restul merg direct la ES1 */
GLC void glMultiTexCoord2f(unsigned t, float s, float v) {
    if (t == 0x84C0) { glTexCoord2f(s, v); return; }
    RP(glMultiTexCoord4f, F_utex)(t, s, v, 0.0f, 1.0f);
}
GLC void glMultiTexCoord1f(unsigned t, float s)                      { glMultiTexCoord2f(t, s, 0.0f); }
GLC void glMultiTexCoord3f(unsigned t, float s, float v, float r)    { (void)r; glMultiTexCoord2f(t, s, v); }
GLC void glMultiTexCoord4fv(unsigned t, const float* v) {
    if (t == 0x84C0) { glTexCoord2f(v[0], v[1]); return; }
    RP(glMultiTexCoord4f, F_utex)(t, v[0], v[1], v[2], v[3]);
}
GLC void glMultiTexCoord1fv(unsigned t, const float* v) { glMultiTexCoord2f(t, v[0], 0.0f); }
GLC void glMultiTexCoord2fv(unsigned t, const float* v) { glMultiTexCoord2f(t, v[0], v[1]); }
GLC void glMultiTexCoord3fv(unsigned t, const float* v) { glMultiTexCoord2f(t, v[0], v[1]); }
GLC void glMultiTexCoord1d(unsigned t, double s)                    { glMultiTexCoord2f(t, (float)s, 0.0f); }
GLC void glMultiTexCoord2d(unsigned t, double s, double v)          { glMultiTexCoord2f(t, (float)s, (float)v); }
GLC void glMultiTexCoord3d(unsigned t, double s, double v, double r){ (void)r; glMultiTexCoord2f(t, (float)s, (float)v); }
GLC void glMultiTexCoord4d(unsigned t, double s, double v, double r, double q){ (void)r;(void)q; glMultiTexCoord2f(t, (float)s, (float)v); }
GLC void glMultiTexCoord1dv(unsigned t, const double* v) { glMultiTexCoord2f(t, (float)v[0], 0.0f); }
GLC void glMultiTexCoord2dv(unsigned t, const double* v) { glMultiTexCoord2f(t, (float)v[0], (float)v[1]); }
GLC void glMultiTexCoord3dv(unsigned t, const double* v) { glMultiTexCoord2f(t, (float)v[0], (float)v[1]); }
GLC void glMultiTexCoord4dv(unsigned t, const double* v) { glMultiTexCoord2f(t, (float)v[0], (float)v[1]); }
GLC void glMultiTexCoord1i(unsigned t, int s)                 { glMultiTexCoord2f(t, (float)s, 0.0f); }
GLC void glMultiTexCoord2i(unsigned t, int s, int v)          { glMultiTexCoord2f(t, (float)s, (float)v); }
GLC void glMultiTexCoord3i(unsigned t, int s, int v, int r)   { (void)r; glMultiTexCoord2f(t, (float)s, (float)v); }
GLC void glMultiTexCoord4i(unsigned t, int s, int v, int r, int q){ (void)r;(void)q; glMultiTexCoord2f(t, (float)s, (float)v); }
GLC void glMultiTexCoord1iv(unsigned t, const int* v) { glMultiTexCoord2f(t, (float)v[0], 0.0f); }
GLC void glMultiTexCoord2iv(unsigned t, const int* v) { glMultiTexCoord2f(t, (float)v[0], (float)v[1]); }
GLC void glMultiTexCoord3iv(unsigned t, const int* v) { glMultiTexCoord2f(t, (float)v[0], (float)v[1]); }
GLC void glMultiTexCoord4iv(unsigned t, const int* v) { glMultiTexCoord2f(t, (float)v[0], (float)v[1]); }
GLC void glMultiTexCoord1s(unsigned t, short s)                 { glMultiTexCoord2f(t, s, 0.0f); }
GLC void glMultiTexCoord2s(unsigned t, short s, short v)        { glMultiTexCoord2f(t, s, v); }
GLC void glMultiTexCoord3s(unsigned t, short s, short v, short r){ (void)r; glMultiTexCoord2f(t, s, v); }
GLC void glMultiTexCoord4s(unsigned t, short s, short v, short r, short q){ (void)r;(void)q; glMultiTexCoord2f(t, s, v); }
GLC void glMultiTexCoord1sv(unsigned t, const short* v) { glMultiTexCoord2f(t, v[0], 0.0f); }
GLC void glMultiTexCoord2sv(unsigned t, const short* v) { glMultiTexCoord2f(t, v[0], v[1]); }
GLC void glMultiTexCoord3sv(unsigned t, const short* v) { glMultiTexCoord2f(t, v[0], v[1]); }
GLC void glMultiTexCoord4sv(unsigned t, const short* v) { glMultiTexCoord2f(t, v[0], v[1]); }

/* FogCoord / SecondaryColor: ES1 nu le are -> ignorate (rar folosite) */
GLC void glFogCoordf(float c)                  { (void)c; }
GLC void glFogCoordd(double c)                 { (void)c; }
GLC void glFogCoordfv(const float* c)          { (void)c; }
GLC void glFogCoorddv(const double* c)         { (void)c; }
GLC void glFogCoordPointer(unsigned t, int s, const void* p) { (void)t; (void)s; (void)p; }
GLC void glSecondaryColor3b(signed char r, signed char g, signed char b)      { (void)r;(void)g;(void)b; }
GLC void glSecondaryColor3d(double r, double g, double b)                     { (void)r;(void)g;(void)b; }
GLC void glSecondaryColor3f(float r, float g, float b)                        { (void)r;(void)g;(void)b; }
GLC void glSecondaryColor3i(int r, int g, int b)                              { (void)r;(void)g;(void)b; }
GLC void glSecondaryColor3s(short r, short g, short b)                        { (void)r;(void)g;(void)b; }
GLC void glSecondaryColor3ub(unsigned char r, unsigned char g, unsigned char b){ (void)r;(void)g;(void)b; }
GLC void glSecondaryColor3ui(unsigned r, unsigned g, unsigned b)              { (void)r;(void)g;(void)b; }
GLC void glSecondaryColor3us(unsigned short r, unsigned short g, unsigned short b){ (void)r;(void)g;(void)b; }
GLC void glSecondaryColor3bv(const signed char* v)   { (void)v; }
GLC void glSecondaryColor3dv(const double* v)        { (void)v; }
GLC void glSecondaryColor3fv(const float* v)         { (void)v; }
GLC void glSecondaryColor3iv(const int* v)           { (void)v; }
GLC void glSecondaryColor3sv(const short* v)         { (void)v; }
GLC void glSecondaryColor3ubv(const unsigned char* v){ (void)v; }
GLC void glSecondaryColor3uiv(const unsigned* v)     { (void)v; }
GLC void glSecondaryColor3usv(const unsigned short* v){ (void)v; }
GLC void glSecondaryColorPointer(int s, unsigned t, int st, const void* p) { (void)s;(void)t;(void)st;(void)p; }

/* PointParameter int: ES1 are doar float */
GLC void glPointParameteri(unsigned p, int v) {
    RP(glPointParameterf, F_uif)(p, (float)v);
}
GLC void glPointParameteriv(unsigned p, const int* v) {
    float f = (float)v[0];
RP(glPointParameterfv, F_clip)(p, &f);
}

/* Transpose matrices: transpune pe CPU, apoi Load/Mult */
static void tr16(const double* s, float* d) { for (int i=0;i<4;i++) for (int j=0;j<4;j++) d[i*4+j]=(float)s[j*4+i]; }
static void tr16f(const float* s, float* d) { for (int i=0;i<4;i++) for (int j=0;j<4;j++) d[i*4+j]=s[j*4+i]; }
GLC void glLoadTransposeMatrixf(const float* m)  { float t[16]; tr16f(m, t); RP(glLoadMatrixf, F_pf)(t); }
GLC void glMultTransposeMatrixf(const float* m)  { float t[16]; tr16f(m, t); glMultMatrixf(t); }
GLC void glLoadTransposeMatrixd(const double* m) { float t[16]; tr16(m, t);  RP(glLoadMatrixf, F_pf)(t); }
GLC void glMultTransposeMatrixd(const double* m) { float t[16]; tr16(m, t);  glMultMatrixf(t); }

GLC void glCompressedTexImage2D(unsigned, int, unsigned, int, int, int, int, const void*);
GLC void glCompressedTexSubImage2D(unsigned, int, int, int, int, int, unsigned, int, const void*);