/*
 * gl_drawlog.c - GL state log on each draw (unique, deduplicated signatures)
 *                + WR_FEATURE_* features of each program.
 *
 * Usage:
 *   GLX_DRAWLOG=150  ~/ff.sh 2>&1 | grep '^\[draw\]' > ~/draw.txt     (first 150 distinct signatures)
 *   GLX_DRAWLOG=text ~/ff.sh 2>&1 | grep '^\[draw\]' > ~/draw.txt     (text programs only)
 *
 * Integration (4 lines), all in the same TU (glx_shim.c):
 *   1) in gl_es_emul.c, BEFORE  #include "gl_dsb.c" :
 *          static void dl_shader(unsigned, const char*);
 *          static void dl_attach(unsigned, unsigned);
 *          static void dl_install(void);
 *   2) in gl_es_emul.c, inside glShaderSource, after computing "t" (before sh_store):
 *          dl_shader(sh, j);
 *   3) in gl_dsb.c, first line of dsb_on_attach():
 *          dl_attach(prog, sh);
 *   4) next to ad_install(), in glUseProgram:
 *          dl_install();
 *   Then in glx_shim.c:  #include "gl_drawlog.c"   (after gl_atlasdump.c)
 *
 * If p_glDrawElementsInstanced / p_glDrawArraysInstanced do not exist in the list, remove the respective line from dl_install.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <dlfcn.h>

#define DL_MAX 4096
#define DL_SIG 512
static void ad_scan(int tex);
typedef void (*DL_gi)(unsigned, int*);
typedef void (*DL_at)(unsigned);
typedef void (*DL_gtp)(unsigned, unsigned, int*);
typedef void (*DL_gtlp)(unsigned, int, unsigned, int*);
typedef unsigned (*DL_cfs)(unsigned);
typedef unsigned (*DL_ge)(void);
typedef unsigned char (*DL_ise)(unsigned);
typedef void (*DL_da)(unsigned, int, int);
typedef void (*DL_de)(unsigned, int, unsigned, const void*);
typedef void (*DL_dai)(unsigned, int, int, int);
typedef void (*DL_dei)(unsigned, int, unsigned, const void*, int);

static char dl_sf[DL_MAX][96];          /* per-shader features */
static unsigned char dl_sk[DL_MAX];     /* 1=TEXT 2=DUAL_SOURCE 4=ALPHA_PASS 8=VERTEX */
static char dl_pf[DL_MAX][192];         /* per-program features */
static unsigned char dl_pk[DL_MAX];
static int dl_on = -1, dl_textonly = 0, dl_fb0 = 0, dl_swapm = 0, dl_limit = 150;

static void dl_cfg(void) {
    if (dl_on >= 0) return;
    const char* e = getenv("GLX_DRAWLOG");
    if (!e || !*e) { dl_on = 0; return; }
    dl_on = 1;
    if (!strcmp(e, "text")) dl_textonly = 1;
    else if (!strcmp(e, "swap")) dl_swapm = 1;   /* last draws before each swap (final composition, regardless of fbo) */
    else if (!strcmp(e, "fb0")) dl_fb0 = 1;      /* draws on the default framebuffer only (final composition) */
    else { int n = atoi(e); if (n > 0) dl_limit = n; }
}

