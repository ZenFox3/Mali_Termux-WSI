#include <GL/gl.h>
#include <GL/glu.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define MAXP 8
#define MAXORD 16
#define CAP 512

typedef struct {
  int kind, dim, ord[2], nk[2], n[2];  /* kind: 0 v3, 1 v4, 2 normal, 3 color, 4..7 tex1..4 */
  float *kn[2], *ctl;
} Part;

struct GLUnurbs {
  GLfloat samp_tol, par_tol, ustep, vstep;
  GLenum display, method;
  GLboolean cull, autoload, tess;
  _GLUfuncptr cb[12], err;  /* cb: BEGIN,VERTEX,NORMAL,COLOR,TEX,END, apoi variantele _DATA */
  void* ud;
  int mode, intrim, np;     /* mode: 0 nimic, 1 curbă, 2 suprafață */
  Part p[MAXP];
};

static void nerr(GLUnurbs* n, GLenum e) { if (n->err) ((void (*)(GLenum))n->err)(e); }

static void pfree(Part* p) { free(p->kn[0]); free(p->kn[1]); free(p->ctl); memset(p, 0, sizeof *p); }
static void pclear(GLUnurbs* n) { for (int i = 0; i < n->np; i++) pfree(&n->p[i]); n->np = 0; }

GLUnurbs* gluNewNurbsRenderer(void) {
  GLUnurbs* n = calloc(1, sizeof *n);
  if (n) {
    n->samp_tol = 50; n->par_tol = 0.5f; n->ustep = n->vstep = 100;
    n->display = GLU_FILL; n->method = GLU_PATH_LENGTH; n->autoload = GL_TRUE;
  }
  return n;
}
void gluDeleteNurbsRenderer(GLUnurbs* n) { if (!n) return; pclear(n); free(n); }

/* ---- proprietăți și callback-uri ---- */

void gluNurbsProperty(GLUnurbs* n, GLenum prop, GLfloat v) {
  if (!n) return;
  GLenum e = (GLenum)v;
  switch (prop) {
    case GLU_SAMPLING_TOLERANCE: if (v <= 0) goto bad; n->samp_tol = v; break;
    case GLU_PARAMETRIC_TOLERANCE: if (v <= 0) goto bad; n->par_tol = v; break;
    case GLU_U_STEP: if (v <= 0) goto bad; n->ustep = v; break;
    case GLU_V_STEP: if (v <= 0) goto bad; n->vstep = v; break;
    case GLU_SAMPLING_METHOD:
      if (e != GLU_PATH_LENGTH && e != GLU_PARAMETRIC_ERROR && e != GLU_DOMAIN_DISTANCE &&
          e != GLU_OBJECT_PATH_LENGTH && e != GLU_OBJECT_PARAMETRIC_ERROR) goto bad;
      n->method = e; break;
    case GLU_DISPLAY_MODE:
      if (e != GLU_FILL && e != GLU_OUTLINE_POLYGON && e != GLU_OUTLINE_PATCH) goto bad;
      n->display = e; break;
    case GLU_CULLING: n->cull = v != 0; break;
    case GLU_AUTO_LOAD_MATRIX: n->autoload = v != 0; break;
    case GLU_NURBS_MODE:
      if (e != GLU_NURBS_RENDERER && e != GLU_NURBS_TESSELLATOR) goto bad;
      n->tess = (e == GLU_NURBS_TESSELLATOR); break;
    default: goto bad;
  }
  return;
bad:
  nerr(n, GLU_NURBS_ERROR26);
}

void gluGetNurbsProperty(GLUnurbs* n, GLenum prop, GLfloat* d) {
  if (!n || !d) return;
  switch (prop) {
    case GLU_SAMPLING_TOLERANCE: *d = n->samp_tol; break;
    case GLU_PARAMETRIC_TOLERANCE: *d = n->par_tol; break;
    case GLU_U_STEP: *d = n->ustep; break;
    case GLU_V_STEP: *d = n->vstep; break;
    case GLU_SAMPLING_METHOD: *d = (GLfloat)n->method; break;
    case GLU_DISPLAY_MODE: *d = (GLfloat)n->display; break;
    case GLU_CULLING: *d = n->cull; break;
    case GLU_AUTO_LOAD_MATRIX: *d = n->autoload; break;
    case GLU_NURBS_MODE: *d = n->tess ? GLU_NURBS_TESSELLATOR : GLU_NURBS_RENDERER; break;
    default: nerr(n, GLU_NURBS_ERROR26);
  }
}

