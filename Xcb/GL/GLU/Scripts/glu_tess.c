#include <GL/gl.h>
#include <GL/glu.h>
#include <stdlib.h>
#include <string.h>
#include "tesselator.h"

typedef double D;

struct GLUtesselator {
  GLenum winding; GLboolean boundary; D tol, normal[3];
  void *pdata;
  _GLUfuncptr begin, vertex, end, error, combine;
  _GLUfuncptr begin_d, vertex_d, end_d, error_d, combine_d;
  D *v; void **vd; int nv, vcap;
  int *cs, nc, ccap;          /* start index al fiecărui contur */
  int inpoly, incont;
};

GLUtesselator* gluNewTess(void) {
  GLUtesselator* t = calloc(1, sizeof *t);
  if (t) t->winding = GLU_TESS_WINDING_ODD;
  return t;
}
void gluDeleteTess(GLUtesselator* t) {
  if (!t) return;
  free(t->v); free(t->vd); free(t->cs); free(t);
}

void gluTessCallback(GLUtesselator* t, GLenum w, _GLUfuncptr f) {
  if (!t) return;
  switch (w) {
    case GLU_TESS_BEGIN: t->begin = f; break;
    case GLU_TESS_VERTEX: t->vertex = f; break;
    case GLU_TESS_END: t->end = f; break;
    case GLU_TESS_ERROR: t->error = f; break;
    case GLU_TESS_COMBINE: t->combine = f; break;
    case GLU_TESS_BEGIN_DATA: t->begin_d = f; break;
    case GLU_TESS_VERTEX_DATA: t->vertex_d = f; break;
    case GLU_TESS_END_DATA: t->end_d = f; break;
    case GLU_TESS_ERROR_DATA: t->error_d = f; break;
    case GLU_TESS_COMBINE_DATA: t->combine_d = f; break;
    default: break; /* EDGE_FLAG ignorat: ieșirea e mereu GL_TRIANGLES */
  }
}

void gluTessProperty(GLUtesselator* t, GLenum w, D v) {
  if (!t) return;
  if (w == GLU_TESS_WINDING_RULE) t->winding = (GLenum)v;
  else if (w == GLU_TESS_BOUNDARY_ONLY) t->boundary = v != 0;
  else if (w == GLU_TESS_TOLERANCE) t->tol = v;
}
void gluGetTessProperty(GLUtesselator* t, GLenum w, D* d) {
  if (!t || !d) return;
  if (w == GLU_TESS_WINDING_RULE) *d = t->winding;
  else if (w == GLU_TESS_BOUNDARY_ONLY) *d = t->boundary;
  else if (w == GLU_TESS_TOLERANCE) *d = t->tol;
}
void gluTessNormal(GLUtesselator* t, D x, D y, D z) {
  if (t) { t->normal[0] = x; t->normal[1] = y; t->normal[2] = z; }
}

static void terr(GLUtesselator* t, GLenum e) {
  if (t->error_d) ((void (*)(GLenum, void*))t->error_d)(e, t->pdata);
  else if (t->error) ((void (*)(GLenum))t->error)(e);
}

void gluTessBeginPolygon(GLUtesselator* t, GLvoid* data) {
  if (!t) return;
  if (t->inpoly) { terr(t, GLU_TESS_MISSING_END_POLYGON); return; }
  t->inpoly = 1; t->pdata = data; t->nv = 0; t->nc = 0; t->incont = 0;
}
void gluTessBeginContour(GLUtesselator* t) {
  if (!t) return;
  if (!t->inpoly) { terr(t, GLU_TESS_MISSING_BEGIN_POLYGON); return; }
  if (t->incont) { terr(t, GLU_TESS_MISSING_END_CONTOUR); return; }
  if (t->nc == t->ccap) {
    t->ccap = t->ccap ? t->ccap * 2 : 8;
    t->cs = realloc(t->cs, sizeof(int) * t->ccap);
  }
  t->cs[t->nc++] = t->nv; t->incont = 1;
}
void gluTessVertex(GLUtesselator* t, D* loc, GLvoid* data) {
  if (!t) return;
  if (!t->incont) { terr(t, GLU_TESS_MISSING_BEGIN_CONTOUR); return; }
  for (int i = 0; i < 3; i++)
    if (loc[i] > GLU_TESS_MAX_COORD || loc[i] < -GLU_TESS_MAX_COORD) { terr(t, GLU_TESS_COORD_TOO_LARGE); return; }
  if (t->nv == t->vcap) {
    t->vcap = t->vcap ? t->vcap * 2 : 64;
    t->v = realloc(t->v, sizeof(D) * 3 * t->vcap);
    t->vd = realloc(t->vd, sizeof(void*) * t->vcap);
  }
  memcpy(t->v + 3 * t->nv, loc, sizeof(D) * 3);
  t->vd[t->nv++] = data;
}
void gluTessEndContour(GLUtesselator* t) {
  if (!t) return;
  if (!t->incont) { terr(t, GLU_TESS_MISSING_BEGIN_CONTOUR); return; }
  t->incont = 0;
}