static void dl_shader(unsigned sh, const char* src) {
    dl_cfg();
    if (dl_on <= 0 || sh >= DL_MAX || !src) return;
    dl_sf[sh][0] = 0; dl_sk[sh] = 0;
    if (strstr(src, "#define WR_VERTEX_SHADER")) dl_sk[sh] |= 8;
    if (strstr(src, "// shader: ps_text_run")) dl_sk[sh] |= 1;       /* REAL text (not just COLOR_MODE_*, which also appears in brush_*) */
    size_t n = 0;
    {   /* shader name from the "// shader: NAME FEATURES" comment */
        const char* nm = strstr(src, "// shader: ");
        if (nm) {
            nm += 11;
            size_t l = 0;
            while (nm[l] && nm[l] != ' ' && nm[l] != '\n' && nm[l] != '\r' && l < 30) l++;
            if (l + 6 < sizeof dl_sf[0]) {
                memcpy(dl_sf[sh], "NAME=", 5); memcpy(dl_sf[sh] + 5, nm, l); n = 5 + l; dl_sf[sh][n] = 0;
            }
        }
    }
    const char* p = src;
    while ((p = strstr(p, "#define WR_FEATURE_"))) {
        p += 8 + 11;                                    /* "#define " + "WR_FEATURE_" */
        size_t l = 0;
        while (p[l] && p[l] != ' ' && p[l] != '\n' && p[l] != '\r' && p[l] != '\t') l++;
        if (l == 20 && !memcmp(p, "DUAL_SOURCE_BLENDING", 20)) dl_sk[sh] |= 2;
        if (l == 10 && !memcmp(p, "ALPHA_PASS", 10)) dl_sk[sh] |= 4;
        if (n + l + 2 < sizeof dl_sf[0]) {
            if (n) dl_sf[sh][n++] = ' ';
            memcpy(dl_sf[sh] + n, p, l); n += l; dl_sf[sh][n] = 0;
        }
        p += l;
    }
}

static void dl_attach(unsigned prog, unsigned sh) {
    if (dl_on <= 0 || prog >= DL_MAX || sh >= DL_MAX) return;
    dl_pk[prog] |= dl_sk[sh] & 7;
    if (dl_sf[sh][0] && !strstr(dl_pf[prog], dl_sf[sh])) {
        size_t n = strlen(dl_pf[prog]), l = strlen(dl_sf[sh]);
        if (n + l + 2 < sizeof dl_pf[0]) {
            if (n) dl_pf[prog][n++] = ' ';
            memcpy(dl_pf[prog] + n, dl_sf[sh], l + 1);
        }
    }
}

static struct { uint64_t h; unsigned long n; } dl_sig[DL_SIG];
static unsigned dl_nsig;
static unsigned long dl_draws;
static int dl_ae;
static char dl_ring[4][1600];
static unsigned dl_rn;

static uint64_t dl_hash(const char* s, size_t n) {
    uint64_t h = 1469598103934665603ULL;
    for (size_t i = 0; i < n; i++) { h ^= (unsigned char)s[i]; h *= 1099511628211ULL; }
    return h;
}

static void dl_summary(void) {
    fprintf(stderr, "[draw] ---- summary: %lu draws, %u distinct signatures ----\n", dl_draws, dl_nsig);
    for (unsigned i = 0; i < dl_nsig; i++) fprintf(stderr, "[draw] #%u x%lu\n", i, dl_sig[i].n);
}

#define DL_ADD(...) do { if (n < sizeof L) n += (size_t)snprintf(L + n, sizeof L - n, __VA_ARGS__); \
                         if (n >= sizeof L) n = sizeof L - 1; } while (0)

