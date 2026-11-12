/*
 * gl_dsb.c - dual-source blending (ARB/EXT_blend_func_extended) emulated on Mali
 *            with GL_ARM_shader_framebuffer_fetch.
 *
 * Include in gl_es_emul.c AFTER cd_inject() and BEFORE glsl_to_es().
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------- 1) Shader Side */

static __thread int gl_last_dsb;     /* 1 = last glsl_to_es translation uses dual-source output */

/* blank [a,e) keeping newlines */
static void dsb_blank(char* b, size_t a, size_t e) { 
    for (size_t z = a; z < e; z++) if (b[z] != '\n') b[z] = ' '; 
}

/* Looks for "layout(...) out <type> <name>;".
 * Returns 1 if the shader is dual-source (n0 and n1 filled) and renames main -> xl_m. */
static int dsb_find(char* b, size_t n, char* n0, char* n1) {
    int has1 = 0;
    n0[0] = n1[0] = 0;
    for (size_t i = 0; i + 6 <= n; i++) {
        if (memcmp(b + i, "layout", 6) || (i && gl_idch(b[i - 1])) || (i + 6 < n && gl_idch(b[i + 6]))) continue;
        size_t j = i + 6;
        while (j < n && s1d_ws(b[j])) j++;
        if (j >= n || b[j] != '(') continue;
        size_t pe = j;
        while (pe < n && b[pe] != ')') pe++;
        if (pe >= n) break;

        int idxv = -1; size_t ik = 0, ie = 0;
        for (size_t k = j + 1; k + 5 <= pe; k++) {
            if (memcmp(b + k, "index", 5) || gl_idch(b[k - 1]) || gl_idch(b[k + 5])) continue;
            size_t m = k + 5;
            while (m < pe && s1d_ws(b[m])) m++;
            if (m < pe && b[m] == '=') {
                m++;
                while (m < pe && s1d_ws(b[m])) m++;
                if (m < pe && (b[m] == '0' || b[m] == '1') && !(m + 1 < pe && b[m + 1] >= '0' && b[m + 1] <= '9')) {
                    idxv = b[m] - '0'; ik = k; ie = m + 1;
                }
            }
        }
        if (idxv >= 0) {                                    /* strip "index = N" and one neighbouring comma */
            size_t a = ik, e = ie, pa = ik;
            while (pa > j + 1 && s1d_ws(b[pa - 1])) pa--;
            if (pa > j + 1 && b[pa - 1] == ',') a = pa - 1;
            else { size_t ne = ie; while (ne < pe && s1d_ws(b[ne])) ne++; if (ne < pe && b[ne] == ',') e = ne + 1; }
            dsb_blank(b, a, e);
        }

        size_t q = pe + 1;
        while (q < n && s1d_ws(b[q])) q++;
        if (q + 3 > n || memcmp(b + q, "out", 3) || (q + 3 < n && gl_idch(b[q + 3]))) continue;
        size_t t = q + 3;
        while (t < n && s1d_ws(b[t])) t++;
        while (t < n && gl_idch(b[t])) t++;                 /* type */
        while (t < n && s1d_ws(b[t])) t++;
        size_t ns = t;
        while (t < n && gl_idch(b[t])) t++;                 /* name */
        if (t == ns || t - ns >= 48) continue;

        if (idxv == 1) {
            memcpy(n1, b + ns, t - ns); n1[t - ns] = 0;
            has1 = 1;
            dsb_blank(b, i, q + 3);
        } else if (!n0[0]) {
            memcpy(n0, b + ns, t - ns); n0[t - ns] = 0;
        }
        i = t;
    }
    if (!has1 || !n0[0]) return 0;
    gl_rep_word(b, n, "main", "xl_m");                      /* same length */
    return 1;
}

