// USER32 shims: windows, message queue, input translation (SDL2).
//
// UE2's Window.dll keeps a C++ object per window and finds it from WM_NCCREATE's CREATESTRUCT, so windows are real
// objects here: CreateWindowEx sends WM_NCCREATE/WM_CREATE to the class's window procedure, Get/SetWindowLong work,
// SendMessage calls the procedure directly. There is one SDL window; it shows the game viewport (the window
// Direct3D renders to, d3d8.c: user32_set_viewport) and all input goes to that window.
//
// The mouse: while the game hides the cursor and captures it (in play) SDL's relative mode (pointer lock in the
// browser) drives a virtual cursor that GetCursorPos/SetCursorPos and DirectInput see; otherwise the cursor maps
// from SDL window coordinates to the viewport.
#include "host.h"
#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>
#include <ctype.h>
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

uint32_t g_hwnd;                    // the viewport window: input goes here
int g_win_w = 640, g_win_h = 480;   // its client size (the game's resolution)
SDL_Window *g_window;
static int cursor_count = 0;

void gfx_map_mouse(int wx, int wy, int *gx, int *gy);     // d3d8.c
void gfx_window_created(SDL_Window *w);

// ---------------------------------------------------------------- windows and classes
#define MAXWIN 256
#define MAXCLS 64
#define NEXTRA 16
typedef struct {
    uint32_t hwnd, proc, style, exstyle, parent, id, userdata, menu, instance;
    int x, y, w, h, visible, used, is_dialog;
    uint32_t extra[NEXTRA];
    char cls[64], text[128];
    struct { char name[32]; uint32_t v; } props[8];
} Win;
static Win wins[MAXWIN];
static struct { char name[64]; uint32_t proc, extra, atom, wide; } classes[MAXCLS];
static int nclasses;
#define HWND_BASE 0x00010010u

Win *user32_win(uint32_t hwnd) {
    if (hwnd < HWND_BASE) return NULL;
    uint32_t i = (hwnd - HWND_BASE) / 4;
    if (i >= MAXWIN || !wins[i].used) return NULL;
    return &wins[i];
}
static void wstr(uint32_t a, char *out, int cap) {
    int k = 0;
    if (a && a < 0x10000) { snprintf(out, cap, "#%u", a); return; }   // atom
    for (uint32_t i = 0; a && k + 1 < cap; i++) { uint32_t c = R16(a + 2 * i); if (!c) break; out[k++] = c < 128 ? (char)c : '?'; }
    out[k] = 0;
}
static void astr(uint32_t a, char *out, int cap) {
    if (a && a < 0x10000) { snprintf(out, cap, "#%u", a); return; }
    snprintf(out, cap, "%s", a ? GSTR(a) : "");
}
static uint32_t put_w(uint32_t a, uint32_t cap, const char *s) {
    uint32_t k = 0;
    while (s[k] && k + 1 < cap) { W16(a + 2 * k, (uint8_t)s[k]); k++; }
    if (cap) W16(a + 2 * k, 0);
    return k;
}

static int find_class(const char *name) {
    for (int i = 0; i < nclasses; i++) if (!strcasecmp(classes[i].name, name)) return i;
    return -1;
}
static void register_class(int wide) {
    uint32_t wc = ARG(0);
    if (nclasses >= MAXCLS) SRET(0, 1);
    char name[64];
    if (wide) wstr(R32(wc + 40), name, sizeof name); else astr(R32(wc + 40), name, sizeof name);
    int i = find_class(name);
    if (i < 0) i = nclasses++;
    snprintf(classes[i].name, 64, "%s", name);
    classes[i].proc = R32(wc + 8);
    classes[i].extra = R32(wc + 16);
    classes[i].atom = 0xC100 + i;
    classes[i].wide = wide;
    host_log("RegisterClassEx%c '%s' wndproc=%08x\n", wide ? 'W' : 'A', name, classes[i].proc);
    SRET(classes[i].atom, 1);
}
static void u_RegisterClassExA(void) { register_class(0); }
static void u_RegisterClassExW(void) { register_class(1); }
static void u_DefWindowProc(void);
// System classes (BUTTON, EDIT, the common controls) exist on demand: Window.dll subclasses them, asking for their
// window procedure first. Theirs is DefWindowProc.
static void get_class_info(int wide) {
    char name[64];
    if (wide) wstr(ARG(1), name, sizeof name); else astr(ARG(1), name, sizeof name);
    int i = find_class(name);
    if (i < 0) {
        if (nclasses >= MAXCLS) SRET(0, 3);
        i = nclasses++;
        snprintf(classes[i].name, 64, "%s", name);
        classes[i].proc = host_thunk(u_DefWindowProc, "DefWindowProc (system class)");
        classes[i].atom = 0xC100 + i;
        classes[i].wide = wide;
    }
    uint32_t wc = ARG(2);
    memset(G + wc + 4, 0, 44);
    W32(wc + 8, classes[i].proc);
    W32(wc + 40, ARG(1));
    SRET(classes[i].atom, 3);
}
static void u_GetClassInfoExA(void) { get_class_info(0); }
static void u_GetClassInfoExW(void) { get_class_info(1); }

static uint32_t send(uint32_t hwnd, uint32_t msg, uint32_t wp, uint32_t lp) {
    Win *w = user32_win(hwnd);
    if (!w || !w->proc) return 0;
    return guest_call(w->proc, 4, hwnd, msg, wp, lp);
}