static void dl_draw(const char* kind, unsigned mode, int count, int inst) {
    (void)count; (void)inst;
    if (dl_on <= 0) return;
    dl_draws++;
    if (!dl_ae) { dl_ae = 1; atexit(dl_summary); }
    if (!(dl_draws % 20000)) dl_summary();
    if (!dl_swapm && dl_nsig >= (unsigned)dl_limit) return;

    int prog = 0;
    RP(glGetIntegerv, DL_gi)(0x8B8D, &prog);                          /* CURRENT_PROGRAM */
    int known = prog > 0 && prog < DL_MAX;
    if (dl_textonly && !(known && (dl_pk[prog] & 1))) return;

    if (dl_fb0) {
        int fb = 0;
        RP(glGetIntegerv, DL_gi)(0x8CA6, &fb);
        if (fb) return;
    }
    for (int k = 0; k < 8 && RP(glGetError, DL_ge)(); k++) {}

    char L[1600]; size_t n = 0;
    unsigned char pk = known ? dl_pk[prog] : 0;
    DL_ADD("%s mode=0x%x prog=%d [%s%s%s] feat={%s}", kind, mode, prog,
           (pk & 1) ? "TEXT " : "", (pk & 2) ? "DSB " : "", (pk & 4) ? "ALPHA" : "",
           known ? dl_pf[prog] : "?");

    int rb = RP(glIsEnabled, DL_ise)(0x0BE2);
    DL_ADD(" blend=%d f=(%x,%x,%x,%x) eq=(%x,%x) real_blend=%d",
           dsb_st.en, dsb_st.f[0], dsb_st.f[1], dsb_st.f[2], dsb_st.f[3], dsb_st.eq[0], dsb_st.eq[1], rb);

    int fbo = 0, vp[4] = {0, 0, 0, 0};
    RP(glGetIntegerv, DL_gi)(0x8CA6, &fbo);                           /* DRAW_FRAMEBUFFER_BINDING */
    RP(glGetIntegerv, DL_gi)(0x0BA2, vp);                             /* VIEWPORT */
    DL_ADD(" fbo=%d", fbo);
    if (fbo) DL_ADD("(%s)", RP(glCheckFramebufferStatus, DL_cfs)(0x8CA9) == 0x8CD5 ? "ok" : "INCOMPLETE");
    DL_ADD(" vp=%dx%d@%d,%d", vp[2], vp[3], vp[0], vp[1]);

    int act = 0x84C0;
    RP(glGetIntegerv, DL_gi)(0x84E0, &act);                           /* ACTIVE_TEXTURE */
    for (int u = 0; u < 8; u++) {
        RP(glActiveTexture, DL_at)(0x84C0 + (unsigned)u);
        int tex = 0, smp = 0;
        RP(glGetIntegerv, DL_gi)(0x8069, &tex);                       /* TEXTURE_BINDING_2D */
        if (!tex) continue;
        RP(glGetIntegerv, DL_gi)(0x8919, &smp);                       /* SAMPLER_BINDING */
        int w = 0, h = 0, fm = 0, mn = 0, mg = 0, ws = 0, wt = 0;
        RP(glGetTexLevelParameteriv, DL_gtlp)(0x0DE1, 0, 0x1000, &w);
        RP(glGetTexLevelParameteriv, DL_gtlp)(0x0DE1, 0, 0x1001, &h);
        RP(glGetTexLevelParameteriv, DL_gtlp)(0x0DE1, 0, 0x1003, &fm);
        RP(glGetTexParameteriv, DL_gtp)(0x0DE1, 0x2801, &mn);
        RP(glGetTexParameteriv, DL_gtp)(0x0DE1, 0x2800, &mg);
        RP(glGetTexParameteriv, DL_gtp)(0x0DE1, 0x2802, &ws);
        RP(glGetTexParameteriv, DL_gtp)(0x0DE1, 0x2803, &wt);
        DL_ADD(" | u%d tex=%d %dx%d fmt=%x min=%x mag=%x wrap=%x,%x%s%s%s", u, tex, w, h, fm, mn, mg, ws, wt,
               smp ? " SAMPLER!" : "", !w ? " EMPTY!" : "", (mn >= 0x2700 && mn <= 0x2703) ? " MIPMAP-MIN!" : "");
    }
    RP(glActiveTexture, DL_at)((unsigned)act);
    for (int k = 0; k < 8 && RP(glGetError, DL_ge)(); k++) {}         /* do not leak our errors to the application */

    if (dl_swapm) {                                   /* keep only the last 4 draws */
        memcpy(dl_ring[dl_rn % 4], L, n + 1);
        dl_rn++;
        return;
    }
    uint64_t h = dl_hash(L, n);
    for (unsigned i = 0; i < dl_nsig; i++)
        if (dl_sig[i].h == h) { dl_sig[i].n++; return; }
    if (dl_nsig < DL_SIG) {
        dl_sig[dl_nsig].h = h; dl_sig[dl_nsig].n = 1;
        fprintf(stderr, "[draw] #%u %s\n", dl_nsig, L);
        dl_nsig++;
    }
}

/* GLX_SWAPDUMP=x,y,w,h : at swaps 6/20/60 prints ASCII of the region (x,y from top-left, max 160x60)
 * from the window framebuffer, right BEFORE presentation. Shows how text looks in GL, without a viewer. */
