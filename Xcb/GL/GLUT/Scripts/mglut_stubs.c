/* GLUT functions declared in glut.h, but without real support here: they exist to prevent
   "undefined symbol" errors. They do NOT do anything useful (see comments). */
#include <GL/glut.h>

/* --- menus: unimplemented (created, but do not appear on screen) --- */
static int menu_id=0;
int  glutCreateMenu(void(*f)(int)){ (void)f; return ++menu_id; }
void glutDestroyMenu(int m){ (void)m; }
int  glutGetMenu(void){ return menu_id; }
void glutSetMenu(int m){ (void)m; }
void glutAddMenuEntry(const char*l,int v){ (void)l;(void)v; }
void glutAddSubMenu(const char*l,int s){ (void)l;(void)s; }
void glutChangeToMenuEntry(int i,const char*l,int v){ (void)i;(void)l;(void)v; }
void glutChangeToSubMenu(int i,const char*l,int s){ (void)i;(void)l;(void)s; }
void glutRemoveMenuItem(int i){ (void)i; }
void glutAttachMenu(int b){ (void)b; }
void glutDetachMenu(int b){ (void)b; }
void glutMenuStateFunc(void(*f)(int)){ (void)f; }
void glutMenuStatusFunc(void(*f)(int,int,int)){ (void)f; }

/* --- overlay --- */
void glutEstablishOverlay(void){}   void glutRemoveOverlay(void){}
void glutUseLayer(GLenum l){ (void)l; }
void glutPostOverlayRedisplay(void){} void glutPostWindowOverlayRedisplay(int w){ (void)w; }
void glutShowOverlay(void){}        void glutHideOverlay(void){}
void glutOverlayDisplayFunc(void(*f)(void)){ (void)f; }
int  glutLayerGet(GLenum t){ (void)t; return 0; }

/* --- color index --- */
void glutSetColor(int n,GLfloat r,GLfloat g,GLfloat b){ (void)n;(void)r;(void)g;(void)b; }
GLfloat glutGetColor(int n,int c){ (void)n;(void)c; return 0; }
void glutCopyColormap(int w){ (void)w; }

/* --- missing input devices --- */
void glutSpaceballMotionFunc(void(*f)(int,int,int)){ (void)f; }
void glutSpaceballRotateFunc(void(*f)(int,int,int)){ (void)f; }
void glutSpaceballButtonFunc(void(*f)(int,int)){ (void)f; }
void glutButtonBoxFunc(void(*f)(int,int)){ (void)f; }
void glutDialsFunc(void(*f)(int,int)){ (void)f; }
void glutTabletMotionFunc(void(*f)(int,int)){ (void)f; }
void glutTabletButtonFunc(void(*f)(int,int,int,int)){ (void)f; }
void glutJoystickFunc(void(*f)(unsigned int,int,int,int),int p){ (void)f;(void)p; }
void glutForceJoystickFunc(void){}
void glutWindowStatusFunc(void(*f)(int)){ (void)f; }

/* --- video resize --- */
int  glutVideoResizeGet(GLenum p){ (void)p; return 0; }
void glutSetupVideoResizing(void){}  void glutStopVideoResizing(void){}
void glutVideoResize(int x,int y,int w,int h){ (void)x;(void)y;(void)w;(void)h; }
void glutVideoPan(int x,int y,int w,int h){ (void)x;(void)y;(void)w;(void)h; }

/* --- stroke fonts: unimplemented (do not draw anything) --- */
void *glutStrokeRoman, *glutStrokeMonoRoman;
void glutStrokeCharacter(void*f,int c){ (void)f;(void)c; }
int  glutStrokeWidth(void*f,int c){ (void)f;(void)c; return 0; }
int  glutStrokeLength(void*f,const unsigned char*s){ (void)f;(void)s; return 0; }
