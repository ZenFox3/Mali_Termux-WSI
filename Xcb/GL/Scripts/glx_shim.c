/*
 * glx_shim.c - libGL.so.1: GLX 1.4 over EGL (our libEGL.so.1 shim) on Mali.
 *
 * Includes libGLESv2_shim.c: same trampolines gl* -> Mali (+ framebuffer 0), so
 * libGL.so.1 exports gl* functions as well, not just glX*.
 *
 * (requires libEGL.so.1 and gles_list.h already generated)
 *
 * Context: Mali only has GLES. ES version is selected as follows:
 *   GLX_ES=1|2|3 (env)              -> forced
 *   glXCreateContextAttribsARB      -> ES profile (0x4): major 1/2/3; no profile: major>=3 => ES3
 *   glXCreateContext (legacy)       -> ES2 (upgraded to ES3 by EGL shim)
 * Desktop GL 1.x (glBegin/glEnd, display lists, glFrustum...) is emulated over GLES 1.1 in gl_compat.c;
 * run fixed-function applications with GLX_ES=1.
 */
#include <pthread.h>
#include <xcb/xcb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Functions overridden by gl_compat.c / gl_es_emul.c / gl_desktop_ext.c (wrapper with display list recording / GL_FRONT_AND_BACK)
   that Mali already exports: their trampolines get an alternative name, pointer p_<name> remains. */

#define glBlendFunc                glx_ovr_glBlendFunc
#define glBlendFuncSeparate        glx_ovr_glBlendFuncSeparate
#define glBlendFunci               glx_ovr_glBlendFunci  
#define glBlendEquation            glx_ovr_glBlendEquation
#define glBlendEquationSeparate    glx_ovr_glBlendEquationSeparate
#define glBlendColor               glx_ovr_glBlendColor
#define glUseProgram               glx_ovr_glUseProgram
#define glDeleteProgram            glx_ovr_glDeleteProgram

#define glNormal3f                 glx_ovr_glNormal3f
#define glColor4f                  glx_ovr_glColor4f
#define glColor4ub                 glx_ovr_glColor4ub
#define glMaterialfv               glx_ovr_glMaterialfv
#define glMaterialf                glx_ovr_glMaterialf
#define glLightfv                  glx_ovr_glLightfv
#define glLightf                   glx_ovr_glLightf
#define glShadeModel               glx_ovr_glShadeModel
#define glFrontFace                glx_ovr_glFrontFace
#define glEnable                   glx_ovr_glEnable
#define glDisable                  glx_ovr_glDisable
#define glBindTexture              glx_ovr_glBindTexture
#define glPushMatrix               glx_ovr_glPushMatrix
#define glPopMatrix                glx_ovr_glPopMatrix
#define glLoadIdentity             glx_ovr_glLoadIdentity
#define glTranslatef               glx_ovr_glTranslatef
#define glScalef                   glx_ovr_glScalef
#define glRotatef                  glx_ovr_glRotatef
#define glMultMatrixf              glx_ovr_glMultMatrixf
#define glGetIntegerv              glx_ovr_glGetIntegerv
#define glGetStringi               glx_ovr_glGetStringi
#define glGetString                glx_ovr_glGetString
#define glShaderSource             glx_ovr_glShaderSource
#define glGetShaderiv              glx_ovr_glGetShaderiv
#define glGetShaderSource          glx_ovr_glGetShaderSource
#define glCompressedTexImage2D     glx_ovr_glCompressedTexImage2D
#define glCompressedTexSubImage2D  glx_ovr_glCompressedTexSubImage2D
#define glCompressedTexImage3D     glx_ovr_glCompressedTexImage3D
#define glCompressedTexSubImage3D  glx_ovr_glCompressedTexSubImage3D
#define glFlush                    glx_ovr_glFlush
#define glFinish                   glx_ovr_glFinish
#define glCompileShader            glx_ovr_glCompileShader
#define glTexParameteri            glx_ovr_glTexParameteri
#define glTexParameterf            glx_ovr_glTexParameterf
#define glTexParameteriv           glx_ovr_glTexParameteriv
#define glTexParameterfv           glx_ovr_glTexParameterfv
#define glGetTexParameteriv        glx_ovr_glGetTexParameteriv
#define glGetTexParameterfv        glx_ovr_glGetTexParameterfv
#define glGenerateMipmap           glx_ovr_glGenerateMipmap
#define glGetTexLevelParameteriv   glx_ovr_glGetTexLevelParameteriv
#define glGetTexLevelParameterfv   glx_ovr_glGetTexLevelParameterfv
#define glIsEnabled                glx_ovr_glIsEnabled
#define glLogicOp                  glx_ovr_glLogicOp
#define glDrawArrays               glx_ovr_glDrawArrays
#define glDrawElements             glx_ovr_glDrawElements
#define glTexImage2D               glx_ovr_glTexImage2D
#define glTexSubImage2D            glx_ovr_glTexSubImage2D
#define glAttachShader             glx_ovr_glAttachShader
#define glDetachShader             glx_ovr_glDetachShader
#define glLinkProgram              glx_ovr_glLinkProgram
#define glBindTransformFeedback   glx_ovr_glBindTransformFeedback
#define glBeginTransformFeedback  glx_ovr_glBeginTransformFeedback
#define glEndTransformFeedback    glx_ovr_glEndTransformFeedback
#define glGetFramebufferAttachmentParameteriv glx_ovr_glGetFramebufferAttachmentParameteriv

#include "libGLESv2_shim.c"

#undef glGetFramebufferAttachmentParameteriv
#undef glEndTransformFeedback
#undef glBeginTransformFeedback
#undef glBindTransformFeedback
#undef glBlendFunci
#undef glLinkProgram
#undef glDetachShader
#undef glAttachShader
#undef glTexSubImage2D
#undef glTexImage2D
#undef glDrawElements
#undef glDrawArrays
#undef glLogicOp
#undef glIsEnabled
#undef glGetTexLevelParameterfv
#undef glGetTexLevelParameteriv
#undef glGenerateMipmap
#undef glGetTexParameterfv
#undef glGetTexParameteriv
#undef glTexParameterfv
#undef glTexParameteriv
#undef glTexParameterf
#undef glTexParameteri
#undef glCompileShader
#undef glFinish
#undef glFlush
#undef glCompressedTexSubImage3D
#undef glCompressedTexImage3D
#undef glCompressedTexSubImage2D
#undef glCompressedTexImage2D
#undef glGetShaderSource
#undef glGetShaderiv
#undef glShaderSource
#undef glGetString
#undef glGetStringi
#undef glGetIntegerv
#undef glMultMatrixf
#undef glRotatef
#undef glScalef
#undef glTranslatef
#undef glLoadIdentity
#undef glPopMatrix
#undef glPushMatrix
#undef glBindTexture
#undef glDisable
#undef glEnable
#undef glFrontFace
#undef glShadeModel
#undef glLightf
#undef glLightfv
#undef glMaterialf
#undef glMaterialfv
#undef glColor4ub
#undef glColor4f
#undef glNormal3f
#undef glDeleteProgram
#undef glUseProgram
#undef glBlendColor
#undef glBlendEquationSeparate
#undef glBlendEquation
#undef glBlendFuncSeparate
#undef glBlendFunc

#define GLX_EXPORT __attribute__((visibility("default")))

/* ---------- Debug Logging Helper ---------- */
static inline int glx_debug_enabled(void) {
    const char* env_gl = getenv("GL_DEBUG");
    const char* env_mali = getenv("MALI_DEBUG");
    const char* env_egl = getenv("EGL_DEBUG");
    return (env_gl && env_gl[0] != '0') || 
           (env_mali && env_mali[0] != '0') || 
           (env_egl && env_egl[0] != '0');
}

