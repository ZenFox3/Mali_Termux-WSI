/*
 * gl_es_alias.c - paste at the END of gl_es_emul.c (or #include it right after).
 * Fills the functions that glewinfo reports as MISSING under extensions the driver advertises:
 *   A) ARB aliases of core functions (buffers, compressed tex, instancing)
 *   B) EXT aliases / small emulations (blend equation, FBO, tex storage, multi draw)
 *   C) Direct-state-access (EXT_direct_state_access style) wrappers: bind -> call -> restore
 *   D) Misc: atomic counter query, dual-source index queries, fetch barrier, double attribs
 *
 * Everything is resolved lazily through glXGetProcAddressARB, so it does not depend on the p_* table
 * and it automatically uses the shim's own wrappers when they exist (e.g. glCompressedTexImage2D).
 */
#include <dlfcn.h>
#ifndef RTLD_DEFAULT
#define RTLD_DEFAULT ((void*)0)
#endif

static void* al_gp(const char* n) {
    static void* (*f)(const unsigned char*);
    if (!f) f = (void* (*)(const unsigned char*))dlsym(RTLD_DEFAULT, "glXGetProcAddressARB");
    return f ? f((const unsigned char*)n) : NULL;
}
#define GP(n)     ({ static void* c_; if (!c_) c_ = al_gp(n); c_; })
#define AL(T, n)  ((T)GP(n))

#define FWDV(name, target, params, args) \
    GLC void name params { void* f_ = GP(target); if (f_) ((void (*) params)f_) args; }
#define FWDR(ret, name, target, params, args, dflt) \
    GLC ret name params { void* f_ = GP(target); return f_ ? ((ret (*) params)f_) args : (dflt); }

/* ===== A) ARB aliases of core functions ===== */
FWDV(glBindBufferARB,          "glBindBuffer",          (unsigned t, unsigned b), (t, b))
FWDV(glBufferDataARB,          "glBufferData",          (unsigned t, long sz, const void* d, unsigned u), (t, sz, d, u))
FWDV(glBufferSubDataARB,       "glBufferSubData",       (unsigned t, long o, long sz, const void* d), (t, o, sz, d))
FWDV(glDeleteBuffersARB,       "glDeleteBuffers",       (int n, const unsigned* b), (n, b))
FWDV(glGenBuffersARB,          "glGenBuffers",          (int n, unsigned* b), (n, b))
FWDV(glGetBufferParameterivARB,"glGetBufferParameteriv",(unsigned t, unsigned p, int* v), (t, p, v))
FWDV(glGetBufferPointervARB,   "glGetBufferPointerv",   (unsigned t, unsigned p, void** v), (t, p, v))
FWDV(glGetBufferSubDataARB,    "glGetBufferSubData",    (unsigned t, long o, long sz, void* d), (t, o, sz, d))
FWDR(unsigned char, glIsBufferARB,  "glIsBuffer",  (unsigned b), (b), 0)
FWDR(void*,         glMapBufferARB, "glMapBuffer", (unsigned t, unsigned a), (t, a), 0)
FWDR(unsigned char, glUnmapBufferARB, "glUnmapBuffer", (unsigned t), (t), 0)

FWDV(glCompressedTexImage1DARB,    "glCompressedTexImage1D",    (unsigned t, int l, unsigned f, int w, int b, int sz, const void* d), (t, l, f, w, b, sz, d))
FWDV(glCompressedTexImage2DARB,    "glCompressedTexImage2D",    (unsigned t, int l, unsigned f, int w, int h, int b, int sz, const void* d), (t, l, f, w, h, b, sz, d))
FWDV(glCompressedTexImage3DARB,    "glCompressedTexImage3D",    (unsigned t, int l, unsigned f, int w, int h, int dp, int b, int sz, const void* d), (t, l, f, w, h, dp, b, sz, d))
FWDV(glCompressedTexSubImage1DARB, "glCompressedTexSubImage1D", (unsigned t, int l, int xo, int w, unsigned f, int sz, const void* d), (t, l, xo, w, f, sz, d))
FWDV(glCompressedTexSubImage2DARB, "glCompressedTexSubImage2D", (unsigned t, int l, int xo, int yo, int w, int h, unsigned f, int sz, const void* d), (t, l, xo, yo, w, h, f, sz, d))
FWDV(glCompressedTexSubImage3DARB, "glCompressedTexSubImage3D", (unsigned t, int l, int xo, int yo, int zo, int w, int h, int dp, unsigned f, int sz, const void* d), (t, l, xo, yo, zo, w, h, dp, f, sz, d))
FWDV(glGetCompressedTexImageARB,   "glGetCompressedTexImage",   (unsigned t, int l, void* img), (t, l, img))