typedef void (*DL_rp)(int, int, int, int, unsigned, unsigned, void*);
typedef void (*DL_ps)(unsigned, int);
typedef void (*DL_bb)(unsigned, unsigned);

/* glBindFramebuffer is not in gles_list.h => resolve it manually (independent of gl_atlasdump.c) */
static DL_bb dl_bindfb(void) {
    static DL_bb f;
    if (!f) {
        f = (DL_bb)dlsym(RTLD_DEFAULT, "glBindFramebuffer");
        if (!f) { void* h = dlopen("libGLESv2.so", RTLD_NOW); if (h) f = (DL_bb)dlsym(h, "glBindFramebuffer"); }
    }
    return f;
}

static void dl_swapdump(void) {
    static int cfg = -1, x, y, w, h, dl_ch = -1;
    static unsigned ns, done;
    if (cfg < 0) {
        const char* e = getenv("GLX_SWAPDUMP");
        cfg = (e && sscanf(e, "%d,%d,%d,%d", &x, &y, &w, &h) == 4 && w > 0 && h > 0 && w <= 160 && h <= 60) ? 1 : 0;
        {   /* GLX_SWAPDUMP_CH=r|g|b : print a single channel (defaults to RGB average) */
            const char* c = getenv("GLX_SWAPDUMP_CH");
            dl_ch = (c && (c[0] == 'r' || c[0] == 'R')) ? 0 : (c && (c[0] == 'g' || c[0] == 'G')) ? 1 : (c && (c[0] == 'b' || c[0] == 'B')) ? 2 : -1;
        }
        if (e) fprintf(stderr, "[swapdump] %s (GLX_SWAPDUMP=%s)\n",
                       cfg ? "active; dumping at swaps 6, 20 and 60" : "INVALID FORMAT, use x,y,w,h (w<=160, h<=60)", e);
    }
    if (!cfg || done >= 3) return;
    ++ns;
    if (ns != 6 && ns != 20 && ns != 60) return;
    DL_bb bindfb = dl_bindfb();
    if (!bindfb) return;

    int fb = 0, rfb = 0, pbo = 0, pack = 4, vp[4] = {0, 0, 0, 0};
    RP(glGetIntegerv, DL_gi)(0x8CA6, &fb);
    RP(glGetIntegerv, DL_gi)(0x8CAA, &rfb);
    RP(glGetIntegerv, DL_gi)(0x88ED, &pbo);
    RP(glGetIntegerv, DL_gi)(0x0D05, &pack);
    RP(glGetIntegerv, DL_gi)(0x0BA2, vp);
    int gy = vp[3] - y - h;
    if (gy < 0) gy = 0;
    unsigned char* px = (unsigned char*)malloc((size_t)w * h * 4);
    if (!px) return;

    bindfb(0x8CA8, (unsigned)fb);
    RP(glBindBuffer, DL_bb)(0x88EB, 0);
    RP(glPixelStorei, DL_ps)(0x0D05, 1);
    RP(glReadPixels, DL_rp)(x, gy, w, h, 0x1908, 0x1401, px);

    int amin = 255;
    for (int i = 0; i < w * h; i++) if (px[i * 4 + 3] < amin) amin = px[i * 4 + 3];
    static const char ramp[] = " .:-=+*#%@";
    fprintf(stderr, "[swapdump] swap #%u fbo=%d vp=%dx%d region x=%d y=%d %dx%d alpha_min=%d\n",
            ns, fb, vp[2], vp[3], x, y, w, h, amin);
    for (int j = 0; j < h; j++) {
        char line[512]; int k = 0;
        const unsigned char* r = px + (size_t)(h - 1 - j) * w * 4;     /* GL: row 0 is at the bottom */
        for (int i = 0; i < w && k < 500; i++) {
            int v = dl_ch < 0 ? (r[i * 4] + r[i * 4 + 1] + r[i * 4 + 2]) / 3 : r[i * 4 + dl_ch];
            line[k++] = ramp[v * 9 / 255];
        }
        line[k] = 0;
        fprintf(stderr, "[swapdump] %s\n", line);
    }
    free(px);
    RP(glBindBuffer, DL_bb)(0x88EB, (unsigned)pbo);
    RP(glPixelStorei, DL_ps)(0x0D05, pack);
    bindfb(0x8CA8, (unsigned)rfb);
    done++;
}