/* ---------- EGL (resolved from libEGL.so.1) ---------- */
extern void*    eglGetDisplay(void*);
extern unsigned eglInitialize(void*, int*, int*);
extern unsigned eglBindAPI(unsigned);
extern unsigned eglChooseConfig(void*, const int*, void**, int, int*);
extern unsigned eglGetConfigs(void*, void**, int, int*);
extern unsigned eglGetConfigAttrib(void*, void*, int, int*);
extern void*    eglCreateContext(void*, void*, void*, const int*);
extern unsigned eglDestroyContext(void*, void*);
extern unsigned eglMakeCurrent(void*, void*, void*, void*);
extern void*    eglCreateWindowSurface(void*, void*, unsigned long, const int*);
extern void*    eglCreatePbufferSurface(void*, void*, const int*);
extern unsigned eglDestroySurface(void*, void*);
extern unsigned eglSwapBuffers(void*, void*);
extern unsigned eglSwapInterval(void*, int);
extern unsigned eglQuerySurface(void*, void*, int, int*);
extern unsigned eglWaitClient(void);
extern void*    eglGetProcAddress(const char*);
extern int      eglGetError(void);

typedef unsigned long glx_xid;
typedef struct { void* ectx; void* cfg; int ver; int want4; int want_ver; int es_native;
                 int profile_mask; int flags; int from_egl; } glx_ctx;
typedef struct { void* visual; unsigned long visualid; int screen, depth, cls;
                 unsigned long red_mask, green_mask, blue_mask;
                 int colormap_size, bits_per_rgb; } glx_xvi;      /* = XVisualInfo */


static pthread_mutex_t glx_mu = PTHREAD_MUTEX_INITIALIZER;
static void* glx_edpy;
static void* glx_xdpy;
static void* glx_last_cfg;                    /* last configuration selected by glXChooseVisual */


#define GLX_VMAP_N 16
static struct { unsigned long vid; void* cfg; } glx_vmap[GLX_VMAP_N];
static int glx_vmap_next;

static void glx_vmap_put(unsigned long vid, void* cfg) {
    pthread_mutex_lock(&glx_mu);
    int k = -1;
    for (int i = 0; i < GLX_VMAP_N; i++)
        if (glx_vmap[i].vid == vid) { k = i; break; }
    if (k < 0) { k = glx_vmap_next; glx_vmap_next = (k + 1) % GLX_VMAP_N; }
    glx_vmap[k].vid = vid; glx_vmap[k].cfg = cfg;
    glx_last_cfg = cfg;
    pthread_mutex_unlock(&glx_mu);
}

static void* glx_vmap_get(unsigned long vid) {
    void* c = NULL;
    pthread_mutex_lock(&glx_mu);
    for (int i = 0; i < GLX_VMAP_N && vid; i++)
        if (glx_vmap[i].vid == vid) { c = glx_vmap[i].cfg; break; }
    if (!c) c = glx_last_cfg;              /* fallback actual */
    pthread_mutex_unlock(&glx_mu);
    return c;
}

static struct { glx_xid id; void* surf; int is_pix, w, h; unsigned gc; int depth; } glx_surfs[64];
static glx_xid glx_next_pb = 0x7e000000UL;
static __thread struct { glx_ctx* ctx; glx_xid draw, read; void* dpy; int is_pix; } T_glx;
static int glx_fbattr(void* ed, void* cfg, int attr, int* v);

#include "glx_autoes.h"  
#include "gl_compat.c"      
#include "gl_es_emul.c"     
#include "gl_drawlog.c"
#include "gl_atlasdump.c"
#include "gl_desktop_ext.c" 
#include "gl_clipdist.c"
#define GL_ALIAS_IMPL
#define g_mali_handle G_mali
#include "gl_alias.h"       /* core->OES->EXT->KHR, glMapBuffer, glDrawBuffer, TexGen */

/* ---------- Xlib via dlsym (no direct libX11 linking) ---------- */
static struct {
    void* (*XDefaultVisual)(void*, int);
    unsigned long (*XVisualIDFromVisual)(void*);
    void* (*XGetVisualInfo)(void*, long, void*, int*);
    int (*XDefaultScreen)(void*);
    int (*XFree)(void*);          
    int ok;
} GX;

static int glx_xlib(void) {
    if (GX.ok) return 1;
    void* h = dlopen("libX11.so", RTLD_NOW);
    if (!h) h = dlopen("libX11.so.6", RTLD_NOW);
    if (!h) return 0;
    *(void**)&GX.XDefaultVisual       = dlsym(h, "XDefaultVisual");
    *(void**)&GX.XVisualIDFromVisual  = dlsym(h, "XVisualIDFromVisual");
    *(void**)&GX.XGetVisualInfo       = dlsym(h, "XGetVisualInfo");
    *(void**)&GX.XDefaultScreen       = dlsym(h, "XDefaultScreen");
    *(void**)&GX.XFree = dlsym(h, "XFree");  
    GX.ok = GX.XDefaultVisual && GX.XVisualIDFromVisual && GX.XGetVisualInfo && GX.XDefaultScreen;
    return GX.ok;
}

/* Single visual: root visual (same reported by libEGL shim). Released with XFree. */
static void* glx_root_vi(void* xdpy, int screen) {
    if (!glx_xlib()) return NULL;
    if (screen < 0) screen = GX.XDefaultScreen(xdpy);
    glx_xvi t; memset(&t, 0, sizeof t);
    t.visualid = GX.XVisualIDFromVisual(GX.XDefaultVisual(xdpy, screen));
    int n = 0;
    return GX.XGetVisualInfo(xdpy, 1 /*VisualIDMask*/, &t, &n);
}

static void* glx_vi_depth(void* xdpy, int depth) {
    if (!glx_xlib()) return NULL;
    glx_xvi t; memset(&t, 0, sizeof t);
    t.screen = GX.XDefaultScreen(xdpy);
    t.depth = depth; t.cls = 4;
    int n = 0;
    glx_xvi* l = GX.XGetVisualInfo(xdpy, 2|4|8, &t, &n);
    if (!l || n < 1 || depth != 32) return l;
    const char* e = getenv("GLX_ARGB_VISUAL");       /* ex: 0x44 */
    unsigned long want = e ? strtoul(e, NULL, 0) : 0;
    for (int i = 0; i < n; i++) {
        int ok = want ? l[i].visualid == want
                      : (l[i].red_mask == 0xff0000 && l[i].green_mask == 0xff00 &&
                         l[i].blue_mask == 0xff);
        if (ok) { glx_xvi s = l[0]; l[0] = l[i]; l[i] = s; break; }
    }
    return l;
}

static int glx_cfg_depth(void* ed, void* cfg) {
    if (getenv("GLX_FORCE_DEPTH24")) return 24;
    int a = 0;
    return (eglGetConfigAttrib(ed, cfg, 0x3021, &a) && a > 0) ? 32 : 24;
}

/* ---------- Display / Configurations ---------- */
static void* glx_display(void* xdpy) {
    pthread_mutex_lock(&glx_mu);
    if (glx_edpy && glx_xdpy == xdpy) { void* d = glx_edpy; pthread_mutex_unlock(&glx_mu); return d; }
    void* d = eglGetDisplay(xdpy);
    int a, b;
    if (d && eglInitialize(d, &a, &b)) {
        eglBindAPI(0x30A0);                       /* OPENGL_ES_API */
        glx_edpy = d; glx_xdpy = xdpy;
    } else d = NULL;
    pthread_mutex_unlock(&glx_mu);
    return d;
}