void gluNurbsCallback(GLUnurbs* n, GLenum which, _GLUfuncptr f) {
  if (!n) return;
  if (which == GLU_NURBS_ERROR) { n->err = f; return; }
  if (which >= GLU_NURBS_BEGIN && which <= GLU_NURBS_END_DATA) n->cb[which - GLU_NURBS_BEGIN] = f;
  else nerr(n, GLU_INVALID_ENUM);
}
void gluNurbsCallbackData(GLUnurbs* n, GLvoid* d) { if (n) n->ud = d; }
void gluNurbsCallbackDataEXT(GLUnurbs* n, GLvoid* d) { if (n) n->ud = d; }
void gluLoadSamplingMatrices(GLUnurbs* n, const GLfloat* m, const GLfloat* p, const GLint* v) {
  (void)n; (void)m; (void)p; (void)v;  /* sampling e în spațiul parametric, nu în pixeli */
}

/* ---- emitere: GL direct (renderer) sau callback-uri (tessellator) ---- */

static void n_begin(GLUnurbs* n, GLenum m) {
  if (!n->tess) { glBegin(m); return; }
  if (n->cb[6]) ((void (*)(GLenum, void*))n->cb[6])(m, n->ud);
  else if (n->cb[0]) ((void (*)(GLenum))n->cb[0])(m);
}
static void n_end(GLUnurbs* n) {
  if (!n->tess) { glEnd(); return; }
  if (n->cb[11]) ((void (*)(void*))n->cb[11])(n->ud);
  else if (n->cb[5]) ((void (*)(void))n->cb[5])();
}
static void n_vec(GLUnurbs* n, int i, const float* v) { /* 1 vertex, 2 normal, 3 color, 4 tex */
  if (n->cb[i + 6]) ((void (*)(const float*, void*))n->cb[i + 6])(v, n->ud);
  else if (n->cb[i]) ((void (*)(const float*))n->cb[i])(v);
}
static void emit(GLUnurbs* n, const float* pos, const float* nr, const float* col, const float* tx, int tdim) {
  if (n->tess) {
    if (nr) n_vec(n, 2, nr);
    if (col) n_vec(n, 3, col);
    if (tx) n_vec(n, 4, tx);
    n_vec(n, 1, pos);
    return;
  }
  if (nr) glNormal3fv(nr);
  if (col) glColor4fv(col);
  if (tx) switch (tdim) {
    case 1: glTexCoord1fv(tx); break;
    case 2: glTexCoord2fv(tx); break;
    case 3: glTexCoord3fv(tx); break;
    default: glTexCoord4fv(tx);
  }
  glVertex3fv(pos);
}

/* ---- B-spline ---- */

static int kclass(GLenum t, int surf, int* dim) {
  int off = (int)t - (int)(surf ? GL_MAP2_COLOR_4 : GL_MAP1_COLOR_4);
  if (off < 0 || off > 8) return -2;
  switch (off) {
    case 0: *dim = 4; return 3;
    case 1: return -1;                       /* INDEX: nesuportat */
    case 2: *dim = 3; return 2;
    case 7: *dim = 3; return 0;
    case 8: *dim = 4; return 1;
    default: *dim = off - 2; return off + 1; /* texcoord 1..4 */
  }
}

static int kcheck(const float* k, int nk, int ord) {
  if (ord < 1 || ord > MAXORD) return GLU_NURBS_ERROR1;
  if (nk < 2 * ord) return GLU_NURBS_ERROR2;
  for (int i = 1; i < nk; i++) if (k[i] < k[i - 1]) return GLU_NURBS_ERROR4;
  if (!(k[nk - ord] > k[ord - 1])) return GLU_NURBS_ERROR3;
  for (int i = 0; i < nk;) {
    int j = i; while (j < nk && k[j] == k[i]) j++;
    if (j - i > ord) return GLU_NURBS_ERROR5;
    i = j;
  }
  return 0;
}

static int span(const float* kn, int nk, int ord, float t) {
  int n = nk - ord;
  if (t >= kn[n]) { int i = n - 1; while (i > ord - 1 && kn[i] >= kn[i + 1]) i--; return i; }
  if (t <= kn[ord - 1]) return ord - 1;
  int lo = ord - 1, hi = n;
  while (hi - lo > 1) { int m = (lo + hi) / 2; if (t < kn[m]) hi = m; else lo = m; }
  return lo;
}

static void basis(const float* kn, int i, int ord, float t, float* N) {
  float L[MAXORD + 1], R[MAXORD + 1];
  N[0] = 1;
  for (int j = 1; j < ord; j++) {
    L[j] = t - kn[i + 1 - j]; R[j] = kn[i + j] - t;
    float saved = 0;
    for (int r = 0; r < j; r++) {
      float den = R[r + 1] + L[j - r];
      float tmp = den != 0 ? N[r] / den : 0;
      N[r] = saved + R[r + 1] * tmp;
      saved = L[j - r] * tmp;
    }
    N[j] = saved;
  }
}

