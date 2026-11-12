/* GLUT primitives: cube, sphere, cone, torus, polyhedra. Only glBegin/glEnd + f-variants. */
#include <math.h>
#include <stdlib.h>
#include "glut.h"

#define PI 3.14159265358979323846

/* ---------- cube ---------- */
static const signed char CF[6][4][3]={
 {{1,-1,-1},{1,1,-1},{1,1,1},{1,-1,1}},     {{-1,-1,1},{-1,1,1},{-1,1,-1},{-1,-1,-1}},
 {{-1,1,-1},{-1,1,1},{1,1,1},{1,1,-1}},     {{-1,-1,1},{-1,-1,-1},{1,-1,-1},{1,-1,1}},
 {{-1,-1,1},{1,-1,1},{1,1,1},{-1,1,1}},     {{-1,1,-1},{1,1,-1},{1,-1,-1},{-1,-1,-1}}};
static const signed char CN[6][3]={{1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1}};

static void cube(double size,int wire){
  float h=(float)size*0.5f;
  for(int f=0;f<6;f++){
    glBegin(wire?GL_LINE_LOOP:GL_TRIANGLES);
    glNormal3f(CN[f][0],CN[f][1],CN[f][2]);
    static const int tri[6]={0,1,2,0,2,3};
    for(int k=0;k<(wire?4:6);k++){
      int v=wire?k:tri[k];
      glVertex3f(CF[f][v][0]*h,CF[f][v][1]*h,CF[f][v][2]*h);
    }
    glEnd();
  }
}
void glutSolidCube(GLdouble s){ cube(s,0); }
void glutWireCube(GLdouble s){ cube(s,1); }

/* ---------- sphere ---------- */
static void sphere(double r,int sl,int st,int wire){
  if(sl<3) sl=3; if(st<2) st=2;
  float *ct=malloc(sizeof(float)*(sl+1)*2), *sn=ct+(sl+1);
  if(!ct) return;
  for(int j=0;j<=sl;j++){ double t=(j==sl)?0:2*PI*j/sl; ct[j]=(float)cos(t); sn[j]=(float)sin(t); }
  if(!wire){
    for(int i=0;i<st;i++){
      float p0=(float)(PI*i/st), p1=(float)(PI*(i+1)/st);
      float z0=cosf(p0), r0=sinf(p0), z1=cosf(p1), r1=sinf(p1);
      glBegin(GL_TRIANGLE_STRIP);
      for(int j=0;j<=sl;j++){
        glNormal3f(ct[j]*r0,sn[j]*r0,z0); glVertex3f((float)r*ct[j]*r0,(float)r*sn[j]*r0,(float)r*z0);
        glNormal3f(ct[j]*r1,sn[j]*r1,z1); glVertex3f((float)r*ct[j]*r1,(float)r*sn[j]*r1,(float)r*z1);
      }
      glEnd();
    }
  } else {
    for(int i=1;i<st;i++){
      float p=(float)(PI*i/st), z=cosf(p), rr=sinf(p);
      glBegin(GL_LINE_LOOP);
      for(int j=0;j<sl;j++){ glNormal3f(ct[j]*rr,sn[j]*rr,z);
        glVertex3f((float)r*ct[j]*rr,(float)r*sn[j]*rr,(float)r*z); }
      glEnd();
    }
    for(int j=0;j<sl;j++){
      glBegin(GL_LINE_STRIP);
      for(int i=0;i<=st;i++){ float p=(float)(PI*i/st), z=cosf(p), rr=sinf(p);
        glNormal3f(ct[j]*rr,sn[j]*rr,z);
        glVertex3f((float)r*ct[j]*rr,(float)r*sn[j]*rr,(float)r*z); }
      glEnd();
    }
  }
  free(ct);
}
void glutSolidSphere(GLdouble r,GLint sl,GLint st){ sphere(r,sl,st,0); }
void glutWireSphere(GLdouble r,GLint sl,GLint st){ sphere(r,sl,st,1); }