static uint32_t create_window(uint32_t exstyle, const char *cls, const char *title, uint32_t style, int x, int y,
                              int cw, int ch, uint32_t parent, uint32_t menu, uint32_t inst, uint32_t param, uint32_t proc,
                              int is_dialog) {
    int slot = -1;
    for (int i = 0; i < MAXWIN; i++) if (!wins[i].used) { slot = i; break; }
    if (slot < 0) host_fatal("too many windows");
    Win *w = &wins[slot];
    memset(w, 0, sizeof *w);
    w->used = 1; w->hwnd = HWND_BASE + slot * 4;
    w->exstyle = exstyle; w->style = style; w->parent = parent; w->menu = menu; w->instance = inst;
    w->is_dialog = is_dialog;
    if ((int32_t)x == (int32_t)0x80000000) x = 0;
    if ((int32_t)y == (int32_t)0x80000000) y = 0;
    if (cw <= 0 || (int32_t)cw == (int32_t)0x80000000) cw = 640;
    if (ch <= 0 || (int32_t)ch == (int32_t)0x80000000) ch = 480;
    w->x = x; w->y = y; w->w = cw; w->h = ch;
    w->visible = (style & 0x10000000u) != 0;
    snprintf(w->cls, sizeof w->cls, "%s", cls);
    snprintf(w->text, sizeof w->text, "%s", title);
    if (!proc) { int c = find_class(cls); if (c >= 0) proc = classes[c].proc; }
    w->proc = proc;
    if (style & 0x40000000u) w->id = menu;      // WS_CHILD: hMenu is the control id
    host_log("CreateWindow '%s' '%s' %dx%d style=%08x proc=%08x -> %08x\n", cls, title, cw, ch, style, proc, w->hwnd);
    if (!proc) return w->hwnd;
    if (is_dialog) return w->hwnd;
    // CREATESTRUCTW on the guest stack below the caller's frame
    uint32_t cs = R.esp - 0x100;
    W32(cs + 0, param); W32(cs + 4, inst); W32(cs + 8, menu); W32(cs + 12, parent);
    W32(cs + 16, ch); W32(cs + 20, cw); W32(cs + 24, y); W32(cs + 28, x);
    W32(cs + 32, style); W32(cs + 36, 0); W32(cs + 40, 0); W32(cs + 44, exstyle);
    uint32_t save = R.esp;
    R.esp = cs - 16;
    uint32_t hwnd = w->hwnd;
    if (!send(hwnd, 0x81, 0, cs)) { R.esp = save; host_log("  WM_NCCREATE refused\n"); w->used = 0; return 0; }
    if (g_unwind) { R.esp = save; return 0; }
    if ((int32_t)send(hwnd, 0x01, 0, cs) == -1) { R.esp = save; w->used = 0; return 0; }
    if (g_unwind) { R.esp = save; return 0; }
    send(hwnd, 0x05, 0, ((uint32_t)ch << 16) | (uint32_t)cw);    // WM_SIZE
    R.esp = save;
    return hwnd;
}
static void u_CreateWindowExW(void) {
    char cls[64], title[128];
    wstr(ARG(1), cls, sizeof cls); wstr(ARG(2), title, sizeof title);
    uint32_t h = create_window(ARG(0), cls, title, ARG(3), (int32_t)ARG(4), (int32_t)ARG(5), (int32_t)ARG(6), (int32_t)ARG(7),
                               ARG(8), ARG(9), ARG(10), ARG(11), 0, 0);
    if (g_unwind) return;
    SRET(h, 12);
}
static void u_CreateWindowExA(void) {
    char cls[64], title[128];
    astr(ARG(1), cls, sizeof cls); astr(ARG(2), title, sizeof title);
    uint32_t h = create_window(ARG(0), cls, title, ARG(3), (int32_t)ARG(4), (int32_t)ARG(5), (int32_t)ARG(6), (int32_t)ARG(7),
                               ARG(8), ARG(9), ARG(10), ARG(11), 0, 0);
    if (g_unwind) return;
    SRET(h, 12);
}
// Dialogs (the splash screen, the log window's dialogs): a window that gets WM_INITDIALOG; no controls are made.
static uint32_t create_dialog(uint32_t inst, uint32_t parent, uint32_t proc, uint32_t param) {
    uint32_t h = create_window(0, "#32770", "", 0x10000000u, 0, 0, 320, 240, parent, 0, inst, param, proc, 1);
    Win *w = user32_win(h);
    if (w && proc) guest_call(proc, 4, h, 0x110, 0, param);    // WM_INITDIALOG
    return h;
}
static void u_CreateDialogParam(void) { uint32_t h = create_dialog(ARG(0), ARG(2), ARG(3), ARG(4)); if (g_unwind) return; SRET(h, 5); }
static void u_CreateDialogIndirectParamA(void) { uint32_t h = create_dialog(ARG(0), ARG(2), ARG(3), ARG(4)); if (g_unwind) return; SRET(h, 5); }
static void u_DialogBoxParam(void) {
    host_log("DialogBoxParam template=%08x proc=%08x (auto-OK)\n", ARG(1), ARG(3));
    SRET(1, 5);
}
static void u_EndDialog(void) { SRET(1, 2); }
static void u_DestroyWindow(void) {
    Win *w = user32_win(ARG(0));
    if (w) {
        uint32_t h = w->hwnd;
        send(h, 0x02, 0, 0);       // WM_DESTROY
        if (g_unwind) return;
        send(h, 0x82, 0, 0);       // WM_NCDESTROY
        if (g_unwind) return;
        w = user32_win(h);
        if (w) w->used = 0;
        if (h == g_hwnd) g_hwnd = 0;
    }
    SRET(1, 1);
}
static void u_IsWindow(void) { SRET(user32_win(ARG(0)) != NULL, 1); }
static void u_IsWindowVisible(void) { Win *w = user32_win(ARG(0)); SRET(w && w->visible, 1); }
static void u_IsWindowEnabled(void) { SRET(1, 1); }
static void u_ShowWindow(void) {
    Win *w = user32_win(ARG(0));
    int was = w && w->visible;
    if (w) w->visible = ARG(1) != 0;
    SRET(was, 2);
}
static void u_UpdateWindow(void) { SRET(1, 1); }
static void u_GetParent(void) { Win *w = user32_win(ARG(0)); SRET(w ? w->parent : 0, 1); }
static void u_SetParent(void) { Win *w = user32_win(ARG(0)); uint32_t o = w ? w->parent : 0; if (w) w->parent = ARG(1); SRET(o, 2); }
static void u_GetWindow(void) { SRET(0, 2); }
static void u_GetDlgItem(void) { SRET(0, 2); }
static void u_GetDlgCtrlID(void) { Win *w = user32_win(ARG(0)); SRET(w ? w->id : 0, 1); }
static void u_EnumChildWindows(void) { SRET(1, 3); }
// EnumThreadWindows(thread, proc, lparam): every top-level window, until the callback returns FALSE
static void u_EnumThreadWindows(void) {
    uint32_t proc = ARG(1), lp = ARG(2);
    for (int i = 0; i < MAXWIN; i++) {
        if (!wins[i].used || (wins[i].style & 0x40000000u)) continue;
        uint32_t r = guest_call(proc, 2, wins[i].hwnd, lp);
        if (g_unwind) return;
        if (!r) break;
    }
    SRET(1, 3);
}
static void u_FindWindowEx(void) { SRET(0, 4); }
static void u_GetDesktopWindow(void) { SRET(0x00010004u, 0); }
static void u_GetActiveWindow(void) { SRET(g_hwnd, 0); }
static void u_ret_hwnd0(void) { SRET(g_hwnd, 0); }
static void u_ret_hwnd1(void) { SRET(g_hwnd, 1); }
static void u_GetWindowThreadProcessId(void) { if (ARG(1)) W32(ARG(1), 0x1000); SRET(0x1004, 2); }

