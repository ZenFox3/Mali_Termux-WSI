/* mglut v2: GLUT implementation over Xlib + GLX. Single window support. */
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/XKBlib.h>
#include <X11/keysym.h>
#include <X11/cursorfont.h>
#include <GL/glx.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/time.h>
#include <sys/select.h>
#include <GL/glut.h>

static Display *dpy; static Window win; static GLXContext ctx; static int scr;
static XVisualInfo *vinfo;
static unsigned display_mode; static int W=300, H=300, PX=0, PY=0, WX=0, WY=0;
static int redisplay_needed=1, running=1, ignore_repeat=0, gamemode_active=0;
static int got_alpha=0, got_stencil=0, got_depth=0, got_accum=0;
static int modifiers=0, mouse_x=0, mouse_y=0;
static Atom wm_delete_window;
static unsigned char key_down_states[256];

/* Event callback function pointers */
static void (*f_disp)(void), (*f_idle)(void);
static void (*f_resh)(int,int);
static void (*f_key)(unsigned char,int,int), (*f_keyup)(unsigned char,int,int);
static void (*f_spec)(int,int,int), (*f_specup)(int,int,int);
static void (*f_mouse)(int,int,int,int);
static void (*f_motion)(int,int), (*f_pmotion)(int,int);
static void (*f_vis)(int), (*f_entry)(int);

#define MAX_TIMERS 256
static struct { long target_time; void (*func)(int); int value; int used; } Timers[MAX_TIMERS];
static long start_time_ms;

static long get_now_ms(void) {
  struct timeval tv;
  gettimeofday(&tv, NULL);
  return tv.tv_sec * 1000L + tv.tv_usec / 1000;
}

void glutInit(int *argc, char **argv) {
  (void)argc; (void)argv;
  dpy = XOpenDisplay(NULL);
  if (!dpy) {
    fputs("mglut: failed to open X display (check your DISPLAY variable)\n", stderr);
    exit(1);
  }
  scr = DefaultScreen(dpy);
  Bool supported;
  XkbSetDetectableAutoRepeat(dpy, True, &supported);
  start_time_ms = get_now_ms();
}

void glutInitDisplayMode(unsigned mode) { display_mode = mode; }

void glutInitDisplayString(const char *str) {
  unsigned mode = 0;
  if (!str) return;
  if (strstr(str, "double"))  mode |= GLUT_DOUBLE;
  if (strstr(str, "depth"))   mode |= GLUT_DEPTH;
  if (strstr(str, "alpha"))   mode |= GLUT_ALPHA;
  if (strstr(str, "stencil")) mode |= GLUT_STENCIL;
  if (strstr(str, "accum"))   mode |= GLUT_ACCUM;
  display_mode = mode;
}

void glutInitWindowSize(int width, int height) { W = width; H = height; }
void glutInitWindowPosition(int x, int y) { PX = x; PY = y; }

/* Attempts increasingly permissive visual attributes; returns NULL if none are matching */
static XVisualInfo *pick_visual(unsigned mode, int *has_alpha, int *has_stencil, int *has_depth, int *has_accum) {
  for (int level = 0; level < 3; level++) {
    int attribs[32], n = 0;
    attribs[n++] = GLX_RGBA;
    attribs[n++] = GLX_RED_SIZE; attribs[n++] = 8;
    attribs[n++] = GLX_GREEN_SIZE; attribs[n++] = 8;
    attribs[n++] = GLX_BLUE_SIZE; attribs[n++] = 8;

    if (mode & GLUT_DOUBLE) attribs[n++] = GLX_DOUBLEBUFFER;
    int al = (mode & GLUT_ALPHA) && level < 2;
    int st = (mode & GLUT_STENCIL) && level < 1;
    int ac = (mode & GLUT_ACCUM) && level < 1;
    int de = (mode & GLUT_DEPTH) ? (level < 2 ? 16 : 1) : 0;

    if (al) { attribs[n++] = GLX_ALPHA_SIZE; attribs[n++] = 8; }
    if (st) { attribs[n++] = GLX_STENCIL_SIZE; attribs[n++] = 8; }
    if (ac) {
      attribs[n++] = GLX_ACCUM_RED_SIZE; attribs[n++] = 8;
      attribs[n++] = GLX_ACCUM_GREEN_SIZE; attribs[n++] = 8;
      attribs[n++] = GLX_ACCUM_BLUE_SIZE; attribs[n++] = 8;
    }
    if (de) { attribs[n++] = GLX_DEPTH_SIZE; attribs[n++] = de; }
    attribs[n] = None;

    XVisualInfo *vi = glXChooseVisual(dpy, scr, attribs);
    if (vi) {
      if (has_alpha)   *has_alpha = al ? 8 : 0;
      if (has_stencil) *has_stencil = st ? 8 : 0;
      if (has_depth)   *has_depth = de;
      if (has_accum)   *has_accum = ac ? 8 : 0;
      return vi;
    }
  }
  return NULL;
}