/* ---------- cone (base at z=0, apex at z=height) ---------- */
static void cone(double base,double h,int sl,int st,int wire){
  if(sl<3) sl=3; if(st<1) st=1;
  double len=sqrt(base*base+h*h); if(len<=0) return;
  float nr=(float)(h/len), nz=(float)(base/len);
  float *ct=malloc(sizeof(float)*(sl+1)*2), *sn=ct+(sl+1);
  if(!ct) return;
  for(int j=0;j<=sl;j++){ double t=(j==sl)?0:2*PI*j/sl; ct[j]=(float)cos(t); sn[j]=(float)sin(t); }
  if(!wire){
    for(int i=0;i<st;i++){
      float t0=(float)i/st, t1=(float)(i+1)/st;
      float z0=(float)h*t0, r0=(float)base*(1-t0), z1=(float)h*t1, r1=(float)base*(1-t1);
      glBegin(GL_TRIANGLE_STRIP);
      for(int j=0;j<=sl;j++){
        glNormal3f(ct[j]*nr,sn[j]*nr,nz); glVertex3f(ct[j]*r1,sn[j]*r1,z1);
        glNormal3f(ct[j]*nr,sn[j]*nr,nz); glVertex3f(ct[j]*r0,sn[j]*r0,z0);
      }
      glEnd();
    }
    glBegin(GL_TRIANGLE_FAN); glNormal3f(0,0,-1); glVertex3f(0,0,0);
    for(int j=sl;j>=0;j--) glVertex3f((float)base*ct[j],(float)base*sn[j],0);
    glEnd();
  } else {
    for(int i=0;i<st;i++){
      float t=(float)i/st, z=(float)h*t, rr=(float)base*(1-t);
      glBegin(GL_LINE_LOOP);
      for(int j=0;j<sl;j++){ glNormal3f(ct[j]*nr,sn[j]*nr,nz); glVertex3f(ct[j]*rr,sn[j]*rr,z); }
      glEnd();
    }
    glBegin(GL_LINES);
    for(int j=0;j<sl;j++){ glNormal3f(ct[j]*nr,sn[j]*nr,nz);
      glVertex3f((float)base*ct[j],(float)base*sn[j],0); glVertex3f(0,0,(float)h); }
    glEnd();
  }
  free(ct);
}
void glutSolidCone(GLdouble b,GLdouble h,GLint sl,GLint st){ cone(b,h,sl,st,0); }
void glutWireCone(GLdouble b,GLdouble h,GLint sl,GLint st){ cone(b,h,sl,st,1); }

/* ---------- torus (Z-axis): inner = tube radius, outer = ring radius, as in original GLUT ---------- */
static void torus(double r,double R,int ns,int nr,int wire){
  if(ns<3) ns=3; if(nr<3) nr=3;
#define TP(a,b) do{ float ca=cosf(a),sa=sinf(a),cb=cosf(b),sb=sinf(b); \
    glNormal3f(cb*ca,cb*sa,sb); \
    glVertex3f(((float)R+(float)r*cb)*ca,((float)R+(float)r*cb)*sa,(float)r*sb); }while(0)
  if(!wire){
    for(int i=0;i<nr;i++){
      float a0=(float)(2*PI*i/nr), a1=(float)(2*PI*(i+1)/nr);
      glBegin(GL_TRIANGLE_STRIP);
      for(int j=0;j<=ns;j++){ float b=(float)(2*PI*j/ns); TP(a0,b); TP(a1,b); }
      glEnd();
    }
  } else {
    for(int i=0;i<nr;i++){ float a=(float)(2*PI*i/nr);
      glBegin(GL_LINE_LOOP); for(int j=0;j<ns;j++){ float b=(float)(2*PI*j/ns); TP(a,b);} glEnd(); }
    for(int j=0;j<ns;j++){ float b=(float)(2*PI*j/ns);
      glBegin(GL_LINE_LOOP); for(int i=0;i<nr;i++){ float a=(float)(2*PI*i/nr); TP(a,b);} glEnd(); }
  }
#undef TP
}
void glutSolidTorus(GLdouble r,GLdouble R,GLint ns,GLint nr){ torus(r,R,ns,nr,0); }
void glutWireTorus(GLdouble r,GLdouble R,GLint ns,GLint nr){ torus(r,R,ns,nr,1); }

/* ---------- polyhedra (centered at the origin) ---------- */
/* convex face; winding order is automatically corrected to point outward */
static void face(const float (*v)[3],const int *ix,int n,int wire){
  float c[3]={0,0,0};
  for(int k=0;k<n;k++) for(int a=0;a<3;a++) c[a]+=v[ix[k]][a]/n;
  const float *p0=v[ix[0]],*p1=v[ix[1]],*p2=v[ix[2]];
  float a[3]={p1[0]-p0[0],p1[1]-p0[1],p1[2]-p0[2]}, b[3]={p2[0]-p0[0],p2[1]-p0[1],p2[2]-p0[2]};
  float nn[3]={a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0]};
  int rev=(nn[0]*c[0]+nn[1]*c[1]+nn[2]*c[2])<0;
  float l=sqrtf(nn[0]*nn[0]+nn[1]*nn[1]+nn[2]*nn[2]); if(l<=0) return;
  float s=(rev?-1.f:1.f)/l;
  glBegin(wire?GL_LINE_LOOP:(n==3?GL_TRIANGLES:GL_TRIANGLE_FAN));
  glNormal3f(nn[0]*s,nn[1]*s,nn[2]*s);
  for(int k=0;k<n;k++){ int q=rev?ix[n-1-k]:ix[k]; glVertex3f(v[q][0],v[q][1],v[q][2]); }
  glEnd();
}

static void tetra(int wire){
  static const float V[4][3]={{1,1,1},{1,-1,-1},{-1,1,-1},{-1,-1,1}};
  static const int F[4][3]={{1,2,3},{0,2,3},{0,1,3},{0,1,2}};
  for(int i=0;i<4;i++) face(V,F[i],3,wire);
}
static void octa(int wire){
  static const float V[6][3]={{1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1}};
  for(int sx=0;sx<2;sx++) for(int sy=2;sy<4;sy++) for(int sz=4;sz<6;sz++){
    int f[3]={sx,sy,sz}; face(V,f,3,wire); }
}