static uint32_t *win_long(Win *w, int32_t idx) {
    switch (idx) {
    case -4: return &w->proc;
    case -16: return &w->style;
    case -20: return &w->exstyle;
    case -21: return &w->userdata;
    case -12: return &w->id;
    case -6: return &w->instance;
    case -8: return &w->parent;
    }
    if (idx >= 0 && idx / 4 < NEXTRA) return &w->extra[idx / 4];
    return NULL;
}
static void u_GetWindowLong(void) {
    Win *w = user32_win(ARG(0));
    uint32_t *p = w ? win_long(w, (int32_t)ARG(1)) : NULL;
    SRET(p ? *p : 0, 2);
}
static void u_SetWindowLong(void) {
    Win *w = user32_win(ARG(0));
    uint32_t *p = w ? win_long(w, (int32_t)ARG(1)) : NULL;
    uint32_t old = p ? *p : 0;
    if (p) *p = ARG(2);
    if (w && (int32_t)ARG(1) == -16) w->visible = (ARG(2) & 0x10000000u) != 0;
    SRET(old, 3);
}
static void prop_name(uint32_t a, int wide, char *out) {
    if (a < 0x10000) snprintf(out, 32, "#%u", a);
    else if (wide) wstr(a, out, 32);
    else snprintf(out, 32, "%s", GSTR(a));
}
static void set_prop(int wide) {
    Win *w = user32_win(ARG(0));
    char n[32]; prop_name(ARG(1), wide, n);
    if (!w) SRET(0, 3);
    for (int i = 0; i < 8; i++) if (!w->props[i].name[0] || !strcmp(w->props[i].name, n)) {
        snprintf(w->props[i].name, 32, "%s", n); w->props[i].v = ARG(2); SRET(1, 3);
    }
    SRET(0, 3);
}
static void get_prop(int wide, int remove) {
    Win *w = user32_win(ARG(0));
    char n[32]; prop_name(ARG(1), wide, n);
    if (w) for (int i = 0; i < 8; i++) if (!strcmp(w->props[i].name, n)) {
        uint32_t v = w->props[i].v;
        if (remove) w->props[i].name[0] = 0;
        SRET(v, 2);
    }
    SRET(0, 2);
}
static void u_SetPropA(void) { set_prop(0); }
static void u_SetPropW(void) { set_prop(1); }
static void u_GetPropA(void) { get_prop(0, 0); }
static void u_GetPropW(void) { get_prop(1, 0); }
static void u_RemovePropA(void) { get_prop(0, 1); }
static void u_RemovePropW(void) { get_prop(1, 1); }

static void u_GetWindowTextA(void) { Win *w = user32_win(ARG(0)); uint32_t n = ARG(2); if (n) { snprintf(GSTR(ARG(1)), n, "%s", w ? w->text : ""); } SRET(n ? strlen(GSTR(ARG(1))) : 0, 3); }
static void u_GetWindowTextW(void) { Win *w = user32_win(ARG(0)); SRET(put_w(ARG(1), ARG(2), w ? w->text : ""), 3); }

// geometry: windows have no borders, so client and window rectangles are the same size
static void put_rect(uint32_t r, int x0, int y0, int x1, int y1) { W32(r, x0); W32(r + 4, y0); W32(r + 8, x1); W32(r + 12, y1); }
static void u_GetClientRect(void) {
    Win *w = user32_win(ARG(0));
    if (!w) { put_rect(ARG(1), 0, 0, g_win_w, g_win_h); SRET(1, 2); }
    put_rect(ARG(1), 0, 0, w->w, w->h);
    SRET(1, 2);
}
static void u_GetWindowRect(void) {
    Win *w = user32_win(ARG(0));
    if (!w) { put_rect(ARG(1), 0, 0, g_win_w, g_win_h); SRET(1, 2); }
    put_rect(ARG(1), w->x, w->y, w->x + w->w, w->y + w->h);
    SRET(1, 2);
}
static void resize(Win *w, int x, int y, int cw, int ch, int move, int size) {
    if (move) { w->x = x; w->y = y; }
    if (size && cw > 0 && ch > 0 && (cw != w->w || ch != w->h)) {
        w->w = cw; w->h = ch;
        send(w->hwnd, 0x05, 0, ((uint32_t)ch << 16) | (uint32_t)cw);
    }
}
static void u_SetWindowPos(void) {
    Win *w = user32_win(ARG(0));
    uint32_t fl = ARG(6);
    if (w) {
        resize(w, (int32_t)ARG(2), (int32_t)ARG(3), (int32_t)ARG(4), (int32_t)ARG(5), !(fl & 2), !(fl & 1));
        if (g_unwind) return;
        if (fl & 0x40) w->visible = 1;
        if (fl & 0x80) w->visible = 0;
    }
    SRET(1, 7);
}
static void u_MoveWindow(void) {
    Win *w = user32_win(ARG(0));
    if (w) { resize(w, (int32_t)ARG(1), (int32_t)ARG(2), (int32_t)ARG(3), (int32_t)ARG(4), 1, 1); if (g_unwind) return; }
    SRET(1, 6);
}
static void u_AdjustWindowRect(void) { SRET(1, 3); }
static void u_ClientToScreen(void) {
    Win *w = user32_win(ARG(0));
    if (w) { W32(ARG(1), R32(ARG(1)) + w->x); W32(ARG(1) + 4, R32(ARG(1) + 4) + w->y); }
    SRET(1, 2);
}
static void u_ScreenToClient(void) {
    Win *w = user32_win(ARG(0));
    if (w) { W32(ARG(1), R32(ARG(1)) - w->x); W32(ARG(1) + 4, R32(ARG(1) + 4) - w->y); }
    SRET(1, 2);
}
static void u_MapWindowPoints(void) { SRET(0, 4); }
static void u_IsIconic(void) { SRET(0, 1); }
static void u_IsZoomed(void) { SRET(0, 1); }
static void u_GetSystemMetrics(void) {
    int32_t i = (int32_t)ARG(0), v = 0;
    switch (i) {
    case 0: case 16: v = 1024; break; case 1: case 17: v = 768; break; case 4: v = 19; break;
    case 5: case 6: v = 1; break; case 7: case 8: v = 3; break; case 32: case 33: v = 4; break;
    case 11: case 12: case 13: case 14: v = 32; break; case 19: v = 1; break; case 43: v = 3; break;
    case 76: case 77: v = 0; break; case 78: v = 1024; break; case 79: v = 768; break; case 80: v = 1; break;
    }
    SRET(v, 1);
}
static void u_SystemParametersInfo(void) { if (ARG(2) && ARG(0) != 0x2f) W32(ARG(2), 0); SRET(1, 4); }