int glutCreateWindow(const char *title) {
  if (!dpy) { int dummy = 0; glutInit(&dummy, NULL); }
  vinfo = pick_visual(display_mode, &got_alpha, &got_stencil, &got_depth, &got_accum);
  if (!vinfo) {
    fputs("mglut: no suitable GLX visual found\n", stderr);
    exit(1);
  }
  XSetWindowAttributes sa;
  sa.colormap = XCreateColormap(dpy, RootWindow(dpy, scr), vinfo->visual, AllocNone);
  sa.border_pixel = 0;
  sa.event_mask = ExposureMask | StructureNotifyMask | KeyPressMask | KeyReleaseMask |
                  ButtonPressMask | ButtonReleaseMask | PointerMotionMask | EnterWindowMask | LeaveWindowMask;

  win = XCreateWindow(dpy, RootWindow(dpy, scr), PX, PY, W, H, 0, vinfo->depth,
                      InputOutput, vinfo->visual, CWColormap | CWBorderPixel | CWEventMask, &sa);

  XSizeHints sh; memset(&sh, 0, sizeof(sh));
  sh.flags = PSize | PPosition; sh.x = PX; sh.y = PY; sh.width = W; sh.height = H;
  XSetWMNormalHints(dpy, win, &sh);
  XStoreName(dpy, win, title); XSetIconName(dpy, win, title);

  wm_delete_window = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
  XSetWMProtocols(dpy, win, &wm_delete_window, 1);

  ctx = glXCreateContext(dpy, vinfo, NULL, True);
  if (!ctx) {
    fputs("mglut: glXCreateContext failed\n", stderr);
    exit(1);
  }

  XMapWindow(dpy, win);
  glXMakeCurrent(dpy, win, ctx);
  WX = PX; WY = PY; redisplay_needed = 1;
  return 1;
}

void glutSetWindowTitle(const char *title) { if (win) XStoreName(dpy, win, title); }
void glutSetIconTitle(const char *title)  { if (win) XSetIconName(dpy, win, title); }

void glutDestroyWindow(int window_id) {
  (void)window_id;
  if (!win) return;
  glXMakeCurrent(dpy, None, NULL);
  glXDestroyContext(dpy, ctx);
  XDestroyWindow(dpy, win);
  XFlush(dpy);
  win = 0; ctx = 0; running = 0;
}

int  glutGetWindow(void) { return win ? 1 : 0; }
void glutSetWindow(int window_id) { (void)window_id; }
int  glutCreateSubWindow(int parent, int x, int y, int width, int height) {
  (void)parent; (void)x; (void)y; (void)width; (void)height;
  return 0;
}

