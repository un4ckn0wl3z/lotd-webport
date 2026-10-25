// WINMM, ADVAPI32 (registry, user name), OLE32/OLEAUT32, SHELL32, COMDLG32, COMCTL32, GDI32, IMM32, DDRAW and
// IFC23 (Immersion force feedback) shims, the COM helpers and the shim lookup.
#include "host.h"
#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <strings.h>

extern char g_save_dir[512];
extern int g_fs_dirty;

static void ret0_0(void) { SRET(0, 0); }
static void ret0_1(void) { SRET(0, 1); }
static void ret0_2(void) { SRET(0, 2); }
static void ret0_3(void) { SRET(0, 3); }
static void ret0_4(void) { SRET(0, 4); }
static void ret0_5(void) { SRET(0, 5); }
static void ret0_6(void) { SRET(0, 6); }
static void ret1_1(void) { SRET(1, 1); }
static void ret1_2(void) { SRET(1, 2); }
static void ret1_3(void) { SRET(1, 3); }
static void ret1_5(void) { SRET(1, 5); }
static void ret1_9(void) { SRET(1, 9); }
static void ret1_11(void) { SRET(1, 11); }

// ---------------------------------------------------------------- WINMM
static void w_timeGetTime(void) { SRET(host_ticks(), 0); }