// ---------------------------------------------------------------- the viewport (called by d3d8.c)
void user32_set_viewport(uint32_t hwnd, int w, int h) {
    if (hwnd) g_hwnd = hwnd;
    if (w > 0 && h > 0) { g_win_w = w; g_win_h = h; }
    if (!g_window) {
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
        SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
        SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
        SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
        int sw = g_win_w, sh = g_win_h;
#ifdef __EMSCRIPTEN__
        g_window = SDL_CreateWindow(GAME_TITLE, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, sw, sh, SDL_WINDOW_OPENGL);
#else
        g_window = SDL_CreateWindow(GAME_TITLE, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, sw, sh,
                                    SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
        if (!g_window) {
            SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, 0);
            g_window = SDL_CreateWindow(GAME_TITLE, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, sw, sh,
                                        SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE);
        }
#endif
        if (!g_window) host_fatal("SDL_CreateWindow: %s", SDL_GetError());
        gfx_window_created(g_window);
        SDL_StartTextInput();
    }
    host_log("viewport window %08x %dx%d\n", g_hwnd, g_win_w, g_win_h);
}

// ---------------------------------------------------------------- message queue
typedef struct { uint32_t hwnd, msg, wp, lp, time, x, y; } Msg;
#define QN 1024
static Msg q[QN];
static int qh, qt;
static int mouse_x, mouse_y, mouse_buttons;     // virtual cursor in viewport coordinates
static int captured;                             // relative mouse mode (pointer lock)
static uint8_t keystate[256];

static void post_to(uint32_t hwnd, uint32_t msg, uint32_t wp, uint32_t lp) {
    int n = (qt + 1) % QN;
    if (n == qh) return;   // drop on overflow
    q[qt].hwnd = hwnd; q[qt].msg = msg; q[qt].wp = wp; q[qt].lp = lp;
    q[qt].time = host_ticks(); q[qt].x = mouse_x; q[qt].y = mouse_y;
    qt = n;
}
static void post(uint32_t msg, uint32_t wp, uint32_t lp) { if (g_hwnd) post_to(g_hwnd, msg, wp, lp); }

static uint32_t sdl_to_vk(SDL_Keycode k) {
    if (k >= 'a' && k <= 'z') return (uint32_t)(k - 'a' + 'A');
    if (k >= '0' && k <= '9') return (uint32_t)k;
    if (k >= SDLK_F1 && k <= SDLK_F12) return 0x70 + (k - SDLK_F1);
    switch (k) {
    case SDLK_LEFT: return 0x25; case SDLK_UP: return 0x26; case SDLK_RIGHT: return 0x27; case SDLK_DOWN: return 0x28;
    case SDLK_ESCAPE: return 0x1B; case SDLK_RETURN: case SDLK_KP_ENTER: return 0x0D; case SDLK_SPACE: return 0x20;
    case SDLK_TAB: return 0x09; case SDLK_BACKSPACE: return 0x08;
    case SDLK_LSHIFT: case SDLK_RSHIFT: return 0x10;
    case SDLK_LCTRL: case SDLK_RCTRL: return 0x11;
    case SDLK_LALT: case SDLK_RALT: return 0x12;
    case SDLK_PAUSE: return 0x13; case SDLK_CAPSLOCK: return 0x14;
    case SDLK_PAGEUP: return 0x21; case SDLK_PAGEDOWN: return 0x22; case SDLK_END: return 0x23; case SDLK_HOME: return 0x24;
    case SDLK_INSERT: return 0x2D; case SDLK_DELETE: return 0x2E; case SDLK_PRINTSCREEN: return 0x2C;
    case SDLK_KP_0: return 0x60; case SDLK_KP_1: return 0x61; case SDLK_KP_2: return 0x62; case SDLK_KP_3: return 0x63;
    case SDLK_KP_4: return 0x64; case SDLK_KP_5: return 0x65; case SDLK_KP_6: return 0x66; case SDLK_KP_7: return 0x67;
    case SDLK_KP_8: return 0x68; case SDLK_KP_9: return 0x69; case SDLK_KP_MULTIPLY: return 0x6A; case SDLK_KP_PLUS: return 0x6B;
    case SDLK_KP_MINUS: return 0x6D; case SDLK_KP_PERIOD: return 0x6E; case SDLK_KP_DIVIDE: return 0x6F;
    case SDLK_SEMICOLON: return 0xBA; case SDLK_EQUALS: return 0xBB; case SDLK_COMMA: return 0xBC; case SDLK_MINUS: return 0xBD;
    case SDLK_PERIOD: return 0xBE; case SDLK_SLASH: return 0xBF; case SDLK_BACKQUOTE: return 0xC0;
    case SDLK_LEFTBRACKET: return 0xDB; case SDLK_BACKSLASH: return 0xDC; case SDLK_RIGHTBRACKET: return 0xDD; case SDLK_QUOTE: return 0xDE;
    }
    return 0;
}

// dinput.c: the DirectInput devices see the same input
void dinput_key_sdl(int scancode, int down);
void dinput_key_dik(uint8_t dik, int down);
uint8_t dinput_vk_to_dik(uint32_t vk);
void dinput_mouse_move(int dx, int dy);
void dinput_mouse_wheel(int dz);
void dinput_mouse_button(int b, int down);

static uint32_t mouse_lp(void) { return ((uint32_t)(mouse_y & 0xffff) << 16) | (uint32_t)(mouse_x & 0xffff); }
static void mouse_to(int x, int y) {
    if (x < 0) x = 0; if (y < 0) y = 0;
    if (x >= g_win_w) x = g_win_w - 1; if (y >= g_win_h) y = g_win_h - 1;
    mouse_x = x; mouse_y = y;
    post(0x200, (uint32_t)mouse_buttons, mouse_lp());    // WM_MOUSEMOVE
}

static void vk_event(uint32_t vk, int down, int repeat, uint32_t sc) {
    uint32_t lp = 1 | ((sc & 0xff) << 16);
    keystate[vk & 0xff] = down ? 0x80 : 0;
    if (down) {
        if (repeat) lp |= 1u << 30;
        post(vk == 0x12 ? 0x104 : 0x100, vk, lp);
        if (vk == 0x0D || vk == 0x08 || vk == 0x1B || vk == 0x09) post(0x102, vk, lp);
    } else {
        post(vk == 0x12 ? 0x105 : 0x101, vk, lp | 0xC0000000u);
    }
}