/* Callback Registration */
void glutDisplayFunc(void (*func)(void))                   { f_disp = func; }
void glutReshapeFunc(void (*func)(int, int))               { f_resh = func; }
void glutKeyboardFunc(void (*func)(unsigned char, int, int)) { f_key = func; }
void glutKeyboardUpFunc(void (*func)(unsigned char, int, int)) { f_keyup = func; }
void glutSpecialFunc(void (*func)(int, int, int))          { f_spec = func; }
void glutSpecialUpFunc(void (*func)(int, int, int))        { f_specup = func; }
void glutMouseFunc(void (*func)(int, int, int, int))       { f_mouse = func; }
void glutMotionFunc(void (*func)(int, int))                { f_motion = func; }
void glutPassiveMotionFunc(void (*func)(int, int))         { f_pmotion = func; }
void glutVisibilityFunc(void (*func)(int))                 { f_vis = func; }
void glutEntryFunc(void (*func)(int))                      { f_entry = func; }
void glutIdleFunc(void (*func)(void))                      { f_idle = func; }

void glutPostRedisplay(void) { redisplay_needed = 1; }
void glutPostWindowRedisplay(int window_id) { (void)window_id; redisplay_needed = 1; }

void glutSwapBuffers(void) {
  if (!win) return;
  if (display_mode & GLUT_DOUBLE) glXSwapBuffers(dpy, win); else glFlush();
}

void glutLeaveMainLoop(void) { running = 0; }
void glutIgnoreKeyRepeat(int ignore) { ignore_repeat = ignore; }
void glutSetKeyRepeat(int repeat_mode) { ignore_repeat = (repeat_mode == GLUT_KEY_REPEAT_OFF); }
int  glutGetModifiers(void) { return modifiers; }

void glutPositionWindow(int x, int y) { if (win) XMoveWindow(dpy, win, x, y); }
void glutReshapeWindow(int width, int height) { if (win) XResizeWindow(dpy, win, width, height); }
void glutShowWindow(void)   { if (win) XMapWindow(dpy, win); }
void glutHideWindow(void)   { if (win) XUnmapWindow(dpy, win); }
void glutIconifyWindow(void) { if (win) XIconifyWindow(dpy, win, scr); }
void glutPopWindow(void)    { if (win) XRaiseWindow(dpy, win); }
void glutPushWindow(void)   { if (win) XLowerWindow(dpy, win); }

void glutFullScreen(void) {
  if (!win) return;
  XEvent event; memset(&event, 0, sizeof(event));
  event.xclient.type = ClientMessage;
  event.xclient.window = win;
  event.xclient.format = 32;
  event.xclient.message_type = XInternAtom(dpy, "_NET_WM_STATE", False);
  event.xclient.data.l[0] = 1;
  event.xclient.data.l[1] = XInternAtom(dpy, "_NET_WM_STATE_FULLSCREEN", False);
  event.xclient.data.l[3] = 1;
  XSendEvent(dpy, RootWindow(dpy, scr), False, SubstructureRedirectMask | SubstructureNotifyMask, &event);

  /* Fallback for bare X servers without a window manager */
  XMoveResizeWindow(dpy, win, 0, 0, DisplayWidth(dpy, scr), DisplayHeight(dpy, scr));
  XFlush(dpy);
}

void glutWarpPointer(int x, int y) { if (win) XWarpPointer(dpy, None, win, 0, 0, 0, 0, x, y); }

void glutSetCursor(int cursor_type) {
  if (!win) return;
  if (cursor_type == GLUT_CURSOR_INHERIT) { XUndefineCursor(dpy, win); return; }
  if (cursor_type == GLUT_CURSOR_NONE) {
    static Cursor blank_cursor;
    if (!blank_cursor) {
      char dummy_data = 0; XColor col; memset(&col, 0, sizeof(col));
      Pixmap pm = XCreateBitmapFromData(dpy, win, &dummy_data, 1, 1);
      blank_cursor = XCreatePixmapCursor(dpy, pm, pm, &col, &col, 0, 0);
      XFreePixmap(dpy, pm);
    }
    XDefineCursor(dpy, win, blank_cursor);
    return;
  }
  static const unsigned short cursor_map[] = {
    XC_arrow, XC_left_ptr, XC_hand2, XC_pirate,
    XC_question_arrow, XC_exchange, XC_spraycan, XC_watch, XC_xterm, XC_crosshair,
    XC_sb_v_double_arrow, XC_sb_h_double_arrow, XC_top_side, XC_bottom_side, XC_left_side,
    XC_right_side, XC_top_left_corner, XC_top_right_corner, XC_bottom_right_corner,
    XC_bottom_left_corner
  };
  unsigned shape = (cursor_type >= 0 && cursor_type < 20) ? cursor_map[cursor_type] : XC_crosshair;
  XDefineCursor(dpy, win, XCreateFontCursor(dpy, shape));
}