/* wrappers callbacks */
static void c_begin(GLUtesselator* t, GLenum m) {
  if (t->begin_d) ((void (*)(GLenum, void*))t->begin_d)(m, t->pdata);
  else if (t->begin) ((void (*)(GLenum))t->begin)(m);
}
static void c_vertex(GLUtesselator* t, void* d) {
  if (t->vertex_d) ((void (*)(void*, void*))t->vertex_d)(d, t->pdata);
  else if (t->vertex) ((void (*)(void*))t->vertex)(d);
}
static void c_end(GLUtesselator* t) {
  if (t->end_d) ((void (*)(void*))t->end_d)(t->pdata);
  else if (t->end) ((void (*)(void))t->end)();
}

void gluTessEndPolygon(GLUtesselator* t) {
  if (!t) return;
  if (!t->inpoly) { terr(t, GLU_TESS_MISSING_BEGIN_POLYGON); return; }
  if (t->incont) { terr(t, GLU_TESS_MISSING_END_CONTOUR); t->incont = 0; }
  t->inpoly = 0;
  if (t->nv < 3) return;

  TESStesselator* ts = tessNewTess(NULL);
  if (!ts) { terr(t, GLU_OUT_OF_MEMORY); return; }
  TESSreal* f = malloc(sizeof(TESSreal) * 3 * t->nv);
  if (!f) { tessDeleteTess(ts); terr(t, GLU_OUT_OF_MEMORY); return; }
  for (int i = 0; i < t->nv * 3; i++) f[i] = (TESSreal)t->v[i];
  for (int c = 0; c < t->nc; c++) {
    int s = t->cs[c], e = (c + 1 < t->nc) ? t->cs[c + 1] : t->nv;
    if (e - s >= 3)
      tessAddContour(ts, 3, f + 3 * s, sizeof(TESSreal) * 3, e - s);
  }
  free(f);

  int rule;
  switch (t->winding) {
    case GLU_TESS_WINDING_NONZERO: rule = TESS_WINDING_NONZERO; break;
    case GLU_TESS_WINDING_POSITIVE: rule = TESS_WINDING_POSITIVE; break;
    case GLU_TESS_WINDING_NEGATIVE: rule = TESS_WINDING_NEGATIVE; break;
    case GLU_TESS_WINDING_ABS_GEQ_TWO: rule = TESS_WINDING_ABS_GEQ_TWO; break;
    default: rule = TESS_WINDING_ODD;
  }
  TESSreal nrm[3] = {(TESSreal)t->normal[0], (TESSreal)t->normal[1], (TESSreal)t->normal[2]};
  int hasn = nrm[0] != 0 || nrm[1] != 0 || nrm[2] != 0;
  int et = t->boundary ? TESS_BOUNDARY_CONTOURS : TESS_POLYGONS;

  if (!tessTesselate(ts, rule, et, 3, 3, hasn ? nrm : NULL)) {
    tessDeleteTess(ts); terr(t, GLU_OUT_OF_MEMORY); return;
  }
  int nvo = tessGetVertexCount(ts), neo = tessGetElementCount(ts);
  const TESSreal* ov = tessGetVertices(ts);
  const TESSindex* oi = tessGetVertexIndices(ts);
  const TESSindex* oe = tessGetElements(ts);

  /* date pentru fiecare vertex de ieșire; cele noi trec prin COMBINE */
  void** od = calloc(nvo ? nvo : 1, sizeof(void*));
  for (int i = 0; i < nvo; i++) {
    if (oi[i] != TESS_UNDEF) { od[i] = t->vd[oi[i]]; continue; }
    D co[3] = {ov[3 * i], ov[3 * i + 1], ov[3 * i + 2]};
    void* dt[4] = {t->vd[0], t->vd[0], t->vd[0], t->vd[0]}; /* vezi notă */
    GLfloat w[4] = {1, 0, 0, 0};
    void* out = NULL;
    if (t->combine_d) ((void (*)(D*, void**, GLfloat*, void**, void*))t->combine_d)(co, dt, w, &out, t->pdata);
    else if (t->combine) ((void (*)(D*, void**, GLfloat*, void**))t->combine)(co, dt, w, &out);
    else terr(t, GLU_TESS_NEED_COMBINE_CALLBACK);
    od[i] = out;
  }

  if (t->boundary) {
    for (int c = 0; c < neo; c++) {
      int base = oe[c * 2], cnt = oe[c * 2 + 1];
      c_begin(t, GL_LINE_LOOP);
      for (int k = 0; k < cnt; k++) c_vertex(t, od[base + k]);
      c_end(t);
    }
  } else if (neo > 0) {
    c_begin(t, GL_TRIANGLES);
    for (int e = 0; e < neo; e++)
      for (int k = 0; k < 3; k++) {
        TESSindex ix = oe[e * 3 + k];
        if (ix != TESS_UNDEF) c_vertex(t, od[ix]);
      }
    c_end(t);
  }
  free(od);
  tessDeleteTess(ts);
}

/* API vechi */
void gluBeginPolygon(GLUtesselator* t) { gluTessBeginPolygon(t, NULL); gluTessBeginContour(t); }
void gluNextContour(GLUtesselator* t, GLenum type) { (void)type; gluTessEndContour(t); gluTessBeginContour(t); }
void gluEndPolygon(GLUtesselator* t) { gluTessEndContour(t); gluTessEndPolygon(t); }