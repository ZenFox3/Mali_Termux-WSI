/*
 * gl_litfix.c - included by gl_es_emul.c (after glsl_to_es, before glShaderSource).
 *
 * Desktop GLSL implicitly converts int -> float ("float a = 1;", "max(x, 0)", "a * 2"); GLSL ES does not.
 * Mali reports the error with the line number ("0:3: S0001: Type mismatch, cannot convert from 'int' to 'float'").
 * On glCompileShader failure for a shader translated by us:
 *   1. read error lines from the log;
 *   2. promote integer literals on THOSE lines to float (1 -> 1.0), excluding layout(...), [indices], 1u, hex, directives;
 *   3. promote integer macros (e.g. #define FOO 10 -> float(FOO)) on error lines;
 *   4. recompile; repeat up to 8 times (new errors may appear on other lines);
 *   5. if it still fails, restore the initial translated source: the application's view of the log remains original.
 * A shader that already compiles is never modified. GLX_NO_LITFIX=1 disables literal fixing.
 */

typedef struct { char* p; size_t n, cap; } gl_sb;

static void sb_put(gl_sb* b, const char* s, size_t n) {
    if (b->n + n + 1 > b->cap) { b->cap = (b->n + n + 1) * 2; b->p = realloc(b->p, b->cap); }
    memcpy(b->p + b->n, s, n);
    b->n += n;
    b->p[b->n] = 0;
}

