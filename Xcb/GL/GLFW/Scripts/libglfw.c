/*
 * libglfw.c - custom GLFW 3.6 for Termux/Mali: pure xcb + EGL (libEGL shim) + xkbcommon.
 *
 * - Window/input: xcb (no Xlib). Keys: evdev keycode (X keycode - 8) -> GLFW_KEY_*,
 *   text via xkbcommon (xkbcommon-x11 when available, default keymap otherwise).
 * - Context: ONLY via EGL. GLFW_OPENGL_ES_API -> ES1/2/3; GLFW_OPENGL_API -> eglBindAPI(OPENGL_API),
 *   the shim maps it to ES and libGL emulates desktop GL.
 * - Display: eglGetPlatformDisplay(EGL_PLATFORM_XCB_EXT, conn); surface: &xcb_window (what native_win() reads).
 * - Vulkan: supported via VK_KHR_surface + VK_KHR_xcb_surface.
 * - Stubbed (correct error codes): joystick, gamma, custom cursors, drag&drop, raw mouse motion.
 *
 * Env: GLFW_SCALE=<float> content scale (default 1.0), GLFW_DEBUG=1 for xcb error logging.
 *
 * Build: see Scripts/build.sh
 */
#define _GNU_SOURCE
#define _GLFW_BUILD_DLL
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <math.h>
#include <errno.h>
#include <time.h>
#include <poll.h>
#include <fcntl.h>
#include <unistd.h>
#include <dlfcn.h>

#include <xcb/xcb.h>
#include <xcb/randr.h>
#include <xcb/xkb.h>
#include <xkbcommon/xkbcommon.h>
#include <xkbcommon/xkbcommon-x11.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>

#ifndef EGL_PLATFORM_XCB_EXT
#define EGL_PLATFORM_XCB_EXT 0x31DC
#endif
#ifndef EGL_PLATFORM_XCB_SCREEN_EXT
#define EGL_PLATFORM_XCB_SCREEN_EXT 0x31DE
#endif

#define VK_USE_PLATFORM_XCB_KHR
#include <vulkan/vulkan.h>

#define STICK 3
#define NET_ADD 1
#define NET_REMOVE 0

/* ====================================================================== */
/*                               Types                                    */
/* ====================================================================== */
typedef struct { int w, h, r, g, b, hz; } vmode_t;

struct GLFWmonitor {
    char* name;
    int x, y, wmm, hmm;
    vmode_t* modes; int nmodes;
    GLFWvidmode* vm; GLFWvidmode cur;
    void* user;
    xcb_randr_output_t out;
    GLFWwindow* window;
};

struct GLFWcursor { xcb_cursor_t id; };

struct GLFWwindow {
    struct GLFWwindow* next;
    xcb_window_t id; xcb_colormap_t cmap; int depth;
    int x, y, w, h;
    int verParsed;
    char* title;
    GLFWmonitor* monitor;
    int resizable, decorated, floating, autoIconify, focusOnShow, passthrough, transparent, hintPos;
    int shouldClose, visible, focused, iconified, maximized, hovered;
    int minw, minh, maxw, maxh, numer, denom;
    int client, major, minor, profile, fwd, debug, release, noerror, robust, doublebuffer, source;
    EGLSurface surf; EGLContext ctx; EGLConfig cfg;
    void* user;
    int stickyKeys, stickyButtons, lockMods, rawMotion, unlimited, cursorMode;
    char keys[GLFW_KEY_LAST + 1];
    char buttons[GLFW_MOUSE_BUTTON_LAST + 1];
    double vx, vy;                   /* virtual cursor (disabled mode) */
    int lastx, lasty, warpx, warpy;
    GLFWcursor* cursor;
    struct {
        GLFWwindowposfun pos; GLFWwindowsizefun size; GLFWwindowclosefun close;
        GLFWwindowrefreshfun refresh; GLFWwindowfocusfun focus; GLFWwindowiconifyfun iconify;
        GLFWwindowmaximizefun maximize; GLFWframebuffersizefun fbsize; GLFWwindowcontentscalefun scale;
        GLFWmousebuttonfun mouse; GLFWcursorposfun cursorpos; GLFWcursorenterfun enter;
        GLFWscrollfun scroll; GLFWkeyfun key; GLFWcharfun chr; GLFWcharmodsfun charmods;
        GLFWdropfun drop;
    } cb;
};

static struct {
    int init, debug;
    xcb_connection_t* c; xcb_screen_t* scr; int scrNum; xcb_window_t root;
    xcb_window_t helper;
    int pipe[2];
    struct timespec t0;
    GLFWwindow* wins;
    GLFWwindow* disabledWin; int restx, resty;
    GLFWmonitor** mons; int nmons;
    EGLDisplay dpy;
    xcb_cursor_t hidden; xcb_font_t curfont;
    struct xkb_context* xctx; struct xkb_keymap* xmap; struct xkb_state* xst; int xkbX11;
    short keycodes[256]; short scancodes[GLFW_KEY_LAST + 1];
    char* clip; int selDone; xcb_atom_t selProp;
    float scale;
    void* joyUser[16];
    GLFWerrorfun errcb; GLFWmonitorfun moncb; GLFWjoystickfun joycb;
    char keyname[8];
    char* wintitle;
} g;

static struct {
    int resizable, visible, decorated, focused, autoIconify, floating, maximized, centerCursor,
        transparent, focusOnShow, scaleToMonitor, passthrough, posx, posy;
    int r, gr, b, a, depth, stencil, samples, srgb, doublebuf, refresh, stereo, aux;
    int client, source, major, minor, robust, fwd, debug, profile, release, noerror;
    char cls[64], inst[64];
    int platform, hatButtons;
} H;

static struct {
    xcb_atom_t WM_PROTOCOLS, WM_DELETE_WINDOW, NET_WM_PING, NET_WM_NAME, NET_WM_ICON_NAME, UTF8_STRING,
        NET_WM_STATE, NET_WM_STATE_FULLSCREEN, NET_WM_STATE_ABOVE, NET_WM_STATE_MAX_V, NET_WM_STATE_MAX_H,
        NET_WM_STATE_ATTENTION, NET_ACTIVE_WINDOW, NET_WM_PID, NET_WM_ICON, MOTIF_WM_HINTS, WM_STATE,
        WM_CHANGE_STATE, CLIPBOARD, TARGETS, GLFW_SEL, NET_WM_OPACITY, NET_FRAME_EXTENTS;
} A;

static __thread GLFWwindow* t_cur;
static __thread int t_err;
static __thread char t_desc[256];

/* ====================================================================== */
/*                              Utilities                                 */
/* ====================================================================== */
static void ferr(int code, const char* fmt, ...) {
    va_list a; va_start(a, fmt);
    vsnprintf(t_desc, sizeof t_desc, fmt, a);
    va_end(a);
    t_err = code;
    if (g.errcb) g.errcb(code, t_desc);
}
#define REQ_INIT(r) do { if (!g.init) { ferr(GLFW_NOT_INITIALIZED, "GLFW not initialized"); return r; } } while (0)

static char* sdup(const char* s) { size_t n = strlen(s) + 1; char* p = malloc(n); if (p) memcpy(p, s, n); return p; }
static int imax(int a, int b) { return a > b ? a : b; }

static uint32_t dec_utf8(const char** s) {
    const unsigned char* p = (const unsigned char*)*s;
    uint32_t cp; int n;
    if (p[0] < 0x80) { cp = p[0]; n = 1; }
    else if ((p[0] & 0xe0) == 0xc0) { cp = p[0] & 0x1f; n = 2; }
    else if ((p[0] & 0xf0) == 0xe0) { cp = p[0] & 0x0f; n = 3; }
    else { cp = p[0] & 0x07; n = 4; }
    for (int i = 1; i < n && (p[i] & 0xc0) == 0x80; i++) cp = (cp << 6) | (p[i] & 0x3f);
    *s += n;
    return cp;
}

static double now_s(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)(t.tv_sec - g.t0.tv_sec) + (t.tv_nsec - g.t0.tv_nsec) / 1e9;
}

static xcb_atom_t intern_wait(xcb_intern_atom_cookie_t ck) {
    xcb_intern_atom_reply_t* r = xcb_intern_atom_reply(g.c, ck, NULL);
    xcb_atom_t a = r ? r->atom : XCB_NONE;
    free(r);
    return a;
}

static void* get_prop(xcb_window_t w, xcb_atom_t prop, xcb_atom_t type, uint32_t* items) {
    xcb_get_property_reply_t* r = xcb_get_property_reply(g.c,
        xcb_get_property(g.c, 0, w, prop, type, 0, 0x3fffffff), NULL);
    if (!r) return NULL;
    int len = xcb_get_property_value_length(r);
    if (!len || r->type == XCB_NONE || !r->format) { free(r); return NULL; }
    char* out = malloc(len + 1);
    if (out) { memcpy(out, xcb_get_property_value(r), len); out[len] = 0; if (items) *items = len / (r->format / 8); }
    free(r);
    return out;
}

static GLFWwindow* find_win(xcb_window_t id) {
    for (GLFWwindow* w = g.wins; w; w = w->next) if (w->id == id) return w;
    return NULL;
}

static void send_wm(GLFWwindow* w, xcb_atom_t type, uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t e) {
    xcb_client_message_event_t ev; memset(&ev, 0, sizeof ev);
    ev.response_type = XCB_CLIENT_MESSAGE; ev.format = 32; ev.window = w->id; ev.type = type;
    ev.data.data32[0] = a; ev.data.data32[1] = b; ev.data.data32[2] = c; ev.data.data32[3] = d; ev.data.data32[4] = e;
    xcb_send_event(g.c, 0, g.root, XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY | XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT, (const char*)&ev);
}

/* ====================================================================== */
/*                           Input dispatch                               */
/* ====================================================================== */
static void in_key(GLFWwindow* w, int key, int sc, int act, int mods) {
    if (key >= 0 && key <= GLFW_KEY_LAST) {
        int rep = 0;
        if (act == GLFW_RELEASE && w->keys[key] == GLFW_RELEASE) return;
        if (act == GLFW_PRESS && w->keys[key] == GLFW_PRESS) rep = 1;
        w->keys[key] = (act == GLFW_RELEASE && w->stickyKeys) ? STICK : (char)act;
        if (rep) act = GLFW_REPEAT;
    }
    if (!w->lockMods) mods &= ~(GLFW_MOD_CAPS_LOCK | GLFW_MOD_NUM_LOCK);
    if (w->cb.key) w->cb.key(w, key, sc, act, mods);
}

static void in_char(GLFWwindow* w, uint32_t cp, int mods, int plain) {
    if (cp < 32 || (cp > 126 && cp < 160)) return;
    if (!w->lockMods) mods &= ~(GLFW_MOD_CAPS_LOCK | GLFW_MOD_NUM_LOCK);
    if (w->cb.charmods) w->cb.charmods(w, cp, mods);
    if (plain && w->cb.chr) w->cb.chr(w, cp);
}

static void in_mouse(GLFWwindow* w, int btn, int act, int mods) {
    if (btn < 0 || (!w->unlimited && btn > GLFW_MOUSE_BUTTON_LAST)) return;
    if (!w->lockMods) mods &= ~(GLFW_MOD_CAPS_LOCK | GLFW_MOD_NUM_LOCK);
    if (btn <= GLFW_MOUSE_BUTTON_LAST)
        w->buttons[btn] = (act == GLFW_RELEASE && w->stickyButtons) ? STICK : (char)act;
    if (w->cb.mouse) w->cb.mouse(w, btn, act, mods);
}

static void in_cursor(GLFWwindow* w, double x, double y) {
    if (w->vx == x && w->vy == y) return;
    w->vx = x; w->vy = y;
    if (w->cb.cursorpos) w->cb.cursorpos(w, x, y);
}

static void in_focus(GLFWwindow* w, int f) {
    if (w->cb.focus) w->cb.focus(w, f);
    if (!f) {
        for (int k = 0; k <= GLFW_KEY_LAST; k++)
            if (w->keys[k] == GLFW_PRESS) {
                int sc = g.scancodes[k];
                if (sc > 0 && g.xst) xkb_state_update_key(g.xst, sc, XKB_KEY_UP);
                in_key(w, k, sc, GLFW_RELEASE, 0);
            }
        for (int b = 0; b <= GLFW_MOUSE_BUTTON_LAST; b++)
            if (w->buttons[b] == GLFW_PRESS) in_mouse(w, b, GLFW_RELEASE, 0);
    }
}

static int x_mods(unsigned st) {
    int m = 0;
    if (st & XCB_MOD_MASK_SHIFT) m |= GLFW_MOD_SHIFT;
    if (st & XCB_MOD_MASK_CONTROL) m |= GLFW_MOD_CONTROL;
    if (st & XCB_MOD_MASK_1) m |= GLFW_MOD_ALT;
    if (st & XCB_MOD_MASK_4) m |= GLFW_MOD_SUPER;
    if (st & XCB_MOD_MASK_LOCK) m |= GLFW_MOD_CAPS_LOCK;
    if (st & XCB_MOD_MASK_2) m |= GLFW_MOD_NUM_LOCK;
    return m;
}

/* ====================================================================== */
/*                            Cursor helpers                              */
/* ====================================================================== */
static void update_cursor_image(GLFWwindow* w) {
    uint32_t c = XCB_NONE;
    if (w->cursorMode == GLFW_CURSOR_NORMAL || w->cursorMode == GLFW_CURSOR_CAPTURED)
        c = w->cursor ? w->cursor->id : XCB_NONE;
    else c = g.hidden;
    xcb_change_window_attributes(g.c, w->id, XCB_CW_CURSOR, &c);
}

static void warp(GLFWwindow* w, int x, int y) {
    w->warpx = x; w->warpy = y;
    xcb_warp_pointer(g.c, XCB_NONE, w->id, 0, 0, 0, 0, (int16_t)x, (int16_t)y);
    xcb_flush(g.c);
}

static void query_ptr(GLFWwindow* w, int* x, int* y) {
    xcb_query_pointer_reply_t* r = xcb_query_pointer_reply(g.c, xcb_query_pointer(g.c, w->id), NULL);
    *x = r ? r->win_x : 0; *y = r ? r->win_y : 0;
    free(r);
}

static void grab_ptr(GLFWwindow* w) {
    xcb_grab_pointer_reply_t* r = xcb_grab_pointer_reply(g.c, xcb_grab_pointer(g.c, 1, w->id,
        XCB_EVENT_MASK_BUTTON_PRESS | XCB_EVENT_MASK_BUTTON_RELEASE | XCB_EVENT_MASK_POINTER_MOTION,
        XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC, w->id, XCB_NONE, XCB_CURRENT_TIME), NULL);
    free(r);
}
static void ungrab_ptr(void) { xcb_ungrab_pointer(g.c, XCB_CURRENT_TIME); xcb_flush(g.c); }