static const char dsb_glsl[] =
    "uniform ivec4 xl_bfn;\n"          /* srcRGB, dstRGB, srcA, dstA */
    "uniform ivec3 xl_beq;\n"          /* eqRGB, eqA, blend enabled */
    "uniform vec4 xl_bcc;\n"           /* blend color */
    "vec4 xl_bf(int f,vec4 s0,vec4 s1,vec4 d){\n"
    "switch(f){\n"
    "case 0:return vec4(0.);\n"
    "case 1:return vec4(1.);\n"
    "case 0x300:return s0;\n"
    "case 0x301:return 1.-s0;\n"
    "case 0x302:return s0.aaaa;\n"
    "case 0x303:return 1.-s0.aaaa;\n"
    "case 0x304:return d.aaaa;\n"
    "case 0x305:return 1.-d.aaaa;\n"
    "case 0x306:return d;\n"
    "case 0x307:return 1.-d;\n"
    "case 0x308:return vec4(vec3(min(s0.a,1.-d.a)),1.);\n"
    "case 0x8001:return xl_bcc;\n"
    "case 0x8002:return 1.-xl_bcc;\n"
    "case 0x8003:return xl_bcc.aaaa;\n"
    "case 0x8004:return 1.-xl_bcc.aaaa;\n"
    "case 0x88F9:return s1;\n"
    "case 0x88FA:return 1.-s1;\n"
    "case 0x8589:return s1.aaaa;\n"
    "case 0x88FB:return 1.-s1.aaaa;\n"
    "}\n"
    "return vec4(1.);}\n"
    "vec4 xl_be(int e,vec4 s,vec4 d,vec4 sc,vec4 dc){\n"
    "if(e==0x800A)return s-d;\n"
    "if(e==0x800B)return d-s;\n"
    "if(e==0x8007)return min(sc,dc);\n"
    "if(e==0x8008)return max(sc,dc);\n"
    "return s+d;}\n"
    "vec4 xl_blend(vec4 s0,vec4 s1,vec4 d){\n"
    "if(xl_beq.z==0)return s0;\n"
    "vec4 a=xl_bf(xl_bfn.x,s0,s1,d),b=xl_bf(xl_bfn.y,s0,s1,d);\n"
    "vec4 c=xl_bf(xl_bfn.z,s0,s1,d),e=xl_bf(xl_bfn.w,s0,s1,d);\n"
    "vec4 r=xl_be(xl_beq.x,s0*a,d*b,s0,d);\n"
    "vec4 t=xl_be(xl_beq.y,s0*c,d*e,s0,d);\n"
    "return vec4(r.rgb,t.a);}\n";

/* Appended AFTER original body */
static size_t dsb_emit(char* o, const char* n0, const char* n1) {
    size_t n = (size_t)sprintf(o, "\n%s", dsb_glsl);
    const char* te = getenv("GLX_DSB_TEST");
    int tm = te ? atoi(te) : 0;
    if (tm == 1)
        n += (size_t)sprintf(o + n, "\nvoid main(){xl_m();%s=vec4(1.,0.,1.,1.);}\n", n0);
    else if (tm == 2)
        n += (size_t)sprintf(o + n, "\nvoid main(){xl_m();%s=vec4(%s.rgb,1.);}\n", n0, n1);
    else if (tm == 3)
        n += (size_t)sprintf(o + n, "\nvoid main(){xl_m();%s=vec4(float(xl_beq.z!=0),float(xl_bfn.x==0x88F9),float(xl_bfn.y==0x88FA),1.);}\n", n0);
    else if (tm == 4)
        n += (size_t)sprintf(o + n, "\nvoid main(){xl_m();%s=vec4(gl_LastFragColorARM.rgb,1.);}\n", n0);
    else if (tm == 5)
        n += (size_t)sprintf(o + n,
            "\nvoid main(){xl_m();vec4 o=xl_blend(%s,%s,gl_LastFragColorARM);"
            "o.a=max(o.a,max(o.r,max(o.g,o.b)));%s=o;}\n", n0, n1, n0);
    else
        n += (size_t)sprintf(o + n, "\nvoid main(){xl_m();%s=xl_blend(%s,%s,gl_LastFragColorARM);}\n", n0, n0, n1);
    return n;
}
/* ---------------------------------------------------------------- 2) State Side */

#define DSB_MAX (1u << 16)
static unsigned char dsb_shf[DSB_MAX];          /* shader uses dual-source */
static unsigned char dsb_prf[DSB_MAX];          /* program has dual-source shader */
static int  dsb_loc[DSB_MAX][3];
static unsigned char dsb_locok[DSB_MAX];
static unsigned dsb_cur;                        /* current program */
static int dsb_off;                             /* 1 = forced real GL_BLEND off */

static struct { int en; int f[4]; int eq[2]; float c[4]; } dsb_st =
    { 0, { 1, 0, 1, 0 }, { 0x8006, 0x8006 }, { 0, 0, 0, 0 } };

typedef void (*D_bf)(unsigned, unsigned);
typedef void (*D_bfs)(unsigned, unsigned, unsigned, unsigned);
typedef void (*D_be)(unsigned);
typedef void (*D_bes)(unsigned, unsigned);
typedef void (*D_bc)(float, float, float, float);
typedef void (*D_en)(unsigned);
typedef int  (*D_gul)(unsigned, const char*);
typedef void (*D_u4i)(int, int, int, int, int);
typedef void (*D_u3i)(int, int, int, int);
typedef void (*D_u4f)(int, float, float, float, float);
typedef void (*D_bfi)(unsigned, unsigned, unsigned);

static int dsb_is1(unsigned f) { return f == 0x88F9 || f == 0x88FA || f == 0x8589 || f == 0x88FB; }
static unsigned dsb_safe(unsigned f) { return dsb_is1(f) ? 1u : f; }