// Scripted input for testing: PORT_INPUT="frame:key:VK;frame:down:VK;frame:up:VK;frame:char:c;frame:move:x,y;
// frame:click:x,y;frame:rclick:x,y;frame:ldown:x,y;frame:lup:x,y;frame:rel:dx,dy"
static void scripted_input(void) {
    static char *spec; static int init; static uint32_t last = 0xffffffffu;
    static uint32_t release_vk[8]; static int n_release;
    if (!init) { init = 1; const char *e = getenv("PORT_INPUT"); if (e) spec = strdup(e); }
    if (!spec) return;
    uint32_t fr = host_frame_count();
    if (fr == last) return;
    last = fr;
    for (int i = 0; i < n_release; i++) {
        if (release_vk[i] == 0x10001) { mouse_buttons &= ~1; post(0x202, mouse_buttons, mouse_lp()); dinput_mouse_button(0, 0); }
        else if (release_vk[i] == 0x10002) { mouse_buttons &= ~2; post(0x205, mouse_buttons, mouse_lp()); dinput_mouse_button(1, 0); }
        else { vk_event(release_vk[i], 0, 0, 0); dinput_key_dik(dinput_vk_to_dik(release_vk[i]), 0); }
    }
    n_release = 0;
    char *copy = strdup(spec);
    for (char *tok = strtok(copy, ";"); tok; tok = strtok(NULL, ";")) {
        unsigned f; char act[16]; char arg[64] = "";
        if (sscanf(tok, "%u:%15[^:]:%63s", &f, act, arg) < 2 || f != fr) continue;
        int x = 0, y = 0; sscanf(arg, "%d,%d", &x, &y);
        uint32_t vk = (uint32_t)strtol(arg, NULL, 0);
        if (!strcmp(act, "key") || !strcmp(act, "down")) {
            vk_event(vk, 1, 0, 0); dinput_key_dik(dinput_vk_to_dik(vk), 1);
            if (act[0] == 'k' && n_release < 8) release_vk[n_release++] = vk;
        }
        else if (!strcmp(act, "up")) { vk_event(vk, 0, 0, 0); dinput_key_dik(dinput_vk_to_dik(vk), 0); }
        else if (!strcmp(act, "char")) post(0x102, (uint32_t)(unsigned char)arg[0], 1);
        else if (!strcmp(act, "move")) { dinput_mouse_move(x - mouse_x, y - mouse_y); mouse_to(x, y); }
        else if (!strcmp(act, "rel")) { dinput_mouse_move(x, y); mouse_to(mouse_x + x, mouse_y + y); }
        else if (!strcmp(act, "click") || !strcmp(act, "rclick") || !strcmp(act, "ldown") || !strcmp(act, "lup")) {
            int right = act[0] == 'r';
            dinput_mouse_move(x - mouse_x, y - mouse_y); mouse_to(x, y);
            if (strcmp(act, "lup")) { mouse_buttons |= right ? 2 : 1; post(right ? 0x204 : 0x201, mouse_buttons, mouse_lp()); dinput_mouse_button(right, 1); }
            if (!strcmp(act, "lup")) { mouse_buttons &= ~1; post(0x202, mouse_buttons, mouse_lp()); dinput_mouse_button(0, 0); }
            if ((!strcmp(act, "click") || !strcmp(act, "rclick")) && n_release < 8) release_vk[n_release++] = right ? 0x10002 : 0x10001;
        }
        host_log("input @%u: %s %s\n", fr, act, arg);
    }
    free(copy);
}

static void handle_event(SDL_Event e) {
    switch (e.type) {
    case SDL_QUIT:
        host_log("window closed\n");
        post(0x10, 0, 0);       // WM_CLOSE
#ifndef __EMSCRIPTEN__
        { static int n; if (++n > 2) { host_shutdown(); exit(0); } }
#endif
        break;
    case SDL_KEYDOWN: case SDL_KEYUP: {
        if (!e.key.repeat) dinput_key_sdl(e.key.keysym.scancode, e.type == SDL_KEYDOWN);
#ifndef __EMSCRIPTEN__
        if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_RETURN && (e.key.keysym.mod & KMOD_ALT)) {
            uint32_t fl = SDL_GetWindowFlags(g_window);
            SDL_SetWindowFullscreen(g_window, (fl & SDL_WINDOW_FULLSCREEN_DESKTOP) ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP);
            break;
        }
#endif
        uint32_t vk = sdl_to_vk(e.key.keysym.sym);
        if (vk) vk_event(vk, e.type == SDL_KEYDOWN, e.key.repeat, e.key.keysym.scancode);
        break;
    }
    case SDL_TEXTINPUT: {
        const unsigned char *t = (const unsigned char *)e.text.text;
        for (; *t; t++) if (*t >= 32 && *t < 128) post(0x102, *t, 1);
        break;
    }
    case SDL_MOUSEMOTION:
        if (captured) {
            dinput_mouse_move(e.motion.xrel, e.motion.yrel);
            mouse_to(mouse_x + e.motion.xrel, mouse_y + e.motion.yrel);
        } else {
            int gx, gy; gfx_map_mouse(e.motion.x, e.motion.y, &gx, &gy);
            dinput_mouse_move(gx - mouse_x, gy - mouse_y);
            mouse_to(gx, gy);
        }
        break;
    case SDL_MOUSEBUTTONDOWN: case SDL_MOUSEBUTTONUP: {
        int down = e.type == SDL_MOUSEBUTTONDOWN;
        if (!captured) { int gx, gy; gfx_map_mouse(e.button.x, e.button.y, &gx, &gy); if (gx != mouse_x || gy != mouse_y) mouse_to(gx, gy); }
        uint32_t bit = e.button.button == SDL_BUTTON_LEFT ? 1 : e.button.button == SDL_BUTTON_RIGHT ? 2 : 0x10;
        if (down) mouse_buttons |= bit; else mouse_buttons &= ~bit;
        keystate[e.button.button == SDL_BUTTON_LEFT ? 1 : e.button.button == SDL_BUTTON_RIGHT ? 2 : 4] = down ? 0x80 : 0;
        dinput_mouse_button(e.button.button == SDL_BUTTON_LEFT ? 0 : e.button.button == SDL_BUTTON_RIGHT ? 1 : 2, down);
        uint32_t msg = e.button.button == SDL_BUTTON_LEFT ? 0x201 : e.button.button == SDL_BUTTON_RIGHT ? 0x204 : 0x207;
        post(down ? msg : msg + 1, (uint32_t)mouse_buttons, mouse_lp());
        { static int lg = -1; if (lg < 0) lg = getenv("PORT_INPUTLOG") != NULL;
          if (lg) host_log("input: button %d %s at %d,%d frame %u (sdl %d,%d)\n", e.button.button, down ? "down" : "up", mouse_x, mouse_y, host_frame_count(), e.button.x, e.button.y); }
        break;
    }
    case SDL_MOUSEWHEEL: {
        int d = e.wheel.y * 120;
        dinput_mouse_wheel(d);
        post(0x20A, ((uint32_t)(d & 0xffff) << 16) | (uint32_t)mouse_buttons, mouse_lp());
        break;
    }
    case SDL_WINDOWEVENT:
        // The window stays active (engines that pause on WM_ACTIVATEAPP 0 stop when the mouse leaves); held mouse
        // buttons are released when focus goes.
        if (e.window.event == SDL_WINDOWEVENT_FOCUS_LOST) {
            if (mouse_buttons & 1) { mouse_buttons &= ~1; post(0x202, mouse_buttons, mouse_lp()); }
            if (mouse_buttons & 2) { mouse_buttons &= ~2; post(0x205, mouse_buttons, mouse_lp()); }
            dinput_mouse_button(0, 0); dinput_mouse_button(1, 0); dinput_mouse_button(2, 0);
        }
        break;
    }
}