// ---------------------------------------------------------------- registry (kept in the save folder)
#define MAXK 64
#define MAXV 256
static char keys[MAXK][256]; static int nkeys;
static struct { int key; char name[64]; uint32_t type; uint8_t data[512]; uint32_t len; } vals[MAXV];
static int nvals, reg_loaded;
static void reg_save(void) {
    char path[1024]; snprintf(path, sizeof path, "%s/registry.dat", g_save_dir);
    FILE *f = fopen(path, "wb");
    if (!f) return;
    for (int i = 0; i < nvals; i++) {
        fprintf(f, "%s\t%s\t%u\t%u\t", keys[vals[i].key], vals[i].name, vals[i].type, vals[i].len);
        for (uint32_t j = 0; j < vals[i].len; j++) fprintf(f, "%02x", vals[i].data[j]);
        fprintf(f, "\n");
    }
    fclose(f);
    g_fs_dirty = 1;
}
static int key_index(const char *path, int create) {
    for (int i = 0; i < nkeys; i++) if (!strcasecmp(keys[i], path)) return i;
    if (!create || nkeys >= MAXK) return -1;
    snprintf(keys[nkeys], 256, "%s", path);
    return nkeys++;
}
static void reg_load(void) {
    if (reg_loaded) return;
    reg_loaded = 1;
    char path[1024]; snprintf(path, sizeof path, "%s/registry.dat", g_save_dir);
    FILE *f = fopen(path, "rb");
    if (!f) return;
    char line[2048];
    while (fgets(line, sizeof line, f) && nvals < MAXV) {
        char *k = strtok(line, "\t"), *n = strtok(NULL, "\t"), *t = strtok(NULL, "\t"), *l = strtok(NULL, "\t"), *d = strtok(NULL, "\t\r\n");
        if (!k || !n || !t || !l) continue;
        int ki = key_index(k, 1);
        vals[nvals].key = ki; snprintf(vals[nvals].name, 64, "%s", n);
        vals[nvals].type = (uint32_t)atoi(t); vals[nvals].len = (uint32_t)atoi(l);
        if (vals[nvals].len > 512) vals[nvals].len = 512;
        for (uint32_t j = 0; d && j < vals[nvals].len; j++) { unsigned v; sscanf(d + j * 2, "%2x", &v); vals[nvals].data[j] = (uint8_t)v; }
        nvals++;
    }
    fclose(f);
}
static void key_path(uint32_t hkey, const char *sub, char *out) {
    const char *root = "HKCU";
    if (hkey == 0x80000002u) root = "HKLM";
    else if (hkey >= 0x100 && hkey < 0x100 + MAXK) root = keys[hkey - 0x100];
    if (sub && *sub) snprintf(out, 256, "%s\\%s", root, sub); else snprintf(out, 256, "%s", root);
}
static void a_RegOpenKeyExA(void) {
    reg_load();
    char p[256]; key_path(ARG(0), ARG(1) ? GSTR(ARG(1)) : "", p);
    int k = key_index(p, 0);
    if (k < 0) SRET(2, 5);
    W32(ARG(4), 0x100 + k);
    SRET(0, 5);
}
static void a_RegOpenKeyA(void) {
    reg_load();
    char p[256]; key_path(ARG(0), ARG(1) ? GSTR(ARG(1)) : "", p);
    int k = key_index(p, 0);
    if (k < 0) SRET(2, 3);
    W32(ARG(2), 0x100 + k);
    SRET(0, 3);
}
static void a_RegCreateKeyExA(void) {
    reg_load();
    char p[256]; key_path(ARG(0), ARG(1) ? GSTR(ARG(1)) : "", p);
    int ex = key_index(p, 0);
    int k = key_index(p, 1);
    if (k < 0) SRET(8, 9);
    W32(ARG(7), 0x100 + k);
    if (ARG(8)) W32(ARG(8), ex < 0 ? 1 : 2);
    SRET(0, 9);
}
static void a_RegCloseKey(void) { SRET(0, 1); }
int reg_get_value(const char *keypath, const char *name, char *out, int outsz) {
    reg_load();
    int k = key_index(keypath, 0);
    for (int i = 0; k >= 0 && i < nvals; i++) if (vals[i].key == k && !strcasecmp(vals[i].name, name)) {
        snprintf(out, (size_t)outsz, "%.*s", (int)vals[i].len, (const char *)vals[i].data);
        return 1;
    }
    return 0;
}
void reg_set_value(const char *keypath, const char *name, const char *value) {
    reg_load();
    int k = key_index(keypath, 1);
    if (k < 0) return;
    int i;
    for (i = 0; i < nvals; i++) if (vals[i].key == k && !strcasecmp(vals[i].name, name)) break;
    if (i == nvals) { if (nvals >= MAXV) return; nvals++; }
    vals[i].key = k; snprintf(vals[i].name, 64, "%s", name); vals[i].type = 1;
    vals[i].len = (uint32_t)strlen(value) + 1;
    memcpy(vals[i].data, value, vals[i].len);
    reg_save();
}
static void a_RegQueryValueExA(void) {
    reg_load();
    int k = (int)ARG(0) - 0x100;
    const char *name = ARG(1) ? GSTR(ARG(1)) : "";
    uint32_t ptype = ARG(3), pdata = ARG(4), plen = ARG(5);
    for (int i = 0; i < nvals; i++) {
        if (vals[i].key == k && !strcasecmp(vals[i].name, name)) {
            if (ptype) W32(ptype, vals[i].type);
            if (plen) {
                uint32_t cap = R32(plen);
                W32(plen, vals[i].len);
                if (pdata) {
                    if (cap < vals[i].len) SRET(234, 6);
                    COW(pdata, vals[i].len); memcpy(G + pdata, vals[i].data, vals[i].len);
                }
            }
            SRET(0, 6);
        }
    }
    SRET(2, 6);
}
static void a_RegSetValueExA(void) {
    reg_load();
    int k = (int)ARG(0) - 0x100;
    const char *name = ARG(1) ? GSTR(ARG(1)) : "";
    uint32_t type = ARG(3), data = ARG(4), len = ARG(5);
    if (len > 512) len = 512;
    int i;
    for (i = 0; i < nvals; i++) if (vals[i].key == k && !strcasecmp(vals[i].name, name)) break;
    if (i == nvals) { if (nvals >= MAXV) SRET(8, 6); nvals++; }
    vals[i].key = k; snprintf(vals[i].name, 64, "%s", name); vals[i].type = type; vals[i].len = len;
    memcpy(vals[i].data, G + data, len);
    reg_save();
    SRET(0, 6);
}
static void a_GetUserNameA(void) { snprintf(GSTR(ARG(0)), R32(ARG(1)), "Player"); W32(ARG(1), 7); SRET(1, 2); }
static void a_GetUserNameW(void) {
    const char *s = "Player"; uint32_t i = 0;
    for (; s[i] && i + 1 < R32(ARG(1)); i++) W16(ARG(0) + 2 * i, (uint8_t)s[i]);
    W16(ARG(0) + 2 * i, 0); W32(ARG(1), i + 1);
    SRET(1, 2);
}