FWDV(glDrawArraysInstancedARB,   "glDrawArraysInstanced",   (unsigned m, int f, int c, int n), (m, f, c, n))
FWDV(glDrawElementsInstancedARB, "glDrawElementsInstanced", (unsigned m, int c, unsigned t, const void* i, int n), (m, c, t, i, n))
FWDV(glVertexAttribDivisorARB,   "glVertexAttribDivisor",   (unsigned i, unsigned d), (i, d))

/* ===== B) EXT aliases / small emulations ===== */
FWDV(glBlendEquationEXT,        "glBlendEquation",       (unsigned m), (m))
FWDV(glGetInteger64vEXT,        "glGetInteger64v",       (unsigned p, long long* v), (p, v))
FWDV(glFramebufferTexture1DEXT, "glFramebufferTexture1D",(unsigned t, unsigned a, unsigned tt, unsigned tex, int l), (t, a, tt, tex, l))
FWDV(glFramebufferTexture3DEXT, "glFramebufferTexture3D",(unsigned t, unsigned a, unsigned tt, unsigned tex, int l, int z), (t, a, tt, tex, l, z))
FWDV(glGetFramebufferAttachmentParameterivEXT, "glGetFramebufferAttachmentParameteriv",
     (unsigned t, unsigned a, unsigned p, int* v), (t, a, p, v))
FWDV(glGetRenderbufferParameterivEXT, "glGetRenderbufferParameteriv", (unsigned t, unsigned p, int* v), (t, p, v))
FWDV(glTexStorage1DEXT,         "glTexStorage1D",        (unsigned t, int lv, unsigned f, int w), (t, lv, f, w))
FWDV(glBufferStorage,           "glBufferStorageEXT",    (unsigned t, long sz, const void* d, unsigned fl), (t, sz, d, fl))

GLC void glMultiDrawElementsBaseVertexEXT(unsigned mode, const int* count, unsigned type,
                                          const void* const* indices, int n, const int* bv) {
    void (*f)(unsigned, int, unsigned, const void*, int) = AL(void (*)(unsigned, int, unsigned, const void*, int),
                                                              "glDrawElementsBaseVertex");
    if (!f) return;
    for (int i = 0; i < n; i++) if (count[i] > 0) f(mode, count[i], type, indices[i], bv[i]);
}

/* ===== C) Direct state access wrappers ===== */
static unsigned al_texq(unsigned t) {
    switch (t) {
    case 0x806F: return 0x806A;   /* 3D */
    case 0x8513: return 0x8514;   /* CUBE_MAP */
    case 0x8C1A: return 0x8C1D;   /* 2D_ARRAY */
    case 0x9100: return 0x9104;   /* 2D_MULTISAMPLE */
    case 0x9102: return 0x9105;   /* 2D_MULTISAMPLE_ARRAY */
    case 0x9009: return 0x900A;   /* CUBE_MAP_ARRAY */
    default:     return 0x8069;   /* 2D */
    }
}
static void al_bbuf(unsigned t, unsigned b) { AL(void (*)(unsigned, unsigned), "glBindBuffer")(t, b); }
static void al_bfb(unsigned t, unsigned f)  { AL(void (*)(unsigned, unsigned), "glBindFramebuffer")(t, f); }
static void al_bvao(unsigned v)             { AL(void (*)(unsigned), "glBindVertexArray")(v); }