/* GLX attributes -> EGL. fb=1: key-value list (FBConfig); fb=0: glXChooseVisual style (booleans without values) */
static int glx_attribs(const int* a, int fb, int* o) {
    int n = 0, rgb = 0, surf = 0;
#define PUT(k, v) do { if (n < 56) { o[n++] = (k); o[n++] = (v); } } while (0)
    while (a && *a) {
        int k = *a++;
        if (!fb && (k == 1 || k == 4 || k == 5 || k == 6)) continue;   /* USE_GL, RGBA, DOUBLEBUFFER, STEREO */
        int v = *a++;
        switch (k) {
        case 2:  PUT(0x3020, v); break;                  /* BUFFER_SIZE */
        case 8:  PUT(0x3024, v); rgb = 1; break;         /* RED */
        case 9:  PUT(0x3023, v); rgb = 1; break;         /* GREEN */
        case 10: PUT(0x3022, v); rgb = 1; break;         /* BLUE */
        case 11: PUT(0x3021, v); break;                  /* ALPHA */
        case 12: PUT(0x3025, v); break;                  /* DEPTH */
        case 13: PUT(0x3026, v); break;                  /* STENCIL */
        case 100000: PUT(0x3032, v); break;              /* SAMPLE_BUFFERS */
        case 100001: PUT(0x3031, v); break;              /* SAMPLES */
        case 0x8010: {                                   /* DRAWABLE_TYPE */
            int s = ((v & 1) ? 4 : 0) | ((v & 6) ? 1 : 0);   /* WINDOW->EGL_WINDOW, PIXMAP/PBUFFER->EGL_PBUFFER */
            if (s) { PUT(0x3033, s); surf = 1; }
            break; }
        default: break;
        }
    }
    if (!rgb) { PUT(0x3024, 8); PUT(0x3023, 8); PUT(0x3022, 8); }
    if (!surf) PUT(0x3033, 4);                           /* EGL_WINDOW_BIT */
    PUT(0x3040, 0x5);                                    /* RENDERABLE_TYPE: ES1 | ES2 */
    o[n] = 0x3038;
#undef PUT
    return n;
}

static int glx_choose(void* ed, const int* a, int fb, void** out, int max) {
    int at[64], n = 0;
    if (!a || !*a) {                                     /* no constraints: all configurations */
        if (!eglGetConfigs(ed, out, max, &n)) return 0;
        return n;
    }
    int m = glx_attribs(a, fb, at);
    if (eglChooseConfig(ed, at, out, max, &n) && n) return n;
    for (int i = 0; i < m; i += 2)
        if (at[i]==0x3021 || at[i]==0x3025 || at[i]==0x3026 || at[i]==0x3031 || at[i]==0x3032)
            at[i+1] = 0;   /* alpha, depth, stencil, samples */
    if (eglChooseConfig(ed, at, out, max, &n) && n) return n;
    return 0;
}

/* GLX_* (attribute) -> value, from an EGL configuration. 0 = ok, 2 = GLX_BAD_ATTRIBUTE */
static int glx_fbattr(void* ed, void* cfg, int attr, int* v) {
    int e = 0;
#define EGLA(code) (eglGetConfigAttrib(ed, cfg, (code), &e) ? (*v = e, 0) : 2)
    switch (attr) {
    case 1: *v = 1; return 0;                            /* USE_GL */
    case 2: return EGLA(0x3020);                         /* BUFFER_SIZE */
    case 3: *v = 0; return 0;                            /* LEVEL */
    case 4: *v = 1; return 0;                            /* RGBA */
    case 5: *v = 1; return 0;                            /* DOUBLEBUFFER */
    case 6: case 7: *v = 0; return 0;                    /* STEREO, AUX_BUFFERS */
    case 8:  return EGLA(0x3024);
    case 9:  return EGLA(0x3023);
    case 10: return EGLA(0x3022);
    case 11: return EGLA(0x3021);
    case 12: return EGLA(0x3025);
    case 13: return EGLA(0x3026);
    case 14: case 15: case 16: case 17: *v = 0; return 0;        /* ACCUM_* */
    case 0x20: *v = 0x8000; return 0;                    /* CONFIG_CAVEAT = GLX_NONE */
    case 0x22: *v = 0x8002; return 0;                    /* X_VISUAL_TYPE = TRUE_COLOR */
    case 0x23: *v = 0x8000; return 0;                    /* TRANSPARENT_TYPE = NONE */
case 0x800B: {
    glx_xvi* vi = glx_vi_depth(glx_xdpy, glx_cfg_depth(ed, cfg));
    if (!vi) return 2;
    *v = (int)vi->visualid;
    if (GX.XFree) GX.XFree(vi);
    return 0;
}
    case 0x800C: *v = 0; return 0;                        /* GLX_SCREEN */
    case 0x25: case 0x26: case 0x27: case 0x28: *v = 0; return 0;  /* TRANSPARENT_*_VALUE */
    case 0x8010: *v = 1 | 2 | 4; return 0;               /* DRAWABLE_TYPE: WINDOW | PIXMAP | PBUFFER */
    case 0x8011: *v = 1; return 0;                       /* RENDER_TYPE = RGBA_BIT */
    case 0x8012: *v = 1; return 0;                       /* X_RENDERABLE */
    case 0x8013: return EGLA(0x3028);                    /* FBCONFIG_ID */
    case 0x8016: case 0x8017: *v = 4096; return 0;       /* MAX_PBUFFER_WIDTH/HEIGHT */
    case 0x8018: *v = 4096 * 4096; return 0;             /* MAX_PBUFFER_PIXELS */
    case 0x20B2: *v = 0; return 0;                       /* FRAMEBUFFER_SRGB_CAPABLE */
    case 100000: return EGLA(0x3032);                    /* SAMPLE_BUFFERS */
    case 100001: return EGLA(0x3031);                    /* SAMPLES */
    default: if (glx_debug_enabled()) fprintf(stderr,"[glx] FBattr 0x%x unsupported\n",attr); return 2;
    }
#undef EGLA
}

static void* glx_default_cfg(void* ed) {
    static const int at[] = { 0x3024, 8, 0x3023, 8, 0x3022, 8, 0x3033, 4, 0x3040, 0x5, 0x3038 };
    void* c = NULL; int n = 0;
    if (eglChooseConfig(ed, at, &c, 1, &n) && n) return c;
    return NULL;
}

/* ---------- Drawables: Window XID -> EGLSurface (created on first use) ---------- */
static void* glx_surface(void* ed, glx_xid id, void* cfg) {
    void* s = NULL;
    pthread_mutex_lock(&glx_mu);
    for (int i = 0; i < 64; i++) if (glx_surfs[i].id == id) { s = glx_surfs[i].surf; break; }
    pthread_mutex_unlock(&glx_mu);
    if (s) return s;
    if (!cfg) cfg = glx_default_cfg(ed);
    s = eglCreateWindowSurface(ed, cfg, id, NULL);
    if (!s) return NULL;
    pthread_mutex_lock(&glx_mu);
    for (int i = 0; i < 64; i++) if (!glx_surfs[i].id) { glx_surfs[i].id = id; glx_surfs[i].surf = s; break; }
    pthread_mutex_unlock(&glx_mu);
    return s;
}

static void* glx_surface_find(glx_xid id) {
    void* s = NULL;
    pthread_mutex_lock(&glx_mu);
    for (int i = 0; i < 64; i++) if (glx_surfs[i].id == id) { s = glx_surfs[i].surf; break; }
    pthread_mutex_unlock(&glx_mu);
    return s;
}

static void* glx_surface_drop(glx_xid id) {
    void* s = NULL;
    pthread_mutex_lock(&glx_mu);
    for (int i = 0; i < 64; i++) if (glx_surfs[i].id == id) { s = glx_surfs[i].surf; glx_surfs[i].id = 0; glx_surfs[i].surf = NULL; break; }
    pthread_mutex_unlock(&glx_mu);
    return s;
}

/* ---------- GLX Pixmaps: EGL pbuffer + read back to X pixmap ----------
   Mali lacks native pixmap surfaces. We render into a pbuffer, and on glFlush/glFinish/glXWaitGL/glXSwapBuffers/
   context switch we copy pixels back to the X pixmap via xcb_put_image (slow, but accurate). */
static xcb_connection_t* glx_xcb(void* xdpy) {
    static xcb_connection_t* (*f)(void*);
    if (!f) {
        void* h = dlopen("libX11-xcb.so", RTLD_NOW);
        if (!h) h = dlopen("libX11-xcb.so.1", RTLD_NOW);
        if (h) *(void**)&f = dlsym(h, "XGetXCBConnection");
    }
    return f ? f(xdpy) : NULL;
}

