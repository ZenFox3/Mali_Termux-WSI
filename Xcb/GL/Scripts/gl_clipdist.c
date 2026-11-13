/*
 * gl_clipdist.c - gl_ClipDistance emulation (included by glx_shim.c AFTER gl_es_emul.c).
 *
 * Vertex shader: gl_ClipDistance[i] -> varying "vClipDistance_x[N]" (translation is in gl_es_emul.c: cd_apply).
 * On glLinkProgram: if the program has a vertex shader with clip distance, each attached fragment shader
 * is replaced (only within that program) with a CLONE that begins with:
 *     for (i < N) if (vClipDistance_x[i] < 0.0) discard;
 * The application's shader remains untouched (can be used by other programs as well). On relink / glDetachShader
 * clones are detached and originals reattached.
 * Limitations: clipping is always applied (ignores glEnable(GL_CLIP_DISTANCEn)), per-pixel not geometric,
 * no geometry/tessellation, no gl_PerVertex redeclaration, no reading inside the fragment shader.
 */

typedef unsigned (*F_cs)(unsigned);

#define CD_MAXSH 8
typedef struct {
    unsigned att[CD_MAXSH]; int n;                       /* shaders attached by the application */
    unsigned orig[CD_MAXSH], clone[CD_MAXSH]; int nc;    /* original -> clone pairs */
} cd_prog;

static cd_prog* cd_pr;
static unsigned cd_pr_cap;
static unsigned char* cd_sh;                             /* per shader: N clip distances (0 = no) */
static unsigned cd_sh_cap;

static void cd_note(unsigned sh, int n) {
    if (sh >= 1u << 20) return;
    pthread_mutex_lock(&sh_mu);
    if (sh >= cd_sh_cap) {
        if (!n) { pthread_mutex_unlock(&sh_mu); return; }
        unsigned nc = cd_sh_cap ? cd_sh_cap : 64;
        while (nc <= sh) nc *= 2;
        cd_sh = realloc(cd_sh, nc);
        memset(cd_sh + cd_sh_cap, 0, nc - cd_sh_cap);
        cd_sh_cap = nc;
    }
    cd_sh[sh] = (unsigned char)n;
    pthread_mutex_unlock(&sh_mu);
}

/* called with sh_mu locked */
static cd_prog* cd_prog_get(unsigned id) {
    if (id >= 1u << 20) return NULL;
    if (id >= cd_pr_cap) {
        unsigned nc = cd_pr_cap ? cd_pr_cap : 64;
        while (nc <= id) nc *= 2;
        cd_pr = realloc(cd_pr, nc * sizeof *cd_pr);
        memset(cd_pr + cd_pr_cap, 0, (nc - cd_pr_cap) * sizeof *cd_pr);
        cd_pr_cap = nc;
    }
    return &cd_pr[id];
}

/* detaches clones: detach clone, reattach original, delete clone */
static void cd_restore(unsigned prog) {
    unsigned o[CD_MAXSH], c[CD_MAXSH];
    int k = 0;
    pthread_mutex_lock(&sh_mu);
    if (prog < cd_pr_cap) {
        cd_prog* P = &cd_pr[prog];
        for (int i = 0; i < P->nc; i++) { o[k] = P->orig[i]; c[k] = P->clone[i]; k++; }
        P->nc = 0;
    }
    pthread_mutex_unlock(&sh_mu);
    for (int i = 0; i < k; i++) {
        RP(glDetachShader, F_uiui)(prog, c[i]);
        RP(glAttachShader, F_uiui)(prog, o[i]);
        RP(glDeleteShader, F_ui)(c[i]);
    }
}

GLC void glAttachShader(unsigned prog, unsigned sh) {
    pthread_mutex_lock(&sh_mu);
    cd_prog* P = cd_prog_get(prog);
    if (P) {
        int dup = 0;
        for (int i = 0; i < P->n; i++) if (P->att[i] == sh) dup = 1;
        if (!dup && P->n < CD_MAXSH) P->att[P->n++] = sh;
    }
    pthread_mutex_unlock(&sh_mu);
    RP(glAttachShader, F_uiui)(prog, sh);
    dsb_on_attach(prog, sh);
}