static void disable_cursor(GLFWwindow* w) {
    g.disabledWin = w;
    query_ptr(w, &g.restx, &g.resty);
    update_cursor_image(w);
    warp(w, w->w / 2, w->h / 2);
    w->lastx = w->w / 2; w->lasty = w->h / 2;
    grab_ptr(w);
}
static void enable_cursor(GLFWwindow* w) {
    g.disabledWin = NULL;
    ungrab_ptr();
    warp(w, g.restx, g.resty);
    update_cursor_image(w);
}

/* ====================================================================== */
/*                           Window properties                            */
/* ====================================================================== */
static void update_size_hints(GLFWwindow* w, int width, int height) {
    uint32_t h[18]; memset(h, 0, sizeof h);
    uint32_t fl = 0;
    if (!w->monitor) {
        if (w->resizable) {
            if (w->minw != GLFW_DONT_CARE && w->minh != GLFW_DONT_CARE) { fl |= 16; h[5] = w->minw; h[6] = w->minh; }
            if (w->maxw != GLFW_DONT_CARE && w->maxh != GLFW_DONT_CARE) { fl |= 32; h[7] = w->maxw; h[8] = w->maxh; }
            if (w->numer != GLFW_DONT_CARE && w->denom != GLFW_DONT_CARE) {
                fl |= 128; h[11] = h[13] = w->numer; h[12] = h[14] = w->denom;
            }
        } else { fl |= 16 | 32; h[5] = h[7] = width; h[6] = h[8] = height; }
    }
    if (w->hintPos) fl |= 4;
    fl |= 512; h[17] = 10;           /* StaticGravity */
    h[0] = fl;
    xcb_change_property(g.c, XCB_PROP_MODE_REPLACE, w->id, XCB_ATOM_WM_NORMAL_HINTS, XCB_ATOM_WM_SIZE_HINTS, 32, 18, h);
}

static void set_title(GLFWwindow* w, const char* t) {
    size_t n = strlen(t);
    xcb_change_property(g.c, XCB_PROP_MODE_REPLACE, w->id, XCB_ATOM_WM_NAME, A.UTF8_STRING, 8, n, t);
    xcb_change_property(g.c, XCB_PROP_MODE_REPLACE, w->id, A.NET_WM_NAME, A.UTF8_STRING, 8, n, t);
    xcb_change_property(g.c, XCB_PROP_MODE_REPLACE, w->id, A.NET_WM_ICON_NAME, A.UTF8_STRING, 8, n, t);
}

static void set_decorated(GLFWwindow* w, int on) {
    uint32_t h[5] = { 2, 0, on ? 1 : 0, 0, 0 };   /* MWM_HINTS_DECORATIONS */
    xcb_change_property(g.c, XCB_PROP_MODE_REPLACE, w->id, A.MOTIF_WM_HINTS, A.MOTIF_WM_HINTS, 32, 5, h);
}

static int wm_state_has(GLFWwindow* w, xcb_atom_t a1, xcb_atom_t a2) {
    uint32_t n = 0; xcb_atom_t* st = get_prop(w->id, A.NET_WM_STATE, XCB_ATOM_ATOM, &n);
    int found = 0;
    for (uint32_t i = 0; st && i < n; i++) if (st[i] == a1 || (a2 && st[i] == a2)) found = 1;
    free(st);
    return found;
}

static int query_iconified(GLFWwindow* w) {
    uint32_t n = 0; uint32_t* s = get_prop(w->id, A.WM_STATE, A.WM_STATE, &n);
    int r = s && n >= 1 && s[0] == 3;
    free(s);
    return r;
}

/* ====================================================================== */
/*                             Clipboard                                  */
/* ====================================================================== */
static void sel_request(xcb_selection_request_event_t* rq) {
    xcb_selection_notify_event_t n; memset(&n, 0, sizeof n);
    n.response_type = XCB_SELECTION_NOTIFY; n.time = rq->time; n.requestor = rq->requestor;
    n.selection = rq->selection; n.target = rq->target; n.property = XCB_NONE;
    xcb_atom_t prop = rq->property ? rq->property : rq->target;
    if (rq->target == A.TARGETS) {
        xcb_atom_t t[] = { A.TARGETS, A.UTF8_STRING, XCB_ATOM_STRING };
        xcb_change_property(g.c, XCB_PROP_MODE_REPLACE, rq->requestor, prop, XCB_ATOM_ATOM, 32, 3, t);
        n.property = prop;
    } else if ((rq->target == A.UTF8_STRING || rq->target == XCB_ATOM_STRING) && g.clip) {
        xcb_change_property(g.c, XCB_PROP_MODE_REPLACE, rq->requestor, prop, rq->target, 8, strlen(g.clip), g.clip);
        n.property = prop;
    }
    xcb_send_event(g.c, 0, rq->requestor, 0, (const char*)&n);
    xcb_flush(g.c);
}

/* ====================================================================== */
/*                            Event handling                              */
/* ====================================================================== */
static void handle_event(xcb_generic_event_t* ev) {
    int type = ev->response_type & 0x7f;
    if (type == 0) {
        if (g.debug) fprintf(stderr, "[glfw] X error code %d\n", ((xcb_generic_error_t*)ev)->error_code);
        return;
    }
    switch (type) {
    case XCB_SELECTION_REQUEST: sel_request((xcb_selection_request_event_t*)ev); return;
    case XCB_SELECTION_NOTIFY: { xcb_selection_notify_event_t* e = (void*)ev; g.selDone = 1; g.selProp = e->property; return; }
    case XCB_SELECTION_CLEAR: free(g.clip); g.clip = NULL; return;
    default: break;
    }

    xcb_window_t id = 0;
    switch (type) {
    case XCB_KEY_PRESS: case XCB_KEY_RELEASE: id = ((xcb_key_press_event_t*)ev)->event; break;
    case XCB_BUTTON_PRESS: case XCB_BUTTON_RELEASE: id = ((xcb_button_press_event_t*)ev)->event; break;
    case XCB_MOTION_NOTIFY: id = ((xcb_motion_notify_event_t*)ev)->event; break;
    case XCB_ENTER_NOTIFY: case XCB_LEAVE_NOTIFY: id = ((xcb_enter_notify_event_t*)ev)->event; break;
    case XCB_FOCUS_IN: case XCB_FOCUS_OUT: id = ((xcb_focus_in_event_t*)ev)->event; break;
    case XCB_EXPOSE: id = ((xcb_expose_event_t*)ev)->window; break;
    case XCB_MAP_NOTIFY: id = ((xcb_map_notify_event_t*)ev)->window; break;
    case XCB_UNMAP_NOTIFY: id = ((xcb_unmap_notify_event_t*)ev)->window; break;
    case XCB_CONFIGURE_NOTIFY: id = ((xcb_configure_notify_event_t*)ev)->window; break;
    case XCB_PROPERTY_NOTIFY: id = ((xcb_property_notify_event_t*)ev)->window; break;
    case XCB_CLIENT_MESSAGE: id = ((xcb_client_message_event_t*)ev)->window; break;
    default: return;
    }
    GLFWwindow* w = find_win(id);
    if (!w) return;

    switch (type) {
    case XCB_KEY_PRESS: {
        xcb_key_press_event_t* e = (void*)ev;
        int kc = e->detail, key = kc < 256 ? g.keycodes[kc] : GLFW_KEY_UNKNOWN;
        int mods = x_mods(e->state), plain = !(mods & (GLFW_MOD_CONTROL | GLFW_MOD_ALT));
        int repeat = (key >= 0 && w->keys[key] == GLFW_PRESS);
        char buf[16] = "";
        if (g.xst) xkb_state_key_get_utf8(g.xst, kc, buf, sizeof buf);
        if (g.xst && !repeat) xkb_state_update_key(g.xst, kc, XKB_KEY_DOWN);
        in_key(w, key, kc, GLFW_PRESS, mods);
        for (const char* p = buf; *p; ) in_char(w, dec_utf8(&p), mods, plain);
        return;
    }
    case XCB_KEY_RELEASE: {
        xcb_key_press_event_t* e = (void*)ev;
        int kc = e->detail, key = kc < 256 ? g.keycodes[kc] : GLFW_KEY_UNKNOWN;
        if (g.xst) xkb_state_update_key(g.xst, kc, XKB_KEY_UP);
        in_key(w, key, kc, GLFW_RELEASE, x_mods(e->state));
        return;
    }
    case XCB_BUTTON_PRESS: {
        xcb_button_press_event_t* e = (void*)ev; int m = x_mods(e->state), b = e->detail;
        if (b == 1) in_mouse(w, GLFW_MOUSE_BUTTON_LEFT, GLFW_PRESS, m);
        else if (b == 2) in_mouse(w, GLFW_MOUSE_BUTTON_MIDDLE, GLFW_PRESS, m);
        else if (b == 3) in_mouse(w, GLFW_MOUSE_BUTTON_RIGHT, GLFW_PRESS, m);
        else if (b == 4) { if (w->cb.scroll) w->cb.scroll(w, 0.0, 1.0); }
        else if (b == 5) { if (w->cb.scroll) w->cb.scroll(w, 0.0, -1.0); }
        else if (b == 6) { if (w->cb.scroll) w->cb.scroll(w, 1.0, 0.0); }
        else if (b == 7) { if (w->cb.scroll) w->cb.scroll(w, -1.0, 0.0); }
        else in_mouse(w, b - 1 - 4, GLFW_PRESS, m);
        return;
    }
    case XCB_BUTTON_RELEASE: {
        xcb_button_press_event_t* e = (void*)ev; int m = x_mods(e->state), b = e->detail;
        if (b == 1) in_mouse(w, GLFW_MOUSE_BUTTON_LEFT, GLFW_RELEASE, m);
        else if (b == 2) in_mouse(w, GLFW_MOUSE_BUTTON_MIDDLE, GLFW_RELEASE, m);
        else if (b == 3) in_mouse(w, GLFW_MOUSE_BUTTON_RIGHT, GLFW_RELEASE, m);
        else if (b > 7) in_mouse(w, b - 1 - 4, GLFW_RELEASE, m);
        return;
    }
    case XCB_ENTER_NOTIFY: {
        xcb_enter_notify_event_t* e = (void*)ev;
        w->hovered = 1;
        if (w->cb.enter) w->cb.enter(w, 1);
        in_cursor(w, e->event_x, e->event_y);
        w->lastx = e->event_x; w->lasty = e->event_y;
        return;
    }
    case XCB_LEAVE_NOTIFY:
        w->hovered = 0;
        if (w->cb.enter) w->cb.enter(w, 0);
        return;
    case XCB_MOTION_NOTIFY: {
        xcb_motion_notify_event_t* e = (void*)ev; int x = e->event_x, y = e->event_y;
        if (x != w->warpx || y != w->warpy) {
            if (w->cursorMode == GLFW_CURSOR_DISABLED) {
                if (g.disabledWin != w) return;
                in_cursor(w, w->vx + (x - w->lastx), w->vy + (y - w->lasty));
            } else in_cursor(w, x, y);
        }
        w->lastx = x; w->lasty = y;
        return;
    }
    case XCB_CONFIGURE_NOTIFY: {
        xcb_configure_notify_event_t* e = (void*)ev;
        if (e->width != w->w || e->height != w->h) {
            w->w = e->width; w->h = e->height;
            if (w->cb.fbsize) w->cb.fbsize(w, w->w, w->h);
            if (w->cb.size) w->cb.size(w, w->w, w->h);
        }
        int x = e->x, y = e->y;
        if (!(ev->response_type & 0x80)) {      /* not synthetic: coords relative to parent */
            xcb_translate_coordinates_reply_t* t = xcb_translate_coordinates_reply(g.c,
                xcb_translate_coordinates(g.c, w->id, g.root, 0, 0), NULL);
            if (t) { x = t->dst_x; y = t->dst_y; free(t); }
        }
        if (x != w->x || y != w->y) { w->x = x; w->y = y; if (w->cb.pos) w->cb.pos(w, x, y); }
        return;
    }
    case XCB_CLIENT_MESSAGE: {
        xcb_client_message_event_t* e = (void*)ev;
        if (e->type == A.WM_PROTOCOLS) {
            xcb_atom_t p = e->data.data32[0];
            if (p == A.WM_DELETE_WINDOW) { w->shouldClose = 1; if (w->cb.close) w->cb.close(w); }
            else if (p == A.NET_WM_PING) {
                xcb_client_message_event_t r = *e; r.window = g.root;
                xcb_send_event(g.c, 0, g.root, XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY | XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT, (const char*)&r);
            }
        }
        return;
    }
    case XCB_FOCUS_IN: {
        xcb_focus_in_event_t* e = (void*)ev;
        if (e->mode == XCB_NOTIFY_MODE_GRAB || e->mode == XCB_NOTIFY_MODE_UNGRAB) return;
        w->focused = 1;
        if (w->cursorMode == GLFW_CURSOR_DISABLED) disable_cursor(w);
        else if (w->cursorMode == GLFW_CURSOR_CAPTURED) grab_ptr(w);
        in_focus(w, 1);
        return;
    }
    case XCB_FOCUS_OUT: {
        xcb_focus_in_event_t* e = (void*)ev;
        if (e->mode == XCB_NOTIFY_MODE_GRAB || e->mode == XCB_NOTIFY_MODE_UNGRAB) return;
        w->focused = 0;
        if (w->cursorMode == GLFW_CURSOR_DISABLED) enable_cursor(w);
        else if (w->cursorMode == GLFW_CURSOR_CAPTURED) ungrab_ptr();
        if (w->monitor && w->autoIconify) glfwIconifyWindow(w);
        in_focus(w, 0);
        return;
    }
    case XCB_EXPOSE:
        if (w->cb.refresh) w->cb.refresh(w);
        return;
    case XCB_MAP_NOTIFY: w->visible = 1; return;
    case XCB_UNMAP_NOTIFY: w->visible = 0; return;
    case XCB_PROPERTY_NOTIFY: {
        xcb_property_notify_event_t* e = (void*)ev;
        if (e->state != XCB_PROPERTY_NEW_VALUE) return;
        if (e->atom == A.WM_STATE) {
            int ic = query_iconified(w);
            if (ic != w->iconified) { w->iconified = ic; if (w->cb.iconify) w->cb.iconify(w, ic); }
        } else if (e->atom == A.NET_WM_STATE) {
            int mx = wm_state_has(w, A.NET_WM_STATE_MAX_V, A.NET_WM_STATE_MAX_H);
            if (mx != w->maximized) { w->maximized = mx; if (w->cb.maximize) w->cb.maximize(w, mx); }
        }
        return;
    }
    }
}