static void peval(const Part* p, int nd, float u, float v, float* out) {
  float Nu[MAXORD], Nv[MAXORD];
  int d = p->dim, o0 = p->ord[0];
  float lo = p->kn[0][o0 - 1], hi = p->kn[0][p->n[0]];
  if (u < lo) u = lo; if (u > hi) u = hi;
  int iu = span(p->kn[0], p->nk[0], o0, u);
  basis(p->kn[0], iu, o0, u, Nu);
  for (int k = 0; k < d; k++) out[k] = 0;
  if (nd == 1) {
    for (int a = 0; a < o0; a++) {
      const float* c = p->ctl + (size_t)(iu - o0 + 1 + a) * d;
      for (int k = 0; k < d; k++) out[k] += Nu[a] * c[k];
    }
    return;
  }
  int o1 = p->ord[1], nt = p->n[1];
  lo = p->kn[1][o1 - 1]; hi = p->kn[1][nt];
  if (v < lo) v = lo; if (v > hi) v = hi;
  int iv = span(p->kn[1], p->nk[1], o1, v);
  basis(p->kn[1], iv, o1, v, Nv);
  for (int a = 0; a < o0; a++)
    for (int b = 0; b < o1; b++) {
      const float* c = p->ctl + ((size_t)(iu - o0 + 1 + a) * nt + (iv - o1 + 1 + b)) * d;
      float w = Nu[a] * Nv[b];
      for (int k = 0; k < d; k++) out[k] += w * c[k];
    }
}

static void ppos(const Part* p, int nd, float u, float v, float* o3) {
  float o[4];
  peval(p, nd, u, v, o);
  if (p->kind == 1 && o[3] != 0) { o[0] /= o[3]; o[1] /= o[3]; o[2] /= o[3]; }
  o3[0] = o[0]; o3[1] = o[1]; o3[2] = o[2];
}

static int nspans(const Part* p, int ax) {
  int c = 0;
  for (int i = p->ord[ax] - 1; i < p->n[ax]; i++) if (p->kn[ax][i] < p->kn[ax][i + 1]) c++;
  return c ? c : 1;
}

/* Eșantionare: DOMAIN_DISTANCE după specificație (puncte pe unitate de domeniu);
   restul metodelor: euristică din tolerance (50 -> 16 puncte/interval), fără pixeli. */
static int nsamp(GLUnurbs* n, const Part* p, int ax, float step, int cap) {
  int c;
  if (n->method == GLU_DOMAIN_DISTANCE) {
    float len = p->kn[ax][p->n[ax]] - p->kn[ax][p->ord[ax] - 1];
    c = (int)ceilf(step * len);
  } else {
    int per = n->samp_tol > 0 ? (int)(800.0f / n->samp_tol) : 16;
    if (per < 4) per = 4; if (per > 64) per = 64;
    c = nspans(p, ax) * per;
  }
  return c < 1 ? 1 : c > cap ? cap : c;
}

/* ---- construcție ---- */

void gluBeginCurve(GLUnurbs* n) { if (!n) return; if (n->mode) { nerr(n, GLU_NURBS_ERROR6); return; } n->mode = 1; pclear(n); }
void gluBeginSurface(GLUnurbs* n) { if (!n) return; if (n->mode) { nerr(n, GLU_NURBS_ERROR13); return; } n->mode = 2; pclear(n); }
void gluBeginTrim(GLUnurbs* n) { if (!n) return; if (n->mode != 2) { nerr(n, GLU_NURBS_ERROR15); return; } n->intrim = 1; }
void gluEndTrim(GLUnurbs* n) { if (!n) return; if (!n->intrim) { nerr(n, GLU_NURBS_ERROR17); return; } n->intrim = 0; }
void gluPwlCurve(GLUnurbs* n, GLint c, GLfloat* d, GLint s, GLenum t) {
  (void)c; (void)d; (void)s; (void)t;
  if (n && !n->intrim) nerr(n, GLU_NURBS_ERROR19);  /* trimming: ignorat */
}