static int glx_is_pixmap(glx_xid id) {
    int r = 0;
    if (!id) return 0;
    pthread_mutex_lock(&glx_mu);
    for (int i = 0; i < 64; i++) if (glx_surfs[i].id == id) { r = glx_surfs[i].is_pix; break; }
    pthread_mutex_unlock(&glx_mu);
    return r;
}

static unsigned glx_pix_gc(glx_xid id) {
    unsigned g = 0;
    pthread_mutex_lock(&glx_mu);
    for (int i = 0; i < 64; i++) if (glx_surfs[i].id == id) { g = glx_surfs[i].gc; break; }
    pthread_mutex_unlock(&glx_mu);
    return g;
}

static glx_xid glx_pixmap_create(void* dpy, void* cfg, glx_xid pix) {
    void* ed = glx_display(dpy);
    xcb_connection_t* c = glx_xcb(dpy);
    if (!ed || !c) {
        if (glx_debug_enabled()) {
            fprintf(stderr, "[glx] glXCreatePixmap: No EGL display (%p) or xcb connection (%p; libX11-xcb.so missing?)\n", ed, (void*)c);
        }
        return 0;
    }
    xcb_get_geometry_reply_t* g = xcb_get_geometry_reply(c, xcb_get_geometry(c, (xcb_drawable_t)pix), NULL);
    if (!g) { 
        if (glx_debug_enabled()) {
            fprintf(stderr, "[glx] glXCreatePixmap: xcb_get_geometry failed for 0x%lx (invalid pixmap?)\n", (unsigned long)pix); 
        }
        return 0; 
    }
    int w = g->width, h = g->height, pd = g->depth;
    free(g);
    if (!cfg) cfg = glx_last_cfg ? glx_last_cfg : glx_default_cfg(ed);
    const int ea[] = { 0x3057, w, 0x3056, h, 0x3038 };
    void* s = eglCreatePbufferSurface(ed, cfg, ea);
    if (!s) {
        int e1 = eglGetError();
        /* Application config might lack EGL_PBUFFER_BIT: try one that has it */
        static const int pa[] = { 0x3024, 8, 0x3023, 8, 0x3022, 8, 0x3033, 1 /*PBUFFER*/, 0x3040, 0x5, 0x3038 };
        void* pc = NULL; int n = 0;
        if (eglChooseConfig(ed, pa, &pc, 1, &n) && n) s = eglCreatePbufferSurface(ed, pc, ea);
        if (!s) {
            if (glx_debug_enabled()) {
                fprintf(stderr, "[glx] glXCreatePixmap: eglCreatePbufferSurface %dx%d failed (EGL 0x%x, then 0x%x with PBUFFER config)\n",
                        w, h, e1, eglGetError());
            }
            return 0;
        }
        if (glx_debug_enabled()) {
            fprintf(stderr, "[glx] glXCreatePixmap: Application config does not support pbuffer (EGL 0x%x); falling back\n", e1);
        }
    }
    unsigned gc = xcb_generate_id(c);
    xcb_create_gc(c, gc, (xcb_drawable_t)pix, 0, NULL);
    xcb_flush(c);
    pthread_mutex_lock(&glx_mu);
    for (int i = 0; i < 64; i++)
        if (!glx_surfs[i].id) {
            glx_surfs[i].id = pix; glx_surfs[i].surf = s; glx_surfs[i].is_pix = 1;
            glx_surfs[i].w = w; glx_surfs[i].h = h; glx_surfs[i].gc = gc;
            glx_surfs[i].depth = pd;
            break;
        }
    pthread_mutex_unlock(&glx_mu);
    return pix;
}

/* Copies current pbuffer contents into X pixmap (requires current context on this thread) */
static void glx_pixmap_sync_current(void) {
    glx_xid id = T_glx.draw;
    int w = 0, h = 0, depth = 24;
    unsigned gc = 0;
    if (!id || !T_glx.is_pix || !T_glx.dpy) return;
    pthread_mutex_lock(&glx_mu);
    for (int i = 0; i < 64; i++)
        if (glx_surfs[i].id == id && glx_surfs[i].is_pix) {
    w = glx_surfs[i].w; h = glx_surfs[i].h; gc = glx_surfs[i].gc;
    depth = glx_surfs[i].depth ? glx_surfs[i].depth : 24;
    break;
        }
    pthread_mutex_unlock(&glx_mu);
    xcb_connection_t* c = glx_xcb(T_glx.dpy);
    if (!c || !gc || w <= 0 || h <= 0) return;

    if (glx_debug_enabled()) {
        static int dbg_done;
        if (!dbg_done) {
            dbg_done = 1;
            int vp[4] = { 0, 0, 0, 0 };
            ((F_geti)p_glGetIntegerv)(0x0BA2, vp);
            int ver = T_glx.ctx ? T_glx.ctx->ver : 0;
            fprintf(stderr, "[glx-debug] Pixmap 0x%lx %dx%d, ES%d, viewport=%d,%d,%d,%d\n",
                    (unsigned long)id, w, h, ver, vp[0], vp[1], vp[2], vp[3]);
            if (ver == 1) {
                float m[16];
                ((F_getf)p_glGetFloatv)(0x0BA7, m);
                fprintf(stderr, "[glx-debug] Projection matrix:");
                for (int i = 0; i < 16; i++) fprintf(stderr, " %.3g", m[i]);
                fprintf(stderr, "\n");
            }
        }
    }
    size_t stride = (size_t)w * 4;
    unsigned char* px  = malloc(stride * h);
    unsigned char* out = malloc(stride * h);
    if (!px || !out) { free(px); free(out); return; }
    ((void (*)(int, int, int, int, unsigned, unsigned, void*))p_glReadPixels)(0, 0, w, h, 0x1908 /*RGBA*/, 0x1401 /*UBYTE*/, px);
for (int y = 0; y < h; y++) {
    const unsigned char* src = px + (size_t)(h - 1 - y) * stride;
    unsigned char* dst = out + (size_t)y * stride;
    for (int x = 0; x < w; x++) {
        dst[4 * x]     = src[4 * x + 2];
        dst[4 * x + 1] = src[4 * x + 1];
        dst[4 * x + 2] = src[4 * x];
        dst[4 * x + 3] = (depth == 32) ? src[4 * x + 3] : 0xff;
    }
}
    size_t maxb = (size_t)xcb_get_maximum_request_length(c) * 4;
    maxb = maxb > 128 ? maxb - 128 : maxb;
    size_t rows = maxb / stride;
    if (rows < 1) rows = 1;
    for (size_t y0 = 0; y0 < (size_t)h; y0 += rows) {
        size_t r = (y0 + rows > (size_t)h) ? (size_t)h - y0 : rows;
        xcb_put_image(c, XCB_IMAGE_FORMAT_Z_PIXMAP, (xcb_drawable_t)id, gc, (uint16_t)w, (uint16_t)r,
              0, (int16_t)y0, 0, (uint8_t)depth, (uint32_t)(r * stride), out + y0 * stride);
    }
    xcb_flush(c);
    free(px); free(out);
}

/* ---------- Contexts ---------- */
/* Select ES version: GLX_ES (env) > requested by app at creation > imported by app > default ES2 */
static int glx_es_version(int major, int profile, int have_attribs) {
    const char* e = getenv("GLX_ES");
    if (e && *e) { int v = atoi(e); return v < 1 ? 1 : (v > 3 ? 3 : v); }          /* manual override */
    int v = 2, a = 0;
    const char* why = "";
    const char* eg = getenv("GLX_GL");
    int envgl = eg ? atoi(eg) : 0;
    if (envgl == 121 || (envgl > 0 && envgl < 20)) {
        v = 1; why = "GLX_GL < 2.0 forces ES1 (fixed-function)";
    } else if (envgl >= 30) {
        v = 3; why = "GLX_GL >= 3.0 forces ES3";
    } else if (have_attribs && (profile & 0x4)) {
        int mj = major ? major : 1;
        v = mj >= 3 ? 3 : (mj == 1 ? 1 : 2); why = "explicit ES profile requested";
    } else if (have_attribs && major == 1) {
        v = 1; why = "app requested GL 1.x";
    } else if (have_attribs && major >= 3) {
        v = 3; why = "app requested GL 3.x+";
    } else if ((a = ae_detect()) == 1) {
        v = 1; why = "app imports fixed-function calls only";
    } else if (a == 2) {
        v = 2; why = "app imports shader functions";
} else {
    const char* d = getenv("GLX_DEFAULT_ES");
    v = (d && atoi(d) == 3) ? 3 : 2;
    why = "default fallback, app analysis inconclusive";
}
    if (glx_debug_enabled()) {
        static int said;
        if (!said) { said = 1; fprintf(stderr, "[glx-auto] ES%d context (%s)\n", v, why); }
    }
    return v;
}