/* Textures */
GLC void glTextureStorage1DEXT(unsigned tex, unsigned target, int lv, unsigned f, int w) {
    target = t1d(target);
    int sv = 0; glGetIntegerv(al_texq(target), &sv);
    glBindTexture(target, tex);
    AL(void (*)(unsigned, int, unsigned, int), "glTexStorage1D")(target, lv, f, w);
    glBindTexture(target, (unsigned)sv);
}
GLC void glTextureStorage2DEXT(unsigned tex, unsigned target, int lv, unsigned f, int w, int h) {
    target = t1d(target);
    int sv = 0; glGetIntegerv(al_texq(target), &sv);
    glBindTexture(target, tex);
    AL(void (*)(unsigned, int, unsigned, int, int), "glTexStorage2D")(target, lv, f, w, h);
    glBindTexture(target, (unsigned)sv);
}
GLC void glTextureStorage3DEXT(unsigned tex, unsigned target, int lv, unsigned f, int w, int h, int d) {
    int sv = 0; glGetIntegerv(al_texq(target), &sv);
    glBindTexture(target, tex);
    AL(void (*)(unsigned, int, unsigned, int, int, int), "glTexStorage3D")(target, lv, f, w, h, d);
    glBindTexture(target, (unsigned)sv);
}
GLC void glTextureStorage2DMultisampleEXT(unsigned tex, unsigned target, int s, unsigned f, int w, int h, unsigned char fx) {
    int sv = 0; glGetIntegerv(al_texq(target), &sv);
    glBindTexture(target, tex);
    AL(void (*)(unsigned, int, unsigned, int, int, unsigned char), "glTexStorage2DMultisample")(target, s, f, w, h, fx);
    glBindTexture(target, (unsigned)sv);
}
GLC void glTextureStorage3DMultisampleEXT(unsigned tex, unsigned target, int s, unsigned f, int w, int h, int d, unsigned char fx) {
    int sv = 0; glGetIntegerv(al_texq(target), &sv);
    glBindTexture(target, tex);
    AL(void (*)(unsigned, int, unsigned, int, int, int, unsigned char), "glTexStorage3DMultisampleOES")(target, s, f, w, h, d, fx);
    glBindTexture(target, (unsigned)sv);
}

/* Buffers (use COPY_WRITE_BUFFER 0x8F37; its binding query enum has the same value) */
GLC void glNamedBufferStorageEXT(unsigned buf, long sz, const void* d, unsigned fl) {
    int sv = 0; glGetIntegerv(0x8F37, &sv);
    al_bbuf(0x8F37, buf);
    AL(void (*)(unsigned, long, const void*, unsigned), "glBufferStorageEXT")(0x8F37, sz, d, fl);
    al_bbuf(0x8F37, (unsigned)sv);
}
GLC void glNamedBufferStorageExternalEXT(unsigned buf, long off, long sz, void* client, unsigned fl) {
    int sv = 0; glGetIntegerv(0x8F37, &sv);
    al_bbuf(0x8F37, buf);
    AL(void (*)(unsigned, long, long, void*, unsigned), "glBufferStorageExternalEXT")(0x8F37, off, sz, client, fl);
    al_bbuf(0x8F37, (unsigned)sv);
}
GLC void glClearNamedBufferDataEXT(unsigned buf, unsigned ifmt, unsigned fmt, unsigned type, const void* d) {
    int sv = 0; glGetIntegerv(0x8F37, &sv);
    al_bbuf(0x8F37, buf);
    glClearBufferData(0x8F37, ifmt, fmt, type, d);
    al_bbuf(0x8F37, (unsigned)sv);
}
GLC void glClearNamedBufferSubDataEXT(unsigned buf, unsigned ifmt, long off, long sz, unsigned fmt, unsigned type, const void* d) {
    int sv = 0; glGetIntegerv(0x8F37, &sv);
    al_bbuf(0x8F37, buf);
    glClearBufferSubData(0x8F37, ifmt, off, sz, fmt, type, d);
    al_bbuf(0x8F37, (unsigned)sv);
}