static void drain_pipe(void) {
    char d[64];
    while (read(g.pipe[0], d, sizeof d) > 0) {}
}

static void pump(void) {
    drain_pipe();
    xcb_generic_event_t* e;
    while ((e = xcb_poll_for_event(g.c))) { handle_event(e); free(e); }
    if (xcb_connection_has_error(g.c)) {
        static int said;
        if (!said) { said = 1; ferr(GLFW_PLATFORM_ERROR, "X11: connection lost"); }
        for (GLFWwindow* w = g.wins; w; w = w->next) w->shouldClose = 1;
    }
    GLFWwindow* dw = g.disabledWin;
    if (dw && (dw->lastx != dw->w / 2 || dw->lasty != dw->h / 2)) {
        warp(dw, dw->w / 2, dw->h / 2);
        dw->lastx = dw->w / 2; dw->lasty = dw->h / 2;
    }
    xcb_flush(g.c);
}

static void wait_for(double timeout) {
    xcb_flush(g.c);
    xcb_generic_event_t* e = xcb_poll_for_event(g.c);
    if (e) { handle_event(e); free(e); pump(); return; }
    struct pollfd pf[2] = { { xcb_get_file_descriptor(g.c), POLLIN, 0 }, { g.pipe[0], POLLIN, 0 } };
    int ms = timeout < 0 ? -1 : (int)ceil(timeout * 1000.0);
    poll(pf, 2, ms);
    pump();
}

/* ====================================================================== */
/*                           Keyboard tables                              */
/* ====================================================================== */
static const struct { int ev, key; } EVKEYS[] = {
    {1, GLFW_KEY_ESCAPE}, {11, GLFW_KEY_0}, {12, GLFW_KEY_MINUS}, {13, GLFW_KEY_EQUAL}, {14, GLFW_KEY_BACKSPACE},
    {15, GLFW_KEY_TAB}, {16, GLFW_KEY_Q}, {17, GLFW_KEY_W}, {18, GLFW_KEY_E}, {19, GLFW_KEY_R}, {20, GLFW_KEY_T},
    {21, GLFW_KEY_Y}, {22, GLFW_KEY_U}, {23, GLFW_KEY_I}, {24, GLFW_KEY_O}, {25, GLFW_KEY_P},
    {26, GLFW_KEY_LEFT_BRACKET}, {27, GLFW_KEY_RIGHT_BRACKET}, {28, GLFW_KEY_ENTER}, {29, GLFW_KEY_LEFT_CONTROL},
    {30, GLFW_KEY_A}, {31, GLFW_KEY_S}, {32, GLFW_KEY_D}, {33, GLFW_KEY_F}, {34, GLFW_KEY_G}, {35, GLFW_KEY_H},
    {36, GLFW_KEY_J}, {37, GLFW_KEY_K}, {38, GLFW_KEY_L}, {39, GLFW_KEY_SEMICOLON}, {40, GLFW_KEY_APOSTROPHE},
    {41, GLFW_KEY_GRAVE_ACCENT}, {42, GLFW_KEY_LEFT_SHIFT}, {43, GLFW_KEY_BACKSLASH}, {44, GLFW_KEY_Z},
    {45, GLFW_KEY_X}, {46, GLFW_KEY_C}, {47, GLFW_KEY_V}, {48, GLFW_KEY_B}, {49, GLFW_KEY_N}, {50, GLFW_KEY_M},
    {51, GLFW_KEY_COMMA}, {52, GLFW_KEY_PERIOD}, {53, GLFW_KEY_SLASH}, {54, GLFW_KEY_RIGHT_SHIFT},
    {55, GLFW_KEY_KP_MULTIPLY}, {56, GLFW_KEY_LEFT_ALT}, {57, GLFW_KEY_SPACE}, {58, GLFW_KEY_CAPS_LOCK},
    {69, GLFW_KEY_NUM_LOCK}, {70, GLFW_KEY_SCROLL_LOCK}, {71, GLFW_KEY_KP_7}, {72, GLFW_KEY_KP_8},
    {73, GLFW_KEY_KP_9}, {74, GLFW_KEY_KP_SUBTRACT}, {75, GLFW_KEY_KP_4}, {76, GLFW_KEY_KP_5},
    {77, GLFW_KEY_KP_6}, {78, GLFW_KEY_KP_ADD}, {79, GLFW_KEY_KP_1}, {80, GLFW_KEY_KP_2}, {81, GLFW_KEY_KP_3},
    {82, GLFW_KEY_KP_0}, {83, GLFW_KEY_KP_DECIMAL}, {86, GLFW_KEY_WORLD_1}, {87, GLFW_KEY_F11}, {88, GLFW_KEY_F12},
    {96, GLFW_KEY_KP_ENTER}, {97, GLFW_KEY_RIGHT_CONTROL}, {98, GLFW_KEY_KP_DIVIDE}, {99, GLFW_KEY_PRINT_SCREEN},
    {100, GLFW_KEY_RIGHT_ALT}, {102, GLFW_KEY_HOME}, {103, GLFW_KEY_UP}, {104, GLFW_KEY_PAGE_UP},
    {105, GLFW_KEY_LEFT}, {106, GLFW_KEY_RIGHT}, {107, GLFW_KEY_END}, {108, GLFW_KEY_DOWN},
    {109, GLFW_KEY_PAGE_DOWN}, {110, GLFW_KEY_INSERT}, {111, GLFW_KEY_DELETE}, {117, GLFW_KEY_KP_EQUAL},
    {119, GLFW_KEY_PAUSE}, {125, GLFW_KEY_LEFT_SUPER}, {126, GLFW_KEY_RIGHT_SUPER}, {127, GLFW_KEY_MENU},
};

static void build_key_tables(void) {
    for (int i = 0; i < 256; i++) g.keycodes[i] = GLFW_KEY_UNKNOWN;
    for (int i = 0; i <= GLFW_KEY_LAST; i++) g.scancodes[i] = -1;
    #define MAP(ev, key) do { int kc_ = (ev) + 8; g.keycodes[kc_] = (key); g.scancodes[key] = kc_; } while (0)
    for (unsigned i = 0; i < sizeof EVKEYS / sizeof *EVKEYS; i++) MAP(EVKEYS[i].ev, EVKEYS[i].key);
    for (int i = 0; i < 9; i++) MAP(2 + i, GLFW_KEY_1 + i);
    for (int i = 0; i < 10; i++) MAP(59 + i, GLFW_KEY_F1 + i);
    for (int i = 0; i < 12; i++) MAP(183 + i, GLFW_KEY_F13 + i);
    #undef MAP
}

static void init_xkb(void) {
    g.xctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    if (!g.xctx) return;
    uint16_t a, b; uint8_t c, d;
    if (xkb_x11_setup_xkb_extension(g.c, 1, 0, XKB_X11_SETUP_XKB_EXTENSION_NO_FLAGS, &a, &b, &c, &d)) {
        int32_t dev = xkb_x11_get_core_keyboard_device_id(g.c);
        if (dev >= 0) {
            g.xmap = xkb_x11_keymap_new_from_device(g.xctx, g.c, dev, XKB_KEYMAP_COMPILE_NO_FLAGS);
            if (g.xmap) { g.xst = xkb_x11_state_new_from_device(g.xmap, g.c, dev); g.xkbX11 = 1; }
        }
        /* detectable autorepeat: repeats arrive as presses without releases */
        xcb_xkb_per_client_flags_reply_t* r = xcb_xkb_per_client_flags_reply(g.c,
            xcb_xkb_per_client_flags(g.c, XCB_XKB_ID_USE_CORE_KBD, XCB_XKB_PER_CLIENT_FLAG_DETECTABLE_AUTO_REPEAT,
                                     XCB_XKB_PER_CLIENT_FLAG_DETECTABLE_AUTO_REPEAT, 0, 0, 0), NULL);
        free(r);
    }
    if (!g.xmap) {
        g.xmap = xkb_keymap_new_from_names(g.xctx, NULL, XKB_KEYMAP_COMPILE_NO_FLAGS);
        if (g.xmap) g.xst = xkb_state_new(g.xmap);
    }
}

/* ====================================================================== */
/*                               Monitors                                 */
/* ====================================================================== */
static void add_mode(GLFWmonitor* m, int w, int h, int hz) {
    for (int i = 0; i < m->nmodes; i++)
        if (m->modes[i].w == w && m->modes[i].h == h && m->modes[i].hz == hz) return;
    m->modes = realloc(m->modes, (m->nmodes + 1) * sizeof(vmode_t));
    m->modes[m->nmodes++] = (vmode_t){ w, h, 8, 8, 8, hz };
}

static void finish_monitor(GLFWmonitor* m, int curw, int curh, int curhz) {
    m->vm = calloc(m->nmodes ? m->nmodes : 1, sizeof(GLFWvidmode));
    for (int i = 0; i < m->nmodes; i++)
        m->vm[i] = (GLFWvidmode){ m->modes[i].w, m->modes[i].h, 8, 8, 8, m->modes[i].hz };
    m->cur = (GLFWvidmode){ curw, curh, 8, 8, 8, curhz };
    g.mons = realloc(g.mons, (g.nmons + 1) * sizeof *g.mons);
    g.mons[g.nmons++] = m;
}

static void init_monitors(void) {
    const xcb_query_extension_reply_t* ext = xcb_get_extension_data(g.c, &xcb_randr_id);
    if (ext && ext->present) {
        xcb_randr_query_version_reply_t* v = xcb_randr_query_version_reply(g.c, xcb_randr_query_version(g.c, 1, 3), NULL);
        if (v) {
            free(v);
            xcb_randr_get_screen_resources_current_reply_t* res = xcb_randr_get_screen_resources_current_reply(g.c,
                xcb_randr_get_screen_resources_current(g.c, g.root), NULL);
            xcb_randr_get_output_primary_reply_t* pr = xcb_randr_get_output_primary_reply(g.c,
                xcb_randr_get_output_primary(g.c, g.root), NULL);
            if (res) {
                xcb_randr_output_t* outs = xcb_randr_get_screen_resources_current_outputs(res);
                int no = xcb_randr_get_screen_resources_current_outputs_length(res);
                xcb_randr_mode_info_t* mi = xcb_randr_get_screen_resources_current_modes(res);
                int nm = xcb_randr_get_screen_resources_current_modes_length(res);
                for (int i = 0; i < no; i++) {
                    xcb_randr_get_output_info_reply_t* oi = xcb_randr_get_output_info_reply(g.c,
                        xcb_randr_get_output_info(g.c, outs[i], res->config_timestamp), NULL);
                    if (!oi) continue;
                    if (oi->connection != XCB_RANDR_CONNECTION_CONNECTED || oi->crtc == XCB_NONE) { free(oi); continue; }
                    xcb_randr_get_crtc_info_reply_t* ci = xcb_randr_get_crtc_info_reply(g.c,
                        xcb_randr_get_crtc_info(g.c, oi->crtc, res->config_timestamp), NULL);
                    if (!ci) { free(oi); continue; }
                    GLFWmonitor* m = calloc(1, sizeof *m);
                    int nl = xcb_randr_get_output_info_name_length(oi);
                    m->name = malloc(nl + 1);
                    memcpy(m->name, xcb_randr_get_output_info_name(oi), nl); m->name[nl] = 0;
                    m->x = ci->x; m->y = ci->y; m->wmm = oi->mm_width; m->hmm = oi->mm_height; m->out = outs[i];
                    xcb_randr_mode_t* om = xcb_randr_get_output_info_modes(oi);
                    int nom = xcb_randr_get_output_info_modes_length(oi), curhz = 60;
                    for (int j = 0; j < nom; j++)
                        for (int k = 0; k < nm; k++)
                            if (mi[k].id == om[j]) {
                                double d = (double)mi[k].htotal * mi[k].vtotal;
                                int hz = d > 0 ? (int)lround(mi[k].dot_clock / d) : 0;
                                add_mode(m, mi[k].width, mi[k].height, hz);
                                if (mi[k].id == ci->mode) curhz = hz;
                            }
                    finish_monitor(m, ci->width, ci->height, curhz);
                    if (pr && pr->output == outs[i] && g.nmons > 1) {
                        GLFWmonitor* t = g.mons[0]; g.mons[0] = g.mons[g.nmons - 1]; g.mons[g.nmons - 1] = t;
                    }
                    free(ci); free(oi);
                }
                free(res);
            }
            free(pr);
        }
    }
    if (!g.nmons) {
        GLFWmonitor* m = calloc(1, sizeof *m);
        m->name = sdup("X11 screen");
        m->wmm = g.scr->width_in_millimeters; m->hmm = g.scr->height_in_millimeters;
        add_mode(m, g.scr->width_in_pixels, g.scr->height_in_pixels, 60);
        finish_monitor(m, g.scr->width_in_pixels, g.scr->height_in_pixels, 60);
    }
}

/* ====================================================================== */
/*                       Init / Terminate / misc                          */
/* ====================================================================== */
static void default_hints(void) {
    memset(&H, 0, sizeof H);
    H.resizable = H.visible = H.decorated = H.focused = H.autoIconify = H.centerCursor = H.focusOnShow = 1;
    H.posx = H.posy = (int)GLFW_ANY_POSITION;
    H.r = H.gr = H.b = H.a = 8; H.depth = 24; H.stencil = 8; H.doublebuf = 1;
    H.samples = 0; H.refresh = GLFW_DONT_CARE;
    H.client = GLFW_OPENGL_API; H.source = GLFW_NATIVE_CONTEXT_API; H.major = 1; H.minor = 0;
    H.release = GLFW_ANY_RELEASE_BEHAVIOR;
    H.hatButtons = 1;
    if (!H.platform) H.platform = GLFW_ANY_PLATFORM;
}