void gluNurbsCurve(GLUnurbs* n, GLint nk, GLfloat* knots, GLint stride, GLfloat* ctl, GLint ord, GLenum type) {
  if (!n || n->intrim) return;
  if (n->mode != 1) { nerr(n, GLU_NURBS_ERROR10); return; }
  int dim = 0, kind = kclass(type, 0, &dim);
  if (kind < 0) { nerr(n, GLU_NURBS_ERROR35); return; }
  int e = kcheck(knots, nk, ord);
  if (e) { nerr(n, e); return; }
  if (stride < dim) { nerr(n, GLU_NURBS_ERROR34); return; }
  if (!ctl) { nerr(n, GLU_NURBS_ERROR36); return; }
  if (n->np >= MAXP) { nerr(n, GLU_NURBS_ERROR8); return; }
  Part* p = &n->p[n->np];
  int nc = nk - ord;
  p->kind = kind; p->dim = dim; p->ord[0] = ord; p->nk[0] = nk; p->n[0] = nc;
  p->kn[0] = malloc(sizeof(float) * nk);
  p->ctl = malloc(sizeof(float) * (size_t)nc * dim);
  if (!p->kn[0] || !p->ctl) { pfree(p); nerr(n, GLU_OUT_OF_MEMORY); return; }
  memcpy(p->kn[0], knots, sizeof(float) * nk);
  for (int i = 0; i < nc; i++) for (int k = 0; k < dim; k++) p->ctl[i * dim + k] = ctl[(size_t)i * stride + k];
  n->np++;
}

void gluNurbsSurface(GLUnurbs* n, GLint sk, GLfloat* sKn, GLint tk, GLfloat* tKn, GLint sStr, GLint tStr,
                     GLfloat* ctl, GLint sOrd, GLint tOrd, GLenum type) {
  if (!n || n->intrim) return;
  if (n->mode != 2) { nerr(n, GLU_NURBS_ERROR11); return; }
  int dim = 0, kind = kclass(type, 1, &dim);
  if (kind < 0) { nerr(n, GLU_NURBS_ERROR35); return; }
  int e = kcheck(sKn, sk, sOrd); if (!e) e = kcheck(tKn, tk, tOrd);
  if (e) { nerr(n, e); return; }
  if (sStr <= 0 || tStr <= 0) { nerr(n, GLU_NURBS_ERROR34); return; }
  if (!ctl) { nerr(n, GLU_NURBS_ERROR36); return; }
  if (n->np >= MAXP) { nerr(n, GLU_NURBS_ERROR8); return; }
  Part* p = &n->p[n->np];
  int ns = sk - sOrd, nt = tk - tOrd;
  p->kind = kind; p->dim = dim;
  p->ord[0] = sOrd; p->ord[1] = tOrd; p->nk[0] = sk; p->nk[1] = tk; p->n[0] = ns; p->n[1] = nt;
  p->kn[0] = malloc(sizeof(float) * sk); p->kn[1] = malloc(sizeof(float) * tk);
  p->ctl = malloc(sizeof(float) * (size_t)ns * nt * dim);
  if (!p->kn[0] || !p->kn[1] || !p->ctl) { pfree(p); nerr(n, GLU_OUT_OF_MEMORY); return; }
  memcpy(p->kn[0], sKn, sizeof(float) * sk); memcpy(p->kn[1], tKn, sizeof(float) * tk);
  for (int i = 0; i < ns; i++) for (int j = 0; j < nt; j++) for (int k = 0; k < dim; k++)
    p->ctl[((size_t)i * nt + j) * dim + k] = ctl[(size_t)i * sStr + (size_t)j * tStr + k];
  n->np++;
}

/* ---- randare ---- */

static void by_kind(GLUnurbs* n, Part** by) {
  for (int i = 0; i < 8; i++) by[i] = NULL;
  for (int i = 0; i < n->np; i++) by[n->p[i].kind] = &n->p[i];
}
static Part* tex_part(Part** by) { for (int k = 7; k >= 4; k--) if (by[k]) return by[k]; return NULL; }

static void render_curve(GLUnurbs* n) {
  Part* by[8]; by_kind(n, by);
  Part* pv = by[0] ? by[0] : by[1];
  if (!pv) { nerr(n, GLU_NURBS_ERROR8); return; }
  Part* pt = tex_part(by);
  float lo = pv->kn[0][pv->ord[0] - 1], hi = pv->kn[0][pv->n[0]];
  int N = nsamp(n, pv, 0, n->ustep, 4096);
  n_begin(n, GL_LINE_STRIP);
  for (int i = 0; i <= N; i++) {
    float t = i == N ? hi : lo + (hi - lo) * i / N;
    float pos[3], nr[4], col[4], tx[4] = {0, 0, 0, 1};
    ppos(pv, 1, t, 0, pos);
    if (by[2]) peval(by[2], 1, t, 0, nr);
    if (by[3]) peval(by[3], 1, t, 0, col);
    if (pt) peval(pt, 1, t, 0, tx);
    emit(n, pos, by[2] ? nr : NULL, by[3] ? col : NULL, pt ? tx : NULL, pt ? pt->dim : 0);
  }
  n_end(n);
}