// ---------------------------------------------------------------- OLE32 / OLEAUT32 / SHELL32 / COMDLG32 / COMCTL32
static void o_CoInitializeEx(void) { SRET(0, 2); }
static void o_CoCreateInstance(void) {
    host_log("CoCreateInstance (not supported)\n");
    if (ARG(4)) W32(ARG(4), 0);
    SRET(0x80040154u, 5);
}
static void o_CoCreateGuid(void) {
    static uint32_t n;
    uint32_t g = ARG(0);
    W32(g, 0x4C4F5444u); W32(g + 4, host_ticks()); W32(g + 8, 0x12345678u); W32(g + 12, ++n);
    SRET(0, 1);
}
static void o_SysAllocString(void) {
    uint32_t s = ARG(0), n = 0;
    while (R16(s + 2 * n)) n++;
    uint32_t b = heap_alloc(4 + 2 * n + 2);
    W32(b, 2 * n); memcpy(G + b + 4, G + s, 2 * n + 2);
    SRET(b + 4, 1);
}
static void o_SysFreeString(void) { if (ARG(0)) heap_free(ARG(0) - 4); SRET(0, 1); }
static void s_ShellExecute(void) { host_log("ShellExecute (ignored)\n"); SRET(2, 6); }

// ---------------------------------------------------------------- GDI32 (only Window.dll's editor controls and gamma)
static void g_GetDeviceCaps(void) {
    int32_t i = (int32_t)ARG(1), v = 0;
    switch (i) {
    case 8: v = 1024; break; case 10: v = 768; break;      // HORZRES / VERTRES
    case 12: v = 32; break; case 14: v = 1; break;         // BITSPIXEL / PLANES
    case 88: case 90: v = 96; break;                       // LOGPIXELSX/Y
    case 116: v = 60; break;                               // VREFRESH
    }
    SRET(v, 2);
}
static void g_GetStockObject(void) { SRET(0x000A0000u | ARG(0), 1); }
static void g_handle(void) { SRET(0x000B0001u, 1); }
static void g_GetTextExtentPoint32(void) { W32(ARG(3), 8 * ARG(2)); W32(ARG(3) + 4, 16); SRET(1, 4); }
static void g_GetObjectA(void) { if (ARG(2)) memset(G + ARG(2), 0, ARG(1)); SRET(ARG(1), 3); }
static void g_CreateCompatibleBitmap(void) { SRET(0x000B0002u, 3); }
static void g_CreateDIBitmap(void) { SRET(0x000B0003u, 6); }
static void g_CreatePen(void) { SRET(0x000B0004u, 3); }
static void g_SelectObject(void) { SRET(0x000A0000u, 2); }
static void g_SetColor(void) { SRET(0, 2); }

// ---------------------------------------------------------------- IMM32 (no input method editor)
static void i_ImmCreateContext(void) { SRET(0x000C0001u, 0); }
// WinDrv makes a context whenever the window has none, every frame (and logs it): the window always has one
static void i_ImmGetContext(void) { SRET(0x000C0001u, 1); }

// ---------------------------------------------------------------- DDRAW: only used to ask for the video memory size
static void dd_ret_ok(void) { SRET(0, 1); }
static void dd_Release(void) { SRET(0, 1); }
static void dd_GetAvailableVidMem(void) {
    if (ARG(2)) W32(ARG(2), 256u << 20);
    if (ARG(3)) W32(ARG(3), 240u << 20);
    SRET(0, 4);
}
static void dd_QueryInterface(void) {
    // IDirectDraw2/4/7 share the layout up to GetAvailableVidMem; hand back the same object
    if (ARG(2)) { W32(ARG(2), ARG(0)); }
    SRET(0, 3);
}
static void dd_unimpl(void) { host_log("IDirectDraw method not implemented\n"); SRET(0x80004001u, 1); }
static void shim_DirectDrawCreate(void) {
    static uint32_t obj;
    if (!obj) {
        GFunc m[30];
        for (int i = 0; i < 30; i++) m[i] = dd_unimpl;
        m[0] = dd_QueryInterface; m[1] = dd_ret_ok; m[2] = dd_Release;
        m[23] = dd_GetAvailableVidMem;      // IDirectDraw2+ method 23
        obj = com_new(com_vtable(30, m, "IDirectDraw"), 16);
    }
    if (ARG(1)) W32(ARG(1), obj);
    SRET(0, 3);
}