GLFWAPI int glfwInit(void) {
    if (g.init) return GLFW_TRUE;
    int plat = H.platform;
    memset(&g, 0, sizeof g);
    H.platform = plat;
    if (plat && plat != GLFW_ANY_PLATFORM && plat != GLFW_PLATFORM_X11) {
        ferr(GLFW_PLATFORM_UNAVAILABLE, "Only the X11 platform is supported"); return GLFW_FALSE;
    }
    int hp = H.hatButtons;
    g.debug = getenv("GLFW_DEBUG") != NULL;
    g.c = xcb_connect(NULL, &g.scrNum);
    if (!g.c || xcb_connection_has_error(g.c)) {
        ferr(GLFW_PLATFORM_UNAVAILABLE, "X11: cannot connect to the X server (DISPLAY?)");
        if (g.c) xcb_disconnect(g.c);
        memset(&g, 0, sizeof g);
        return GLFW_FALSE;
    }
    xcb_screen_iterator_t it = xcb_setup_roots_iterator(xcb_get_setup(g.c));
    for (int i = 0; i < g.scrNum && it.rem; i++) xcb_screen_next(&it);
    g.scr = it.data; g.root = g.scr->root;

    #define ATOM(var, name) xcb_intern_atom_cookie_t ck_##var = xcb_intern_atom(g.c, 0, strlen(name), name)
    ATOM(WM_PROTOCOLS, "WM_PROTOCOLS"); ATOM(WM_DELETE_WINDOW, "WM_DELETE_WINDOW"); ATOM(NET_WM_PING, "_NET_WM_PING");
    ATOM(NET_WM_NAME, "_NET_WM_NAME"); ATOM(NET_WM_ICON_NAME, "_NET_WM_ICON_NAME"); ATOM(UTF8_STRING, "UTF8_STRING");
    ATOM(NET_WM_STATE, "_NET_WM_STATE"); ATOM(NET_WM_STATE_FULLSCREEN, "_NET_WM_STATE_FULLSCREEN");
    ATOM(NET_WM_STATE_ABOVE, "_NET_WM_STATE_ABOVE"); ATOM(NET_WM_STATE_MAX_V, "_NET_WM_STATE_MAXIMIZED_VERT");
    ATOM(NET_WM_STATE_MAX_H, "_NET_WM_STATE_MAXIMIZED_HORZ"); ATOM(NET_WM_STATE_ATTENTION, "_NET_WM_STATE_DEMANDS_ATTENTION");
    ATOM(NET_ACTIVE_WINDOW, "_NET_ACTIVE_WINDOW"); ATOM(NET_WM_PID, "_NET_WM_PID"); ATOM(NET_WM_ICON, "_NET_WM_ICON");
    ATOM(MOTIF_WM_HINTS, "_MOTIF_WM_HINTS"); ATOM(WM_STATE, "WM_STATE"); ATOM(WM_CHANGE_STATE, "WM_CHANGE_STATE");
    ATOM(CLIPBOARD, "CLIPBOARD"); ATOM(TARGETS, "TARGETS"); ATOM(GLFW_SEL, "GLFW_SELECTION");
    ATOM(NET_WM_OPACITY, "_NET_WM_WINDOW_OPACITY"); ATOM(NET_FRAME_EXTENTS, "_NET_FRAME_EXTENTS");
    #undef ATOM
    #define GET(var) A.var = intern_wait(ck_##var)
    GET(WM_PROTOCOLS); GET(WM_DELETE_WINDOW); GET(NET_WM_PING); GET(NET_WM_NAME); GET(NET_WM_ICON_NAME);
    GET(UTF8_STRING); GET(NET_WM_STATE); GET(NET_WM_STATE_FULLSCREEN); GET(NET_WM_STATE_ABOVE);
    GET(NET_WM_STATE_MAX_V); GET(NET_WM_STATE_MAX_H); GET(NET_WM_STATE_ATTENTION); GET(NET_ACTIVE_WINDOW);
    GET(NET_WM_PID); GET(NET_WM_ICON); GET(MOTIF_WM_HINTS); GET(WM_STATE); GET(WM_CHANGE_STATE);
    GET(CLIPBOARD); GET(TARGETS); GET(GLFW_SEL); GET(NET_WM_OPACITY); GET(NET_FRAME_EXTENTS);
    #undef GET

    if (pipe2(g.pipe, O_CLOEXEC | O_NONBLOCK)) {
        ferr(GLFW_PLATFORM_ERROR, "X11: failed to create event pipe");
        xcb_disconnect(g.c); memset(&g, 0, sizeof g); return GLFW_FALSE;
    }
    clock_gettime(CLOCK_MONOTONIC, &g.t0);
    const char* sc = getenv("GLFW_SCALE");
    g.scale = sc && atof(sc) > 0 ? (float)atof(sc) : 1.0f;

    /* helper window (clipboard owner) */
    g.helper = xcb_generate_id(g.c);
    xcb_create_window(g.c, 0, g.helper, g.root, 0, 0, 1, 1, 0, XCB_WINDOW_CLASS_INPUT_ONLY, XCB_COPY_FROM_PARENT, 0, NULL);

    /* hidden cursor */
    xcb_pixmap_t pm = xcb_generate_id(g.c); xcb_gcontext_t gc = xcb_generate_id(g.c);
    uint32_t zero = 0; xcb_rectangle_t rc = { 0, 0, 1, 1 };
    xcb_create_pixmap(g.c, 1, pm, g.root, 1, 1);
    xcb_create_gc(g.c, gc, pm, XCB_GC_FOREGROUND, &zero);
    xcb_poly_fill_rectangle(g.c, pm, gc, 1, &rc);
    g.hidden = xcb_generate_id(g.c);
    xcb_create_cursor(g.c, g.hidden, pm, pm, 0, 0, 0, 0, 0, 0, 0, 0);
    xcb_free_gc(g.c, gc); xcb_free_pixmap(g.c, pm);

    /* cursor font for standard shapes */
    g.curfont = xcb_generate_id(g.c);
    xcb_open_font(g.c, g.curfont, 6, "cursor");

    build_key_tables();
    init_xkb();
    init_monitors();
    xcb_flush(g.c);
    g.init = 1;
    H.hatButtons = hp;
    return GLFW_TRUE;
}

GLFWAPI void glfwTerminate(void) {
    if (!g.init) return;
    while (g.wins) glfwDestroyWindow(g.wins);
    for (int i = 0; i < g.nmons; i++) {
        free(g.mons[i]->name); free(g.mons[i]->modes); free(g.mons[i]->vm); free(g.mons[i]);
    }
    free(g.mons);
    if (g.dpy) { eglMakeCurrent(g.dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT); eglTerminate(g.dpy); }
    if (g.xst) xkb_state_unref(g.xst);
    if (g.xmap) xkb_keymap_unref(g.xmap);
    if (g.xctx) xkb_context_unref(g.xctx);
    free(g.clip);
    close(g.pipe[0]); close(g.pipe[1]);
    xcb_disconnect(g.c);
    GLFWerrorfun ecb = g.errcb;
    memset(&g, 0, sizeof g);
    g.errcb = ecb;
    t_cur = NULL;
    default_hints();
}

GLFWAPI void glfwInitHint(int hint, int value) {
    if (hint == GLFW_JOYSTICK_HAT_BUTTONS) H.hatButtons = value;
    else if (hint == GLFW_PLATFORM) {
        if (value == GLFW_ANY_PLATFORM || value == GLFW_PLATFORM_X11 || value == GLFW_PLATFORM_WIN32 ||
            value == GLFW_PLATFORM_COCOA || value == GLFW_PLATFORM_WAYLAND || value == GLFW_PLATFORM_NULL)
            H.platform = value;
        else ferr(GLFW_INVALID_ENUM, "Invalid platform 0x%08X", value);
    }
}
GLFWAPI void glfwInitAllocator(const GLFWallocator* a) {
    if (a && (!a->allocate || !a->reallocate || !a->deallocate)) ferr(GLFW_INVALID_VALUE, "Missing function in allocator");
}
GLFWAPI void glfwGetVersion(int* ma, int* mi, int* re) {
    if (ma) *ma = GLFW_VERSION_MAJOR; if (mi) *mi = GLFW_VERSION_MINOR; if (re) *re = GLFW_VERSION_REVISION;
}
GLFWAPI const char* glfwGetVersionString(void) { return "ARM"; }
GLFWAPI int glfwGetError(const char** d) {
    int e = t_err; t_err = 0;
    if (d) *d = e ? t_desc : NULL;
    return e;
}
GLFWAPI GLFWerrorfun glfwSetErrorCallback(GLFWerrorfun cb) { GLFWerrorfun o = g.errcb; g.errcb = cb; return o; }
GLFWAPI int glfwGetPlatform(void) { REQ_INIT(0); return GLFW_PLATFORM_X11; }
GLFWAPI int glfwPlatformSupported(int p) {
    if (p == GLFW_PLATFORM_X11) return GLFW_TRUE;
    if (p == GLFW_PLATFORM_WIN32 || p == GLFW_PLATFORM_COCOA || p == GLFW_PLATFORM_WAYLAND || p == GLFW_PLATFORM_NULL) return GLFW_FALSE;
    ferr(GLFW_INVALID_ENUM, "Invalid platform 0x%08X", p); return GLFW_FALSE;
}

/* ---- Monitors ---- */
GLFWAPI GLFWmonitor** glfwGetMonitors(int* count) {
    *count = 0; REQ_INIT(NULL); *count = g.nmons; return g.mons;
}
GLFWAPI GLFWmonitor* glfwGetPrimaryMonitor(void) { REQ_INIT(NULL); return g.nmons ? g.mons[0] : NULL; }
GLFWAPI void glfwGetMonitorPos(GLFWmonitor* m, int* x, int* y) {
    if (x) *x = 0; if (y) *y = 0; REQ_INIT();
    if (x) *x = m->x; if (y) *y = m->y;
}
GLFWAPI void glfwGetMonitorWorkarea(GLFWmonitor* m, int* x, int* y, int* w, int* h) {
    if (x) *x = 0; if (y) *y = 0; if (w) *w = 0; if (h) *h = 0; REQ_INIT();
    if (x) *x = m->x; if (y) *y = m->y; if (w) *w = m->cur.width; if (h) *h = m->cur.height;
}
GLFWAPI void glfwGetMonitorPhysicalSize(GLFWmonitor* m, int* w, int* h) {
    if (w) *w = 0; if (h) *h = 0; REQ_INIT();
    if (w) *w = m->wmm; if (h) *h = m->hmm;
}
GLFWAPI void glfwGetMonitorContentScale(GLFWmonitor* m, float* x, float* y) {
    (void)m; if (x) *x = 0; if (y) *y = 0; REQ_INIT();
    if (x) *x = g.scale; if (y) *y = g.scale;
}
GLFWAPI const char* glfwGetMonitorName(GLFWmonitor* m) { REQ_INIT(NULL); return m->name; }
GLFWAPI void glfwSetMonitorUserPointer(GLFWmonitor* m, void* p) { REQ_INIT(); m->user = p; }
GLFWAPI void* glfwGetMonitorUserPointer(GLFWmonitor* m) { REQ_INIT(NULL); return m->user; }
GLFWAPI GLFWmonitorfun glfwSetMonitorCallback(GLFWmonitorfun cb) { REQ_INIT(NULL); GLFWmonitorfun o = g.moncb; g.moncb = cb; return o; }
GLFWAPI const GLFWvidmode* glfwGetVideoModes(GLFWmonitor* m, int* count) { *count = 0; REQ_INIT(NULL); *count = m->nmodes; return m->vm; }
GLFWAPI const GLFWvidmode* glfwGetVideoMode(GLFWmonitor* m) { REQ_INIT(NULL); return &m->cur; }
GLFWAPI void glfwSetGamma(GLFWmonitor* m, float gm) { (void)m; (void)gm; REQ_INIT(); ferr(GLFW_FEATURE_UNAVAILABLE, "X11: gamma not supported"); }
GLFWAPI const GLFWgammaramp* glfwGetGammaRamp(GLFWmonitor* m) { (void)m; REQ_INIT(NULL); ferr(GLFW_FEATURE_UNAVAILABLE, "X11: gamma not supported"); return NULL; }
GLFWAPI void glfwSetGammaRamp(GLFWmonitor* m, const GLFWgammaramp* r) { (void)m; (void)r; REQ_INIT(); ferr(GLFW_FEATURE_UNAVAILABLE, "X11: gamma not supported"); }

/* ====================================================================== */
/*                          EGL context creation                          */
/* ====================================================================== */
static int init_egl(void) {
    if (g.dpy) return 1;
    EGLAttrib at[] = { EGL_PLATFORM_XCB_SCREEN_EXT, g.scrNum, EGL_NONE };
    g.dpy = eglGetPlatformDisplay(EGL_PLATFORM_XCB_EXT, g.c, at);
    if (g.dpy == EGL_NO_DISPLAY) { g.dpy = NULL; ferr(GLFW_API_UNAVAILABLE, "EGL: failed to get XCB platform display"); return 0; }
    EGLint ma, mi;
    if (!eglInitialize(g.dpy, &ma, &mi)) { g.dpy = NULL; ferr(GLFW_PLATFORM_ERROR, "EGL: failed to initialize (0x%x)", eglGetError()); return 0; }
    return 1;
}

static int choose_config(GLFWwindow* w, const int fb[8], EGLConfig* out) {
    for (int attempt = 0; attempt < 2; attempt++) {
        EGLint a[48]; int n = 0;
        #define AT(k, v) do { a[n++] = (k); a[n++] = (v); } while (0)
        AT(EGL_SURFACE_TYPE, EGL_WINDOW_BIT);
        if (w->client == GLFW_OPENGL_ES_API) AT(EGL_RENDERABLE_TYPE, w->major >= 2 ? EGL_OPENGL_ES2_BIT : EGL_OPENGL_ES_BIT);
        else AT(EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT);
        /* fb = r,g,b,a,depth,stencil,samples */
        if (fb[0] >= 0) AT(EGL_RED_SIZE, fb[0]);
        if (fb[1] >= 0) AT(EGL_GREEN_SIZE, fb[1]);
        if (fb[2] >= 0) AT(EGL_BLUE_SIZE, fb[2]);
        if (attempt == 0) {
            if (fb[3] > 0) AT(EGL_ALPHA_SIZE, fb[3]);
            if (fb[4] > 0) AT(EGL_DEPTH_SIZE, fb[4]);
            if (fb[5] > 0) AT(EGL_STENCIL_SIZE, fb[5]);
            if (fb[6] > 0) { AT(EGL_SAMPLE_BUFFERS, 1); AT(EGL_SAMPLES, fb[6]); }
        }
        AT(EGL_NONE, EGL_NONE);
        #undef AT
        EGLint cnt = 0;
        if (eglChooseConfig(g.dpy, a, out, 1, &cnt) && cnt > 0) return 1;
    }
    return 0;
}