static void dsb_sync(void) {
    unsigned p = dsb_cur;
    if (p && p < DSB_MAX && dsb_prf[p]) {
        if (!dsb_locok[p]) {
            dsb_loc[p][0] = RP(glGetUniformLocation, D_gul)(p, "xl_bfn");
            dsb_loc[p][1] = RP(glGetUniformLocation, D_gul)(p, "xl_beq");
            dsb_loc[p][2] = RP(glGetUniformLocation, D_gul)(p, "xl_bcc");
            dsb_locok[p] = 1;
        }
        if (dsb_loc[p][0] >= 0) RP(glUniform4i, D_u4i)(dsb_loc[p][0], dsb_st.f[0], dsb_st.f[1], dsb_st.f[2], dsb_st.f[3]);
        if (dsb_loc[p][1] >= 0) RP(glUniform3i, D_u3i)(dsb_loc[p][1], dsb_st.eq[0], dsb_st.eq[1], dsb_st.en);
        if (dsb_loc[p][2] >= 0) RP(glUniform4f, D_u4f)(dsb_loc[p][2], dsb_st.c[0], dsb_st.c[1], dsb_st.c[2], dsb_st.c[3]);
        
        if (!dsb_off) { 
            RP(glDisable, D_en)(0x0BE2 /* GL_BLEND */); 
            dsb_off = 1; 
        }
    } else {
        if (dsb_off) {
            if (dsb_st.en) RP(glEnable, D_en)(0x0BE2);
            dsb_off = 0;
        }
    }
}

static void dsb_note(unsigned sh, int flag) { if (sh < DSB_MAX) dsb_shf[sh] = (unsigned char)flag; }

static void dsb_on_attach(unsigned prog, unsigned sh) {
    dl_attach(prog, sh);
    if (prog < DSB_MAX && sh < DSB_MAX && dsb_shf[sh]) {
        dsb_prf[prog] = 1;
        if (getenv("GLX_DSB_DEBUG")) fprintf(stderr, "[dsb] program %u uses dual-source (shader %u)\n", prog, sh);
    }
}

static void dsb_on_link(unsigned prog) {
    if (prog >= DSB_MAX) return;
    dsb_locok[prog] = 0;
    if (prog == dsb_cur) dsb_sync();
}

static void dsb_on_delete(unsigned prog) { if (prog < DSB_MAX) { dsb_prf[prog] = 0; dsb_locok[prog] = 0; } }
static void dsb_on_use(unsigned prog) { dsb_cur = prog; dsb_sync(); }

static void dsb_on_enable(unsigned cap, int on) {
    if (cap != 0x0BE2) return;
    dsb_st.en = on;
    dsb_off = 0;
    dsb_sync();
}

/* API Wrappers */

GLC void glBlendFunc(unsigned s, unsigned d) {
    dsb_st.f[0] = dsb_st.f[2] = (int)s; dsb_st.f[1] = dsb_st.f[3] = (int)d;
    RP(glBlendFunc, D_bf)(dsb_safe(s), dsb_safe(d));
    dsb_sync();
}

GLC void glBlendFuncSeparate(unsigned sc, unsigned dc, unsigned sa, unsigned da) {
    dsb_st.f[0] = (int)sc; dsb_st.f[1] = (int)dc; dsb_st.f[2] = (int)sa; dsb_st.f[3] = (int)da;
    RP(glBlendFuncSeparate, D_bfs)(dsb_safe(sc), dsb_safe(dc), dsb_safe(sa), dsb_safe(da));
    dsb_sync();
}

GLC void glBlendFunci(unsigned buf, unsigned s, unsigned d) {
    if (buf == 0) {
        dsb_st.f[0] = dsb_st.f[2] = (int)s;
        dsb_st.f[1] = dsb_st.f[3] = (int)d;
        RP(glBlendFunciEXT, D_bfi)(buf, dsb_safe(s), dsb_safe(d));
        dsb_sync();
    } else {
        RP(glBlendFunciEXT, D_bfi)(buf, s, d);
    }
}


GLC void glBlendEquation(unsigned e) {
    dsb_st.eq[0] = dsb_st.eq[1] = (int)e;
    RP(glBlendEquation, D_be)(e);
    dsb_sync();
}

GLC void glBlendEquationSeparate(unsigned c, unsigned a) {
    dsb_st.eq[0] = (int)c; dsb_st.eq[1] = (int)a;
    RP(glBlendEquationSeparate, D_bes)(c, a);
    dsb_sync();
}

GLC void glBlendColor(float r, float g, float b, float a) {
    dsb_st.c[0] = r; dsb_st.c[1] = g; dsb_st.c[2] = b; dsb_st.c[3] = a;
    RP(glBlendColor, D_bc)(r, g, b, a);
    dsb_sync();
}
