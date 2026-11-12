// glu.c - Complete Single-File GLU Implementation
#include <GL/gl.h>
#include <GL/glu.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

typedef double D;

struct GLUquadric {
  GLenum draw, normals, orient;
  GLboolean tex;
  void (*err)(GLenum);
};

#ifndef GLU_VERSION
#define GLU_VERSION    100800
#define GLU_EXTENSIONS 100801
#endif

#define GLU_SMOOTH        100000
#define GLU_FLAT          100001
#define GLU_NONE          100002
#define GLU_POINT         100010
#define GLU_LINE          100011
#define GLU_FILL          100012
#define GLU_SILHOUETTE    100013
#define GLU_OUTSIDE       100020
#define GLU_INSIDE        100021
#define GLU_ERROR         100103
#define GLU_INVALID_ENUM  100900
#define GLU_INVALID_VALUE 100901

/* ---- Matrix & Vector Helpers ---- */

static void mmul(const D* a, const D* b, D* r) {
  for (int c = 0; c < 4; c++) {
    for (int j = 0; j < 4; j++) {
      D s = 0;
      for (int k = 0; k < 4; k++) s += a[k * 4 + j] * b[c * 4 + k];
      r[c * 4 + j] = s;
    }
  }
}

static int minv(const D* m, D* out) {
  D a[4][8];
  for (int i = 0; i < 4; i++) {
    for (int j = 0; j < 4; j++) {
      a[i][j] = m[j * 4 + i];
      a[i][4 + j] = (i == j);
    }
  }
  for (int c = 0; c < 4; c++) {
    int p = c;
    for (int r = c + 1; r < 4; r++) {
      if (fabs(a[r][c]) > fabs(a[p][c])) p = r;
    }
    if (fabs(a[p][c]) < 1e-12) return 0;
    if (p != c) {
      for (int j = 0; j < 8; j++) {
        D t = a[c][j];
        a[c][j] = a[p][j];
        a[p][j] = t;
      }
    }
    D d = a[c][c];
    for (int j = 0; j < 8; j++) a[c][j] /= d;
    for (int r = 0; r < 4; r++) {
      if (r != c) {
        D f = a[r][c];
        for (int j = 0; j < 8; j++) a[r][j] -= f * a[c][j];
      }
    }
  }
  for (int i = 0; i < 4; i++) {
    for (int j = 0; j < 4; j++) out[j * 4 + i] = a[i][4 + j];
  }
  return 1;
}

static void xform(const D* m, const D* v, D* o) {
  for (int i = 0; i < 4; i++) {
    o[i] = m[i] * v[0] + m[4 + i] * v[1] + m[8 + i] * v[2] + m[12 + i] * v[3];
  }
}

/* ---- Projection & Camera ---- */

void gluPerspective(D fovy, D asp, D zn, D zf) {
  D f = 1.0 / tan(fovy * M_PI / 360.0);
  D m[16] = {f / asp, 0, 0, 0, 0, f, 0, 0, 0, 0, (zf + zn) / (zn - zf), -1, 0, 0, 2 * zf * zn / (zn - zf), 0};
  glMultMatrixd(m);
}

void gluOrtho2D(D l, D r, D b, D t) { glOrtho(l, r, b, t, -1, 1); }