static int create_context(GLFWwindow* w, GLFWwindow* share, const int fb[8]) {
    if (!init_egl()) return 0;
    if (!choose_config(w, fb, &w->cfg)) { ferr(GLFW_FORMAT_UNAVAILABLE, "EGL: no suitable framebuffer configuration"); return 0; }
    if (!eglBindAPI(w->client == GLFW_OPENGL_ES_API ? EGL_OPENGL_ES_API : EGL_OPENGL_API)) {
        ferr(GLFW_API_UNAVAILABLE, "EGL: client API unavailable (0x%x)", eglGetError()); return 0;
    }
    EGLint a[24]; int n = 0;
    a[n++] = 0x3098; a[n++] = w->major;                         /* EGL_CONTEXT_CLIENT_VERSION / MAJOR_VERSION */
    if (w->client == GLFW_OPENGL_API) {
        if (w->minor) { a[n++] = 0x30FB; a[n++] = w->minor; }   /* MINOR_VERSION_KHR */
        if (w->profile == GLFW_OPENGL_CORE_PROFILE) { a[n++] = 0x30FD; a[n++] = 1; }
        else if (w->profile == GLFW_OPENGL_COMPAT_PROFILE) { a[n++] = 0x30FD; a[n++] = 2; }
        int fl = (w->debug ? 1 : 0) | (w->fwd ? 2 : 0);
        if (fl) { a[n++] = 0x30FC; a[n++] = fl; }
    }
    a[n++] = EGL_NONE;
    w->ctx = eglCreateContext(g.dpy, w->cfg, share ? share->ctx : EGL_NO_CONTEXT, a);
    if (w->ctx == EGL_NO_CONTEXT) {
        w->ctx = NULL;
        ferr(GLFW_VERSION_UNAVAILABLE, "EGL: failed to create context (0x%x)", eglGetError()); return 0;
    }
    w->surf = eglCreatePlatformWindowSurface(g.dpy, w->cfg, &w->id, NULL);
    if (w->surf == EGL_NO_SURFACE) {
        w->surf = NULL;
        ferr(GLFW_PLATFORM_ERROR, "EGL: failed to create window surface (0x%x)", eglGetError()); return 0;
    }
    return 1;
}

static xcb_visualtype_t* find_argb_visual(void) {
    for (xcb_depth_iterator_t d = xcb_screen_allowed_depths_iterator(g.scr); d.rem; xcb_depth_next(&d)) {
        if (d.data->depth != 32) continue;
        for (xcb_visualtype_iterator_t v = xcb_depth_visuals_iterator(d.data); v.rem; xcb_visualtype_next(&v))
            if (v.data->_class == XCB_VISUAL_CLASS_TRUE_COLOR && v.data->red_mask == 0xff0000 &&
                v.data->green_mask == 0xff00 && v.data->blue_mask == 0xff)
                return v.data;
    }
    return NULL;
}

/* ====================================================================== */
/*                               Windows                                  */
/* ====================================================================== */
GLFWAPI void glfwDefaultWindowHints(void) { REQ_INIT(); default_hints(); }

GLFWAPI void glfwWindowHint(int hint, int v) {
    REQ_INIT();
    switch (hint) {
    case GLFW_RED_BITS: H.r = v; break; case GLFW_GREEN_BITS: H.gr = v; break;
    case GLFW_BLUE_BITS: H.b = v; break; case GLFW_ALPHA_BITS: H.a = v; break;
    case GLFW_DEPTH_BITS: H.depth = v; break; case GLFW_STENCIL_BITS: H.stencil = v; break;
    case GLFW_SAMPLES: H.samples = v; break; case GLFW_SRGB_CAPABLE: H.srgb = v; break;
    case GLFW_DOUBLEBUFFER: H.doublebuf = v; break; case GLFW_REFRESH_RATE: H.refresh = v; break;
    case GLFW_STEREO: H.stereo = v; break; case GLFW_AUX_BUFFERS: H.aux = v; break;
    case GLFW_ACCUM_RED_BITS: case GLFW_ACCUM_GREEN_BITS: case GLFW_ACCUM_BLUE_BITS: case GLFW_ACCUM_ALPHA_BITS: break;
    case GLFW_FOCUSED: H.focused = v; break; case GLFW_RESIZABLE: H.resizable = v; break;
    case GLFW_VISIBLE: H.visible = v; break; case GLFW_DECORATED: H.decorated = v; break;
    case GLFW_AUTO_ICONIFY: H.autoIconify = v; break; case GLFW_FLOATING: H.floating = v; break;
    case GLFW_MAXIMIZED: H.maximized = v; break; case GLFW_CENTER_CURSOR: H.centerCursor = v; break;
    case GLFW_TRANSPARENT_FRAMEBUFFER: H.transparent = v; break;
    case GLFW_FOCUS_ON_SHOW: H.focusOnShow = v; break; case GLFW_MOUSE_PASSTHROUGH: H.passthrough = v; break;
    case GLFW_POSITION_X: H.posx = v; break; case GLFW_POSITION_Y: H.posy = v; break;
    case GLFW_SCALE_TO_MONITOR: case GLFW_SCALE_FRAMEBUFFER: case GLFW_COCOA_RETINA_FRAMEBUFFER: H.scaleToMonitor = v; break;
    case GLFW_CLIENT_API: H.client = v; break; case GLFW_CONTEXT_CREATION_API: H.source = v; break;
    case GLFW_CONTEXT_VERSION_MAJOR: H.major = v; break; case GLFW_CONTEXT_VERSION_MINOR: H.minor = v; break;
    case GLFW_CONTEXT_ROBUSTNESS: H.robust = v; break; case GLFW_OPENGL_FORWARD_COMPAT: H.fwd = v; break;
    case GLFW_CONTEXT_DEBUG: H.debug = v; break; case GLFW_OPENGL_PROFILE: H.profile = v; break;
    case GLFW_CONTEXT_RELEASE_BEHAVIOR: H.release = v; break; case GLFW_CONTEXT_NO_ERROR: H.noerror = v; break;
    case GLFW_COCOA_FRAME_NAME: case GLFW_COCOA_GRAPHICS_SWITCHING: case GLFW_WIN32_KEYBOARD_MENU:
    case GLFW_WIN32_SHOWDEFAULT: case GLFW_WAYLAND_APP_ID: break;
    default: ferr(GLFW_INVALID_ENUM, "Invalid window hint 0x%08X", hint);
    }
}
GLFWAPI void glfwWindowHintString(int hint, const char* v) {
    REQ_INIT();
    if (hint == GLFW_X11_CLASS_NAME) snprintf(H.cls, sizeof H.cls, "%s", v);
    else if (hint == GLFW_X11_INSTANCE_NAME) snprintf(H.inst, sizeof H.inst, "%s", v);
    else if (hint != GLFW_COCOA_FRAME_NAME && hint != GLFW_WAYLAND_APP_ID) ferr(GLFW_INVALID_ENUM, "Invalid window hint string 0x%08X", hint);
}

static void move_to_monitor(GLFWwindow* w, GLFWmonitor* m) {
    uint32_t v[4] = { (uint32_t)m->x, (uint32_t)m->y, (uint32_t)m->cur.width, (uint32_t)m->cur.height };
    xcb_configure_window(g.c, w->id, XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y | XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT, v);
}

GLFWAPI GLFWwindow* glfwCreateWindow(int width, int height, const char* title, GLFWmonitor* mon, GLFWwindow* share) {
    REQ_INIT(NULL);
    if (width <= 0 || height <= 0) { ferr(GLFW_INVALID_VALUE, "Invalid window size %ix%i", width, height); return NULL; }
    if (H.client != GLFW_NO_API && H.client != GLFW_OPENGL_API && H.client != GLFW_OPENGL_ES_API) {
        ferr(GLFW_INVALID_ENUM, "Invalid client API 0x%08X", H.client); return NULL;
    }
    if (H.source == GLFW_OSMESA_CONTEXT_API) { ferr(GLFW_API_UNAVAILABLE, "OSMesa is not supported"); return NULL; }
    if (share && !share->ctx) { ferr(GLFW_NO_WINDOW_CONTEXT, "Share window has no context"); return NULL; }

    GLFWwindow* w = calloc(1, sizeof *w);
    if (!w) { ferr(GLFW_OUT_OF_MEMORY, "Out of memory"); return NULL; }
    w->title = sdup(title ? title : "");
    w->resizable = H.resizable; w->decorated = H.decorated; w->floating = H.floating;
    w->autoIconify = H.autoIconify; w->focusOnShow = H.focusOnShow; w->passthrough = H.passthrough;
    w->minw = w->minh = w->maxw = w->maxh = w->numer = w->denom = GLFW_DONT_CARE;
    w->client = H.client; w->major = H.major; w->minor = H.minor; w->profile = H.profile; w->fwd = H.fwd;
    w->debug = H.debug; w->release = H.release; w->noerror = H.noerror; w->robust = H.robust;
    w->doublebuffer = H.doublebuf; w->source = GLFW_EGL_CONTEXT_API;
    w->cursorMode = GLFW_CURSOR_NORMAL; w->lockMods = 0;
    w->warpx = w->warpy = -1;
    w->transparent = H.transparent;
    w->monitor = mon;

    int fbs[8] = { H.r, H.gr, H.b, H.a, H.depth, H.stencil, H.samples, 0 };

    xcb_visualid_t vis = g.scr->root_visual; w->depth = g.scr->root_depth;
    if (H.transparent) {
        xcb_visualtype_t* vt = find_argb_visual();
        if (vt) { vis = vt->visual_id; w->depth = 32; } else w->transparent = 0;
    }
    int ww = width, wh = height;
    if (H.scaleToMonitor) { ww = (int)(ww * g.scale); wh = (int)(wh * g.scale); }
    ww = imax(1, ww); wh = imax(1, wh);
    int px = 0, py = 0;
    if (H.posx != (int)GLFW_ANY_POSITION && H.posy != (int)GLFW_ANY_POSITION) { px = H.posx; py = H.posy; w->hintPos = 1; }

    w->cmap = xcb_generate_id(g.c);
    xcb_create_colormap(g.c, XCB_COLORMAP_ALLOC_NONE, w->cmap, g.root, vis);
    uint32_t vals[3] = {
        0,
        XCB_EVENT_MASK_KEY_PRESS | XCB_EVENT_MASK_KEY_RELEASE | XCB_EVENT_MASK_BUTTON_PRESS | XCB_EVENT_MASK_BUTTON_RELEASE |
        XCB_EVENT_MASK_ENTER_WINDOW | XCB_EVENT_MASK_LEAVE_WINDOW | XCB_EVENT_MASK_POINTER_MOTION | XCB_EVENT_MASK_EXPOSURE |
        XCB_EVENT_MASK_VISIBILITY_CHANGE | XCB_EVENT_MASK_STRUCTURE_NOTIFY | XCB_EVENT_MASK_FOCUS_CHANGE | XCB_EVENT_MASK_PROPERTY_CHANGE,
        w->cmap
    };
    w->id = xcb_generate_id(g.c);
    xcb_void_cookie_t ck = xcb_create_window_checked(g.c, w->depth, w->id, g.root, px, py, ww, wh, 0,
        XCB_WINDOW_CLASS_INPUT_OUTPUT, vis,
        XCB_CW_BORDER_PIXEL | XCB_CW_EVENT_MASK | XCB_CW_COLORMAP, vals);
    xcb_generic_error_t* xe = xcb_request_check(g.c, ck);
    if (xe) { ferr(GLFW_PLATFORM_ERROR, "X11: failed to create window (error %d)", xe->error_code); free(xe); goto fail; }
    w->x = px; w->y = py; w->w = ww; w->h = wh;

    /* properties */
    { xcb_atom_t p[] = { A.WM_DELETE_WINDOW, A.NET_WM_PING };
      xcb_change_property(g.c, XCB_PROP_MODE_REPLACE, w->id, A.WM_PROTOCOLS, XCB_ATOM_ATOM, 32, 2, p); }
    { uint32_t pid = getpid();
      xcb_change_property(g.c, XCB_PROP_MODE_REPLACE, w->id, A.NET_WM_PID, XCB_ATOM_CARDINAL, 32, 1, &pid); }
    { char cls[200]; const char *inst = H.inst[0] ? H.inst : (getenv("RESOURCE_NAME") ? getenv("RESOURCE_NAME") : (w->title[0] ? w->title : "glfw-application"));
      const char* cl = H.cls[0] ? H.cls : (w->title[0] ? w->title : "GLFW-Application");
      int n = snprintf(cls, sizeof cls, "%s%c%s%c", inst, 0, cl, 0);
      xcb_change_property(g.c, XCB_PROP_MODE_REPLACE, w->id, XCB_ATOM_WM_CLASS, XCB_ATOM_STRING, 8, n, cls); }
    set_title(w, w->title);
    if (!w->decorated) set_decorated(w, 0);
    update_size_hints(w, ww, wh);
    { xcb_atom_t st[3]; int n = 0;
      if (H.floating) st[n++] = A.NET_WM_STATE_ABOVE;
      if (H.maximized && !mon) { st[n++] = A.NET_WM_STATE_MAX_V; st[n++] = A.NET_WM_STATE_MAX_H; w->maximized = 1; }
      if (n) xcb_change_property(g.c, XCB_PROP_MODE_REPLACE, w->id, A.NET_WM_STATE, XCB_ATOM_ATOM, 32, n, st); }

    if (H.client != GLFW_NO_API && !create_context(w, share, fbs)) goto fail;

    w->next = g.wins; g.wins = w;
    if (mon) {
        glfwShowWindow(w);
        send_wm(w, A.NET_WM_STATE, NET_ADD, A.NET_WM_STATE_FULLSCREEN, 0, 1, 0);
        move_to_monitor(w, mon);
        mon->window = w;
    } else if (H.visible) {
        glfwShowWindow(w);
        if (H.focused) glfwFocusWindow(w);
    }
    xcb_flush(g.c);
    return w;

fail:
    if (w->surf) eglDestroySurface(g.dpy, w->surf);
    if (w->ctx) eglDestroyContext(g.dpy, w->ctx);
    if (w->id) xcb_destroy_window(g.c, w->id);
    if (w->cmap) xcb_free_colormap(g.c, w->cmap);
    xcb_flush(g.c);
    free(w->title); free(w);
    return NULL;
}