static void* glx_make_ctx(void* xdpy, void* cfg, void* share, int ver) {
    void* ed = glx_display(xdpy);
    if (!ed) return NULL;
    if (!cfg) cfg = glx_default_cfg(ed);
    if (!cfg) return NULL;
    const int at[] = { 0x3098, ver, 0x3038 };
    glx_ctx* sh = (glx_ctx*)share;
    void* ec = eglCreateContext(ed, cfg, sh ? sh->ectx : NULL, at);
    if (!ec) return NULL;
    glx_ctx* c = calloc(1, sizeof *c);
    c->profile_mask = 2;   /* legacy = compat; CreateContextAttribsARB îl suprascrie */
    c->ectx = ec; c->cfg = cfg; c->ver = ver;
    c->es_native = ae_is_es();                           /* native ES app (imports ES-only functions) */
    return c;
}

/* ================= GLX API ================= */
GLX_EXPORT void* glXChooseVisual(void* dpy, int screen, int* attribs) {
    void* ed = glx_display(dpy);
    if (!ed) return NULL;
    void* cfgs[64];
    int n = glx_choose(ed, attribs, 0, cfgs, 64);
    if (!n) return NULL;
    int depth = glx_cfg_depth(ed, cfgs[0]);
    glx_xvi* vi = glx_vi_depth(dpy, depth);
    if (vi) glx_vmap_put(vi->visualid, cfgs[0]);
    else { pthread_mutex_lock(&glx_mu); glx_last_cfg = cfgs[0]; pthread_mutex_unlock(&glx_mu); }
    return vi;
}

GLX_EXPORT void* glXCreateContext(void* dpy, void* vis, void* share, int direct) {
    (void)vis; (void)direct;
    unsigned long vid = vis ? ((glx_xvi*)vis)->visualid : 0;
return glx_make_ctx(dpy, glx_vmap_get(vid), share, glx_es_version(1, 0, 0));
}

GLX_EXPORT void* glXCreateNewContext(void* dpy, void* cfg, int render_type, void* share, int direct) {
    (void)render_type; (void)direct;
    return glx_make_ctx(dpy, cfg, share, glx_es_version(1, 0, 0));
}

GLX_EXPORT void* glXCreateContextAttribsARB(void* dpy, void* cfg, void* share, int direct, const int* at) {
    (void)direct;
    int major = 0, minor = 0, profile = 0, flags = 0;
    for (; at && *at; at += 2) {
        if (at[0] == 0x2091) major = at[1];              /* CONTEXT_MAJOR_VERSION_ARB */
        else if (at[0] == 0x2092) minor = at[1];         /* CONTEXT_MINOR_VERSION_ARB */
        else if (at[0] == 0x9126) profile = at[1];       /* CONTEXT_PROFILE_MASK_ARB */
        else if (at[0] == 0x2094) flags = at[1];
    }
    glx_ctx* gc = glx_make_ctx(dpy, cfg, share, glx_es_version(major, profile, 1));
    if (gc && (profile & 0x4)) gc->es_native = 1;       /* explicit ES profile requested */
    if (gc) {
        gc->want4 = (major >= 4) && !gc->es_native;
        gc->want_ver = (major > 0) ? (major * 10 + minor) : 0;
        if (profile & 1)      gc->profile_mask = 1;
        else if (profile & 2) gc->profile_mask = 2;
        else                  gc->profile_mask = 0;
        gc->flags = flags;
    }
    return gc;
}

GLX_EXPORT void glXDestroyContext(void* dpy, void* ctx) {
    void* ed = glx_display(dpy);
    glx_ctx* c = ctx;
    if (!ed || !c) return;
    if (T_glx.ctx == c) { eglMakeCurrent(ed, NULL, NULL, NULL); T_glx.ctx = NULL; }
    eglDestroyContext(ed, c->ectx);
    free(c);
}

GLX_EXPORT int glXMakeContextCurrent(void* dpy, glx_xid draw, glx_xid read, void* ctx) {
    void* ed = glx_display(dpy);
    if (!ed) return 0;
    if (T_glx.is_pix) glx_pixmap_sync_current();           /* finish previous pixmap before switching */
    glx_ctx* c = ctx;
    if (!c) {
        eglMakeCurrent(ed, NULL, NULL, NULL);
        memset(&T_glx, 0, sizeof T_glx);
        return 1;
    }
    void* s = NULL;
    if (draw) {
        s = glx_surface(ed, draw, c->cfg);
        if (!s) return 0;
    }
    if (!eglMakeCurrent(ed, s, s, c->ectx)) return 0;
    T_glx.ctx = c; T_glx.draw = draw; T_glx.read = read ? read : draw; T_glx.dpy = dpy;
    T_glx.is_pix = glx_is_pixmap(draw);
    return 1;
}

GLX_EXPORT int glXMakeCurrent(void* dpy, glx_xid draw, void* ctx) {
    return glXMakeContextCurrent(dpy, draw, draw, ctx);
}

static void dl_swap(void);

GLX_EXPORT void glXSwapBuffers(void* dpy, glx_xid draw) {
    void* ed = glx_display(dpy);
    void* s = glx_surface_find(draw);
    if (glx_is_pixmap(draw)) { if (T_glx.draw == draw) glx_pixmap_sync_current(); return; }
    if (ed && s) { dl_swap(); eglSwapBuffers(ed, s); }
}

GLX_EXPORT void* glXGetCurrentContext(void)          { return T_glx.ctx; }
GLX_EXPORT glx_xid glXGetCurrentDrawable(void)       { return T_glx.draw; }
GLX_EXPORT glx_xid glXGetCurrentReadDrawable(void)   { return T_glx.read; }
GLX_EXPORT void* glXGetCurrentDisplay(void)          { return T_glx.dpy; }
GLX_EXPORT int glXIsDirect(void* dpy, void* ctx)     { (void)dpy; (void)ctx; return 1; }
GLX_EXPORT void glXWaitGL(void)                      { eglWaitClient(); if (T_glx.is_pix) glx_pixmap_sync_current(); }
GLX_EXPORT void glXWaitX(void)                       { }
GLX_EXPORT void glXCopyContext(void* d, void* s, void* t, unsigned long m) { (void)d; (void)s; (void)t; (void)m; }
GLX_EXPORT void glXUseXFont(unsigned long font, int first, int count, int list) {
    (void)font; (void)first; (void)count; (void)list;
    if (glx_debug_enabled()) {
        fprintf(stderr, "[glx] glXUseXFont: Display lists are unsupported on GLES\n");
    }
}

GLX_EXPORT int glXQueryExtension(void* dpy, int* errb, int* evb) {
    (void)dpy; if (errb) *errb = 0; if (evb) *evb = 0; return 1;
}
GLX_EXPORT int glXQueryVersion(void* dpy, int* maj, int* min) {
    (void)dpy; if (maj) *maj = 1; if (min) *min = 4; return 1;
}
#define GLX_EXTS "GLX_ARB_get_proc_address GLX_ARB_create_context GLX_ARB_create_context_profile " \
    "GLX_EXT_create_context_es_profile GLX_EXT_create_context_es2_profile GLX_EXT_swap_control " \
    "GLX_SGI_swap_control GLX_MESA_swap_control GLX_EXT_visual_info GLX_ARB_multisample GLX_MESA_query_renderer"