// ---------------------------------------------------------------- IFC23: Immersion TouchSense (no force feedback)
// C++ members, __thiscall: the object is in ecx and the callee pops its arguments.
static void ifc_ctor(void) { R.eax = R.ecx; R.esp += 4; }
static void ifc_dtor(void) { R.esp += 4; }
static void ifc_CreateDevice(void) { R.eax = 0; R.esp += 4; }          // static __cdecl: caller pops
static void ifc_this0(void) { R.eax = 0; R.esp += 4; }
static void ifc_this1(void) { R.eax = 0; R.esp += 8; }
static void ifc_this2(void) { R.eax = 0; R.esp += 12; }
static void ifc_this3(void) { R.eax = 0; R.esp += 16; }
static void ifc_this4(void) { R.eax = 0; R.esp += 20; }

// DirectX entry points
void shim_Direct3DCreate8(void);
void shim_DirectInput8Create(void);
extern const ShimDef shims_bink[], shims_wsock[], shims_openal[];

const ShimDef shims_misc[] = {
    {"WINMM", "timeGetTime", w_timeGetTime}, {"WINMM", "timeBeginPeriod", ret0_1}, {"WINMM", "timeEndPeriod", ret0_1},
    {"ADVAPI32", "RegOpenKeyExA", a_RegOpenKeyExA}, {"ADVAPI32", "RegOpenKeyA", a_RegOpenKeyA},
    {"ADVAPI32", "RegCreateKeyExA", a_RegCreateKeyExA}, {"ADVAPI32", "RegCloseKey", a_RegCloseKey},
    {"ADVAPI32", "RegQueryValueExA", a_RegQueryValueExA}, {"ADVAPI32", "RegSetValueExA", a_RegSetValueExA},
    {"ADVAPI32", "GetUserNameA", a_GetUserNameA}, {"ADVAPI32", "GetUserNameW", a_GetUserNameW},
    {"ole32", "CoInitializeEx", o_CoInitializeEx}, {"ole32", "CoUninitialize", ret0_0},
    {"ole32", "CoCreateInstance", o_CoCreateInstance}, {"ole32", "CoCreateGuid", o_CoCreateGuid},
    {"OLEAUT32", "SysAllocString", o_SysAllocString}, {"OLEAUT32", "SysFreeString", o_SysFreeString},
    {"SHELL32", "ShellExecuteA", s_ShellExecute}, {"SHELL32", "ShellExecuteW", s_ShellExecute},
    {"SHELL32", "Shell_NotifyIconA", ret0_2},
    {"comdlg32", "GetSaveFileNameA", ret0_1}, {"comdlg32", "ChooseColorA", ret0_1},
    {"COMCTL32", "InitCommonControls", ret0_0},
    {"GDI32", "GetDeviceCaps", g_GetDeviceCaps}, {"GDI32", "GetStockObject", g_GetStockObject},
    {"GDI32", "CreateCompatibleDC", g_handle}, {"GDI32", "DeleteDC", ret1_1}, {"GDI32", "DeleteObject", ret1_1},
    {"GDI32", "SelectObject", g_SelectObject}, {"GDI32", "SetBkColor", g_SetColor}, {"GDI32", "SetTextColor", g_SetColor},
    {"GDI32", "SetBkMode", g_SetColor}, {"GDI32", "CreateSolidBrush", g_handle}, {"GDI32", "CreatePen", g_CreatePen},
    {"GDI32", "CreateCompatibleBitmap", g_CreateCompatibleBitmap}, {"GDI32", "CreateDIBitmap", g_CreateDIBitmap},
    {"GDI32", "GetObjectA", g_GetObjectA}, {"GDI32", "GetTextExtentPoint32A", g_GetTextExtentPoint32},
    {"GDI32", "GetTextExtentPoint32W", g_GetTextExtentPoint32}, {"GDI32", "ExtTextOutA", ret1_9},
    {"GDI32", "TextOutW", ret1_5}, {"GDI32", "BitBlt", ret1_9}, {"GDI32", "StretchBlt", ret1_11},
    {"GDI32", "PatBlt", ret1_5}, {"GDI32", "LineTo", ret1_3}, {"GDI32", "MoveToEx", ret1_3}, {"GDI32", "Rectangle", ret1_5},
    {"GDI32", "SetDeviceGammaRamp", ret1_2},
    {"IMM32", "ImmCreateContext", i_ImmCreateContext}, {"IMM32", "ImmDestroyContext", ret1_1},
    {"IMM32", "ImmAssociateContext", ret0_2}, {"IMM32", "ImmGetContext", i_ImmGetContext}, {"IMM32", "ImmReleaseContext", ret1_2},
    {"IMM32", "ImmGetCompositionStringW", ret0_4},
    {"DDRAW", "DirectDrawCreate", shim_DirectDrawCreate},
    {"d3d8", "Direct3DCreate8", shim_Direct3DCreate8}, {"DINPUT8", "DirectInput8Create", shim_DirectInput8Create},
    {"IFC23", "??0CImmProject@@QAE@XZ", ifc_ctor}, {"IFC23", "??1CImmProject@@QAE@XZ", ifc_dtor},
    {"IFC23", "?CreateDevice@CImmDevice@@SAPAV1@PAUHINSTANCE__@@PAUHWND__@@@Z", ifc_CreateDevice},
    {"IFC23", "?CreateEffectByIndex@CImmProject@@QAEPAVCImmCompoundEffect@@HPAVCImmDevice@@K@Z", ifc_this3},
    {"IFC23", "?GetEffectName@CImmProject@@QAEPBDH@Z", ifc_this1},
    {"IFC23", "?GetEffectType@CImmProject@@QAEKPBD@Z", ifc_this1},
    {"IFC23", "?GetNumEffects@CImmProject@@QAEHXZ", ifc_this0},
    {"IFC23", "?OpenFile@CImmProject@@QAEHPBDPAVCImmDevice@@@Z", ifc_this2},
    {"IFC23", "?Start@CImmProject@@QAEHPBDKKPAVCImmDevice@@@Z", ifc_this4},
    {"IFC23", "?Stop@CImmProject@@QAEHPBD@Z", ifc_this1},
    {0, 0, 0}
};

