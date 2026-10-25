// DirectInput 8 emulation: the system keyboard and mouse, fed from the SDL events user32.c translates.
#include "host.h"
#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>

#define QLEN 256
typedef struct { uint32_t ofs, data, time, seq; } DIEvent;
typedef struct {
    int used, kind;              // kind 1 keyboard, 2 mouse
    uint32_t g;
    uint32_t data_size;          // from SetDataFormat
    int buffered, acquired;
    DIEvent q[QLEN]; int qh, qt;
} DIDev;
static DIDev devs[8];
static uint32_t vt_di, vt_dev, g_di;
static uint8_t keys[256];
static int32_t mdx, mdy, mdz;    // mouse motion accumulated since the last GetDeviceState
static uint8_t mbtn[8];
static uint32_t seq;

static DIDev *getd(uint32_t g) {
    uint32_t i = R32(g + 4);
    return i < 8 && devs[i].used ? &devs[i] : NULL;
}
static void push(int kind, uint32_t ofs, uint32_t data) {
    for (int i = 0; i < 8; i++) {
        DIDev *d = &devs[i];
        if (!d->used || d->kind != kind || !d->buffered) continue;
        int n = (d->qt + 1) % QLEN;
        if (n == d->qh) continue;
        d->q[d->qt].ofs = ofs; d->q[d->qt].data = data; d->q[d->qt].time = host_ticks(); d->q[d->qt].seq = ++seq;
        d->qt = n;
    }
}

