/* gl_alias.h - resolve core -> OES -> EXT -> KHR on Mali + desktop aliases.
 *
 * Included by glx_shim.c with GL_ALIAS_IMPL (a single .c file defines the exported functions).
 * Adaptations compared to the initial version:
 *   - GL types are defined here (without <EGL/egl.h>/<GLES3/gl32.h>, which would conflict with
 *     declarations in glx_shim.c); identical typedef redefinitions are allowed in C11;
 *   - g_mali_handle is the Mali handle loaded via SPHAL (plain dlopen fails in Termux);
 *   - glBindFramebufferEXT goes through the shim's glBindFramebuffer (framebuffer 0 -> window FBO);
 *   - glDrawBuffer: GL_BACK/FRONT/LEFT/RIGHT -> GL_COLOR_ATTACHMENT0 (not GL_BACK), because
 *     "framebuffer 0" is actually an FBO and glDrawBuffers on an FBO only accepts GL_NONE / COLOR_ATTACHMENTi.
 */
#ifndef GL_ALIAS_H
#define GL_ALIAS_H

#include <dlfcn.h>
#include <stdio.h>
#include <stdatomic.h>

typedef unsigned int  GLenum;
typedef unsigned int  GLuint;
typedef unsigned int  GLbitfield;
typedef unsigned char GLboolean;
typedef int           GLint;
typedef int           GLsizei;
typedef float         GLfloat;
typedef long          GLintptr;
typedef long          GLsizeiptr;
#ifndef GL_MAP_READ_BIT
#define GL_MAP_READ_BIT  0x0001
#define GL_MAP_WRITE_BIT 0x0002
#endif

extern void* eglGetProcAddress(const char*);

#ifndef GLC
#define GLC __attribute__((visibility("default")))
#endif

extern void* g_mali_handle;            /* Mali handle (SPHAL) */

/* Look up real exports from Mali first (works without a current EGL context),
 * and fall back to eglGetProcAddress (which may return non-NULL even for unsupported functions). */
static inline void* gla_sym(const char* base) {
    static const char* const suf[] = { "", "OES", "EXT", "KHR" };
    char n[128];
    if (g_mali_handle)
        for (int i = 0; i < 4; i++) {
            snprintf(n, sizeof n, "%s%s", base, suf[i]);
            void* p = dlsym(g_mali_handle, n);
            if (p) return p;
        }
    for (int i = 0; i < 4; i++) {
        snprintf(n, sizeof n, "%s%s", base, suf[i]);
        void* p = (void*)eglGetProcAddress(n);
        if (p) return p;
    }
    return NULL;
}

/* GLA(Name, type) -> pointer resolved once, thread-safe (GNU stmt-expr). */
#define GLA(name, T) ({                                                        \
    static _Atomic(void*) _c; static atomic_int _d;                            \
    void* _v = atomic_load_explicit(&_c, memory_order_acquire);                \
    if (!_v && !atomic_load_explicit(&_d, memory_order_acquire)) {             \
        _v = gla_sym("gl" #name);                                              \
        if (g_mali_handle) {          /* cache only after dlopen */            \
            atomic_store_explicit(&_c, _v, memory_order_release);              \
            atomic_store_explicit(&_d, 1, memory_order_release);               \
        }                                                                      \
    }                                                                          \
    (T)_v; })

#define GLA_WARN_ONCE(msg) do { static atomic_int _w;                          \
    if (!atomic_exchange(&_w, 1)) fprintf(stderr, "[gl_alias] %s\n", msg); } while (0)

/* 1:1 aliases (by function name), with NULL check.
 *   GLA_FWD (Ret, newName, CoreName, (param decl), (call))   - with return value
 *   GLA_FWDV(     newName, CoreName, (param decl), (call))   - void */
#define GLA_FWD(R, ext, core, P, A) \
    GLC R ext P { R (*_fn)P = GLA(core, R(*)P); return _fn ? _fn A : (R)0; }
#define GLA_FWDV(ext, core, P, A) \
    GLC void ext P { void (*_fn)P = GLA(core, void(*)P); if (_fn) _fn A; }

#ifdef GL_ALIAS_IMPL