GLFWAPI void glfwDestroyWindow(GLFWwindow* w) {
    REQ_INIT();
    if (!w) return;
    if (g.disabledWin == w) enable_cursor(w);
    if (t_cur == w) glfwMakeContextCurrent(NULL);
    if (w->monitor) w->monitor->window = NULL;
    if (w->surf) eglDestroySurface(g.dpy, w->surf);
    if (w->ctx) eglDestroyContext(g.dpy, w->ctx);
    xcb_unmap_window(g.c, w->id);
    xcb_destroy_window(g.c, w->id);
    xcb_free_colormap(g.c, w->cmap);
    xcb_flush(g.c);
    for (GLFWwindow** p = &g.wins; *p; p = &(*p)->next) if (*p == w) { *p = w->next; break; }
    free(w->title); free(w);
}

GLFWAPI int glfwWindowShouldClose(GLFWwindow* w) { REQ_INIT(0); return w->shouldClose; }
GLFWAPI void glfwSetWindowShouldClose(GLFWwindow* w, int v) { REQ_INIT(); w->shouldClose = v; }
GLFWAPI const char* glfwGetWindowTitle(GLFWwindow* w) { REQ_INIT(NULL); return w->title; }
GLFWAPI void glfwSetWindowTitle(GLFWwindow* w, const char* t) {
    REQ_INIT(); free(w->title); w->title = sdup(t); set_title(w, t); xcb_flush(g.c);
}

GLFWAPI void glfwSetWindowIcon(GLFWwindow* w, int count, const GLFWimage* im) {
    REQ_INIT();
    if (count < 0 || (count && !im)) { ferr(GLFW_INVALID_VALUE, "Invalid image count"); return; }
    if (!count) { xcb_delete_property(g.c, w->id, A.NET_WM_ICON); xcb_flush(g.c); return; }
    size_t n = 0;
    for (int i = 0; i < count; i++) n += 2 + (size_t)im[i].width * im[i].height;
    uint32_t* d = malloc(n * 4), *t = d;
    if (!d) { ferr(GLFW_OUT_OF_MEMORY, "Out of memory"); return; }
    for (int i = 0; i < count; i++) {
        *t++ = im[i].width; *t++ = im[i].height;
        for (int j = 0; j < im[i].width * im[i].height; j++) {
            const unsigned char* p = im[i].pixels + j * 4;
            *t++ = ((uint32_t)p[3] << 24) | ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
        }
    }
    xcb_change_property(g.c, XCB_PROP_MODE_REPLACE, w->id, A.NET_WM_ICON, XCB_ATOM_CARDINAL, 32, n, d);
    xcb_flush(g.c);
    free(d);
}

GLFWAPI void glfwGetWindowPos(GLFWwindow* w, int* x, int* y) {
    if (x) *x = 0; if (y) *y = 0; REQ_INIT();
    xcb_translate_coordinates_reply_t* t = xcb_translate_coordinates_reply(g.c, xcb_translate_coordinates(g.c, w->id, g.root, 0, 0), NULL);
    if (t) { if (x) *x = t->dst_x; if (y) *y = t->dst_y; free(t); }
}
GLFWAPI void glfwSetWindowPos(GLFWwindow* w, int x, int y) {
    REQ_INIT();
    if (w->monitor) return;
    uint32_t v[2] = { (uint32_t)x, (uint32_t)y };
    xcb_configure_window(g.c, w->id, XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y, v);
    xcb_flush(g.c);
}
GLFWAPI void glfwGetWindowSize(GLFWwindow* w, int* ww, int* hh) {
    if (ww) *ww = 0; if (hh) *hh = 0; REQ_INIT();
    xcb_get_geometry_reply_t* r = xcb_get_geometry_reply(g.c, xcb_get_geometry(g.c, w->id), NULL);
    if (r) { if (ww) *ww = r->width; if (hh) *hh = r->height; free(r); }
    else { if (ww) *ww = w->w; if (hh) *hh = w->h; }
}
GLFWAPI void glfwSetWindowSizeLimits(GLFWwindow* w, int a, int b, int c, int d) {
    REQ_INIT();
    if (a != GLFW_DONT_CARE && b != GLFW_DONT_CARE && (a < 0 || b < 0)) { ferr(GLFW_INVALID_VALUE, "Invalid window minimum size"); return; }
    if (c != GLFW_DONT_CARE && d != GLFW_DONT_CARE && (c < a || d < b)) { ferr(GLFW_INVALID_VALUE, "Invalid window maximum size"); return; }
    w->minw = a; w->minh = b; w->maxw = c; w->maxh = d;
    if (w->monitor || !w->resizable) return;
    update_size_hints(w, w->w, w->h); xcb_flush(g.c);
}
GLFWAPI void glfwSetWindowAspectRatio(GLFWwindow* w, int n, int d) {
    REQ_INIT();
    if (n != GLFW_DONT_CARE && d != GLFW_DONT_CARE && (n <= 0 || d <= 0)) { ferr(GLFW_INVALID_VALUE, "Invalid aspect ratio"); return; }
    w->numer = n; w->denom = d;
    if (w->monitor || !w->resizable) return;
    update_size_hints(w, w->w, w->h); xcb_flush(g.c);
}
GLFWAPI void glfwSetWindowSize(GLFWwindow* w, int ww, int hh) {
    REQ_INIT();
    ww = imax(1, ww); hh = imax(1, hh);
    if (w->monitor) { if (w->monitor->window == w) move_to_monitor(w, w->monitor); }
    else {
        if (!w->resizable) update_size_hints(w, ww, hh);
        uint32_t v[2] = { (uint32_t)ww, (uint32_t)hh };
        xcb_configure_window(g.c, w->id, XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT, v);
    }
    xcb_flush(g.c);
}
GLFWAPI void glfwGetFramebufferSize(GLFWwindow* w, int* ww, int* hh) { glfwGetWindowSize(w, ww, hh); }
GLFWAPI void glfwGetWindowFrameSize(GLFWwindow* w, int* l, int* t, int* r, int* b) {
    if (l) *l = 0; if (t) *t = 0; if (r) *r = 0; if (b) *b = 0; REQ_INIT();
    if (w->monitor || !w->decorated) return;
    uint32_t n = 0; uint32_t* e = get_prop(w->id, A.NET_FRAME_EXTENTS, XCB_ATOM_CARDINAL, &n);
    if (e && n == 4) { if (l) *l = e[0]; if (r) *r = e[1]; if (t) *t = e[2]; if (b) *b = e[3]; }
    free(e);
}
GLFWAPI void glfwGetWindowContentScale(GLFWwindow* w, float* x, float* y) {
    (void)w; if (x) *x = 0; if (y) *y = 0; REQ_INIT(); if (x) *x = g.scale; if (y) *y = g.scale;
}
GLFWAPI float glfwGetWindowOpacity(GLFWwindow* w) {
    REQ_INIT(1.f);
    uint32_t n = 0; uint32_t* v = get_prop(w->id, A.NET_WM_OPACITY, XCB_ATOM_CARDINAL, &n);
    float o = (v && n) ? (float)(v[0] / (double)0xffffffffu) : 1.f;
    free(v); return o;
}
GLFWAPI void glfwSetWindowOpacity(GLFWwindow* w, float o) {
    REQ_INIT();
    uint32_t v = (uint32_t)(0xffffffffu * (double)o);
    xcb_change_property(g.c, XCB_PROP_MODE_REPLACE, w->id, A.NET_WM_OPACITY, XCB_ATOM_CARDINAL, 32, 1, &v);
    xcb_flush(g.c);
}

GLFWAPI void glfwIconifyWindow(GLFWwindow* w) {
    REQ_INIT();
    xcb_client_message_event_t ev; memset(&ev, 0, sizeof ev);
    ev.response_type = XCB_CLIENT_MESSAGE; ev.format = 32; ev.window = w->id; ev.type = A.WM_CHANGE_STATE;
    ev.data.data32[0] = 3;   /* IconicState */
    xcb_send_event(g.c, 0, g.root, XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY | XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT, (const char*)&ev);
    xcb_flush(g.c);
}
GLFWAPI void glfwRestoreWindow(GLFWwindow* w) {
    REQ_INIT();
    if (query_iconified(w)) xcb_map_window(g.c, w->id);
    else if (w->visible) send_wm(w, A.NET_WM_STATE, NET_REMOVE, A.NET_WM_STATE_MAX_V, A.NET_WM_STATE_MAX_H, 1, 0);
    xcb_flush(g.c);
}
GLFWAPI void glfwMaximizeWindow(GLFWwindow* w) {
    REQ_INIT();
    if (w->visible) send_wm(w, A.NET_WM_STATE, NET_ADD, A.NET_WM_STATE_MAX_V, A.NET_WM_STATE_MAX_H, 1, 0);
    else {
        xcb_atom_t st[2] = { A.NET_WM_STATE_MAX_V, A.NET_WM_STATE_MAX_H };
        xcb_change_property(g.c, XCB_PROP_MODE_APPEND, w->id, A.NET_WM_STATE, XCB_ATOM_ATOM, 32, 2, st);
    }
    xcb_flush(g.c);
}
GLFWAPI void glfwShowWindow(GLFWwindow* w) {
    REQ_INIT();
    if (w->visible) return;
    xcb_map_window(g.c, w->id);
    w->visible = 1;
    xcb_flush(g.c);
}
GLFWAPI void glfwHideWindow(GLFWwindow* w) { REQ_INIT(); xcb_unmap_window(g.c, w->id); w->visible = 0; xcb_flush(g.c); }
GLFWAPI void glfwRequestWindowAttention(GLFWwindow* w) {
    REQ_INIT(); send_wm(w, A.NET_WM_STATE, NET_ADD, A.NET_WM_STATE_ATTENTION, 0, 1, 0); xcb_flush(g.c);
}
GLFWAPI void glfwFocusWindow(GLFWwindow* w) {
    REQ_INIT();
    send_wm(w, A.NET_ACTIVE_WINDOW, 1, 0, 0, 0, 0);
    if (w->visible) {   /* also works without a window manager (e.g. bare termux-x11) */
        uint32_t z = XCB_STACK_MODE_ABOVE;
        xcb_configure_window(g.c, w->id, XCB_CONFIG_WINDOW_STACK_MODE, &z);
        xcb_set_input_focus(g.c, XCB_INPUT_FOCUS_PARENT, w->id, XCB_CURRENT_TIME);
    }
    xcb_flush(g.c);
}
GLFWAPI GLFWmonitor* glfwGetWindowMonitor(GLFWwindow* w) { REQ_INIT(NULL); return w->monitor; }

GLFWAPI void glfwSetWindowMonitor(GLFWwindow* w, GLFWmonitor* m, int x, int y, int ww, int hh, int hz) {
    (void)hz; REQ_INIT();
    ww = imax(1, ww); hh = imax(1, hh);
    if (w->monitor == m) {
        if (m) { if (m->window == w) move_to_monitor(w, m); }
        else {
            if (!w->resizable) update_size_hints(w, ww, hh);
            uint32_t v[4] = { (uint32_t)x, (uint32_t)y, (uint32_t)ww, (uint32_t)hh };
            xcb_configure_window(g.c, w->id, XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y | XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT, v);
        }
        xcb_flush(g.c); return;
    }
    if (w->monitor) { w->monitor->window = NULL; set_decorated(w, w->decorated); }
    w->monitor = m;
    update_size_hints(w, ww, hh);
    if (m) {
        if (!w->visible) glfwShowWindow(w);
        send_wm(w, A.NET_WM_STATE, NET_ADD, A.NET_WM_STATE_FULLSCREEN, 0, 1, 0);
        move_to_monitor(w, m);
        m->window = w;
    } else {
        send_wm(w, A.NET_WM_STATE, NET_REMOVE, A.NET_WM_STATE_FULLSCREEN, 0, 1, 0);
        uint32_t v[4] = { (uint32_t)x, (uint32_t)y, (uint32_t)ww, (uint32_t)hh };
        xcb_configure_window(g.c, w->id, XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y | XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT, v);
    }
    xcb_flush(g.c);
}

GLFWAPI int glfwGetWindowAttrib(GLFWwindow* w, int a) {
    REQ_INIT(0);
    switch (a) {
    case GLFW_FOCUSED: return w->focused;
    case GLFW_ICONIFIED: return w->iconified;
    case GLFW_VISIBLE: return w->visible;
    case GLFW_HOVERED: return w->hovered;
    case GLFW_RESIZABLE: return w->resizable;
    case GLFW_DECORATED: return w->decorated;
    case GLFW_AUTO_ICONIFY: return w->autoIconify;
    case GLFW_FLOATING: return w->floating;
    case GLFW_MAXIMIZED: return w->maximized;
    case GLFW_TRANSPARENT_FRAMEBUFFER: return w->transparent;
    case GLFW_FOCUS_ON_SHOW: return w->focusOnShow;
    case GLFW_MOUSE_PASSTHROUGH: return w->passthrough;
    case GLFW_DOUBLEBUFFER: return w->doublebuffer;
    case GLFW_CLIENT_API: return w->client;
    case GLFW_CONTEXT_CREATION_API: return w->ctx ? w->source : 0;
    case GLFW_CONTEXT_VERSION_MAJOR: return w->major;
    case GLFW_CONTEXT_VERSION_MINOR: return w->minor;
    case GLFW_CONTEXT_REVISION: return 0;
    case GLFW_CONTEXT_ROBUSTNESS: return w->robust;
    case GLFW_OPENGL_FORWARD_COMPAT: return w->fwd;
    case GLFW_CONTEXT_DEBUG: return w->debug;
    case GLFW_OPENGL_PROFILE: return w->profile;
    case GLFW_CONTEXT_RELEASE_BEHAVIOR: return w->release;
    case GLFW_CONTEXT_NO_ERROR: return w->noerror;
    }
    ferr(GLFW_INVALID_ENUM, "Invalid window attribute 0x%08X", a);
    return 0;
}
GLFWAPI void glfwSetWindowAttrib(GLFWwindow* w, int a, int v) {
    REQ_INIT();
    v = v ? GLFW_TRUE : GLFW_FALSE;
    switch (a) {
    case GLFW_AUTO_ICONIFY: w->autoIconify = v; return;
    case GLFW_RESIZABLE: w->resizable = v; if (!w->monitor) { update_size_hints(w, w->w, w->h); xcb_flush(g.c); } return;
    case GLFW_DECORATED: w->decorated = v; if (!w->monitor) { set_decorated(w, v); xcb_flush(g.c); } return;
    case GLFW_FLOATING:
        w->floating = v;
        if (!w->monitor && w->visible) send_wm(w, A.NET_WM_STATE, v ? NET_ADD : NET_REMOVE, A.NET_WM_STATE_ABOVE, 0, 1, 0);
        xcb_flush(g.c); return;
    case GLFW_FOCUS_ON_SHOW: w->focusOnShow = v; return;
    case GLFW_MOUSE_PASSTHROUGH: w->passthrough = v; ferr(GLFW_FEATURE_UNAVAILABLE, "X11: mouse passthrough not implemented"); return;
    }
    ferr(GLFW_INVALID_ENUM, "Invalid window attribute 0x%08X", a);
}
GLFWAPI void glfwSetWindowUserPointer(GLFWwindow* w, void* p) { REQ_INIT(); w->user = p; }
GLFWAPI void* glfwGetWindowUserPointer(GLFWwindow* w) { REQ_INIT(NULL); return w->user; }