#define PHI 1.61803398874989484820f
static float IV[12][3]; static int IF[20][3]; static int ico_ok=0;
static void ico_init(void){
  if(ico_ok) return;
  int n=0; float k=1.f/sqrtf(1+PHI*PHI);
  for(int a=-1;a<=1;a+=2) for(int b=-1;b<=1;b+=2){
    float p[3][3]={{0,(float)a,b*PHI},{(float)a,b*PHI,0},{b*PHI,0,(float)a}};
    for(int q=0;q<3;q++){ for(int c=0;c<3;c++) IV[n][c]=p[q][c]*k; n++; }
  }
  int nf=0; float e2=4*k*k;
  for(int i=0;i<12;i++) for(int j=i+1;j<12;j++) for(int l=j+1;l<12;l++){
    int t[3]={i,j,l}; int ok=1;
    for(int x=0;x<3&&ok;x++){ int u=t[x],w=t[(x+1)%3]; float d=0;
      for(int c=0;c<3;c++){ float df=IV[u][c]-IV[w][c]; d+=df*df; }
      if(fabsf(d-e2)>1e-3f) ok=0; }
    if(ok&&nf<20){ IF[nf][0]=i; IF[nf][1]=j; IF[nf][2]=l; nf++; }
  }
  ico_ok=1;
}
static void icosa(int wire){ ico_init(); for(int i=0;i<20;i++) face(IV,IF[i],3,wire); }

static float DV[20][3]; static int DF[12][5]; static int dod_ok=0;
static void dod_init(void){
  if(dod_ok) return; ico_init();
  int n=0; float ip=1.f/PHI;
  for(int x=-1;x<=1;x+=2) for(int y=-1;y<=1;y+=2) for(int z=-1;z<=1;z+=2){
    DV[n][0]=x; DV[n][1]=y; DV[n][2]=z; n++; }
  for(int a=-1;a<=1;a+=2) for(int b=-1;b<=1;b+=2){
    float p[3][3]={{0,a*PHI,b*ip},{a*ip,0,b*PHI},{a*PHI,b*ip,0}};
    for(int q=0;q<3;q++){ for(int c=0;c<3;c++) DV[n][c]=p[q][c]; n++; }
  }
  /* dodecahedron faces = dual icosahedron vertex directions: 5 vertices with maximum dot product */
  for(int f=0;f<12;f++){
    const float *d=IV[f]; int pick[5]; float best[5]; int cnt=0;
    float dd[20]; for(int i=0;i<20;i++) dd[i]=d[0]*DV[i][0]+d[1]*DV[i][1]+d[2]*DV[i][2];
    float mx=-1e9f; for(int i=0;i<20;i++) if(dd[i]>mx) mx=dd[i];
    for(int i=0;i<20&&cnt<5;i++) if(dd[i]>mx-1e-3f){ pick[cnt]=i; cnt++; }
    if(cnt<5){ for(int i=0;i<5;i++) DF[f][i]=pick[i<cnt?i:0]; continue; }
    /* sort by angle around d */
    float u[3]; if(fabsf(d[0])<0.9f){u[0]=0;u[1]=-d[2];u[2]=d[1];} else {u[0]=d[2];u[1]=0;u[2]=-d[0];}
    float ul=sqrtf(u[0]*u[0]+u[1]*u[1]+u[2]*u[2]); for(int c=0;c<3;c++) u[c]/=ul;
    float w[3]={d[1]*u[2]-d[2]*u[1],d[2]*u[0]-d[0]*u[2],d[0]*u[1]-d[1]*u[0]};
    for(int i=0;i<5;i++){ const float *p=DV[pick[i]];
      best[i]=atan2f(p[0]*w[0]+p[1]*w[1]+p[2]*w[2],p[0]*u[0]+p[1]*u[1]+p[2]*u[2]); }
    for(int i=1;i<5;i++){ float bk=best[i]; int pk=pick[i]; int j=i-1;
      while(j>=0&&best[j]>bk){ best[j+1]=best[j]; pick[j+1]=pick[j]; j--; }
      best[j+1]=bk; pick[j+1]=pk; }
    for(int i=0;i<5;i++) DF[f][i]=pick[i];
  }
  dod_ok=1;
}
static void dodeca(int wire){ dod_init(); for(int i=0;i<12;i++) face(DV,DF[i],5,wire); }

void glutSolidTetrahedron(void){ tetra(0); }   void glutWireTetrahedron(void){ tetra(1); }
void glutSolidOctahedron(void){ octa(0); }     void glutWireOctahedron(void){ octa(1); }
void glutSolidIcosahedron(void){ icosa(0); }   void glutWireIcosahedron(void){ icosa(1); }
void glutSolidDodecahedron(void){ dodeca(0); } void glutWireDodecahedron(void){ dodeca(1); }