/* ---------- GL_EXT_framebuffer_object -> core ---------- */
/* glBindFramebuffer comes from libGLESv2_shim.c (maps framebuffer 0 to the X11 surface FBO) */
GLC void glBindFramebufferEXT(GLenum t, GLuint f) { glBindFramebuffer(t, f); }
GLA_FWDV(glGenFramebuffersEXT,      GenFramebuffers,      (GLsizei n, GLuint* f), (n, f))
GLA_FWDV(glDeleteFramebuffersEXT,   DeleteFramebuffers,   (GLsizei n, const GLuint* f), (n, f))
GLA_FWDV(glBindRenderbufferEXT,     BindRenderbuffer,     (GLenum t, GLuint r), (t, r))
GLA_FWDV(glGenRenderbuffersEXT,     GenRenderbuffers,     (GLsizei n, GLuint* r), (n, r))
GLA_FWDV(glDeleteRenderbuffersEXT,  DeleteRenderbuffers,  (GLsizei n, const GLuint* r), (n, r))
GLA_FWDV(glRenderbufferStorageEXT,  RenderbufferStorage,  (GLenum t, GLenum f, GLsizei w, GLsizei h), (t, f, w, h))
GLA_FWDV(glFramebufferTexture2DEXT, FramebufferTexture2D, (GLenum t, GLenum a, GLenum tt, GLuint tex, GLint l), (t, a, tt, tex, l))
GLA_FWDV(glFramebufferRenderbufferEXT, FramebufferRenderbuffer, (GLenum t, GLenum a, GLenum rt, GLuint r), (t, a, rt, r))
GLA_FWDV(glGenerateMipmapEXT,       GenerateMipmap,       (GLenum t), (t))
GLA_FWD(GLenum,    glCheckFramebufferStatusEXT, CheckFramebufferStatus, (GLenum t), (t))
GLA_FWD(GLboolean, glIsFramebufferEXT,          IsFramebuffer,          (GLuint f), (f))
GLA_FWD(GLboolean, glIsRenderbufferEXT,         IsRenderbuffer,         (GLuint r), (r))

/* ---------- glMapBuffer (does not exist in ES; only glMapBufferOES / MapBufferRange) ---------- */
GLC void* glMapBuffer(GLenum target, GLenum access) {
    void (*gp)(GLenum, GLenum, GLint*) = GLA(GetBufferParameteriv, void(*)(GLenum, GLenum, GLint*));
    void* (*mr)(GLenum, GLintptr, GLsizeiptr, GLbitfield) =
        GLA(MapBufferRange, void*(*)(GLenum, GLintptr, GLsizeiptr, GLbitfield));
    if (!gp || !mr) return NULL;
    GLint size = 0;
    gp(target, 0x8764 /* GL_BUFFER_SIZE */, &size);
    GLbitfield f = access == 0x88B8 ? GL_MAP_READ_BIT :                       /* GL_READ_ONLY  */
                   access == 0x88B9 ? GL_MAP_WRITE_BIT :                      /* GL_WRITE_ONLY */
                                      (GL_MAP_READ_BIT | GL_MAP_WRITE_BIT);   /* GL_READ_WRITE */
    return mr(target, 0, size, f);
}

/* ---------- MultiDrawElementsBaseVertex ---------- */
GLC void glMultiDrawElementsBaseVertex(GLenum m, const GLsizei* c, GLenum t,
        const void* const* idx, GLsizei n, const GLint* bv) {
    void (*d)(GLenum, GLsizei, GLenum, const void*, GLint) =
        GLA(DrawElementsBaseVertex, void(*)(GLenum, GLsizei, GLenum, const void*, GLint));
    for (GLsizei k = 0; d && k < n; k++) d(m, c[k], t, idx[k], bv[k]);
}

/* ---------- glDrawBuffer (desktop) -> glDrawBuffers (ES3) ---------- */
GLC void glDrawBuffer(GLenum mode) {
    void (*db)(GLsizei, const GLenum*) = GLA(DrawBuffers, void(*)(GLsizei, const GLenum*));
    if (!db) return;                                    /* ES1/ES2: does not exist, ignored */
    GLenum b = mode;
    if (mode == 0x0404 /*FRONT*/ || mode == 0x0405 /*BACK*/ || mode == 0x0408 /*FRONT_AND_BACK*/ ||
        mode == 0x0406 /*LEFT*/  || mode == 0x0407 /*RIGHT*/)
        b = 0x8CE0;                                     /* GL_COLOR_ATTACHMENT0 */
    db(1, &b);
}


#endif /* GL_ALIAS_IMPL */
#endif /* GL_ALIAS_H */