#define CBSET(fn, type, field) \
    GLFWAPI type fn(GLFWwindow* w, type cb) { REQ_INIT(NULL); type o = w->cb.field; w->cb.field = cb; return o; }
CBSET(glfwSetWindowPosCallback, GLFWwindowposfun, pos)
CBSET(glfwSetWindowSizeCallback, GLFWwindowsizefun, size)
CBSET(glfwSetWindowCloseCallback, GLFWwindowclosefun, close)
CBSET(glfwSetWindowRefreshCallback, GLFWwindowrefreshfun, refresh)
CBSET(glfwSetWindowFocusCallback, GLFWwindowfocusfun, focus)
CBSET(glfwSetWindowIconifyCallback, GLFWwindowiconifyfun, iconify)
CBSET(glfwSetWindowMaximizeCallback, GLFWwindowmaximizefun, maximize)
CBSET(glfwSetFramebufferSizeCallback, GLFWframebuffersizefun, fbsize)
CBSET(glfwSetWindowContentScaleCallback, GLFWwindowcontentscalefun, scale)
CBSET(glfwSetKeyCallback, GLFWkeyfun, key)
CBSET(glfwSetCharCallback, GLFWcharfun, chr)
CBSET(glfwSetCharModsCallback, GLFWcharmodsfun, charmods)
CBSET(glfwSetMouseButtonCallback, GLFWmousebuttonfun, mouse)
CBSET(glfwSetCursorPosCallback, GLFWcursorposfun, cursorpos)
CBSET(glfwSetCursorEnterCallback, GLFWcursorenterfun, enter)
CBSET(glfwSetScrollCallback, GLFWscrollfun, scroll)
CBSET(glfwSetDropCallback, GLFWdropfun, drop)

/* ====================================================================== */
/*                                Events                                  */
/* ====================================================================== */
GLFWAPI void glfwPollEvents(void) { REQ_INIT(); pump(); }
GLFWAPI void glfwWaitEvents(void) { REQ_INIT(); wait_for(-1); }
GLFWAPI void glfwWaitEventsTimeout(double t) {
    REQ_INIT();
    if (t != t || t < 0.0 || t > 1e9) { ferr(GLFW_INVALID_VALUE, "Invalid time %f", t); return; }
    wait_for(t);
}
GLFWAPI void glfwPostEmptyEvent(void) {
    REQ_INIT();
    for (;;) { char b = 0; ssize_t r = write(g.pipe[1], &b, 1); if (r == 1 || (r == -1 && errno != EINTR)) break; }
}

/* ====================================================================== */
/*                                 Input                                  */
/* ====================================================================== */
GLFWAPI int glfwGetInputMode(GLFWwindow* w, int m) {
    REQ_INIT(0);
    switch (m) {
    case GLFW_CURSOR: return w->cursorMode;
    case GLFW_STICKY_KEYS: return w->stickyKeys;
    case GLFW_STICKY_MOUSE_BUTTONS: return w->stickyButtons;
    case GLFW_LOCK_KEY_MODS: return w->lockMods;
    case GLFW_RAW_MOUSE_MOTION: return w->rawMotion;
    case GLFW_UNLIMITED_MOUSE_BUTTONS: return w->unlimited;
    }
    ferr(GLFW_INVALID_ENUM, "Invalid input mode 0x%08X", m);
    return 0;
}

GLFWAPI void glfwSetInputMode(GLFWwindow* w, int mode, int v) {
    REQ_INIT();
    switch (mode) {
    case GLFW_CURSOR: {
        if (v != GLFW_CURSOR_NORMAL && v != GLFW_CURSOR_HIDDEN && v != GLFW_CURSOR_DISABLED && v != GLFW_CURSOR_CAPTURED) {
            ferr(GLFW_INVALID_ENUM, "Invalid cursor mode 0x%08X", v); return;
        }
        if (w->cursorMode == v) return;
        int was = w->cursorMode;
        if (v == GLFW_CURSOR_DISABLED) { double x, y; glfwGetCursorPos(w, &x, &y); w->vx = x; w->vy = y; }
        w->cursorMode = v;
        if (w->focused) {
            if (v == GLFW_CURSOR_DISABLED) disable_cursor(w);
            else if (was == GLFW_CURSOR_DISABLED) enable_cursor(w);
            if (v == GLFW_CURSOR_CAPTURED) grab_ptr(w);
            else if (was == GLFW_CURSOR_CAPTURED) ungrab_ptr();
        }
        update_cursor_image(w); xcb_flush(g.c);
        return;
    }
    case GLFW_STICKY_KEYS:
        v = v ? 1 : 0;
        if (w->stickyKeys == v) return;
        if (!v) for (int k = 0; k <= GLFW_KEY_LAST; k++) if (w->keys[k] == STICK) w->keys[k] = GLFW_RELEASE;
        w->stickyKeys = v; return;
    case GLFW_STICKY_MOUSE_BUTTONS:
        v = v ? 1 : 0;
        if (w->stickyButtons == v) return;
        if (!v) for (int b = 0; b <= GLFW_MOUSE_BUTTON_LAST; b++) if (w->buttons[b] == STICK) w->buttons[b] = GLFW_RELEASE;
        w->stickyButtons = v; return;
    case GLFW_LOCK_KEY_MODS: w->lockMods = v ? 1 : 0; return;
    case GLFW_UNLIMITED_MOUSE_BUTTONS: w->unlimited = v ? 1 : 0; return;
    case GLFW_RAW_MOUSE_MOTION:
        if (v) ferr(GLFW_FEATURE_UNAVAILABLE, "X11: raw mouse motion not supported");
        return;
    }
    ferr(GLFW_INVALID_ENUM, "Invalid input mode 0x%08X", mode);
}
GLFWAPI int glfwRawMouseMotionSupported(void) { REQ_INIT(GLFW_FALSE); return GLFW_FALSE; }

GLFWAPI const char* glfwGetKeyName(int key, int sc) {
    REQ_INIT(NULL);
    if (key != GLFW_KEY_UNKNOWN) {
        if (key < GLFW_KEY_SPACE || key > GLFW_KEY_LAST) { ferr(GLFW_INVALID_ENUM, "Invalid key %i", key); return NULL; }
        if ((key > GLFW_KEY_WORLD_2 && key < GLFW_KEY_KP_0) || key == GLFW_KEY_KP_ENTER || key == GLFW_KEY_ENTER ||
            key == GLFW_KEY_TAB || key == GLFW_KEY_BACKSPACE || key > GLFW_KEY_KP_EQUAL) return NULL;
        sc = g.scancodes[key];
    }
    if (sc < 8 || sc > 255 || !g.xmap) return NULL;
    const xkb_keysym_t* syms; 
    if (xkb_keymap_key_get_syms_by_level(g.xmap, sc, 0, 0, &syms) < 1) return NULL;
    int n = xkb_keysym_to_utf8(syms[0], g.keyname, sizeof g.keyname);
    if (n <= 1 || (unsigned char)g.keyname[0] < 32) return NULL;
    return g.keyname;
}
GLFWAPI int glfwGetKeyScancode(int key) {
    if (key < GLFW_KEY_SPACE || key > GLFW_KEY_LAST) { ferr(GLFW_INVALID_ENUM, "Invalid key %i", key); return -1; }
    return g.scancodes[key];
}
GLFWAPI int glfwGetKey(GLFWwindow* w, int key) {
    REQ_INIT(GLFW_RELEASE);
    if (key < GLFW_KEY_SPACE || key > GLFW_KEY_LAST) { ferr(GLFW_INVALID_ENUM, "Invalid key %i", key); return GLFW_RELEASE; }
    if (w->keys[key] == STICK) { w->keys[key] = GLFW_RELEASE; return GLFW_PRESS; }
    return w->keys[key];
}
GLFWAPI int glfwGetMouseButton(GLFWwindow* w, int b) {
    REQ_INIT(GLFW_RELEASE);
    if (b < GLFW_MOUSE_BUTTON_1 || b > GLFW_MOUSE_BUTTON_LAST) { ferr(GLFW_INVALID_ENUM, "Invalid mouse button %i", b); return GLFW_RELEASE; }
    if (w->buttons[b] == STICK) { w->buttons[b] = GLFW_RELEASE; return GLFW_PRESS; }
    return w->buttons[b];
}
GLFWAPI void glfwGetCursorPos(GLFWwindow* w, double* x, double* y) {
    if (x) *x = 0; if (y) *y = 0; REQ_INIT();
    if (w->cursorMode == GLFW_CURSOR_DISABLED) { if (x) *x = w->vx; if (y) *y = w->vy; return; }
    int px, py; query_ptr(w, &px, &py);
    if (x) *x = px; if (y) *y = py;
}
GLFWAPI void glfwSetCursorPos(GLFWwindow* w, double x, double y) {
    REQ_INIT();
    if (x != x || y != y || fabs(x) > 1e9 || fabs(y) > 1e9) { ferr(GLFW_INVALID_VALUE, "Invalid cursor position"); return; }
    if (!w->focused) return;
    if (w->cursorMode == GLFW_CURSOR_DISABLED) { w->vx = x; w->vy = y; return; }
    warp(w, (int)x, (int)y);
}

/* ---- Cursors ---- */
GLFWAPI GLFWcursor* glfwCreateCursor(const GLFWimage* im, int xh, int yh) {
    (void)im; (void)xh; (void)yh; REQ_INIT(NULL);
    ferr(GLFW_FEATURE_UNIMPLEMENTED, "X11: custom cursors not implemented in this build"); return NULL;
}
GLFWAPI GLFWcursor* glfwCreateStandardCursor(int shape) {
    REQ_INIT(NULL);
    int glyph;
    switch (shape) {
    case GLFW_ARROW_CURSOR: glyph = 68; break;          /* left_ptr */
    case GLFW_IBEAM_CURSOR: glyph = 152; break;         /* xterm */
    case GLFW_CROSSHAIR_CURSOR: glyph = 34; break;
    case GLFW_POINTING_HAND_CURSOR: glyph = 60; break;  /* hand2 */
    case GLFW_RESIZE_EW_CURSOR: glyph = 108; break;
    case GLFW_RESIZE_NS_CURSOR: glyph = 116; break;
    case GLFW_RESIZE_ALL_CURSOR: glyph = 52; break;     /* fleur */
    case GLFW_RESIZE_NWSE_CURSOR: case GLFW_RESIZE_NESW_CURSOR: case GLFW_NOT_ALLOWED_CURSOR:
        ferr(GLFW_CURSOR_UNAVAILABLE, "X11: standard cursor shape unavailable"); return NULL;
    default: ferr(GLFW_INVALID_ENUM, "Invalid standard cursor 0x%08X", shape); return NULL;
    }
    GLFWcursor* c = calloc(1, sizeof *c);
    c->id = xcb_generate_id(g.c);
    xcb_create_glyph_cursor(g.c, c->id, g.curfont, g.curfont, glyph, glyph + 1, 0, 0, 0, 0xffff, 0xffff, 0xffff);
    xcb_flush(g.c);
    return c;
}
GLFWAPI void glfwDestroyCursor(GLFWcursor* c) {
    REQ_INIT();
    if (!c) return;
    for (GLFWwindow* w = g.wins; w; w = w->next) if (w->cursor == c) glfwSetCursor(w, NULL);
    xcb_free_cursor(g.c, c->id); xcb_flush(g.c); free(c);
}
GLFWAPI void glfwSetCursor(GLFWwindow* w, GLFWcursor* c) {
    REQ_INIT();
    w->cursor = c;
    if (w->cursorMode == GLFW_CURSOR_NORMAL || w->cursorMode == GLFW_CURSOR_CAPTURED) { update_cursor_image(w); xcb_flush(g.c); }
}

/* ---- Clipboard ---- */
GLFWAPI void glfwSetClipboardString(GLFWwindow* w, const char* s) {
    (void)w; REQ_INIT();
    free(g.clip); g.clip = sdup(s);
    xcb_set_selection_owner(g.c, g.helper, A.CLIPBOARD, XCB_CURRENT_TIME);
    xcb_flush(g.c);
}
GLFWAPI const char* glfwGetClipboardString(GLFWwindow* w) {
    (void)w; REQ_INIT(NULL);
    xcb_get_selection_owner_reply_t* o = xcb_get_selection_owner_reply(g.c, xcb_get_selection_owner(g.c, A.CLIPBOARD), NULL);
    xcb_window_t owner = o ? o->owner : XCB_NONE; free(o);
    if (owner == g.helper) return g.clip;
    if (owner == XCB_NONE) { ferr(GLFW_FORMAT_UNAVAILABLE, "X11: clipboard is empty"); return NULL; }
    static char* got;
    free(got); got = NULL;
    g.selDone = 0;
    xcb_convert_selection(g.c, g.helper, A.CLIPBOARD, A.UTF8_STRING, A.GLFW_SEL, XCB_CURRENT_TIME);
    xcb_flush(g.c);
    double end = now_s() + 1.0;
    while (!g.selDone && now_s() < end) wait_for(0.05);
    if (!g.selDone || g.selProp == XCB_NONE) { ferr(GLFW_FORMAT_UNAVAILABLE, "X11: failed to convert selection to string"); return NULL; }
    xcb_get_property_reply_t* r = xcb_get_property_reply(g.c,
        xcb_get_property(g.c, 1, g.helper, A.GLFW_SEL, XCB_GET_PROPERTY_TYPE_ANY, 0, 0x3fffffff), NULL);
    if (r) {
        int n = xcb_get_property_value_length(r);
        got = malloc(n + 1);
        if (got) { memcpy(got, xcb_get_property_value(r), n); got[n] = 0; }
        free(r);
    }
    if (!got) ferr(GLFW_FORMAT_UNAVAILABLE, "X11: failed to read selection");
    return got;
}