/* Framebuffers: DRAW 0x8CA9 (binding 0x8CA6), READ 0x8CA8 (binding 0x8CAA) */
GLC void glNamedFramebufferParameteriEXT(unsigned fb, unsigned p, int v) {
    int sv = 0; glGetIntegerv(0x8CA6, &sv);
    al_bfb(0x8CA9, fb);
    AL(void (*)(unsigned, unsigned, int), "glFramebufferParameteri")(0x8CA9, p, v);
    al_bfb(0x8CA9, (unsigned)sv);
}
GLC void glGetNamedFramebufferParameterivEXT(unsigned fb, unsigned p, int* v) {
    int sv = 0; glGetIntegerv(0x8CAA, &sv);
    al_bfb(0x8CA8, fb);
    AL(void (*)(unsigned, unsigned, int*), "glGetFramebufferParameteriv")(0x8CA8, p, v);
    al_bfb(0x8CA8, (unsigned)sv);
}
GLC void glNamedFramebufferTextureMultiviewOVR(unsigned fb, unsigned att, unsigned tex, int lv, int base, int num) {
    int sv = 0; glGetIntegerv(0x8CA6, &sv);
    al_bfb(0x8CA9, fb);
    AL(void (*)(unsigned, unsigned, unsigned, int, int, int), "glFramebufferTextureMultiviewOVR")(0x8CA9, att, tex, lv, base, num);
    al_bfb(0x8CA9, (unsigned)sv);
}

/* Vertex arrays (VERTEX_ARRAY_BINDING = 0x85B5) */
#define VAO_SAVE() int sv_ = 0; glGetIntegerv(0x85B5, &sv_)
GLC void glVertexArrayBindVertexBufferEXT(unsigned vao, unsigned bi, unsigned buf, long off, int stride) {
    VAO_SAVE(); al_bvao(vao);
    AL(void (*)(unsigned, unsigned, long, int), "glBindVertexBuffer")(bi, buf, off, stride);
    al_bvao((unsigned)sv_);
}
GLC void glVertexArrayVertexAttribBindingEXT(unsigned vao, unsigned ai, unsigned bi) {
    VAO_SAVE(); al_bvao(vao);
    AL(void (*)(unsigned, unsigned), "glVertexAttribBinding")(ai, bi);
    al_bvao((unsigned)sv_);
}
GLC void glVertexArrayVertexAttribFormatEXT(unsigned vao, unsigned ai, int size, unsigned type, unsigned char norm, unsigned rel) {
    VAO_SAVE(); al_bvao(vao);
    AL(void (*)(unsigned, int, unsigned, unsigned char, unsigned), "glVertexAttribFormat")(ai, size, type, norm, rel);
    al_bvao((unsigned)sv_);
}
GLC void glVertexArrayVertexAttribIFormatEXT(unsigned vao, unsigned ai, int size, unsigned type, unsigned rel) {
    VAO_SAVE(); al_bvao(vao);
    AL(void (*)(unsigned, int, unsigned, unsigned), "glVertexAttribIFormat")(ai, size, type, rel);
    al_bvao((unsigned)sv_);
}
GLC void glVertexArrayVertexBindingDivisorEXT(unsigned vao, unsigned bi, unsigned div) {
    VAO_SAVE(); al_bvao(vao);
    AL(void (*)(unsigned, unsigned), "glVertexBindingDivisor")(bi, div);
    al_bvao((unsigned)sv_);
}
/* 64-bit (double) attributes do not exist in ES: accepted and ignored */
GLC void glVertexAttribLFormat(unsigned ai, int size, unsigned type, unsigned rel) {
    (void)ai; (void)size; (void)type; (void)rel;
    if (gl_emul_debug_enabled()) fprintf(stderr, "[gl_emul] glVertexAttribLFormat ignored (no double attribs in ES)\n");
}
GLC void glVertexArrayVertexAttribLFormatEXT(unsigned vao, unsigned ai, int size, unsigned type, unsigned rel) {
    (void)vao; glVertexAttribLFormat(ai, size, type, rel);
}