// ---------------------------------------------------------------- input from user32.c
static uint8_t sdl_to_dik(int sc) {
    static const uint8_t letters[26] = { 0x1E, 0x30, 0x2E, 0x20, 0x12, 0x21, 0x22, 0x23, 0x17, 0x24, 0x25, 0x26, 0x32,
                                         0x31, 0x18, 0x19, 0x10, 0x13, 0x1F, 0x14, 0x16, 0x2F, 0x11, 0x2D, 0x15, 0x2C };
    if (sc >= SDL_SCANCODE_A && sc <= SDL_SCANCODE_Z) return letters[sc - SDL_SCANCODE_A];
    if (sc >= SDL_SCANCODE_1 && sc <= SDL_SCANCODE_0) return (uint8_t)(2 + sc - SDL_SCANCODE_1);
    if (sc >= SDL_SCANCODE_F1 && sc <= SDL_SCANCODE_F10) return (uint8_t)(0x3B + sc - SDL_SCANCODE_F1);
    switch (sc) {
    case SDL_SCANCODE_ESCAPE: return 0x01; case SDL_SCANCODE_MINUS: return 0x0C; case SDL_SCANCODE_EQUALS: return 0x0D;
    case SDL_SCANCODE_BACKSPACE: return 0x0E; case SDL_SCANCODE_TAB: return 0x0F; case SDL_SCANCODE_LEFTBRACKET: return 0x1A;
    case SDL_SCANCODE_RIGHTBRACKET: return 0x1B; case SDL_SCANCODE_RETURN: return 0x1C; case SDL_SCANCODE_LCTRL: return 0x1D;
    case SDL_SCANCODE_SEMICOLON: return 0x27; case SDL_SCANCODE_APOSTROPHE: return 0x28; case SDL_SCANCODE_GRAVE: return 0x29;
    case SDL_SCANCODE_LSHIFT: return 0x2A; case SDL_SCANCODE_BACKSLASH: return 0x2B; case SDL_SCANCODE_COMMA: return 0x33;
    case SDL_SCANCODE_PERIOD: return 0x34; case SDL_SCANCODE_SLASH: return 0x35; case SDL_SCANCODE_RSHIFT: return 0x36;
    case SDL_SCANCODE_KP_MULTIPLY: return 0x37; case SDL_SCANCODE_LALT: return 0x38; case SDL_SCANCODE_SPACE: return 0x39;
    case SDL_SCANCODE_CAPSLOCK: return 0x3A; case SDL_SCANCODE_NUMLOCKCLEAR: return 0x45; case SDL_SCANCODE_SCROLLLOCK: return 0x46;
    case SDL_SCANCODE_KP_7: return 0x47; case SDL_SCANCODE_KP_8: return 0x48; case SDL_SCANCODE_KP_9: return 0x49;
    case SDL_SCANCODE_KP_MINUS: return 0x4A; case SDL_SCANCODE_KP_4: return 0x4B; case SDL_SCANCODE_KP_5: return 0x4C;
    case SDL_SCANCODE_KP_6: return 0x4D; case SDL_SCANCODE_KP_PLUS: return 0x4E; case SDL_SCANCODE_KP_1: return 0x4F;
    case SDL_SCANCODE_KP_2: return 0x50; case SDL_SCANCODE_KP_3: return 0x51; case SDL_SCANCODE_KP_0: return 0x52;
    case SDL_SCANCODE_KP_PERIOD: return 0x53; case SDL_SCANCODE_F11: return 0x57; case SDL_SCANCODE_F12: return 0x58;
    case SDL_SCANCODE_KP_ENTER: return 0x9C; case SDL_SCANCODE_RCTRL: return 0x9D; case SDL_SCANCODE_KP_DIVIDE: return 0xB5;
    case SDL_SCANCODE_RALT: return 0xB8; case SDL_SCANCODE_PAUSE: return 0xC5; case SDL_SCANCODE_HOME: return 0xC7;
    case SDL_SCANCODE_UP: return 0xC8; case SDL_SCANCODE_PAGEUP: return 0xC9; case SDL_SCANCODE_LEFT: return 0xCB;
    case SDL_SCANCODE_RIGHT: return 0xCD; case SDL_SCANCODE_END: return 0xCF; case SDL_SCANCODE_DOWN: return 0xD0;
    case SDL_SCANCODE_PAGEDOWN: return 0xD1; case SDL_SCANCODE_INSERT: return 0xD2; case SDL_SCANCODE_DELETE: return 0xD3;
    }
    return 0;
}
// Windows virtual key -> DIK (scripted input names keys by VK)
uint8_t dinput_vk_to_dik(uint32_t vk) {
    static const uint8_t letters[26] = { 0x1E, 0x30, 0x2E, 0x20, 0x12, 0x21, 0x22, 0x23, 0x17, 0x24, 0x25, 0x26, 0x32,
                                         0x31, 0x18, 0x19, 0x10, 0x13, 0x1F, 0x14, 0x16, 0x2F, 0x11, 0x2D, 0x15, 0x2C };
    if (vk >= 'A' && vk <= 'Z') return letters[vk - 'A'];
    if (vk >= '1' && vk <= '9') return (uint8_t)(2 + vk - '1');
    if (vk == '0') return 0x0B;
    if (vk >= 0x70 && vk <= 0x79) return (uint8_t)(0x3B + vk - 0x70);
    switch (vk) {
    case 0x1B: return 0x01; case 0x08: return 0x0E; case 0x09: return 0x0F; case 0x0D: return 0x1C; case 0x11: return 0x1D;
    case 0x10: return 0x2A; case 0x12: return 0x38; case 0x20: return 0x39; case 0x25: return 0xCB; case 0x26: return 0xC8;
    case 0x27: return 0xCD; case 0x28: return 0xD0; case 0x21: return 0xC9; case 0x22: return 0xD1; case 0x23: return 0xCF;
    case 0x24: return 0xC7; case 0x2D: return 0xD2; case 0x2E: return 0xD3; case 0xDB: return 0x1A; case 0xDD: return 0x1B;
    case 0xBA: return 0x27; case 0xBC: return 0x33; case 0xBE: return 0x34; case 0xBF: return 0x35; case 0xC0: return 0x29;
    }
    return 0;
}
void dinput_key_dik(uint8_t dik, int down) {
    if (!dik) return;
    uint8_t v = down ? 0x80 : 0;
    if (keys[dik] == v) return;
    keys[dik] = v;
    push(1, dik, v);
}
void dinput_key_sdl(int scancode, int down) { dinput_key_dik(sdl_to_dik(scancode), down); }
void dinput_mouse_move(int dx, int dy) {
    if (!dx && !dy) return;
    mdx += dx; mdy += dy;
    if (dx) push(2, 0, (uint32_t)dx);
    if (dy) push(2, 4, (uint32_t)dy);
}
void dinput_mouse_wheel(int dz) { mdz += dz; push(2, 8, (uint32_t)dz); }
void dinput_mouse_button(int b, int down) {
    if (b < 0 || b > 7) return;
    uint8_t v = down ? 0x80 : 0;
    if (mbtn[b] == v) return;
    mbtn[b] = v;
    push(2, 12 + b, v);
}
void dinput_release_all(void) {
    for (int i = 0; i < 256; i++) if (keys[i]) dinput_key_dik((uint8_t)i, 0);
    for (int b = 0; b < 8; b++) dinput_mouse_button(b, 0);
}