GLX_EXPORT const char* glXQueryExtensionsString(void* dpy, int screen) { (void)dpy; (void)screen; return GLX_EXTS; }
GLX_EXPORT const char* glXQueryServerString(void* dpy, int screen, int name) {
    (void)dpy; (void)screen;
    return name == 1 ? "ARM" : (name == 2 ? "1.4" : GLX_EXTS);
}
GLX_EXPORT const char* glXGetClientString(void* dpy, int name) { return glXQueryServerString(dpy, 0, name); }

GLX_EXPORT int glXGetConfig(void* dpy, void* vis, int attrib, int* value) {
    void* ed = glx_display(dpy);
    if (!ed) return 1;                                   /* GLX_BAD_SCREEN-ish */
    glx_xvi* vi = (glx_xvi*)vis;
    if (vi && glx_xlib() &&
        vi->visualid != GX.XVisualIDFromVisual(GX.XDefaultVisual(dpy, vi->screen))) {
    
        if (vi->depth != 32) {
            if (attrib == 1) { *value = 0; return 0; }       /* GLX_USE_GL = False */
            return 4;                                        /* GLX_BAD_VISUAL */
        }
    }
    void* cfg = glx_vmap_get(vi ? vi->visualid : 0);
    if (!cfg) cfg = glx_default_cfg(ed);
    if (!cfg) return 1;
    return glx_fbattr(ed, cfg, attrib, value);
}

GLX_EXPORT void** glXGetFBConfigs(void* dpy, int screen, int* n) {
    (void)screen;
    void* ed = glx_display(dpy);
    if (n) *n = 0;
    if (!ed) return NULL;
    void* tmp[128]; int cnt = 0;
    if (!eglGetConfigs(ed, tmp, 128, &cnt) || !cnt) return NULL;
    void** r = malloc(cnt * sizeof(void*));
    memcpy(r, tmp, cnt * sizeof(void*));
    if (n) *n = cnt;
    return r;
}

GLX_EXPORT void** glXChooseFBConfig(void* dpy, int screen, const int* attribs, int* n) {
    (void)screen;
    void* ed = glx_display(dpy);
    if (n) *n = 0;
    if (!ed) return NULL;
    void* tmp[128];
    int cnt = glx_choose(ed, attribs, 1, tmp, 128);
    if (!cnt) return NULL;
    void** r = malloc(cnt * sizeof(void*));
    memcpy(r, tmp, cnt * sizeof(void*));
    if (n) *n = cnt;
    return r;
}

GLX_EXPORT int glXGetFBConfigAttrib(void* dpy, void* cfg, int attr, int* v) {
    void* ed = glx_display(dpy);
    return ed ? glx_fbattr(ed, cfg, attr, v) : 1;
}

GLX_EXPORT void* glXGetVisualFromFBConfig(void* dpy, void* cfg) {
    void* ed = glx_display(dpy);
    glx_xvi* vi = glx_vi_depth(dpy, ed ? glx_cfg_depth(ed, cfg) : 24);
    if (vi && cfg) glx_vmap_put(vi->visualid, cfg);
    return vi;
}

GLX_EXPORT glx_xid glXCreateWindow(void* dpy, void* cfg, glx_xid win, const int* at) {
    (void)at;
    void* ed = glx_display(dpy);
    return (ed && glx_surface(ed, win, cfg)) ? win : 0;
}
GLX_EXPORT void glXDestroyWindow(void* dpy, glx_xid win) {
    void* ed = glx_display(dpy);
    unsigned gc = glx_is_pixmap(win) ? glx_pix_gc(win) : 0;
    if (gc && T_glx.draw == win) glx_pixmap_sync_current();
    void* s = glx_surface_drop(win);
    if (ed && s) eglDestroySurface(ed, s);
    if (gc) { xcb_connection_t* c = glx_xcb(dpy); if (c) { xcb_free_gc(c, gc); xcb_flush(c); } }
}

GLX_EXPORT glx_xid glXCreatePbuffer(void* dpy, void* cfg, const int* at) {
    void* ed = glx_display(dpy);
    if (!ed) return 0;
    int w = 1, h = 1;
    for (; at && *at; at += 2) {
        if (at[0] == 0x8041) w = at[1];                  /* PBUFFER_WIDTH */
        else if (at[0] == 0x8040) h = at[1];             /* PBUFFER_HEIGHT */
    }
    const int ea[] = { 0x3057, w, 0x3056, h, 0x3038 };
    void* s = eglCreatePbufferSurface(ed, cfg, ea);
    if (!s) return 0;
    pthread_mutex_lock(&glx_mu);
    glx_xid id = glx_next_pb++;
    for (int i = 0; i < 64; i++) if (!glx_surfs[i].id) { glx_surfs[i].id = id; glx_surfs[i].surf = s; break; }
    pthread_mutex_unlock(&glx_mu);
    return id;
}
GLX_EXPORT void glXDestroyPbuffer(void* dpy, glx_xid pb) { glXDestroyWindow(dpy, pb); }

GLX_EXPORT glx_xid glXCreateGLXPixmap(void* d, void* v, unsigned long p) { (void)v; return glx_pixmap_create(d, NULL, p); }
GLX_EXPORT glx_xid glXCreatePixmap(void* d, void* c, unsigned long p, const int* a) { (void)a; return glx_pixmap_create(d, c, p); }
GLX_EXPORT void glXDestroyGLXPixmap(void* d, glx_xid p) { glXDestroyWindow(d, p); }
GLX_EXPORT void glXDestroyPixmap(void* d, glx_xid p) { glXDestroyWindow(d, p); }

/* glFlush/glFinish: if rendering into a GLX pixmap, synchronize it with the X pixmap */
GLX_EXPORT void glFlush(void)  { ((F_v)p_glFlush)();  if (T_glx.is_pix) glx_pixmap_sync_current(); }
GLX_EXPORT void glFinish(void) { ((F_v)p_glFinish)(); if (T_glx.is_pix) glx_pixmap_sync_current(); }

GLX_EXPORT void glXQueryDrawable(void* dpy, glx_xid d, int attr, unsigned* v) {
    (void)dpy;
    void* ed = glx_edpy;
    void* s = glx_surface_find(d);
    int e = 0;
    *v = 0;
    if (!ed || !s) return;
    if (attr == 0x801D && eglQuerySurface(ed, s, 0x3057, &e)) *v = (unsigned)e;      /* GLX_WIDTH */
    else if (attr == 0x801E && eglQuerySurface(ed, s, 0x3056, &e)) *v = (unsigned)e; /* GLX_HEIGHT */
}

GLX_EXPORT int glXQueryContext(void* dpy, void* ctx, int attr, int* v) {
    (void)dpy;
    glx_ctx* c = ctx;
    if (!c) return 2;
    if (attr == 0x8011) { *v = 0x8014; return 0; }       /* RENDER_TYPE = GLX_RGBA_TYPE */
    if (attr == 0x800C) { *v = 0; return 0; }            /* SCREEN */
    if (attr == 0x9126) { *v = c->profile_mask; return 0; }
    return 2;
}
GLX_EXPORT void glXSelectEvent(void* d, glx_xid w, unsigned long m) { (void)d; (void)w; (void)m; }
GLX_EXPORT void glXGetSelectedEvent(void* d, glx_xid w, unsigned long* m) { (void)d; (void)w; if (m) *m = 0; }

/* Swap interval -> eglSwapInterval (EGL shim applies it to the current surface) */
static int glx_swap_interval = 1;                        /* GLX default: vsync on */

GLX_EXPORT void glXSwapIntervalEXT(void* dpy, glx_xid d, int interval) {
    (void)d;
    void* ed = glx_display(dpy);
    if (ed && eglSwapInterval(ed, interval)) glx_swap_interval = interval;
}
GLX_EXPORT int glXSwapIntervalMESA(unsigned interval) {
    if (!glx_edpy) return 1;
    if (!eglSwapInterval(glx_edpy, (int)interval)) return 1;
    glx_swap_interval = (int)interval;
    return 0;
}
GLX_EXPORT int glXGetSwapIntervalMESA(void) { return glx_swap_interval; }
GLX_EXPORT int glXSwapIntervalSGI(int interval) { return glXSwapIntervalMESA((unsigned)interval); }