/* ===== D) Misc ===== */
/* EXT_shader_framebuffer_fetch (coherent): no barrier needed */
GLC void glFramebufferFetchBarrierEXT(void) { }

/* Dual-source indexed outputs: handled by the framebuffer-fetch emulation (gl_dsb.c) or ignored */
GLC void glBindFragDataLocationIndexedEXT(unsigned prog, unsigned color, unsigned index, const char* name) {
    (void)prog; (void)color; (void)index;
    if (gl_emul_debug_enabled())
        fprintf(stderr, "[gl_emul] glBindFragDataLocationIndexed('%s', idx %u) ignored\n", name ? name : "", index);
}
GLC void glBindFragDataLocationIndexed(unsigned prog, unsigned color, unsigned index, const char* name) {
    glBindFragDataLocationIndexedEXT(prog, color, index, name);
}
GLC int glGetFragDataIndexEXT(unsigned prog, const char* name) {
    int loc = AL(int (*)(unsigned, const char*), "glGetFragDataLocation")(prog, name);
    return loc >= 0 ? 0 : -1;
}
GLC int glGetFragDataIndex(unsigned prog, const char* name) { return glGetFragDataIndexEXT(prog, name); }
GLC int glGetProgramResourceLocationIndex(unsigned prog, unsigned iface, const char* name) {
    int loc = AL(int (*)(unsigned, unsigned, const char*), "glGetProgramResourceLocation")(prog, iface, name);
    return loc >= 0 ? 0 : -1;
}
GLC int glGetProgramResourceLocationIndexEXT(unsigned prog, unsigned iface, const char* name) {
    return glGetProgramResourceLocationIndex(prog, iface, name);
}

/* ARB_shader_atomic_counters query -> ES 3.1 program interface query (GL_ATOMIC_COUNTER_BUFFER = 0x92C0) */
GLC void glGetActiveAtomicCounterBufferiv(unsigned prog, unsigned idx, unsigned pname, int* params) {
    typedef void (*PRI)(unsigned, unsigned, unsigned, int, const unsigned*, int, int*, int*);
    PRI f = AL(PRI, "glGetProgramResourceiv");
    if (!f || !params) return;
    unsigned prop = 0;
    switch (pname) {
    case 0x92C1: prop = 0x9302; break;   /* BINDING        -> BUFFER_BINDING */
    case 0x92C4: prop = 0x9303; break;   /* DATA_SIZE      -> BUFFER_DATA_SIZE */
    case 0x92C5: prop = 0x9304; break;   /* NUM_ACTIVE_VARIABLES */
    case 0x92C6: prop = 0x9305; break;   /* ACTIVE_VARIABLES */
    case 0x92C7: prop = 0x92C9; break;   /* REFERENCED_BY_VERTEX_SHADER */
    case 0x92C8: prop = 0x92CA; break;   /* ... TESS_CONTROL */
    case 0x92C9: prop = 0x92CB; break;   /* ... TESS_EVALUATION */
    case 0x92CA: prop = 0x92CC; break;   /* ... GEOMETRY */
    case 0x92CB: prop = 0x92CD; break;   /* ... FRAGMENT */
    case 0x90ED: prop = 0x930B; break;   /* ... COMPUTE */
    default: *params = 0; return;
    }
    int cnt = 1;
    if (prop == 0x9305) {
        unsigned np = 0x9304; int n = 0;
        f(prog, 0x92C0, idx, 1, &np, 1, NULL, &n);
        cnt = n > 0 ? n : 1;
    }
    f(prog, 0x92C0, idx, 1, &prop, cnt, NULL, params);
}