int glutGet(GLenum query) {
  switch (query) {
    case GLUT_ELAPSED_TIME: return (int)(get_now_ms() - start_time_ms);
    case GLUT_WINDOW_X: return WX;
    case GLUT_WINDOW_Y: return WY;
    case GLUT_WINDOW_WIDTH: return W;
    case GLUT_WINDOW_HEIGHT: return H;
    case GLUT_WINDOW_RGBA: return 1;
    case GLUT_WINDOW_DOUBLEBUFFER: return (display_mode & GLUT_DOUBLE) ? 1 : 0;
    case GLUT_WINDOW_RED_SIZE:
    case GLUT_WINDOW_GREEN_SIZE:
    case GLUT_WINDOW_BLUE_SIZE: return 8;
    case GLUT_WINDOW_ALPHA_SIZE: return got_alpha;
    case GLUT_WINDOW_DEPTH_SIZE: return got_depth;
    case GLUT_WINDOW_STENCIL_SIZE: return got_stencil;
    case GLUT_WINDOW_ACCUM_RED_SIZE:
    case GLUT_WINDOW_ACCUM_GREEN_SIZE:
    case GLUT_WINDOW_ACCUM_BLUE_SIZE:
    case GLUT_WINDOW_ACCUM_ALPHA_SIZE: return got_accum;
    case GLUT_WINDOW_BUFFER_SIZE: return 8 * 3 + got_alpha;
    case GLUT_WINDOW_FORMAT_ID: return vinfo ? (int)vinfo->visualid : 0;
    case GLUT_SCREEN_WIDTH: return dpy ? DisplayWidth(dpy, scr) : 0;
    case GLUT_SCREEN_HEIGHT: return dpy ? DisplayHeight(dpy, scr) : 0;
    case GLUT_SCREEN_WIDTH_MM: return dpy ? DisplayWidthMM(dpy, scr) : 0;
    case GLUT_SCREEN_HEIGHT_MM: return dpy ? DisplayHeightMM(dpy, scr) : 0;
    case GLUT_INIT_WINDOW_X: return PX;
    case GLUT_INIT_WINDOW_Y: return PY;
    case GLUT_INIT_WINDOW_WIDTH: return W;
    case GLUT_INIT_WINDOW_HEIGHT: return H;
    case GLUT_INIT_DISPLAY_MODE: return (int)display_mode;
    case GLUT_DISPLAY_MODE_POSSIBLE: {
      if (!dpy) return 0;
      XVisualInfo *v = pick_visual(display_mode, 0, 0, 0, 0);
      if (v) { XFree(v); return 1; }
      return 0;
    }
    default: return 0;
  }
}

int glutDeviceGet(GLenum query) {
  switch (query) {
    case GLUT_HAS_KEYBOARD:
    case GLUT_HAS_MOUSE: return 1;
    case GLUT_NUM_MOUSE_BUTTONS: return 3;
    case GLUT_DEVICE_IGNORE_KEY_REPEAT: return ignore_repeat;
    default: return 0;
  }
}

int glutExtensionSupported(const char *name) {
  const char *extensions = (const char*)glGetString(GL_EXTENSIONS);
  if (!extensions || !name || !*name) return 0;
  size_t len = strlen(name);
  for (const char *p = extensions; (p = strstr(p, name)); p += len) {
    if ((p == extensions || p[-1] == ' ') && (p[len] == ' ' || p[len] == 0)) return 1;
  }
  return 0;
}

GLUTproc glutGetProcAddress(const char *name) {
  return (GLUTproc)dlsym(RTLD_DEFAULT, name);
}