GLX_EXPORT int glXQueryRendererIntegerMESA(void* dpy, int screen, int renderer, int attr, unsigned* v);
GLX_EXPORT int glXQueryCurrentRendererIntegerMESA(int attr, unsigned* v);
GLX_EXPORT const char* glXQueryRendererStringMESA(void* dpy, int screen, int renderer, int attr);
GLX_EXPORT const char* glXQueryCurrentRendererStringMESA(int attr);


/* ---------- glXGetProcAddress ---------- */
GLX_EXPORT void* glXGetProcAddress(const unsigned char* name);
#define GPA(n) { #n, (void*)n }
static void* glx_self_sym(const char* n) {
    static void* h;
    if (!h) {
        Dl_info i;
        if (dladdr((void*)&glx_self_sym, &i) && i.dli_fname)
            h = dlopen(i.dli_fname, RTLD_NOW | RTLD_NOLOAD);
    }
    return h ? dlsym(h, n) : NULL;
}

static void* glx_gpa(const char* name) {
    static const struct { const char* n; void* f; } tab[] = {
        GPA(glXChooseVisual), GPA(glXCreateContext), GPA(glXCreateNewContext),
        GPA(glXCreateContextAttribsARB), GPA(glXDestroyContext), GPA(glXMakeCurrent),
        GPA(glXMakeContextCurrent), GPA(glXSwapBuffers), GPA(glXGetCurrentContext),
        GPA(glXGetCurrentDrawable), GPA(glXGetCurrentReadDrawable), GPA(glXGetCurrentDisplay),
        GPA(glXIsDirect), GPA(glXWaitGL), GPA(glXWaitX), GPA(glXCopyContext), GPA(glXUseXFont),
        GPA(glXQueryExtension), GPA(glXQueryVersion), GPA(glXQueryExtensionsString),
        GPA(glXQueryServerString), GPA(glXGetClientString), GPA(glXGetConfig),
        GPA(glXGetFBConfigs), GPA(glXChooseFBConfig), GPA(glXGetFBConfigAttrib),
        GPA(glXGetVisualFromFBConfig), GPA(glXCreateWindow), GPA(glXDestroyWindow),
        GPA(glXCreatePbuffer), GPA(glXDestroyPbuffer), GPA(glXCreateGLXPixmap),
        GPA(glXCreatePixmap), GPA(glXDestroyGLXPixmap), GPA(glXDestroyPixmap),
        GPA(glXQueryDrawable), GPA(glXQueryContext), GPA(glXSelectEvent), GPA(glXGetSelectedEvent),
        GPA(glXSwapIntervalEXT), GPA(glXSwapIntervalMESA), GPA(glXGetSwapIntervalMESA), GPA(glXSwapIntervalSGI),
        GPA(glXQueryRendererIntegerMESA), GPA(glXQueryCurrentRendererIntegerMESA),
        GPA(glXQueryRendererStringMESA), GPA(glXQueryCurrentRendererStringMESA),
    };
    if (!name) return NULL;
    if (name[0] == 'g' && name[1] == 'l' && name[2] == 'X') {
        if (!strcmp(name, "glXGetProcAddress") || !strcmp(name, "glXGetProcAddressARB"))
            return (void*)glXGetProcAddress;
        for (unsigned i = 0; i < sizeof tab / sizeof tab[0]; i++)
            if (!strcmp(name, tab[i].n)) return tab[i].f;
        if (glx_debug_enabled())
            fprintf(stderr, "[glx] GetProcAddress('%s') -> NULL\n", name);
        return NULL;
    }
    { void* me = glx_self_sym(name); if (me) return me; }
    void* r = eglGetProcAddress(name);
    return r;
}

GLX_EXPORT void* glXGetProcAddress(const unsigned char* name)    { return glx_gpa((const char*)name); }
GLX_EXPORT void* glXGetProcAddressARB(const unsigned char* name) { return glx_gpa((const char*)name); }

/* ---------- GLX_MESA_query_renderer: probed from the driver, cached ---------- */
typedef struct {
    int ok;
    char vendor[128], renderer[128];
    unsigned id[2], ver[3], accel, vram_mb, uma, pref;
    unsigned core[2], compat[2], es1[2], es2[2];
} glx_qr_t;
static glx_qr_t glx_qr;

static void glx_qr_parse_es(const char* v, unsigned* maj, unsigned* min, unsigned* drv) {
    const char* p = v ? strstr(v, "ES") : NULL;
    unsigned a = 0, b = 0;
    int used = 0;
    while (p && *p && (*p < '0' || *p > '9')) p++;
    if (!p || sscanf(p, "%u.%u%n", &a, &b, &used) < 2) return;
    if (maj) *maj = a;
    if (min) *min = b;
    if (drv) {                                    /* "v1.r44p0": first 3 integers */
        int k = 0;
        for (p += used; *p && k < 3; ) {
            if (*p >= '0' && *p <= '9') { drv[k++] = strtoul(p, (char**)&p, 10); }
            else p++;
        }
    }
}

static int glx_qr_probe(void* ed, void* cfg, int ver, char* ve, char* re, char* gv, size_t n) {
    const int at[] = { 0x3098, ver, 0x3038 };
    void* ec = eglCreateContext(ed, cfg, NULL, at);
    void* s = NULL;
    if (!ec) return 0;
    int ok = eglMakeCurrent(ed, NULL, NULL, ec);              /* surfaceless */
    if (!ok) {
        const int ea[] = { 0x3057, 1, 0x3056, 1, 0x3038 };
        if ((s = eglCreatePbufferSurface(ed, cfg, ea))) ok = eglMakeCurrent(ed, s, s, ec);
    }
    if (ok) {
        const char* (*gs)(unsigned) = (const char* (*)(unsigned))p_glGetString;
        const char *a = gs(0x1F00), *b = gs(0x1F01), *c = gs(0x1F02);
        snprintf(ve, n, "%s", a ? a : "");
        snprintf(re, n, "%s", b ? b : "");
        snprintf(gv, n, "%s", c ? c : "");
    }
    eglMakeCurrent(ed, NULL, NULL, NULL);
    if (s) eglDestroySurface(ed, s);
    eglDestroyContext(ed, ec);
    return ok;
}

static unsigned glx_qr_hex(const char* path) {
    FILE* f = fopen(path, "r");
    char b[32] = "";
    if (!f) return 0xFFFFFFFFu;
    if (!fgets(b, sizeof b, f)) b[0] = 0;
    fclose(f);
    return b[0] ? (unsigned)strtoul(b, NULL, 16) : 0xFFFFFFFFu;
}