static int lf_digit(char c) { return c >= '0' && c <= '9'; }
static int lf_alpha(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
static int lf_idch(char c)  { return lf_alpha(c) || lf_digit(c); }

/* "#define NAME <integer>" in translated source; on error lines NAME -> float(NAME) */
static __thread char lf_mac[32][48];
static __thread int  lf_nmac;

static void lf_scan_macros(const char* t) {
    lf_nmac = 0;
    for (const char* p = t; *p && lf_nmac < 32; ) {
        const char* e = strchr(p, '\n');
        const char* q = p;
        while (*q == ' ' || *q == '\t') q++;
        if (!strncmp(q, "#define", 7) && (q[7] == ' ' || q[7] == '\t')) {
            q += 7;
            while (*q == ' ' || *q == '\t') q++;
            const char* nm = q;
            while (lf_idch(*q)) q++;
            size_t nl = (size_t)(q - nm);
            if (nl && nl < 48 && (*q == ' ' || *q == '\t')) {          /* object-like, not NAME( */
                while (*q == ' ' || *q == '\t') q++;
                const char* d = q;
                while (lf_digit(*q)) q++;
                if (q > d) {
                    while (*q == ' ' || *q == '\t' || *q == '\r') q++;
                    if (*q == 0 || *q == '\n' || (q[0] == '/' && (q[1] == '/' || q[1] == '*'))) {
                        memcpy(lf_mac[lf_nmac], nm, nl);
                        lf_mac[lf_nmac++][nl] = 0;
                    }
                }
            }
        }
        if (!e) break;
        p = e + 1;
    }
}

static int lf_is_intmacro(const char* s, size_t n) {
    for (int i = 0; i < lf_nmac; i++)
        if (strlen(lf_mac[i]) == n && !memcmp(lf_mac[i], s, n)) return 1;
    return 0;
}

/* copies line s[0..n) into b, promoting integer literals and macros to float. Returns 1 if anything changed. */
static int gl_promote_line(gl_sb* b, const char* s, size_t n) {
    size_t i = 0;
    int changed = 0, br = 0;
    while (i < n && (s[i] == ' ' || s[i] == '\t')) i++;
    if (i < n && s[i] == '#') { sb_put(b, s, n); return 0; }                 /* preprocessor directive */
    i = 0;
    while (i < n) {
        char c = s[i];
        if (c == '/' && i + 1 < n && s[i + 1] == '/') { sb_put(b, s + i, n - i); break; }   /* comment */
        if (lf_alpha(c)) {                                                   /* identifier / keyword */
            size_t j = i;
            while (j < n && lf_idch(s[j])) j++;
            int is_layout = (j - i == 6 && !memcmp(s + i, "layout", 6));
            if (!is_layout && br == 0 && lf_is_intmacro(s + i, j - i)) {
                sb_put(b, "float(", 6); sb_put(b, s + i, j - i); sb_put(b, ")", 1);
                changed = 1; i = j; continue;
            }
            sb_put(b, s + i, j - i);
            i = j;
            if (is_layout) {                                                 /* layout(...): values must remain int */
                size_t k = i;
                while (k < n && (s[k] == ' ' || s[k] == '\t')) k++;
                if (k < n && s[k] == '(') {
                    int depth = 0;
                    size_t m = k;
                    for (; m < n; m++) {
                        if (s[m] == '(') depth++;
                        else if (s[m] == ')' && --depth == 0) { m++; break; }
                    }
                    sb_put(b, s + i, m - i);
                    i = m;
                }
            }
            continue;
        }
        if (c == '[') br++;
        else if (c == ']' && br > 0) br--;
        if (lf_digit(c) || (c == '.' && i + 1 < n && lf_digit(s[i + 1]))) {
            size_t j = i;
            int is_float = 0, is_hex = 0, is_u = 0;
            if (c == '0' && i + 1 < n && (s[i + 1] == 'x' || s[i + 1] == 'X')) {
                is_hex = 1; j = i + 2;
                while (j < n && lf_idch(s[j])) j++;
            } else {
                while (j < n && lf_digit(s[j])) j++;
                if (j < n && s[j] == '.') { is_float = 1; j++; while (j < n && lf_digit(s[j])) j++; }
                if (j < n && (s[j] == 'e' || s[j] == 'E')) {
                    is_float = 1; j++;
                    if (j < n && (s[j] == '+' || s[j] == '-')) j++;
                    while (j < n && lf_digit(s[j])) j++;
                }
                if (j < n && (s[j] == 'f' || s[j] == 'F')) { is_float = 1; j++; }
                else if (j < n && (s[j] == 'u' || s[j] == 'U')) { is_u = 1; j++; }
            }
            sb_put(b, s + i, j - i);
            if (!is_float && !is_hex && !is_u && br == 0) { sb_put(b, ".0", 2); changed = 1; }
            i = j;
            continue;
        }
        sb_put(b, &s[i], 1);
        i++;
    }
    return changed;
}

/* text = translated source (with "#line N" before original body); log = driver log.
 * Returns a new copy with literals/macros promoted on reported error lines, or NULL if nothing to change. */
static char* gl_promote_errors(const char* text, const char* log) {
    lf_scan_macros(text);
    int lines[32], nl = 0;
    for (const char* p = log; *p && nl < 32; p++) {
        if (*p == '0' && p[1] == ':' && lf_digit(p[2]) && (p == log || p[-1] == '\n' || p[-1] == ' ')) {
            int L = atoi(p + 2), dup = 0;
            for (int k = 0; k < nl; k++) if (lines[k] == L) dup = 1;
            if (L > 0 && !dup) lines[nl++] = L;
        }
    }
    if (!nl) return NULL;

    const char* d = strstr(text, "#line ");
    int ln = 1;
    const char* body = text;
    if (d) {
        ln = atoi(d + 6);
        const char* e = strchr(d, '\n');
        body = e ? e + 1 : d + strlen(d);
    }
    gl_sb b = { 0 };
    sb_put(&b, text, (size_t)(body - text));
    int changed = 0;
    for (const char* p = body; *p; ln++) {
        const char* e = strchr(p, '\n');
        size_t L = e ? (size_t)(e - p) : strlen(p);
        int hit = 0;
        for (int k = 0; k < nl; k++) if (lines[k] == ln) hit = 1;
        if (hit) changed |= gl_promote_line(&b, p, L);
        else sb_put(&b, p, L);
        if (e) sb_put(&b, "\n", 1);
        p = e ? e + 1 : p + L;
    }
    if (!changed) { free(b.p); return NULL; }
    return b.p;
}

/* ---- translated source kept per shader for recompilation ---- */
static char** sh_tr;
static unsigned sh_tr_cap;

static void sh_tr_store(unsigned id, char* t) {          /* takes ownership; NULL = deletes */
    if (id >= 1u << 20) { free(t); return; }
    pthread_mutex_lock(&sh_mu);
    if (id >= sh_tr_cap) {
        unsigned nc = sh_tr_cap ? sh_tr_cap : 64;
        while (nc <= id) nc *= 2;
        sh_tr = realloc(sh_tr, nc * sizeof *sh_tr);
        memset(sh_tr + sh_tr_cap, 0, (nc - sh_tr_cap) * sizeof *sh_tr);
        sh_tr_cap = nc;
    }
    free(sh_tr[id]);
    sh_tr[id] = t;
    pthread_mutex_unlock(&sh_mu);
}

static char* sh_tr_get_dup(unsigned id) {
    char* r = NULL;
    pthread_mutex_lock(&sh_mu);
    if (id < sh_tr_cap && sh_tr[id]) r = strdup(sh_tr[id]);
    pthread_mutex_unlock(&sh_mu);
    return r;
}

typedef void (*F_gsil)(unsigned, int, int*, char*);

/* NEW - 100% compatible with gles_list.h / GLES2/gl2.h */
#ifndef GLuint
typedef unsigned int GLuint;
#endif

GLC void glCompileShader(GLuint sh) {
    RP(glCompileShader, F_ui)(sh);
    char* t = sh_tr_get_dup(sh);
    if (!t) return;                                       /* not a shader translated by us */
    int ok = 0;
    RP(glGetShaderiv, F_gsiv)(sh, 0x8B81, &ok);          /* COMPILE_STATUS */
    if (ok || getenv("GLX_NO_LITFIX")) { free(t); return; }

    char* cur = t;
    int fixed = 0;
    for (int it = 0; it < 8 && !fixed; it++) {
        char log[4096];
        log[0] = 0;
        RP(glGetShaderInfoLog, F_gsil)(sh, (int)sizeof log, NULL, log);
        char* nw = gl_promote_errors(cur, log);
        if (!nw) break;
        if (cur != t) free(cur);
        cur = nw;
        const char* src = cur;
        RP(glShaderSource, F_shsrc)(sh, 1, &src, NULL);
        RP(glCompileShader, F_ui)(sh);
        RP(glGetShaderiv, F_gsiv)(sh, 0x8B81, &ok);
        if (ok) fixed = 1;
    }
    if (fixed) {
        fprintf(stderr, "[glsl] shader %u: promoted int->float literals/macros on error lines (GLSL ES does not convert implicitly)\n", sh);
        sh_tr_store(sh, cur);                             /* cur: taken by store */
        free(t);
    } else {
        if (cur != t) free(cur);
        const char* src = t;
        RP(glShaderSource, F_shsrc)(sh, 1, &src, NULL);   /* initial source: app log = original log */
        RP(glCompileShader, F_ui)(sh);
        free(t);
    }
}