// ---------------------------------------------------------------- IDirectInputDevice8A
static void d_QueryInterface(void) { if (ARG(2)) W32(ARG(2), 0); SRET(0x80004002u, 3); }
static void d_AddRef(void) { SRET(2, 1); }
static void d_Release(void) {
    DIDev *d = getd(ARG(0));
    if (d) { d->used = 0; }
    SRET(0, 1);
}
static void d_GetCapabilities(void) {
    DIDev *d = getd(ARG(0)); uint32_t p = ARG(1);
    uint32_t sz = R32(p);
    if (sz > 4) (COW(p + 4, sz - 4), memset(G + p + 4, 0, sz - 4));
    W32(p + 4, 0x5);    // ATTACHED | POLLEDDEVICE? (attached + emulated)
    W32(p + 8, d && d->kind == 2 ? 0x12 : 0x13);   // DI8DEVTYPE_MOUSE / KEYBOARD
    W32(p + 12, d && d->kind == 2 ? 3 : 0);  // axes
    W32(p + 16, d && d->kind == 2 ? 3 : 128); // buttons
    SRET(0, 2);
}
static void d_EnumObjects(void) { SRET(0, 4); }
static void d_GetProperty(void) {
    uint32_t guid = ARG(1), p = ARG(2);
    if (guid == 1 && p) { W32(p + 16, QLEN); SRET(0, 3); }   // DIPROP_BUFFERSIZE (passed as MAKEDIPROP(1))
    SRET(0x80070057u, 3);
}
static void d_SetProperty(void) {
    DIDev *d = getd(ARG(0));
    uint32_t guid = ARG(1), p = ARG(2);
    if (d && guid == 1) { d->buffered = R32(p + 16) > 0; host_log("dinput: device %d buffer size %u\n", d->kind, R32(p + 16)); }
    SRET(0, 3);
}
static void d_Acquire(void) { DIDev *d = getd(ARG(0)); if (d) d->acquired = 1; SRET(0, 1); }
static void d_Unacquire(void) { DIDev *d = getd(ARG(0)); if (d) d->acquired = 0; SRET(0, 1); }
static void d_GetDeviceState(void) {
    DIDev *d = getd(ARG(0));
    uint32_t n = ARG(1), p = ARG(2);
    if (!d) SRET(0x80070057u, 3);
    COW(p, n); memset(G + p, 0, n);
    if (d->kind == 1) {
        memcpy(G + p, keys, n < 256 ? n : 256);
    } else {
        W32(p, (uint32_t)mdx); W32(p + 4, (uint32_t)mdy); W32(p + 8, (uint32_t)mdz);
        mdx = mdy = mdz = 0;
        for (uint32_t b = 0; b < 8 && 12 + b < n; b++) W8(p + 12 + b, mbtn[b]);
    }
    SRET(0, 3);
}
static void d_GetDeviceData(void) {
    DIDev *d = getd(ARG(0));
    uint32_t cb = ARG(1), out = ARG(2), pn = ARG(3), flags = ARG(4);
    if (!d) SRET(0x80070057u, 5);
    uint32_t cap = R32(pn), n = 0;
    int peek = flags & 1;
    int h = d->qh;
    while (h != d->qt && (out == 0 ? 1 : n < cap)) {
        if (out) {
            uint32_t a = out + n * cb;
            COW(a, cb); memset(G + a, 0, cb);
            W32(a, d->q[h].ofs); W32(a + 4, d->q[h].data); W32(a + 8, d->q[h].time); W32(a + 12, d->q[h].seq);
        }
        n++;
        h = (h + 1) % QLEN;
        if (!out && cap == 0xffffffffu) continue;
    }
    if (!peek) d->qh = h;
    W32(pn, n);
    SRET(0, 5);
}
static void d_SetDataFormat(void) {
    DIDev *d = getd(ARG(0));
    uint32_t f = ARG(1);
    if (d) { d->data_size = R32(f + 12); host_log("dinput: device %d data format size %u\n", d->kind, d->data_size); }
    SRET(0, 2);
}
static void d_SetEventNotification(void) { SRET(0, 2); }
static void d_SetCooperativeLevel(void) { SRET(0, 3); }
static void d_GetObjectInfo(void) { SRET(0x80070057u, 4); }
static void d_GetDeviceInfo(void) {
    DIDev *d = getd(ARG(0)); uint32_t p = ARG(1);
    uint32_t sz = R32(p);
    if (sz > 4) (COW(p + 4, sz - 4), memset(G + p + 4, 0, sz - 4));
    W32(p + 36, d && d->kind == 2 ? 0x12 : 0x13);
    strcpy(GSTR(p + 40), d && d->kind == 2 ? "Mouse" : "Keyboard");
    strcpy(GSTR(p + 300), d && d->kind == 2 ? "Mouse" : "Keyboard");
    SRET(0, 2);
}
static void d_ok1(void) { SRET(0, 1); }
static void d_ok2(void) { SRET(0, 2); }
static void d_ok3(void) { SRET(0, 3); }
static void d_ok4(void) { SRET(0, 4); }
static void d_ok5(void) { SRET(0, 5); }
static void d_RunControlPanel(void) { SRET(0, 3); }
static void d_Initialize(void) { SRET(0, 4); }
static void d_CreateEffect(void) { SRET(0x80004001u, 5); }