static void glx_qr_fill(void* ed) {
    static const int pa[] = { 0x3024, 8, 0x3023, 8, 0x3022, 8, 0x3033, 1, 0x3040, 0x5, 0x3038 };
    void* cfg = NULL; int n = 0;
    if (!(eglChooseConfig(ed, pa, &cfg, 1, &n) && n)) cfg = glx_default_cfg(ed);
    if (!cfg) return;

    char ve[128], re[128], gv[160], tv[128], tr[128], g1[160];
    glx_qr_t q; memset(&q, 0, sizeof q);

    /* Save thread state so we do not disturb the current context */
    __typeof__(T_glx) sv = T_glx;
    if (sv.is_pix) glx_pixmap_sync_current();

    /* ES2/ES3 (the shim upgrades ES2->ES3 in EGL); the first one is the main renderer */
    int has2 = glx_qr_probe(ed, cfg, 3, ve, re, gv, sizeof ve) ||
               glx_qr_probe(ed, cfg, 2, ve, re, gv, sizeof ve);
    int has1 = glx_qr_probe(ed, cfg, 1, tv, tr, g1, sizeof tv);

    /* Restore the EGL context AND the GLX/emulation state (the probes' eglMakeCurrent
       calls reset T_glx through the EGL shim). Must happen before any return. */
    if (sv.ctx) {
        eglMakeCurrent(ed, glx_surface_find(sv.draw), glx_surface_find(sv.read), sv.ctx->ectx);
    }
    T_glx = sv;

    if (!has2 && !has1) return;
    if (!has2) { snprintf(ve, sizeof ve, "%s", tv); snprintf(re, sizeof re, "%s", tr); snprintf(gv, sizeof gv, "%s", g1); }

    snprintf(q.vendor, sizeof q.vendor, "%s", ve);
    snprintf(q.renderer, sizeof q.renderer, "%s", re);
    if (has2) glx_qr_parse_es(gv, &q.es2[0], &q.es2[1], q.ver);
    if (has1) glx_qr_parse_es(g1, &q.es1[0], &q.es1[1], has2 ? NULL : q.ver);

    /* accelerated: not a software rasterizer */
    static const char* sw[] = { "llvmpipe", "softpipe", "swrast", "SwiftShader", "Software" };
    q.accel = 1;
    for (unsigned i = 0; i < sizeof sw / sizeof *sw; i++)
        if (strstr(re, sw[i])) q.accel = 0;

    /* memory: SoC => unified, size = system RAM */
    q.uma = 1;
    { unsigned long total; um_read(&total, NULL); q.vram_mb = (unsigned)(total / 1024); }

    /* PCI IDs: sysfs DRM if available, otherwise 0xFFFFFFFF (spec, issue 17) */
    q.id[0] = q.id[1] = 0xFFFFFFFFu;
    for (int i = 0; i < 4 && q.id[0] == 0xFFFFFFFFu; i++) {
        char p[96];
        snprintf(p, sizeof p, "/sys/class/drm/card%d/device/vendor", i);
        q.id[0] = glx_qr_hex(p);
        snprintf(p, sizeof p, "/sys/class/drm/card%d/device/device", i);
        q.id[1] = glx_qr_hex(p);
    }
    if (q.id[0] == 0xFFFFFFFFu) {
        static const struct { const char* n; unsigned id; } vt[] = {
            { "ARM", 0x13B5 }, { "Qualcomm", 0x5143 }, { "Imagination", 0x1010 },
            { "NVIDIA", 0x10DE }, { "Intel", 0x8086 }, { "AMD", 0x1002 }, { "Broadcom", 0x14E4 } };
        for (unsigned i = 0; i < sizeof vt / sizeof *vt; i++)
            if (strstr(ve, vt[i].n)) { q.id[0] = vt[i].id; break; }
    }

    /* Versions presented by the shim (single source in gl_es_emul.c) */
    if (has2 && q.es2[0] >= 3) { int g = gl_emu_max_core(); q.core[0] = g / 10; q.core[1] = g % 10; }
    if (has2 && q.es2[0] >= 3)      { q.compat[0] = GL_EMU_COMPAT_ES3 / 10; q.compat[1] = GL_EMU_COMPAT_ES3 % 10; }
    else if (has2)                  { q.compat[0] = GL_EMU_COMPAT_ES2 / 10; q.compat[1] = GL_EMU_COMPAT_ES2 % 10; }
    else if (has1)                  { q.compat[0] = GL_EMU_COMPAT_ES1 / 10; q.compat[1] = GL_EMU_COMPAT_ES1 % 10; }

    /* Preferred profile from what the app imports (glx_autoes.h) */
    int ae = ae_detect();
    if (ae_is_es() && q.es2[0])      q.pref = 0x4;   /* ES-only symbols -> ES */
    else if (ae == 1 && q.compat[0]) q.pref = 0x2;   /* fixed-function only -> compat */
    else if (q.core[0])              q.pref = 0x1;   /* shaders / unknown -> core */
    else                             q.pref = q.compat[0] ? 0x2 : 0x4;

    q.ok = 1;
    glx_qr = q;
}


static const glx_qr_t* glx_qr_get(void* dpy) {
    static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
    void* ed = glx_display(dpy);
    if (!ed) return NULL;
    pthread_mutex_lock(&mu);
    if (!glx_qr.ok) glx_qr_fill(ed);
    pthread_mutex_unlock(&mu);
    return glx_qr.ok ? &glx_qr : NULL;
}

static int glx_qr_int(const glx_qr_t* q, int attr, unsigned* v) {
    switch (attr) {
    case 0x8183: v[0] = q->id[0]; return 1;
    case 0x8184: v[0] = q->id[1]; return 1;
    case 0x8185: v[0] = q->ver[0]; v[1] = q->ver[1]; v[2] = q->ver[2]; return 1;
    case 0x8186: v[0] = q->accel; return 1;
    case 0x8187: v[0] = q->vram_mb; return 1;
    case 0x8188: v[0] = q->uma; return 1;
    case 0x8189: v[0] = q->pref; return 1;
    case 0x818A: v[0] = q->core[0];   v[1] = q->core[1];   return 1;
    case 0x818B: v[0] = q->compat[0]; v[1] = q->compat[1]; return 1;
    case 0x818C: v[0] = q->es1[0];    v[1] = q->es1[1];    return 1;
    case 0x818D: v[0] = q->es2[0];    v[1] = q->es2[1];    return 1;
    default: return 0;
    }
}

GLX_EXPORT int glXQueryRendererIntegerMESA(void* dpy, int screen, int renderer, int attr, unsigned* v) {
    (void)screen;
    const glx_qr_t* q;
    if (renderer != 0 || !v || !(q = glx_qr_get(dpy))) return 0;
    return glx_qr_int(q, attr, v);
}
GLX_EXPORT int glXQueryCurrentRendererIntegerMESA(int attr, unsigned* v) {
    const glx_qr_t* q;
    if (!T_glx.ctx || !v || !(q = glx_qr_get(T_glx.dpy))) return 0;
    return glx_qr_int(q, attr, v);
}
GLX_EXPORT const char* glXQueryRendererStringMESA(void* dpy, int screen, int renderer, int attr) {
    (void)screen;
    const glx_qr_t* q;
    if (renderer != 0 || !(q = glx_qr_get(dpy))) return NULL;
    return attr == 0x8183 ? q->vendor : (attr == 0x8184 ? q->renderer : NULL);
}
GLX_EXPORT const char* glXQueryCurrentRendererStringMESA(int attr) {
    const glx_qr_t* q;
    if (!T_glx.ctx || !(q = glx_qr_get(T_glx.dpy))) return NULL;
    return attr == 0x8183 ? q->vendor : (attr == 0x8184 ? q->renderer : NULL);
}

GLX_EXPORT int glx_shim_es_version(int major, int profile, int have) {
    return glx_es_version(major, profile, have);
}

GLX_EXPORT void* glx_shim_egl_ctx(int ver, int major, int minor, int profile, int flags) {
    glx_ctx* c = calloc(1, sizeof *c);
    if (!c) return NULL;
    c->ver = ver;
    c->es_native = 0;
    c->from_egl = 1;
    c->want4 = major >= 4;
    c->want_ver = major > 0 ? major * 10 + minor : 0;
    c->profile_mask = (profile & 1) ? 1 : (profile & 2) ? 2 : 0;
    c->flags = flags;
    return c;
}
GLX_EXPORT void glx_shim_egl_current(void* c) {
    if (c) T_glx.ctx = (glx_ctx*)c;
    else if (T_glx.ctx && T_glx.ctx->from_egl) T_glx.ctx = NULL;   /* nu atinge contextele GLX */
}
GLX_EXPORT void glx_shim_egl_free(void* c) {
    if (T_glx.ctx == c) T_glx.ctx = NULL;
    free(c);
}

/* Missing symbols added by gl_missing.sh --stub in gl_stubs.h: app launches, call does nothing (with warning).
   If you implement a function, delete its line from gl_stubs.h. */
#if __has_include("gl_stubs.h")
static void gl_stub_warn(const char* n) { 
    if (glx_debug_enabled()) {
        fprintf(stderr, "[gl_stub] %s is not implemented (ignored)\n", n); 
    }
}
#define STUB(n) GLX_EXPORT long n(void) { static int w_; if (!w_) { w_ = 1; gl_stub_warn(#n); } return 0; }
#include "gl_stubs.h"
#undef STUB
#endif