typedef struct { float p[3], nr[3], c[4], t[4]; } SV;

static void render_surface(GLUnurbs* n) {
  Part* by[8]; by_kind(n, by);
  Part* pv = by[0] ? by[0] : by[1];
  if (!pv) { nerr(n, GLU_NURBS_ERROR8); return; }
  Part* pt = tex_part(by);
  int nu = nsamp(n, pv, 0, n->ustep, CAP), nv = nsamp(n, pv, 1, n->vstep, CAP);
  float u0 = pv->kn[0][pv->ord[0] - 1], u1 = pv->kn[0][pv->n[0]];
  float v0 = pv->kn[1][pv->ord[1] - 1], v1 = pv->kn[1][pv->n[1]];
  size_t W = (size_t)nv + 1;
  SV* g = calloc((size_t)(nu + 1) * W, sizeof(SV));
  if (!g) { nerr(n, GLU_OUT_OF_MEMORY); return; }
  float eu = (u1 - u0) * 1e-3f, ev = (v1 - v0) * 1e-3f;
  for (int i = 0; i <= nu; i++)
    for (int j = 0; j <= nv; j++) {
      float u = i == nu ? u1 : u0 + (u1 - u0) * i / nu;
      float v = j == nv ? v1 : v0 + (v1 - v0) * j / nv;
      SV* s = &g[i * W + j];
      float tmp[4];
      ppos(pv, 2, u, v, s->p);
      if (by[2]) { peval(by[2], 2, u, v, tmp); memcpy(s->nr, tmp, sizeof s->nr); }
      else {
        float ua = u - eu < u0 ? u0 : u - eu, ub = u + eu > u1 ? u1 : u + eu;
        float va = v - ev < v0 ? v0 : v - ev, vb = v + ev > v1 ? v1 : v + ev;
        float a[3], b[3], c[3], d[3];
        ppos(pv, 2, ua, v, a); ppos(pv, 2, ub, v, b);
        ppos(pv, 2, u, va, c); ppos(pv, 2, u, vb, d);
        float du[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]};
        float dv[3] = {d[0] - c[0], d[1] - c[1], d[2] - c[2]};
        float nx = du[1] * dv[2] - du[2] * dv[1], ny = du[2] * dv[0] - du[0] * dv[2], nz = du[0] * dv[1] - du[1] * dv[0];
        float l = sqrtf(nx * nx + ny * ny + nz * nz);
        if (l > 1e-12f) { s->nr[0] = nx / l; s->nr[1] = ny / l; s->nr[2] = nz / l; }
        else { s->nr[0] = 0; s->nr[1] = 0; s->nr[2] = 1; }  /* punct degenerat (pol) */
      }
      if (by[3]) peval(by[3], 2, u, v, s->c);
      s->t[3] = 1;
      if (pt) peval(pt, 2, u, v, s->t);
    }
#define EMIT(S) emit(n, (S)->p, (S)->nr, by[3] ? (S)->c : NULL, pt ? (S)->t : NULL, pt ? pt->dim : 0)
  if (n->display == GLU_FILL) {
    for (int i = 0; i < nu; i++) {
      n_begin(n, GL_TRIANGLE_STRIP);
      for (int j = 0; j <= nv; j++) { EMIT(&g[i * W + j]); EMIT(&g[(i + 1) * W + j]); }
      n_end(n);
    }
  } else {
    for (int i = 0; i <= nu; i++) { n_begin(n, GL_LINE_STRIP); for (int j = 0; j <= nv; j++) EMIT(&g[i * W + j]); n_end(n); }
    for (int j = 0; j <= nv; j++) { n_begin(n, GL_LINE_STRIP); for (int i = 0; i <= nu; i++) EMIT(&g[i * W + j]); n_end(n); }
  }
#undef EMIT
  free(g);
}

void gluEndCurve(GLUnurbs* n) {
  if (!n) return;
  if (n->mode != 1) { nerr(n, GLU_NURBS_ERROR7); return; }
  render_curve(n); pclear(n); n->mode = 0;
}
void gluEndSurface(GLUnurbs* n) {
  if (!n) return;
  if (n->mode != 2) { nerr(n, GLU_NURBS_ERROR27); return; }
  if (n->intrim) { nerr(n, GLU_NURBS_ERROR12); n->intrim = 0; }
  render_surface(n); pclear(n); n->mode = 0;
}