void glutReportErrors(void) {
  GLenum err;
  while ((err = glGetError()) != GL_NO_ERROR) {
    fprintf(stderr, "GLUT: OpenGL error 0x%x\n", err);
  }
}

/* Simplified Game Mode: fullscreen window covering total screen resolution */
void glutGameModeString(const char *string) { (void)string; }

int glutEnterGameMode(void) {
  glutInitWindowPosition(0, 0);
  if (dpy) { W = DisplayWidth(dpy, scr); H = DisplayHeight(dpy, scr); }
  glutCreateWindow("mglut game mode");
  glutFullScreen();
  gamemode_active = 1;
  return 1;
}

void glutLeaveGameMode(void) {
  gamemode_active = 0;
  glutDestroyWindow(1);
}

int glutGameModeGet(GLenum query) {
  switch (query) {
    case GLUT_GAME_MODE_ACTIVE: return gamemode_active;
    case GLUT_GAME_MODE_POSSIBLE: return 1;
    case GLUT_GAME_MODE_WIDTH: return dpy ? DisplayWidth(dpy, scr) : 0;
    case GLUT_GAME_MODE_HEIGHT: return dpy ? DisplayHeight(dpy, scr) : 0;
    case GLUT_GAME_MODE_PIXEL_DEPTH: return 24;
    case GLUT_GAME_MODE_REFRESH_RATE: return 60;
    default: return 0;
  }
}

void glutTimerFunc(unsigned msecs, void (*func)(int), int value) {
  for (int i = 0; i < MAX_TIMERS; i++) {
    if (!Timers[i].used) {
      Timers[i].target_time = get_now_ms() + msecs;
      Timers[i].func = func;
      Timers[i].value = value;
      Timers[i].used = 1;
      return;
    }
  }
}

static int translate_special_key(KeySym keysym) {
  switch (keysym) {
    case XK_Left:      return GLUT_KEY_LEFT;
    case XK_Up:        return GLUT_KEY_UP;
    case XK_Right:     return GLUT_KEY_RIGHT;
    case XK_Down:      return GLUT_KEY_DOWN;
    case XK_Page_Up:   return GLUT_KEY_PAGE_UP;
    case XK_Page_Down: return GLUT_KEY_PAGE_DOWN;
    case XK_Home:      return GLUT_KEY_HOME;
    case XK_End:       return GLUT_KEY_END;
    case XK_Insert:    return GLUT_KEY_INSERT;
    default:
      if (keysym >= XK_F1 && keysym <= XK_F12) return (int)(keysym - XK_F1) + 1;
  }
  return 0;
}

static void update_modifiers(unsigned state) {
  modifiers = 0;
  if (state & ShiftMask)   modifiers |= GLUT_ACTIVE_SHIFT;
  if (state & ControlMask) modifiers |= GLUT_ACTIVE_CTRL;
  if (state & Mod1Mask)    modifiers |= GLUT_ACTIVE_ALT;
}

