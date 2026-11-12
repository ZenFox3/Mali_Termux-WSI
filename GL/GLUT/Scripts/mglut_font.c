/* GLUT bitmap fonts (glutBitmapCharacter/Width/Length). Data in mglut_fontdata.h */
#include "glut.h"

typedef struct {
  void **id; int h, desc;
  const unsigned char *data; const unsigned short *off;
  const unsigned char *cw, *adv;
} BFont;

void *glutBitmap9By15, *glutBitmap8By13, *glutBitmapTimesRoman10,
     *glutBitmapTimesRoman24, *glutBitmapHelvetica10,
     *glutBitmapHelvetica12, *glutBitmapHelvetica18;

#include "mglut_fontdata.h"

static const BFont *findf(void *font){
  for(unsigned i=0;i<sizeof(BFONTS)/sizeof(BFONTS[0]);i++)
    if((void*)BFONTS[i].id==font) return &BFONTS[i];
  return 0;
}

void glutBitmapCharacter(void *font,int c){
  const BFont *f=findf(font); if(!f||c<32||c>126) return;
  int i=c-32; GLint al=4;
  glGetIntegerv(GL_UNPACK_ALIGNMENT,&al);
  glPixelStorei(GL_UNPACK_ALIGNMENT,1);
  glBitmap(f->cw[i],f->h,0,(GLfloat)f->desc,(GLfloat)f->adv[i],0,f->data+f->off[i]);
  glPixelStorei(GL_UNPACK_ALIGNMENT,al);
}
int glutBitmapWidth(void *font,int c){
  const BFont *f=findf(font); if(!f||c<32||c>126) return 0;
  return f->adv[c-32];
}
int glutBitmapLength(void *font,const unsigned char *s){
  int n=0; while(s&&*s) n+=glutBitmapWidth(font,*s++); return n;
}