void gluLookAt(D ex, D ey, D ez, D cx, D cy, D cz, D ux, D uy, D uz) {
  D f[3] = {cx - ex, cy - ey, cz - ez}, n = sqrt(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
  for (int i = 0; i < 3; i++) f[i] /= n;
  D s[3] = {f[1] * uz - f[2] * uy, f[2] * ux - f[0] * uz, f[0] * uy - f[1] * ux};
  n = sqrt(s[0] * s[0] + s[1] * s[1] + s[2] * s[2]);
  for (int i = 0; i < 3; i++) s[i] /= n;
  D u[3] = {s[1] * f[2] - s[2] * f[1], s[2] * f[0] - s[0] * f[2], s[0] * f[1] - s[1] * f[0]};
  D m[16] = {s[0], u[0], -f[0], 0, s[1], u[1], -f[1], 0, s[2], u[2], -f[2], 0, 0, 0, 0, 1};
  glMultMatrixd(m);
  glTranslated(-ex, -ey, -ez);
}

void gluPickMatrix(D x, D y, D dx, D dy, GLint* vp) {
  if (dx <= 0 || dy <= 0) return;
  glTranslated((vp[2] - 2 * (x - vp[0])) / dx, (vp[3] - 2 * (y - vp[1])) / dy, 0);
  glScaled(vp[2] / dx, vp[3] / dy, 1.0);
}

GLint gluProject(D x, D y, D z, const D* mv, const D* pr, const GLint* vp, D* wx, D* wy, D* wz) {
  D m[16], in[4] = {x, y, z, 1}, o[4];
  mmul(pr, mv, m);
  xform(m, in, o);
  if (o[3] == 0) return GL_FALSE;
  for (int i = 0; i < 3; i++) o[i] /= o[3];
  *wx = vp[0] + (o[0] * .5 + .5) * vp[2];
  *wy = vp[1] + (o[1] * .5 + .5) * vp[3];
  *wz = o[2] * .5 + .5;
  return GL_TRUE;
}

GLint gluUnProject(D wx, D wy, D wz, const D* mv, const D* pr, const GLint* vp, D* x, D* y, D* z) {
  D m[16], inv[16], in[4], o[4];
  mmul(pr, mv, m);
  if (!minv(m, inv)) return GL_FALSE;
  in[0] = (wx - vp[0]) / vp[2] * 2 - 1;
  in[1] = (wy - vp[1]) / vp[3] * 2 - 1;
  in[2] = wz * 2 - 1;
  in[3] = 1;
  xform(inv, in, o);
  if (o[3] == 0) return GL_FALSE;
  *x = o[0] / o[3];
  *y = o[1] / o[3];
  *z = o[2] / o[3];
  return GL_TRUE;
}

/* ---- Errors & Strings ---- */

const GLubyte* gluErrorString(GLenum e) {
  static const char* const ne[37] = {
    "spline order un-supported", "too few knots", "valid knot range is empty",
    "decreasing knot sequence", "knot multiplicity greater than order of spline",
    "endcurve() must follow bgncurve()", "bgncurve() must precede endcurve()",
    "missing or extra geometric data", "can't draw pwlcurves", "missing bgncurve()",
    "missing bgnsurface()", "endtrim() must precede endsurface()",
    "bgnsurface() must precede endsurface()", "curve of improper type passed as trim curve",
    "bgnsurface() must precede bgntrim()", "endtrim() must follow bgntrim()",
    "bgntrim() must precede endtrim()", "invalid or missing trim curve",
    "bgntrim() must precede pwlcurve()", "pwlcurve referenced twice", "pwlcurve and nurbscurve mixed",
    "improper usage of trim data type", "nurbscurve referenced twice", "nurbscurve and pwlcurve mixed",
    "nurbssurface referenced twice", "invalid property", "endsurface() must follow bgnsurface()",
    "intersecting or misoriented trim curves", "intersecting trim curves", "unused",
    "unconnected trim curves", "unknown knot error", "negative vertex count encountered",
    "negative byte-stride encountered", "unknown type descriptor", "null control point reference",
    "duplicate point on pwlcurve"
  };
  static const char* const te[6] = {
    "gluTessBeginPolygon() must precede a gluTessEndPolygon()",
    "gluTessBeginContour() must precede a gluTessEndContour()",
    "gluTessEndPolygon() must follow a gluTessBeginPolygon()",
    "gluTessEndContour() must follow a gluTessBeginContour()",
    "a coordinate is too large", "need combine callback"
  };
  if (e >= GLU_NURBS_ERROR1 && e <= GLU_NURBS_ERROR37) return (const GLubyte*)ne[e - GLU_NURBS_ERROR1];
  if (e >= GLU_TESS_ERROR1 && e <= GLU_TESS_ERROR6) return (const GLubyte*)te[e - GLU_TESS_ERROR1];
  switch (e) {
    case GL_NO_ERROR: return (const GLubyte*)"no error";
    case GL_INVALID_ENUM: case GLU_INVALID_ENUM: return (const GLubyte*)"invalid enumerant";
    case GL_INVALID_VALUE: case GLU_INVALID_VALUE: return (const GLubyte*)"invalid value";
    case GL_INVALID_OPERATION: case GLU_INVALID_OPERATION: return (const GLubyte*)"invalid operation";
    case GL_OUT_OF_MEMORY: case GLU_OUT_OF_MEMORY: return (const GLubyte*)"out of memory";
    case GLU_INCOMPATIBLE_GL_VERSION: return (const GLubyte*)"incompatible gl version";
    default: return (const GLubyte*)"unknown error";
  }
}

const GLubyte* gluGetString(GLenum name) {
  switch (name) {
    case GLU_VERSION:    return (const GLubyte*)"1.3";
    case GLU_EXTENSIONS: return (const GLubyte*)"GLU_EXT_nurbs_tessellator GLU_EXT_object_space_tess";
    default:             return NULL;
  }
}

GLboolean gluCheckExtension(const GLubyte* ext, const GLubyte* exts) {
  if (!ext || !exts) return GL_FALSE;
  size_t n = strlen((const char*)ext);
  const char* p = (const char*)exts;
  while ((p = strstr(p, (const char*)ext))) {
    int start = (p == (const char*)exts) || p[-1] == ' ';
    int end = p[n] == ' ' || p[n] == '\0';
    if (start && end) return GL_TRUE;
    p += n;
  }
  return GL_FALSE;
}

/* ---- Quadrics State ---- */

GLUquadric* gluNewQuadric(void) {
  GLUquadric* q = calloc(1, sizeof *q);
  if (q) {
    q->draw = GLU_FILL;
    q->normals = GLU_SMOOTH;
    q->orient = GLU_OUTSIDE;
  }
  return q;
}

void gluDeleteQuadric(GLUquadric* q) { free(q); }
void gluQuadricDrawStyle(GLUquadric* q, GLenum s) { if (q) q->draw = s; }
void gluQuadricNormals(GLUquadric* q, GLenum n) { if (q) q->normals = n; }
void gluQuadricOrientation(GLUquadric* q, GLenum o) { if (q) q->orient = o; }
void gluQuadricTexture(GLUquadric* q, GLboolean t) { if (q) q->tex = t; }

void gluQuadricCallback(GLUquadric* q, GLenum w, _GLUfuncptr f) {
  if (q && w == GLU_ERROR) q->err = (void (*)(GLenum))f;
}

static void qerr(GLUquadric* q, GLenum e) { if (q->err) q->err(e); }

/* Emite un vertex cu normală și textură opționale */
static void qv(GLUquadric* q, D nx, D ny, D nz, D s, D t, D x, D y, D z) {
  if (q->normals != GLU_NONE) glNormal3d(nx, ny, nz);
  if (q->tex) glTexCoord2d(s, t);
  glVertex3d(x, y, z);
}

/* ---- Quadrics Rendering ---- */

void gluSphere(GLUquadric* q, D r, GLint sl, GLint st) {
  if (!q) return;
  if (sl < 2 || st < 1 || r < 0) { qerr(q, GLU_INVALID_VALUE); return; }
  D ns = (q->orient == GLU_INSIDE) ? -1 : 1, dr = M_PI / st, dt = 2 * M_PI / sl;
  if (q->draw == GLU_FILL) {
    for (int i = 0; i < st; i++) {
      D r0 = i * dr, r1 = (i + 1) * dr;
      glBegin(GL_TRIANGLE_STRIP);
      for (int j = 0; j <= sl; j++) {
        D th = (j == sl) ? 0 : j * dt, s = 1 - (D)j / sl;
        D x = -sin(th), y = cos(th);
        qv(q, x * sin(r0) * ns, y * sin(r0) * ns, cos(r0) * ns, s, 1 - (D)i / st, x * sin(r0) * r, y * sin(r0) * r, cos(r0) * r);
        qv(q, x * sin(r1) * ns, y * sin(r1) * ns, cos(r1) * ns, s, 1 - (D)(i + 1) / st, x * sin(r1) * r, y * sin(r1) * r, cos(r1) * r);
      }
      glEnd();
    }
  } else if (q->draw == GLU_POINT) {
    glBegin(GL_POINTS);
    for (int i = 0; i <= st; i++) {
      for (int j = 0; j < sl; j++) {
        D rho = i * dr, th = j * dt;
        D x = -sin(th) * sin(rho), y = cos(th) * sin(rho), z = cos(rho);
        qv(q, x * ns, y * ns, z * ns, 0, 0, x * r, y * r, z * r);
      }
    }
    glEnd();
  } else { /* LINE / SILHOUETTE */
    for (int i = 1; i < st; i++) {
      D rho = i * dr;
      glBegin(GL_LINE_LOOP);
      for (int j = 0; j < sl; j++) {
        D th = j * dt, x = -sin(th) * sin(rho), y = cos(th) * sin(rho), z = cos(rho);
        qv(q, x * ns, y * ns, z * ns, 0, 0, x * r, y * r, z * r);
      }
      glEnd();
    }
    for (int j = 0; j < sl; j++) {
      D th = j * dt;
      glBegin(GL_LINE_STRIP);
      for (int i = 0; i <= st; i++) {
        D rho = i * dr, x = -sin(th) * sin(rho), y = cos(th) * sin(rho), z = cos(rho);
        qv(q, x * ns, y * ns, z * ns, 0, 0, x * r, y * r, z * r);
      }
      glEnd();
    }
  }
}

void gluCylinder(GLUquadric* q, D base, D top, D h, GLint sl, GLint st) {
  if (!q) return;
  if (sl < 2 || st < 1 || base < 0 || top < 0 || h < 0) { qerr(q, GLU_INVALID_VALUE); return; }
  D ns = (q->orient == GLU_INSIDE) ? -1 : 1, da = 2 * M_PI / sl;
  D dr = (top - base) / st, dz = h / st;
  D len = sqrt(h * h + (base - top) * (base - top));
  if (len == 0) len = 1;
  D nxy = h / len, nz = (base - top) / len;
  if (q->draw == GLU_FILL) {
    for (int i = 0; i < st; i++) {
      D z0 = i * dz, z1 = (i + 1) * dz, r0 = base + i * dr, r1 = base + (i + 1) * dr;
      glBegin(GL_TRIANGLE_STRIP);
      for (int j = 0; j <= sl; j++) {
        D a = (j == sl) ? 0 : j * da, x = sin(a), y = cos(a), s = 1 - (D)j / sl;
        qv(q, x * nxy * ns, y * nxy * ns, nz * ns, s, (D)i / st, x * r0, y * r0, z0);
        qv(q, x * nxy * ns, y * nxy * ns, nz * ns, s, (D)(i + 1) / st, x * r1, y * r1, z1);
      }
      glEnd();
    }
  } else if (q->draw == GLU_POINT) {
    glBegin(GL_POINTS);
    for (int j = 0; j < sl; j++) {
      D a = j * da, x = sin(a), y = cos(a);
      for (int i = 0; i <= st; i++) {
        D rr = base + i * dr;
        qv(q, x * nxy * ns, y * nxy * ns, nz * ns, 0, 0, x * rr, y * rr, i * dz);
      }
    }
    glEnd();
  } else {
    for (int i = 0; i <= st; i++) {
      D rr = base + i * dr;
      glBegin(GL_LINE_LOOP);
      for (int j = 0; j < sl; j++) {
        D a = j * da, x = sin(a), y = cos(a);
        qv(q, x * nxy * ns, y * nxy * ns, nz * ns, 0, 0, x * rr, y * rr, i * dz);
      }
      glEnd();
    }
    glBegin(GL_LINES);
    for (int j = 0; j < sl; j++) {
      D a = j * da, x = sin(a), y = cos(a);
      qv(q, x * nxy * ns, y * nxy * ns, nz * ns, 0, 0, x * base, y * base, 0);
      qv(q, x * nxy * ns, y * nxy * ns, nz * ns, 0, 0, x * top, y * top, h);
    }
    glEnd();
  }
}

void gluDisk(GLUquadric* q, D inner, D outer, GLint sl, GLint loops) {
  if (!q) return;
  if (sl < 2 || loops < 1 || outer <= 0 || inner < 0 || inner > outer) { qerr(q, GLU_INVALID_VALUE); return; }
  D nz = (q->orient == GLU_INSIDE) ? -1 : 1, da = 2 * M_PI / sl, dr = (outer - inner) / loops;
  if (q->draw == GLU_FILL) {
    for (int i = 0; i < loops; i++) {
      D r1 = inner + i * dr, r2 = r1 + dr;
      D ra = (nz > 0) ? r2 : r1, rb = (nz > 0) ? r1 : r2; /* inversăm ordinea pentru INSIDE */
      glBegin(GL_TRIANGLE_STRIP);
      for (int j = 0; j <= sl; j++) {
        D a = (j == sl) ? 0 : j * da, sa = sin(a), ca = cos(a);
        qv(q, 0, 0, nz, 0.5 + sa * ra / outer / 2, 0.5 + ca * ra / outer / 2, ra * sa, ra * ca, 0);
        qv(q, 0, 0, nz, 0.5 + sa * rb / outer / 2, 0.5 + ca * rb / outer / 2, rb * sa, rb * ca, 0);
      }
      glEnd();
    }
  } else if (q->draw == GLU_POINT) {
    glBegin(GL_POINTS);
    for (int i = 0; i <= loops; i++) {
      for (int j = 0; j < sl; j++) {
        D rr = inner + i * dr, a = j * da;
        qv(q, 0, 0, nz, 0, 0, rr * sin(a), rr * cos(a), 0);
      }
    }
    glEnd();
  } else {
    for (int i = 0; i <= loops; i++) {
      D rr = inner + i * dr;
      glBegin(GL_LINE_LOOP);
      for (int j = 0; j < sl; j++) {
        D a = j * da;
        qv(q, 0, 0, nz, 0, 0, rr * sin(a), rr * cos(a), 0);
      }
      glEnd();
    }
    glBegin(GL_LINES);
    for (int j = 0; j < sl; j++) {
      D a = j * da;
      qv(q, 0, 0, nz, 0, 0, inner * sin(a), inner * cos(a), 0);
      qv(q, 0, 0, nz, 0, 0, outer * sin(a), outer * cos(a), 0);
    }
    glEnd();
  }
}

/* ---- Imagini: scale + mipmaps (GL_UNSIGNED_BYTE) ---- */

static int fcomps(GLenum f) {
  switch (f) {
    case GL_RGB: return 3;
    case GL_RGBA: return 4;
    case GL_LUMINANCE: case GL_ALPHA: return 1;
    case GL_LUMINANCE_ALPHA: return 2;
#ifdef GL_BGR
    case GL_BGR: return 3;
#endif
#ifdef GL_BGRA
    case GL_BGRA: return 4;
#endif
#ifdef GL_RED
    case GL_RED: return 1;
#endif
  }
  return 0;
}

static float* img_load(const GLubyte* d, int w, int h, int c, int al) {
  long row = ((long)w * c + al - 1) / al * al;
  float* f = malloc(sizeof(float) * w * h * c);
  if (!f) return NULL;
  for (int y = 0; y < h; y++)
    for (int i = 0; i < w * c; i++) f[(long)y * w * c + i] = d[y * row + i];
  return f;
}

static void img_store(const float* f, GLubyte* d, int w, int h, int c, int al) {
  long row = ((long)w * c + al - 1) / al * al;
  for (int y = 0; y < h; y++)
    for (int i = 0; i < w * c; i++) {
      float v = f[(long)y * w * c + i] + 0.5f;
      d[y * row + i] = (GLubyte)(v < 0 ? 0 : v > 255 ? 255 : v);
    }
}

/* resample 1D (un singur canal): box la micșorare, liniar la mărire */
static void rs(const float* in, long is, int n, float* out, long os, int m) {
  double sc = (double)n / m;
  if (m <= n) {
    for (int i = 0; i < m; i++) {
      double a = i * sc, b = a + sc, s = 0;
      int k0 = (int)a, k1 = (int)ceil(b);
      if (k1 > n) k1 = n;
      for (int k = k0; k < k1; k++) {
        double lo = k > a ? k : a, hi = (k + 1 < b) ? k + 1 : b;
        if (hi > lo) s += in[k * is] * (hi - lo);
      }
      out[i * os] = (float)(s / sc);
    }
  } else {
    for (int i = 0; i < m; i++) {
      double p = (i + 0.5) * sc - 0.5;
      if (p < 0) p = 0;
      int k = (int)p; if (k > n - 1) k = n - 1;
      int k2 = (k + 1 < n) ? k + 1 : k;
      double t = p - k;
      out[i * os] = (float)(in[k * is] * (1 - t) + in[k2 * is] * t);
    }
  }
}

static float* img_scale(const float* src, int w, int h, int c, int nw, int nh) {
  float* tmp = malloc(sizeof(float) * nw * h * c);
  float* dst = malloc(sizeof(float) * nw * nh * c);
  if (!tmp || !dst) { free(tmp); free(dst); return NULL; }
  for (int y = 0; y < h; y++)
    for (int ch = 0; ch < c; ch++)
      rs(src + (long)y * w * c + ch, c, w, tmp + (long)y * nw * c + ch, c, nw);
  for (int x = 0; x < nw; x++)
    for (int ch = 0; ch < c; ch++)
      rs(tmp + x * c + ch, (long)nw * c, h, dst + x * c + ch, (long)nw * c, nh);
  free(tmp);
  return dst;
}

GLint gluScaleImage(GLenum format, GLsizei wIn, GLsizei hIn, GLenum typeIn, const void* dataIn,
                    GLsizei wOut, GLsizei hOut, GLenum typeOut, GLvoid* dataOut) {
  int c = fcomps(format);
  if (!c || typeIn != GL_UNSIGNED_BYTE || typeOut != GL_UNSIGNED_BYTE) return GLU_INVALID_ENUM;
  if (wIn <= 0 || hIn <= 0 || wOut <= 0 || hOut <= 0 || !dataIn || !dataOut) return GLU_INVALID_VALUE;
  GLint ua = 4, pa = 4;
  glGetIntegerv(GL_UNPACK_ALIGNMENT, &ua);
  glGetIntegerv(GL_PACK_ALIGNMENT, &pa);
  float* src = img_load((const GLubyte*)dataIn, wIn, hIn, c, ua);
  if (!src) return GLU_OUT_OF_MEMORY;
  float* dst = img_scale(src, wIn, hIn, c, wOut, hOut);
  free(src);
  if (!dst) return GLU_OUT_OF_MEMORY;
  img_store(dst, (GLubyte*)dataOut, wOut, hOut, c, pa);
  free(dst);
  return 0;
}

GLint gluBuild2DMipmapLevels(GLenum target, GLint ifmt, GLsizei w, GLsizei h, GLenum format,
                             GLenum type, GLint level, GLint base, GLint max, const void* data) {
  int c = fcomps(format);
  if (!c || type != GL_UNSIGNED_BYTE) return GLU_INVALID_ENUM;
  if (w <= 0 || h <= 0 || !data || level < 0 || base < level || max < base) return GLU_INVALID_VALUE;
  GLint ua = 4;
  glGetIntegerv(GL_UNPACK_ALIGNMENT, &ua);
  float* cur = img_load((const GLubyte*)data, w, h, c, ua);
  if (!cur) return GLU_OUT_OF_MEMORY;
  GLubyte* buf = malloc((size_t)w * h * c);
  if (!buf) { free(cur); return GLU_OUT_OF_MEMORY; }
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  int cw = w, ch = h;
  for (int l = level; l <= max; l++) {
    if (l >= base) {
      img_store(cur, buf, cw, ch, c, 1);
      glTexImage2D(target, l, ifmt, cw, ch, 0, format, GL_UNSIGNED_BYTE, buf);
    }
    if (l == max) break;
    int nw = cw > 1 ? cw / 2 : 1, nh = ch > 1 ? ch / 2 : 1;
    float* nx = img_scale(cur, cw, ch, c, nw, nh);
    free(cur);
    if (!nx) { free(buf); glPixelStorei(GL_UNPACK_ALIGNMENT, ua); return GLU_OUT_OF_MEMORY; }
    cur = nx; cw = nw; ch = nh;
  }
  free(cur); free(buf);
  glPixelStorei(GL_UNPACK_ALIGNMENT, ua);
  return 0;
}

GLint gluBuild2DMipmaps(GLenum target, GLint ifmt, GLsizei w, GLsizei h, GLenum format,
                        GLenum type, const void* data) {
  int m = w > h ? w : h, lv = 0;
  while (m > 1) { m >>= 1; lv++; }
  return gluBuild2DMipmapLevels(target, ifmt, w, h, format, type, 0, 0, lv, data);
}

/* ---- gluPartialDisk ---- */

void gluPartialDisk(GLUquadric* q, D inner, D outer, GLint sl, GLint loops, D start, D sweep) {
  if (!q) return;
  if (sl < 2 || loops < 1 || outer <= 0 || inner < 0 || inner > outer) { qerr(q, GLU_INVALID_VALUE); return; }
  if (sweep > 360) sweep = 360;
  if (sweep < -360) sweep = -360;
  if (sweep < 0) { start += sweep; sweep = -sweep; }
  D nz = (q->orient == GLU_INSIDE) ? -1 : 1;
  D a0 = start * M_PI / 180, da = sweep * M_PI / 180 / sl, dr = (outer - inner) / loops;
  if (q->draw == GLU_FILL) {
    for (int i = 0; i < loops; i++) {
      D r1 = inner + i * dr, r2 = r1 + dr;
      D ra = (nz > 0) ? r2 : r1, rb = (nz > 0) ? r1 : r2;
      glBegin(GL_TRIANGLE_STRIP);
      for (int j = 0; j <= sl; j++) {
        D a = a0 + j * da, sa = sin(a), ca = cos(a);
        qv(q, 0, 0, nz, 0.5 + sa * ra / outer / 2, 0.5 + ca * ra / outer / 2, ra * sa, ra * ca, 0);
        qv(q, 0, 0, nz, 0.5 + sa * rb / outer / 2, 0.5 + ca * rb / outer / 2, rb * sa, rb * ca, 0);
      }
      glEnd();
    }
  } else if (q->draw == GLU_POINT) {
    glBegin(GL_POINTS);
    for (int i = 0; i <= loops; i++)
      for (int j = 0; j <= sl; j++) {
        D rr = inner + i * dr, a = a0 + j * da;
        qv(q, 0, 0, nz, 0, 0, rr * sin(a), rr * cos(a), 0);
      }
    glEnd();
  } else {
    int sil = (q->draw == GLU_SILHOUETTE);
    for (int i = 0; i <= loops; i++) {
      if (sil && i != 0 && i != loops) continue;
      if (sil && i == 0 && inner == 0) continue;
      D rr = inner + i * dr;
      glBegin(GL_LINE_STRIP);
      for (int j = 0; j <= sl; j++) {
        D a = a0 + j * da;
        qv(q, 0, 0, nz, 0, 0, rr * sin(a), rr * cos(a), 0);
      }
      glEnd();
    }
    glBegin(GL_LINES);
    for (int j = 0; j <= sl; j++) {
      if (sil && j != 0 && j != sl) continue;
      D a = a0 + j * da;
      qv(q, 0, 0, nz, 0, 0, inner * sin(a), inner * cos(a), 0);
      qv(q, 0, 0, nz, 0, 0, outer * sin(a), outer * cos(a), 0);
    }
    glEnd();
  }
}

/* ---- gluUnProject4 ---- */

GLint gluUnProject4(D wx, D wy, D wz, D clipW, const D* mv, const D* pr, const GLint* vp,
                    D nearV, D farV, D* ox, D* oy, D* oz, D* ow) {
  D m[16], inv[16], in[4], o[4];
  mmul(pr, mv, m);
  if (!minv(m, inv)) return GL_FALSE;
  in[0] = (wx - vp[0]) / vp[2] * 2 - 1;
  in[1] = (wy - vp[1]) / vp[3] * 2 - 1;
  in[2] = (wz - nearV) / (farV - nearV) * 2 - 1;
  in[3] = clipW;
  xform(inv, in, o);
  if (o[3] == 0) return GL_FALSE;
  *ox = o[0]; *oy = o[1]; *oz = o[2]; *ow = o[3];
  return GL_TRUE;
}

/* ---- Mipmaps 1D / 3D (GL_UNSIGNED_BYTE) ---- */

extern void (*glXGetProcAddressARB(const GLubyte*))(void);
typedef void (*TexImage3DFn)(GLenum, GLint, GLint, GLsizei, GLsizei, GLsizei, GLint, GLenum, GLenum, const void*);

static int ilevels(int m) { int l = 0; while (m > 1) { m >>= 1; l++; } return l; }

GLint gluBuild1DMipmapLevels(GLenum target, GLint ifmt, GLsizei w, GLenum format, GLenum type,
                             GLint level, GLint base, GLint max, const void* data) {
  int c = fcomps(format);
  if (!c || type != GL_UNSIGNED_BYTE) return GLU_INVALID_ENUM;
  if (w <= 0 || !data || level < 0 || base < level || max < base) return GLU_INVALID_VALUE;
  GLint ua = 4;
  glGetIntegerv(GL_UNPACK_ALIGNMENT, &ua);
  float* cur = img_load((const GLubyte*)data, w, 1, c, ua);
  if (!cur) return GLU_OUT_OF_MEMORY;
  GLubyte* buf = malloc((size_t)w * c);
  if (!buf) { free(cur); return GLU_OUT_OF_MEMORY; }
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  int cw = w;
  for (int l = level; l <= max; l++) {
    if (l >= base) {
      img_store(cur, buf, cw, 1, c, 1);
      glTexImage1D(target, l, ifmt, cw, 0, format, GL_UNSIGNED_BYTE, buf);
    }
    if (l == max) break;
    int nw = cw > 1 ? cw / 2 : 1;
    float* nx = img_scale(cur, cw, 1, c, nw, 1);
    free(cur);
    if (!nx) { free(buf); glPixelStorei(GL_UNPACK_ALIGNMENT, ua); return GLU_OUT_OF_MEMORY; }
    cur = nx; cw = nw;
  }
  free(cur); free(buf);
  glPixelStorei(GL_UNPACK_ALIGNMENT, ua);
  return 0;
}

GLint gluBuild1DMipmaps(GLenum target, GLint ifmt, GLsizei w, GLenum format, GLenum type, const void* data) {
  return gluBuild1DMipmapLevels(target, ifmt, w, format, type, 0, 0, ilevels(w), data);
}

/* volum: [z][y][x][canal] */
static float* vol_load(const GLubyte* d, int w, int h, int dp, int c, int al) {
  size_t row = ((size_t)w * c + al - 1) / al * al;
  float* f = malloc(sizeof(float) * (size_t)w * h * dp * c);
  if (!f) return NULL;
  for (int z = 0; z < dp; z++)
    for (int y = 0; y < h; y++)
      for (int i = 0; i < w * c; i++)
        f[((size_t)z * h + y) * w * c + i] = d[((size_t)z * h + y) * row + i];
  return f;
}

static void vol_store(const float* f, GLubyte* d, size_t n) {
  for (size_t i = 0; i < n; i++) {
    float v = f[i] + 0.5f;
    d[i] = (GLubyte)(v < 0 ? 0 : v > 255 ? 255 : v);
  }
}

static float* vol_scale(const float* src, int w, int h, int dp, int c, int nw, int nh, int nd) {
  float* tmp = malloc(sizeof(float) * (size_t)nw * nh * dp * c);
  if (!tmp) return NULL;
  for (int z = 0; z < dp; z++) {
    float* s = img_scale(src + (size_t)z * w * h * c, w, h, c, nw, nh);
    if (!s) { free(tmp); return NULL; }
    memcpy(tmp + (size_t)z * nw * nh * c, s, sizeof(float) * (size_t)nw * nh * c);
    free(s);
  }
  if (nd == dp) return tmp;
  size_t plane = (size_t)nw * nh * c;
  float* dst = malloc(sizeof(float) * plane * nd);
  if (!dst) { free(tmp); return NULL; }
  for (size_t i = 0; i < plane; i++) rs(tmp + i, (long)plane, dp, dst + i, (long)plane, nd);
  free(tmp);
  return dst;
}

GLint gluBuild3DMipmapLevels(GLenum target, GLint ifmt, GLsizei w, GLsizei h, GLsizei dp, GLenum format,
                             GLenum type, GLint level, GLint base, GLint max, const void* data) {
  int c = fcomps(format);
  if (!c || type != GL_UNSIGNED_BYTE) return GLU_INVALID_ENUM;
  if (w <= 0 || h <= 0 || dp <= 0 || !data || level < 0 || base < level || max < base) return GLU_INVALID_VALUE;
  TexImage3DFn tex3d = (TexImage3DFn)glXGetProcAddressARB((const GLubyte*)"glTexImage3D");
  if (!tex3d) return GLU_INVALID_OPERATION;
  GLint ua = 4;
  glGetIntegerv(GL_UNPACK_ALIGNMENT, &ua);
  float* cur = vol_load((const GLubyte*)data, w, h, dp, c, ua);
  if (!cur) return GLU_OUT_OF_MEMORY;
  GLubyte* buf = malloc((size_t)w * h * dp * c);
  if (!buf) { free(cur); return GLU_OUT_OF_MEMORY; }
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  int cw = w, ch = h, cd = dp;
  for (int l = level; l <= max; l++) {
    if (l >= base) {
      vol_store(cur, buf, (size_t)cw * ch * cd * c);
      tex3d(target, l, ifmt, cw, ch, cd, 0, format, GL_UNSIGNED_BYTE, buf);
    }
    if (l == max) break;
    int nw = cw > 1 ? cw / 2 : 1, nh = ch > 1 ? ch / 2 : 1, nd = cd > 1 ? cd / 2 : 1;
    float* nx = vol_scale(cur, cw, ch, cd, c, nw, nh, nd);
    free(cur);
    if (!nx) { free(buf); glPixelStorei(GL_UNPACK_ALIGNMENT, ua); return GLU_OUT_OF_MEMORY; }
    cur = nx; cw = nw; ch = nh; cd = nd;
  }
  free(cur); free(buf);
  glPixelStorei(GL_UNPACK_ALIGNMENT, ua);
  return 0;
}

GLint gluBuild3DMipmaps(GLenum target, GLint ifmt, GLsizei w, GLsizei h, GLsizei d, GLenum format,
                        GLenum type, const void* data) {
  int m = w > h ? w : h; if (d > m) m = d;
  return gluBuild3DMipmapLevels(target, ifmt, w, h, d, format, type, 0, 0, ilevels(m), data);
}