// ---------------------------------------------------------------- IDirectInput8A
static const uint8_t GUID_SysMouse[16] = { 0x60, 0x2b, 0x1d, 0x6f, 0xa0, 0xd5, 0xcf, 0x11, 0xbf, 0xc7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00 };
static const uint8_t GUID_SysKeyboard[16] = { 0x61, 0x2b, 0x1d, 0x6f, 0xa0, 0xd5, 0xcf, 0x11, 0xbf, 0xc7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00 };
static void i_CreateDevice(void) {
    uint32_t guid = ARG(1), out = ARG(2);
    int kind = 0;
    if (!memcmp(G + guid, GUID_SysKeyboard, 16)) kind = 1;
    else if (!memcmp(G + guid, GUID_SysMouse, 16)) kind = 2;
    if (!kind) { host_log("dinput: CreateDevice for an unknown device\n"); W32(out, 0); SRET(0x80040154u, 4); }
    for (int i = 0; i < 8; i++) if (!devs[i].used) {
        memset(&devs[i], 0, sizeof devs[i]);
        devs[i].used = 1; devs[i].kind = kind;
        devs[i].g = com_new(vt_dev, 16);
        W32(devs[i].g + 4, (uint32_t)i);
        W32(out, devs[i].g);
        host_log("dinput: created %s\n", kind == 1 ? "keyboard" : "mouse");
        SRET(0, 4);
    }
    SRET(0x8007000eu, 4);
}
static void i_EnumDevices(void) { SRET(0, 5); }
static void i_GetDeviceStatus(void) { SRET(0, 2); }
static void i_RunControlPanel(void) { SRET(0, 3); }
static void i_Initialize(void) { SRET(0, 3); }
static void i_FindDevice(void) { SRET(0x80070057u, 4); }
static void i_EnumDevicesBySemantics(void) { SRET(0, 6); }
static void i_ConfigureDevices(void) { SRET(0, 5); }

void shim_DirectInput8Create(void) {
    if (!vt_di) {
        GFunc di[11] = { d_QueryInterface, d_AddRef, d_ok1, i_CreateDevice, i_EnumDevices, i_GetDeviceStatus, i_RunControlPanel,
                         i_Initialize, i_FindDevice, i_EnumDevicesBySemantics, i_ConfigureDevices };
        vt_di = com_vtable(11, di, "IDirectInput8A");
        GFunc dev[32] = { d_QueryInterface, d_AddRef, d_Release, d_GetCapabilities, d_EnumObjects, d_GetProperty, d_SetProperty,
                          d_Acquire, d_Unacquire, d_GetDeviceState, d_GetDeviceData, d_SetDataFormat, d_SetEventNotification,
                          d_SetCooperativeLevel, d_GetObjectInfo, d_GetDeviceInfo, d_RunControlPanel, d_Initialize, d_CreateEffect,
                          d_ok4, d_ok3, d_ok2, d_ok2, d_ok4, d_ok2, d_ok1, d_ok3 /* SendDeviceData: 4 args */, d_ok3, d_ok3,
                          d_ok4, d_ok4, d_ok2 };
        dev[26] = d_ok5;   // SendDeviceData(cb, rgdod, pdwInOut, fl)
        dev[27] = d_ok5; dev[28] = d_ok5;   // EnumEffectsInFile / WriteEffectToFile
        vt_dev = com_vtable(32, dev, "IDirectInputDevice8A");
        g_di = com_new(vt_di, 16);
    }
    host_log("DirectInput8Create(version %x)\n", ARG(1));
    W32(ARG(3), g_di);
    SRET(0, 5);
}