static void handle_xevent(XEvent *event) {
  switch (event->type) {
    case Expose:
      if (event->xexpose.count == 0) redisplay_needed = 1;
      break;

    case MapNotify:
      if (f_vis) f_vis(GLUT_VISIBLE);
      redisplay_needed = 1;
      break;

    case UnmapNotify:
      if (f_vis) f_vis(GLUT_NOT_VISIBLE);
      break;

    case EnterNotify:
      if (f_entry) f_entry(GLUT_ENTERED);
      break;

    case LeaveNotify:
      if (f_entry) f_entry(GLUT_LEFT);
      break;

    case ConfigureNotify:
      WX = event->xconfigure.x; WY = event->xconfigure.y;
      if (event->xconfigure.width != W || event->xconfigure.height != H) {
        W = event->xconfigure.width; H = event->xconfigure.height;
        if (f_resh) f_resh(W, H);
        redisplay_needed = 1;
      }
      break;

    case KeyPress:
    case KeyRelease: {
      char buf[8]; KeySym keysym; int is_down = (event->type == KeyPress);
      unsigned keycode = event->xkey.keycode & 255;
      update_modifiers(event->xkey.state);
      mouse_x = event->xkey.x; mouse_y = event->xkey.y;

      if (is_down) {
        int repeat = key_down_states[keycode];
        key_down_states[keycode] = 1;
        if (repeat && ignore_repeat) break;
      } else {
        key_down_states[keycode] = 0;
      }

      int len = XLookupString(&event->xkey, buf, sizeof(buf), &keysym, NULL);
      int spec = translate_special_key(keysym);

      if (spec) {
        void (*func)(int, int, int) = is_down ? f_spec : f_specup;
        if (func) func(spec, mouse_x, mouse_y);
      } else if (len == 1) {
        void (*func)(unsigned char, int, int) = is_down ? f_key : f_keyup;
        if (func) func((unsigned char)buf[0], mouse_x, mouse_y);
      }
      break;
    }

    case ButtonPress:
    case ButtonRelease: {
      int button = event->xbutton.button - 1;
      update_modifiers(event->xbutton.state);
      mouse_x = event->xbutton.x; mouse_y = event->xbutton.y;
      if (!f_mouse) break;

      if (button < 3) {
        f_mouse(button, event->type == ButtonPress ? GLUT_DOWN : GLUT_UP, mouse_x, mouse_y);
      } else if (event->type == ButtonPress) {
        /* Mouse wheel buttons (3 & 4), matching FreeGLUT semantics */
        f_mouse(button, GLUT_DOWN, mouse_x, mouse_y);
        f_mouse(button, GLUT_UP, mouse_x, mouse_y);
      }
      break;
    }

    case MotionNotify:
      mouse_x = event->xmotion.x; mouse_y = event->xmotion.y;
      update_modifiers(event->xmotion.state);
      if (event->xmotion.state & (Button1Mask | Button2Mask | Button3Mask)) {
        if (f_motion) f_motion(mouse_x, mouse_y);
      } else if (f_pmotion) {
        f_pmotion(mouse_x, mouse_y);
      }
      break;

    case ClientMessage:
      if ((Atom)event->xclient.data.l[0] == wm_delete_window) exit(0);
      break;
  }
}

void glutMainLoop(void) {
  if (!dpy || !win) {
    fputs("mglut: glutMainLoop called without a valid window\n", stderr);
    exit(1);
  }
  if (f_resh) f_resh(W, H);

  while (running && win) {
    while (running && win && XPending(dpy)) {
      XEvent event;
      XNextEvent(dpy, &event);
      handle_xevent(&event);
    }
    if (!running || !win) break;

    long now = get_now_ms();
    for (int i = 0; i < MAX_TIMERS; i++) {
      if (Timers[i].used && Timers[i].target_time <= now) {
        void (*func)(int) = Timers[i].func;
        int val = Timers[i].value;
        Timers[i].used = 0;
        func(val);
        if (!running || !win) break;
      }
    }
    if (!running || !win) break;

    if (f_idle) f_idle();
    if (!running || !win) break;

    if (redisplay_needed && f_disp) {
      redisplay_needed = 0;
      f_disp();
    } else if (!f_idle) {
      /* Idle state: sleep until next X event or timer expiry */
      long next_timeout = -1;
      now = get_now_ms();
      for (int i = 0; i < MAX_TIMERS; i++) {
        if (Timers[i].used) {
          long diff = Timers[i].target_time - now;
          if (diff < 0) diff = 0;
          if (next_timeout < 0 || diff < next_timeout) next_timeout = diff;
        }
      }
      XFlush(dpy);

      fd_set read_fds;
      FD_ZERO(&read_fds);
      int x_fd = ConnectionNumber(dpy);
      FD_SET(x_fd, &read_fds);

      struct timeval timeout, *tv_ptr = NULL;
      if (next_timeout >= 0) {
        timeout.tv_sec = next_timeout / 1000;
        timeout.tv_usec = (next_timeout % 1000) * 1000;
        tv_ptr = &timeout;
      }
      select(x_fd + 1, &read_fds, NULL, NULL, tv_ptr);
    }
  }
}