/* ---- Time ---- */
GLFWAPI double glfwGetTime(void) { REQ_INIT(0.0); return now_s(); }
GLFWAPI void glfwSetTime(double t) {
    REQ_INIT();
    if (t != t || t < 0.0 || t > 18446744073.0) { ferr(GLFW_INVALID_VALUE, "Invalid time %f", t); return; }
    struct timespec n; clock_gettime(CLOCK_MONOTONIC, &n);
    long long ns = (long long)(t * 1e9);
    long long base = (long long)n.tv_sec * 1000000000LL + n.tv_nsec - ns;
    g.t0.tv_sec = base / 1000000000LL; g.t0.tv_nsec = base % 1000000000LL;
}
GLFWAPI uint64_t glfwGetTimerValue(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ULL + t.tv_nsec;
}
GLFWAPI uint64_t glfwGetTimerFrequency(void) { return 1000000000ULL; }

/* ---- Joysticks (not implemented) ---- */
GLFWAPI int glfwJoystickPresent(int jid) {
    REQ_INIT(0);
    if (jid < 0 || jid > GLFW_JOYSTICK_LAST) { ferr(GLFW_INVALID_ENUM, "Invalid joystick ID %i", jid); }
    return GLFW_FALSE;
}
GLFWAPI const float* glfwGetJoystickAxes(int jid, int* n) { *n = 0; (void)jid; REQ_INIT(NULL); return NULL; }
GLFWAPI const unsigned char* glfwGetJoystickButtons(int jid, int* n) { *n = 0; (void)jid; REQ_INIT(NULL); return NULL; }
GLFWAPI const unsigned char* glfwGetJoystickHats(int jid, int* n) { *n = 0; (void)jid; REQ_INIT(NULL); return NULL; }
GLFWAPI const char* glfwGetJoystickName(int jid) { (void)jid; REQ_INIT(NULL); return NULL; }
GLFWAPI const char* glfwGetJoystickGUID(int jid) { (void)jid; REQ_INIT(NULL); return NULL; }
GLFWAPI void glfwSetJoystickUserPointer(int jid, void* p) { REQ_INIT(); if (jid >= 0 && jid < 16) g.joyUser[jid] = p; }
GLFWAPI void* glfwGetJoystickUserPointer(int jid) { REQ_INIT(NULL); return (jid >= 0 && jid < 16) ? g.joyUser[jid] : NULL; }
GLFWAPI int glfwJoystickIsGamepad(int jid) { (void)jid; REQ_INIT(0); return GLFW_FALSE; }
GLFWAPI GLFWjoystickfun glfwSetJoystickCallback(GLFWjoystickfun cb) { REQ_INIT(NULL); GLFWjoystickfun o = g.joycb; g.joycb = cb; return o; }
GLFWAPI int glfwUpdateGamepadMappings(const char* s) { (void)s; REQ_INIT(0); return GLFW_TRUE; }
GLFWAPI const char* glfwGetGamepadName(int jid) { (void)jid; REQ_INIT(NULL); return NULL; }
GLFWAPI int glfwGetGamepadState(int jid, GLFWgamepadstate* s) { (void)jid; REQ_INIT(0); memset(s, 0, sizeof *s); return GLFW_FALSE; }

/* ====================================================================== */
/*                               Context                                  */
/* ====================================================================== */
GLFWAPI void glfwMakeContextCurrent(GLFWwindow* w) {
    REQ_INIT();
    if (w && !w->ctx) { 
        ferr(GLFW_NO_WINDOW_CONTEXT, "Cannot make current with a window that has no OpenGL or OpenGL ES context"); 
        return; 
    }
    if (!w) { 
        if (g.dpy) 
            eglMakeCurrent(g.dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT); 
        t_cur = NULL; 
        return; 
    }

    eglBindAPI(w->client == GLFW_OPENGL_ES_API ? EGL_OPENGL_ES_API : EGL_OPENGL_API);
    if (!eglMakeCurrent(g.dpy, w->surf, w->surf, w->ctx)) {
        ferr(GLFW_PLATFORM_ERROR, "EGL: failed to make context current (0x%x)", eglGetError()); 
        return;
    }

    t_cur = w;

    /* Automatically parse version & profile the first time the context is made current */
    if (!w->verParsed) {
        typedef const unsigned char* (*PFNGLGETSTRINGPROC)(unsigned int);
        PFNGLGETSTRINGPROC gs = (PFNGLGETSTRINGPROC) eglGetProcAddress("glGetString");
        
        const char* v = gs ? (const char*) gs(0x1F02) : NULL; /* 0x1F02 = GL_VERSION */
        if (v) {
            const char* v0 = v;

            if (!strncmp(v, "OpenGL ES-CM ", 13)) v += 13;
            else if (!strncmp(v, "OpenGL ES-CL ", 13)) v += 13;
            else if (!strncmp(v, "OpenGL ES ", 10)) v += 10;

            int ma = 0, mi = 0;
            if (sscanf(v, "%d.%d", &ma, &mi) == 2 && ma > 0) {
                w->major = ma;
                w->minor = mi;
            }

            /* Fallback: parse profile from the GL_VERSION string (if present) */
            if (strstr(v0, "Core Profile")) { 
                w->profile = GLFW_OPENGL_CORE_PROFILE; 
                w->fwd = 1; 
            }
            else if (strstr(v0, "Compatibility Profile")) {
                w->profile = GLFW_OPENGL_COMPAT_PROFILE;
            }
        }

        /* Official profile query via GL_CONTEXT_PROFILE_MASK for OpenGL 3.2+ */
        typedef void (*PGI)(unsigned int, int*);
        PGI gi = (PGI) eglGetProcAddress("glGetIntegerv");
        if (gi && w->client == GLFW_OPENGL_API && (w->major >= 4 || (w->major == 3 && w->minor >= 2))) {
            int mask = 0;
            gi(0x9126, &mask); /* 0x9126 = GL_CONTEXT_PROFILE_MASK */
            if (mask & 1) {      /* 0x1 = GL_CONTEXT_CORE_PROFILE_BIT */
                w->profile = GLFW_OPENGL_CORE_PROFILE;
            } 
            else if (mask & 2) { /* 0x2 = GL_CONTEXT_COMPATIBILITY_PROFILE_BIT */
                w->profile = GLFW_OPENGL_COMPAT_PROFILE;
            }
        }

        w->verParsed = 1;
    }
}

GLFWAPI GLFWwindow* glfwGetCurrentContext(void) { REQ_INIT(NULL); return t_cur; }
GLFWAPI void glfwSwapBuffers(GLFWwindow* w) {
    REQ_INIT();
    if (!w->ctx) { ferr(GLFW_NO_WINDOW_CONTEXT, "Cannot swap buffers of a window that has no OpenGL or OpenGL ES context"); return; }
    if (!eglSwapBuffers(g.dpy, w->surf)) ferr(GLFW_PLATFORM_ERROR, "EGL: failed to swap buffers (0x%x)", eglGetError());
}
GLFWAPI void glfwSwapInterval(int i) {
    REQ_INIT();
    if (!t_cur) { ferr(GLFW_NO_CURRENT_CONTEXT, "Cannot set swap interval without a current OpenGL or OpenGL ES context"); return; }
    eglSwapInterval(g.dpy, i);
}

GLFWAPI GLFWglproc glfwGetProcAddress(const char* name) {
    REQ_INIT(NULL);
    if (!t_cur) { ferr(GLFW_NO_CURRENT_CONTEXT, "Cannot query entry point without a current OpenGL or OpenGL ES context"); return NULL; }
    GLFWglproc p = (GLFWglproc)eglGetProcAddress(name);
    if (!p) p = (GLFWglproc)dlsym(RTLD_DEFAULT, name);
    return p;
}

GLFWAPI int glfwExtensionSupported(const char* ext) {
    REQ_INIT(0);
    if (!t_cur) { ferr(GLFW_NO_CURRENT_CONTEXT, "Cannot query extension without a current OpenGL or OpenGL ES context"); return 0; }
    if (!ext || !*ext) { ferr(GLFW_INVALID_VALUE, "Extension name is empty string"); return 0; }
    if (strchr(ext, ' ')) { ferr(GLFW_INVALID_VALUE, "Extension names cannot contain spaces"); return 0; }
    typedef const unsigned char* (*PGS)(unsigned);
    typedef const unsigned char* (*PGSI)(unsigned, unsigned);
    typedef void (*PGI)(unsigned, int*);
    PGS gs = (PGS)eglGetProcAddress("glGetString");
    PGSI gsi = (PGSI)eglGetProcAddress("glGetStringi");
    PGI gi = (PGI)eglGetProcAddress("glGetIntegerv");
    if (!gs) return 0;
    int major = t_cur->major;
    if (major >= 3 && gsi && gi) {
        int n = 0; gi(0x821D, &n);                     /* GL_NUM_EXTENSIONS */
        for (int i = 0; i < n; i++) {
            const char* e = (const char*)gsi(0x1F03, i);
            if (e && !strcmp(e, ext)) return 1;
        }
    } else {
        const char* s = (const char*)gs(0x1F03);
        if (s) {
            size_t l = strlen(ext);
            for (const char* p = s; (p = strstr(p, ext)); p += l)
                if ((p == s || p[-1] == ' ') && (p[l] == ' ' || !p[l])) return 1;
        }
    }
    const char* es = eglQueryString(g.dpy, EGL_EXTENSIONS);
    if (es) {
        size_t l = strlen(ext);
        for (const char* p = es; (p = strstr(p, ext)); p += l)
            if ((p == es || p[-1] == ' ') && (p[l] == ' ' || !p[l])) return 1;
    }
    return 0;
}

/* ====================================================================== */
/*                               Vulkan                                   */
/* ====================================================================== */
static struct { void* h; PFN_vkGetInstanceProcAddr gipa; int tried, ok; } V;
static const char* const VK_REQ_EXT[2] = { "VK_KHR_surface", "VK_KHR_xcb_surface" };

static int vk_load(void) {
    if (V.tried) return V.gipa != NULL;
    V.tried = 1;
    static const char* const names[] = { "libvulkan.so.1", "libvulkan.so", NULL };
    for (int i = 0; names[i] && !V.h; i++) V.h = dlopen(names[i], RTLD_NOW | RTLD_LOCAL);
    if (!V.h) return 0;
    V.gipa = (PFN_vkGetInstanceProcAddr)dlsym(V.h, "vkGetInstanceProcAddr");
    if (!V.gipa) { dlclose(V.h); V.h = NULL; return 0; }

    PFN_vkEnumerateInstanceExtensionProperties en = (PFN_vkEnumerateInstanceExtensionProperties)
        V.gipa(NULL, "vkEnumerateInstanceExtensionProperties");
    uint32_t n = 0;
    if (!en || en(NULL, &n, NULL) != VK_SUCCESS || !n) return 1;
    VkExtensionProperties* p = calloc(n, sizeof *p);
    if (p && en(NULL, &n, p) >= 0) {
        int found = 0;
        for (uint32_t i = 0; i < n; i++)
            for (int k = 0; k < 2; k++)
                if (!strcmp(p[i].extensionName, VK_REQ_EXT[k])) found |= 1 << k;
        V.ok = (found == 3);
    }
    free(p);
    return 1;
}

GLFWAPI int glfwVulkanSupported(void) {
    REQ_INIT(GLFW_FALSE);
    return vk_load() && V.ok ? GLFW_TRUE : GLFW_FALSE;
}

GLFWAPI const char** glfwGetRequiredInstanceExtensions(uint32_t* count) {
    *count = 0; REQ_INIT(NULL);
    if (!vk_load()) { ferr(GLFW_API_UNAVAILABLE, "Vulkan: loader not found"); return NULL; }
    if (!V.ok) { ferr(GLFW_API_UNAVAILABLE, "Vulkan: VK_KHR_xcb_surface not available"); return NULL; }
    *count = 2;
    return (const char**)VK_REQ_EXT;
}

GLFWAPI GLFWvkproc glfwGetInstanceProcAddress(VkInstance inst, const char* name) {
    REQ_INIT(NULL);
    if (!vk_load()) { ferr(GLFW_API_UNAVAILABLE, "Vulkan: loader not found"); return NULL; }
    GLFWvkproc p = (GLFWvkproc)V.gipa(inst, name);
    if (!p) p = (GLFWvkproc)dlsym(V.h, name);
    return p;
}

GLFWAPI int glfwGetPhysicalDevicePresentationSupport(VkInstance inst, VkPhysicalDevice dev, uint32_t qf) {
    REQ_INIT(GLFW_FALSE);
    if (!vk_load() || !V.ok) { ferr(GLFW_API_UNAVAILABLE, "Vulkan: not available"); return GLFW_FALSE; }
    PFN_vkGetPhysicalDeviceXcbPresentationSupportKHR fn = (PFN_vkGetPhysicalDeviceXcbPresentationSupportKHR)
        V.gipa(inst, "vkGetPhysicalDeviceXcbPresentationSupportKHR");
    if (!fn) { ferr(GLFW_API_UNAVAILABLE, "Vulkan: xcb surface extension not loaded"); return GLFW_FALSE; }
    return fn(dev, qf, g.c, g.scr->root_visual) ? GLFW_TRUE : GLFW_FALSE;
}

GLFWAPI VkResult glfwCreateWindowSurface(VkInstance inst, GLFWwindow* w,
                                         const VkAllocationCallbacks* alloc, VkSurfaceKHR* surf) {
    if (surf) *surf = VK_NULL_HANDLE;
    REQ_INIT(VK_ERROR_INITIALIZATION_FAILED);
    if (!vk_load() || !V.ok) { ferr(GLFW_API_UNAVAILABLE, "Vulkan: not available"); return VK_ERROR_EXTENSION_NOT_PRESENT; }
    if (w->client != GLFW_NO_API) {
        ferr(GLFW_INVALID_VALUE, "Vulkan: window must be created with GLFW_NO_API");
        return VK_ERROR_NATIVE_WINDOW_IN_USE_KHR;
    }
    PFN_vkCreateXcbSurfaceKHR fn = (PFN_vkCreateXcbSurfaceKHR)V.gipa(inst, "vkCreateXcbSurfaceKHR");
    if (!fn) { ferr(GLFW_API_UNAVAILABLE, "Vulkan: VK_KHR_xcb_surface not enabled"); return VK_ERROR_EXTENSION_NOT_PRESENT; }
    VkXcbSurfaceCreateInfoKHR ci = { .sType = VK_STRUCTURE_TYPE_XCB_SURFACE_CREATE_INFO_KHR,
                                     .connection = g.c, .window = w->id };
    VkResult r = fn(inst, &ci, alloc, surf);
    if (r) ferr(GLFW_PLATFORM_ERROR, "Vulkan: failed to create window surface (%d)", (int)r);
    return r;
}