// UE2's menus select the item under the cursor during a frame and ignore a click that arrives in the same frame
// as the move to it (a browser delivers move, down and up together), so such button events wait one frame.
static SDL_Event deferred[16];
static int ndeferred;
static uint32_t deferred_frame, moved_frame = 0xffffffffu;
void user32_pump_events(void) {
    SDL_Event e;
    scripted_input();
    uint32_t fr = host_frame_count();
    if (ndeferred && fr != deferred_frame) {
        SDL_Event now[16]; int nnow = ndeferred;
        memcpy(now, deferred, sizeof(SDL_Event) * (size_t)ndeferred);
        ndeferred = 0;
        for (int i = 0; i < nnow; i++) handle_event(now[i]);
    }
    int moved_this_pump = moved_frame == fr;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_MOUSEMOTION) { moved_this_pump = 1; moved_frame = fr; }
        if ((e.type == SDL_MOUSEBUTTONDOWN || e.type == SDL_MOUSEBUTTONUP) && !captured) {
            int gx, gy; gfx_map_mouse(e.button.x, e.button.y, &gx, &gy);
            if ((moved_this_pump || gx != mouse_x || gy != mouse_y || ndeferred) && ndeferred < 16) {
                if (gx != mouse_x || gy != mouse_y) { mouse_to(gx, gy); moved_frame = fr; }
                if (!ndeferred) deferred_frame = fr;
                deferred[ndeferred++] = e;
                continue;
            }
        }
        handle_event(e);
    }
}

static int peek(uint32_t pmsg, uint32_t hwnd, uint32_t lo, uint32_t hi, uint32_t remove) {
    for (int i = qh; i != qt; i = (i + 1) % QN) {
        Msg *m = &q[i];
        if (hwnd && m->hwnd != hwnd) continue;
        if ((lo || hi) && (m->msg < lo || m->msg > hi)) continue;
        W32(pmsg, m->hwnd); W32(pmsg + 4, m->msg); W32(pmsg + 8, m->wp); W32(pmsg + 12, m->lp);
        W32(pmsg + 16, m->time); W32(pmsg + 20, m->x); W32(pmsg + 24, m->y);
        if (remove & 1) {
            // remove entry i, keeping order
            for (int j = i; j != qh; ) { int p = (j - 1 + QN) % QN; q[j] = q[p]; j = p; }
            qh = (qh + 1) % QN;
        }
        return 1;
    }
    return 0;
}
static void u_PeekMessage(void) {
    user32_pump_events();
    SRET(peek(ARG(0), ARG(1), ARG(2), ARG(3), ARG(4)), 5);
}
static void u_TranslateMessage(void) { SRET(0, 1); }
static void u_DispatchMessage(void) {
    uint32_t pmsg = ARG(0);
    uint32_t hwnd = R32(pmsg), msg = R32(pmsg + 4), wp = R32(pmsg + 8), lp = R32(pmsg + 12);
    uint32_t r = send(hwnd, msg, wp, lp);
    if (g_unwind) return;
    SRET(r, 1);
}
static void u_SendMessage(void) {
    uint32_t r = send(ARG(0), ARG(1), ARG(2), ARG(3));
    if (g_unwind) return;
    SRET(r, 4);
}
static void u_SendMessageTimeoutW(void) {
    uint32_t r = send(ARG(0), ARG(1), ARG(2), ARG(3));
    if (g_unwind) return;
    if (ARG(6)) W32(ARG(6), r);
    SRET(1, 7);
}
static void u_PostMessage(void) {
    if (ARG(0) == 0xffffu) SRET(1, 4);      // HWND_BROADCAST
    post_to(ARG(0) ? ARG(0) : g_hwnd, ARG(1), ARG(2), ARG(3));
    SRET(1, 4);
}
static void u_PostQuitMessage(void) { post(0x12, ARG(0), 0); SRET(0, 1); }
static void u_CallWindowProc(void) {
    uint32_t r = guest_call(ARG(0), 4, ARG(1), ARG(2), ARG(3), ARG(4));
    if (g_unwind) return;
    SRET(r, 5);
}
static void u_DefWindowProc(void) {
    uint32_t hwnd = ARG(0), msg = ARG(1);
    uint32_t r = 0;
    switch (msg) {
    case 0x81: r = 1; break;                                // WM_NCCREATE
    case 0x10: {                                            // WM_CLOSE
        Win *w = user32_win(hwnd);
        if (w) { send(hwnd, 0x02, 0, 0); if (g_unwind) return; }
        break;
    }
    case 0x0D: { Win *w = user32_win(hwnd); r = put_w(ARG(3), ARG(2), w ? w->text : ""); break; }   // WM_GETTEXT
    case 0x0E: { Win *w = user32_win(hwnd); r = w ? (uint32_t)strlen(w->text) : 0; break; }        // WM_GETTEXTLENGTH
    }
    SRET(r, 4);
}
static void u_DefMDIChildProc(void) { SRET(0, 4); }
static void u_GetMessageTime(void) { SRET(host_ticks(), 0); }
static uint32_t reg_msg_n;
static void u_RegisterWindowMessage(void) { SRET(0xC200 + (reg_msg_n++), 1); }

