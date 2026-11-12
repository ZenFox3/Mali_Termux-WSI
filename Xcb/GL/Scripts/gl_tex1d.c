/* gl_tex1d.c - TEXTURE_1D emulated via TEXTURE_2D (height 1).
   Include in gl_es_emul.c after gl_litfix.c:  #include "gl_tex1d.c" */

#define TEX_1D 0x0DE0
#define TEX_2D 0x0DE1
static inline unsigned t1d(unsigned t) { return t == TEX_1D ? TEX_2D : t; }

typedef void (*T1_bind)(unsigned, unsigned);
typedef void (*T1_pi)(unsigned, unsigned, int);
typedef void (*T1_pf)(unsigned, unsigned, float);
typedef void (*T1_piv)(unsigned, unsigned, const int*);
typedef void (*T1_pfv)(unsigned, unsigned, const float*);
typedef void (*T1_giv)(unsigned, unsigned, int*);
typedef void (*T1_gfv)(unsigned, unsigned, float*);
typedef void (*T1_gm)(unsigned);
typedef void (*T1_lvi)(unsigned, int, unsigned, int*);
typedef void (*T1_lvf)(unsigned, int, unsigned, float*);
typedef void (*T1_ti2)(unsigned, int, int, int, int, int, unsigned, unsigned, const void*);
typedef void (*T1_tsi2)(unsigned, int, int, int, int, int, unsigned, unsigned, const void*);
typedef void (*T1_cti2)(unsigned, int, unsigned, int, int, int, int, int);
typedef void (*T1_ctsi2)(unsigned, int, int, int, int, int, int, int);
typedef void (*T1_ts2)(unsigned, int, unsigned, int, int);

GLC void glBindTexture(unsigned t, unsigned id) { RP(glBindTexture, T1_bind)(t1d(t), id); }

GLC void glTexParameteri(unsigned t, unsigned p, int v)   { RP(glTexParameteri, T1_pi)(t1d(t), p, v); }
GLC void glTexParameterf(unsigned t, unsigned p, float v) { RP(glTexParameterf, T1_pf)(t1d(t), p, v); }
GLC void glTexParameteriv(unsigned t, unsigned p, const int* v)   { RP(glTexParameteriv, T1_piv)(t1d(t), p, v); }
GLC void glTexParameterfv(unsigned t, unsigned p, const float* v) { RP(glTexParameterfv, T1_pfv)(t1d(t), p, v); }
GLC void glGetTexParameteriv(unsigned t, unsigned p, int* v)   { RP(glGetTexParameteriv, T1_giv)(t1d(t), p, v); }
GLC void glGetTexParameterfv(unsigned t, unsigned p, float* v) { RP(glGetTexParameterfv, T1_gfv)(t1d(t), p, v); }
GLC void glGenerateMipmap(unsigned t) { RP(glGenerateMipmap, T1_gm)(t1d(t)); }

GLC void glGetTexLevelParameteriv(unsigned t, int l, unsigned p, int* v) {
    unsigned tt = t1d(t);
    if (t == TEX_1D && p == 0x1001) { *v = 1; return; }      /* HEIGHT of 1D = 1 */
    RP(glGetTexLevelParameteriv, T1_lvi)(tt, l, p, v);
}
GLC void glGetTexLevelParameterfv(unsigned t, int l, unsigned p, float* v) {
    if (t == TEX_1D && p == 0x1001) { *v = 1.0f; return; }
    RP(glGetTexLevelParameterfv, T1_lvf)(t1d(t), l, p, v);
}

GLC void glTexImage1D(unsigned t, int level, int ifmt, int w, int border,
                      unsigned fmt, unsigned type, const void* px) {
    RP(glTexImage2D, T1_ti2)(t1d(t), level, ifmt, w, 1, border, fmt, type, px);
}
GLC void glTexSubImage1D(unsigned t, int level, int xo, int w,
                         unsigned fmt, unsigned type, const void* px) {
    RP(glTexSubImage2D, T1_tsi2)(t1d(t), level, xo, 0, w, 1, fmt, type, px);
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