/* Call at the beginning of glXSwapBuffers / eglSwapBuffers (GLX_DRAWLOG=swap, GLX_SWAPDUMP=...) */
static void dl_swap(void) {
  { static unsigned k; const char* t = getenv("GLX_ATLAS_TEX");
  if (t && ++k == 40) ad_scan(atoi(t)); }
    static unsigned ns;
    dl_swapdump();
    if (dl_on <= 0 || !dl_swapm) return;
    ns++;
    if (ns <= 12 || !(ns % 120)) {
        unsigned cnt = dl_rn < 4 ? dl_rn : 4;
        fprintf(stderr, "[draw] SWAP #%u: last %u draws before swap (out of %u)\n", ns, cnt, dl_rn);
        for (unsigned i = 0; i < cnt; i++)
            fprintf(stderr, "[draw]   %s\n", dl_ring[(dl_rn - cnt + i) % 4]);
    }
    dl_rn = 0;
}

static void dl_err(const char* kind) {
    unsigned e = RP(glGetError, DL_ge)();
    static int c;
    if (e && c++ < 40) {
        int prog = 0;
        RP(glGetIntegerv, DL_gi)(0x8B8D, &prog);
        fprintf(stderr, "[draw] GL ERROR 0x%x after %s (prog=%d%s%s)\n", e, kind, prog,
                (prog > 0 && prog < DL_MAX && dl_pf[prog][0]) ? " feat=" : "",
                (prog > 0 && prog < DL_MAX) ? dl_pf[prog] : "");
    }
}

static DL_da  dl_r_da;
static DL_de  dl_r_de;
static DL_dai dl_r_dai;
static DL_dei dl_r_dei;

static void dl_h_da(unsigned m, int f, int c)                      { dl_draw("Arrays", m, c, 1);  dl_r_da(m, f, c);        dl_err("glDrawArrays"); }
static void dl_h_de(unsigned m, int c, unsigned t, const void* i)  { dl_draw("Elements", m, c, 1); dl_r_de(m, c, t, i);     dl_err("glDrawElements"); }
static void dl_h_dai(unsigned m, int f, int c, int n)              { dl_draw("ArraysInst", m, c, n); dl_r_dai(m, f, c, n);  dl_err("glDrawArraysInstanced"); }
static void dl_h_dei(unsigned m, int c, unsigned t, const void* i, int n) { dl_draw("ElementsInst", m, c, n); dl_r_dei(m, c, t, i, n); dl_err("glDrawElementsInstanced"); }

static void dl_install(void) {
    static int tried;
    if (tried) return;
    tried = 1;
    dl_cfg();
    if (dl_on <= 0) return;
    dl_r_da  = (DL_da)p_glDrawArrays;                 if (dl_r_da)  p_glDrawArrays = (__typeof__(p_glDrawArrays))dl_h_da;
    dl_r_de  = (DL_de)p_glDrawElements;               if (dl_r_de)  p_glDrawElements = (__typeof__(p_glDrawElements))dl_h_de;
    dl_r_dai = (DL_dai)p_glDrawArraysInstanced;       if (dl_r_dai) p_glDrawArraysInstanced = (__typeof__(p_glDrawArraysInstanced))dl_h_dai;
    dl_r_dei = (DL_dei)p_glDrawElementsInstanced;     if (dl_r_dei) p_glDrawElementsInstanced = (__typeof__(p_glDrawElementsInstanced))dl_h_dei;
    fprintf(stderr, "[draw] log active (limit %d signatures%s)\n", dl_limit, dl_textonly ? ", text only" : (dl_fb0 ? ", fbo=0 only" : ""));
}