GFunc shim_lookup(const char *dll, const char *name) {
    const ShimDef *tabs[] = {shims_kernel32, shims_user32, shims_misc, shims_bink, shims_wsock, shims_openal};
    if (!strncmp(name, "__imp_", 6)) name += 6;
    for (size_t t = 0; t < sizeof tabs / sizeof *tabs; t++)
        for (const ShimDef *s = tabs[t]; s->name; s++)
            if (!strcmp(s->name, name)) {
                // dll match is case-insensitive and ignores ".dll"
                size_t l = strlen(s->dll);
                if (!strncasecmp(s->dll, dll, l) || !strcmp(dll, "?")) return s->fn;
            }
    return NULL;
}

// ---------------------------------------------------------------- COM helpers
uint32_t com_vtable(int n, const GFunc *methods, const char *prefix) {
    uint32_t vt = guest_static_alloc(n * 4);
    for (int i = 0; i < n; i++) {
        char *nm = malloc(64);
        snprintf(nm, 64, "%s::method%d", prefix, i);
        W32(vt + i * 4, host_thunk(methods[i], nm));
    }
    return vt;
}
uint32_t com_new(uint32_t vtbl, uint32_t size) {
    uint32_t o = heap_alloc(size < 16 ? 16 : size);
    COW(o, size < 16 ? 16 : size); memset(G + o, 0, size < 16 ? 16 : size);
    W32(o, vtbl);
    return o;
}