// ---------------------------------------------------------------- cursor and keyboard
static void u_GetCursorPos(void) {
    Win *w = user32_win(g_hwnd);
    W32(ARG(0), mouse_x + (w ? w->x : 0)); W32(ARG(0) + 4, mouse_y + (w ? w->y : 0));
    SRET(1, 1);
}
static void u_SetCursorPos(void) {
    Win *w = user32_win(g_hwnd);
    mouse_x = (int32_t)ARG(0) - (w ? w->x : 0); mouse_y = (int32_t)ARG(1) - (w ? w->y : 0);
    SRET(1, 2);
}
#ifdef __EMSCRIPTEN__
// The page owns pointer lock (web/shell.html: Module.onWantPointerLock): browsers grant it only inside a click or
// key handler, and SDL's relative mode never asks for it when the pointer has not entered the canvas yet (no mouse
// focus) nor again after Esc released it. Locked, SDL still reports movementX/Y as relative motion.
EM_JS(void, web_want_pointer_lock, (int want), { if (Module.onWantPointerLock) Module.onWantPointerLock(!!want); });
#endif
static void update_capture(void) {
    int want = cursor_count < 0;
    if (want != captured) {
        captured = want;
#ifdef __EMSCRIPTEN__
        web_want_pointer_lock(want);
#else
        SDL_SetRelativeMouseMode(want ? SDL_TRUE : SDL_FALSE);
#endif
        host_log("mouse %s\n", want ? "captured (relative)" : "released");
    }
}
static void u_ShowCursor(void) {
    cursor_count += ARG(0) ? 1 : -1;
    SDL_ShowCursor(cursor_count >= 0 ? SDL_ENABLE : SDL_DISABLE);
    update_capture();
    SRET((uint32_t)cursor_count, 1);
}
static void u_ClipCursor(void) { SRET(1, 1); }
static void u_SetCursor(void) { SRET(0, 1); }
static void u_GetCursor(void) { SRET(0x00030001, 0); }
static void u_LoadCursor(void) { SRET(0x00030001, 2); }
static void u_LoadIcon(void) { SRET(0x00040001, 2); }
static void u_LoadImageA(void) { SRET(0x00040002, 6); }
static void u_SetCapture(void) { SRET(0, 1); }
static void u_ReleaseCapture(void) { SRET(1, 0); }
static void u_GetCapture(void) { SRET(0, 0); }
static void u_WindowFromPoint(void) { SRET(g_hwnd, 2); }
static void u_GetKeyState(void) { SRET(keystate[ARG(0) & 0xff] ? 0xff80u : 0, 1); }
static void u_SetKeyboardState(void) { SRET(1, 1); }
static void u_MapVirtualKeyW(void) {
    uint32_t code = ARG(0), type = ARG(1);
    if (type == 0) SRET(dinput_vk_to_dik(code), 2);     // VK -> scan code (DIK numbers are scan codes)
    SRET(0, 2);
}
static void u_GetKeyNameTextW(void) { SRET(put_w(ARG(1), ARG(2), "Key"), 3); }
static void u_RegisterHotKey(void) { SRET(1, 4); }
static void u_UnregisterHotKey(void) { SRET(1, 2); }
static void u_AttachThreadInput(void) { SRET(1, 3); }

// ---------------------------------------------------------------- painting, menus, dialogs, clipboard (no-ops)
static void u_GetDC(void) { SRET(0x00050001, 1); }
static void u_ReleaseDC(void) { SRET(1, 2); }
static void u_BeginPaint(void) { memset(G + ARG(1), 0, 64); W32(ARG(1), 0x00050001); SRET(0x00050001, 2); }
static void u_EndPaint(void) { SRET(1, 2); }
static void u_ret0_1(void) { SRET(0, 1); }
static void u_ret0_2(void) { SRET(0, 2); }
static void u_ret0_3(void) { SRET(0, 3); }
static void u_ret0_4(void) { SRET(0, 4); }
static void u_ret0_5(void) { SRET(0, 5); }
static void u_ret0_7(void) { SRET(0, 7); }
static void u_ret1_0(void) { SRET(1, 0); }
static void u_ret1_1(void) { SRET(1, 1); }
static void u_ret1_2(void) { SRET(1, 2); }
static void u_ret1_3(void) { SRET(1, 3); }
static void u_ret1_4(void) { SRET(1, 4); }
static void u_retm1_2(void) { SRET(0xffffffffu, 2); }
static void u_DrawText(void) { SRET(16, 5); }
static void u_DrawTextEx(void) { SRET(16, 6); }
static void u_GetSysColor(void) { SRET(0xC0C0C0, 1); }
static void u_GetSysColorBrush(void) { SRET(0x00070001, 1); }
static void u_BeginDeferWindowPos(void) { SRET(0x00080001, 1); }
static void u_DeferWindowPos(void) { SRET(ARG(0), 8); }
static void u_CreateMenu(void) { SRET(0x00090001, 0); }
static void u_LoadMenu(void) { SRET(0x00090002, 2); }
static void u_MessageBoxW(void) {
    char t[512], c[128];
    wstr(ARG(1), t, sizeof t); wstr(ARG(2), c, sizeof c);
    host_log("MessageBox: [%s] %s\n", c, t);
    SRET(1, 4);
}
static void u_MessageBoxA(void) {
    host_log("MessageBox: [%s] %s\n", ARG(2) ? GSTR(ARG(2)) : "", ARG(1) ? GSTR(ARG(1)) : "");
    SRET(1, 4);
}