GLC void glDetachShader(unsigned prog, unsigned sh) {
    cd_restore(prog);
    pthread_mutex_lock(&sh_mu);
    if (prog < cd_pr_cap) {
        cd_prog* P = &cd_pr[prog];
        for (int i = 0; i < P->n; i++)
            if (P->att[i] == sh) { P->att[i] = P->att[--P->n]; break; }
    }
    pthread_mutex_unlock(&sh_mu);
    RP(glDetachShader, F_uiui)(prog, sh);
}

GLC void glLinkProgram(unsigned prog) {
    unsigned att[CD_MAXSH];
    int cdn[CD_MAXSH], na = 0, N = 0;

    cd_restore(prog);                                    /* relink: start from the application's shaders */
    pthread_mutex_lock(&sh_mu);
    if (prog < cd_pr_cap) {
        cd_prog* P = &cd_pr[prog];
        na = P->n;
        for (int i = 0; i < na; i++) {
            att[i] = P->att[i];
            cdn[i] = att[i] < cd_sh_cap ? cd_sh[att[i]] : 0;
        }
    }
    pthread_mutex_unlock(&sh_mu);

    for (int i = 0; i < na; i++)
        if (cdn[i]) {
            int ty = 0;
            RP(glGetShaderiv, F_gsiv)(att[i], 0x8B4F, &ty);          /* SHADER_TYPE */
            if (ty == 0x8B31 && cdn[i] > N) N = cdn[i];              /* VERTEX_SHADER */
        }

    if (N) {
        char pre[96], post[160];
        snprintf(pre, sizeof pre, "in float vClipDistance_x[%d];", N);
        snprintf(post, sizeof post, "for(int cdi=0;cdi<%d;cdi++)if(vClipDistance_x[cdi]<0.0)discard;", N);
        for (int i = 0; i < na; i++) {
            int ty = 0;
            RP(glGetShaderiv, F_gsiv)(att[i], 0x8B4F, &ty);
            if (ty != 0x8B30) continue;                              /* FRAGMENT_SHADER only */
            char* t = sh_tr_get_dup(att[i]);
            if (!t) continue;
            char* t2 = cd_inject(t, pre, post);                      /* frees t if modified */
            if (!strstr(t2, "vClipDistance_x")) { free(t2); continue; }
            unsigned ns = RP(glCreateShader, F_cs)(0x8B30);
            const char* src = t2;
            RP(glShaderSource, F_shsrc)(ns, 1, &src, NULL);
            RP(glCompileShader, F_ui)(ns);
            int ok = 0;
            RP(glGetShaderiv, F_gsiv)(ns, 0x8B81, &ok);
            free(t2);
            if (!ok) {
                if (gl_emul_debug_enabled()) fprintf(stderr, "[clipdist] fragment shader clone %u fails to compile: no clipping\n", att[i]);
                RP(glDeleteShader, F_ui)(ns);
                continue;
            }
            RP(glDetachShader, F_uiui)(prog, att[i]);
            RP(glAttachShader, F_uiui)(prog, ns);
            pthread_mutex_lock(&sh_mu);
            cd_prog* P = cd_prog_get(prog);
            if (P && P->nc < CD_MAXSH) { P->orig[P->nc] = att[i]; P->clone[P->nc] = ns; P->nc++; }
            pthread_mutex_unlock(&sh_mu);
        }
        if (gl_emul_debug_enabled()) fprintf(stderr, "[clipdist] program %u: %d clip distances emulated\n", prog, N);
    }
    RP(glLinkProgram, F_ui)(prog);
    cc_forget(prog);
    if (prog == cc_prog) cc_apply();
    dsb_on_link(prog);
}