#define U(n, f) {"USER32", n, f}
const ShimDef shims_user32[] = {
    U("RegisterClassExA", u_RegisterClassExA), U("RegisterClassExW", u_RegisterClassExW),
    U("GetClassInfoExA", u_GetClassInfoExA), U("GetClassInfoExW", u_GetClassInfoExW),
    U("CreateWindowExA", u_CreateWindowExA), U("CreateWindowExW", u_CreateWindowExW),
    U("CreateDialogParamA", u_CreateDialogParam), U("CreateDialogParamW", u_CreateDialogParam),
    U("CreateDialogIndirectParamA", u_CreateDialogIndirectParamA),
    U("DialogBoxParamA", u_DialogBoxParam), U("DialogBoxParamW", u_DialogBoxParam), U("EndDialog", u_EndDialog),
    U("DestroyWindow", u_DestroyWindow), U("IsWindow", u_IsWindow), U("IsWindowVisible", u_IsWindowVisible),
    U("IsWindowEnabled", u_IsWindowEnabled), U("EnableWindow", u_ret0_2), U("ShowWindow", u_ShowWindow),
    U("UpdateWindow", u_UpdateWindow), U("GetParent", u_GetParent), U("SetParent", u_SetParent), U("GetWindow", u_GetWindow),
    U("GetDlgItem", u_GetDlgItem), U("GetDlgCtrlID", u_GetDlgCtrlID), U("EnumChildWindows", u_EnumChildWindows),
    U("EnumThreadWindows", u_EnumThreadWindows), U("FindWindowExA", u_FindWindowEx), U("FindWindowExW", u_FindWindowEx),
    U("GetDesktopWindow", u_GetDesktopWindow), U("GetActiveWindow", u_GetActiveWindow), U("GetFocus", u_ret_hwnd0),
    U("GetForegroundWindow", u_ret_hwnd0), U("SetActiveWindow", u_ret_hwnd1), U("SetFocus", u_ret_hwnd1),
    U("SetForegroundWindow", u_ret_hwnd1), U("BringWindowToTop", u_ret1_1),
    U("GetWindowThreadProcessId", u_GetWindowThreadProcessId),
    U("GetWindowLongA", u_GetWindowLong), U("GetWindowLongW", u_GetWindowLong),
    U("SetWindowLongA", u_SetWindowLong), U("SetWindowLongW", u_SetWindowLong),
    U("SetPropA", u_SetPropA), U("SetPropW", u_SetPropW), U("GetPropA", u_GetPropA), U("GetPropW", u_GetPropW),
    U("RemovePropA", u_RemovePropA), U("RemovePropW", u_RemovePropW),
    U("GetWindowTextA", u_GetWindowTextA), U("GetWindowTextW", u_GetWindowTextW),
    U("GetClientRect", u_GetClientRect), U("GetWindowRect", u_GetWindowRect), U("SetWindowPos", u_SetWindowPos),
    U("MoveWindow", u_MoveWindow), U("AdjustWindowRect", u_AdjustWindowRect), U("ClientToScreen", u_ClientToScreen),
    U("ScreenToClient", u_ScreenToClient), U("MapWindowPoints", u_MapWindowPoints), U("IsIconic", u_IsIconic),
    U("IsZoomed", u_IsZoomed), U("GetSystemMetrics", u_GetSystemMetrics),
    U("SystemParametersInfoA", u_SystemParametersInfo), U("SystemParametersInfoW", u_SystemParametersInfo),
    U("PeekMessageA", u_PeekMessage), U("PeekMessageW", u_PeekMessage), U("TranslateMessage", u_TranslateMessage),
    U("DispatchMessageA", u_DispatchMessage), U("DispatchMessageW", u_DispatchMessage),
    U("SendMessageA", u_SendMessage), U("SendMessageW", u_SendMessage), U("SendMessageTimeoutW", u_SendMessageTimeoutW),
    U("PostMessageA", u_PostMessage), U("PostMessageW", u_PostMessage), U("PostQuitMessage", u_PostQuitMessage),
    U("CallWindowProcA", u_CallWindowProc), U("CallWindowProcW", u_CallWindowProc),
    U("DefWindowProcA", u_DefWindowProc), U("DefWindowProcW", u_DefWindowProc),
    U("DefMDIChildProcA", u_DefMDIChildProc), U("DefMDIChildProcW", u_DefMDIChildProc),
    U("GetMessageTime", u_GetMessageTime), U("RegisterWindowMessageA", u_RegisterWindowMessage),
    U("RegisterWindowMessageW", u_RegisterWindowMessage),
    U("GetCursorPos", u_GetCursorPos), U("SetCursorPos", u_SetCursorPos), U("ShowCursor", u_ShowCursor),
    U("ClipCursor", u_ClipCursor), U("SetCursor", u_SetCursor), U("GetCursor", u_GetCursor),
    U("LoadCursorA", u_LoadCursor), U("LoadCursorW", u_LoadCursor), U("LoadIconA", u_LoadIcon), U("LoadIconW", u_LoadIcon),
    U("LoadImageA", u_LoadImageA), U("SetCapture", u_SetCapture), U("ReleaseCapture", u_ReleaseCapture),
    U("GetCapture", u_GetCapture), U("WindowFromPoint", u_WindowFromPoint), U("GetKeyState", u_GetKeyState),
    U("SetKeyboardState", u_SetKeyboardState), U("MapVirtualKeyW", u_MapVirtualKeyW), U("GetKeyNameTextW", u_GetKeyNameTextW),
    U("RegisterHotKey", u_RegisterHotKey), U("UnregisterHotKey", u_UnregisterHotKey), U("AttachThreadInput", u_AttachThreadInput),
    U("GetDC", u_GetDC), U("ReleaseDC", u_ReleaseDC), U("BeginPaint", u_BeginPaint), U("EndPaint", u_EndPaint),
    U("InvalidateRect", u_ret1_3), U("ValidateRect", u_ret1_2), U("GetUpdateRect", u_ret0_3), U("FillRect", u_ret1_3),
    U("DrawEdge", u_ret1_4), U("DrawFocusRect", u_ret1_2), U("DrawTextA", u_DrawText), U("DrawTextW", u_DrawText),
    U("DrawTextExA", u_DrawTextEx), U("DrawTextExW", u_DrawTextEx), U("GetSysColor", u_GetSysColor),
    U("GetSysColorBrush", u_GetSysColorBrush), U("MessageBeep", u_ret1_1),
    U("BeginDeferWindowPos", u_BeginDeferWindowPos), U("EndDeferWindowPos", u_ret1_1),
    U("CreatePopupMenu", u_CreateMenu), U("DestroyMenu", u_ret1_1), U("LoadMenuA", u_LoadMenu), U("LoadMenuW", u_LoadMenu),
    U("GetMenu", u_ret0_1), U("SetMenu", u_ret1_2), U("GetSubMenu", u_ret0_2), U("GetMenuItemCount", u_ret0_1),
    U("GetMenuItemInfoA", u_ret0_4), U("GetMenuItemInfoW", u_ret0_4), U("SetMenuItemInfoA", u_ret1_4),
    U("SetMenuItemInfoW", u_ret1_4), U("InsertMenuItemA", u_ret1_4), U("GetMenuState", u_retm1_2),
    U("TrackPopupMenu", u_ret0_7), U("OpenClipboard", u_ret0_1), U("CloseClipboard", u_ret1_0), U("EmptyClipboard", u_ret1_0),
    U("GetClipboardData", u_ret0_1), U("SetClipboardData", u_ret0_2),
    U("MessageBoxA", u_MessageBoxA), U("MessageBoxW", u_MessageBoxW),
    {0, 0, 0}
};
