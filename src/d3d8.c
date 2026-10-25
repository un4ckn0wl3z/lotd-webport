// Direct3D 8 on OpenGL ES 3.0 / WebGL 2 for UE2's D3DDrv: fixed-function pipeline only (the caps report no
// vertex or pixel shaders, so the engine never sends shader code; vertex shader handles carry declarations).
//
// Resources: vertex/index buffer contents live in guest memory (Lock hands out pointers there) and go to GL
// buffers on Unlock. Texture locks get a temporary buffer that is converted and uploaded on Unlock (UE2 always
// writes whole levels). Render targets, the back buffer and depth surfaces are GL textures/renderbuffers behind
// framebuffers; see d3d8_ff.c for the coordinate conventions.
#include "host.h"
#include "gl3.h"
#include "d3d8_ff.h"
#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#ifdef __EMSCRIPTEN__
#include <emscripten/html5.h>
#endif

#ifndef __EMSCRIPTEN__
#define GL_DEF(n, r, a) n##_fn p_##n;
GL_FUNCS(GL_DEF)
int gl_load_all(void) {
    int ok = 1;
#define GL_LOAD(n, r, a) p_##n = (n##_fn)SDL_GL_GetProcAddress(#n); if (!p_##n) { host_log("missing GL function %s\n", #n); ok = 0; }
    GL_FUNCS(GL_LOAD)
    return ok;
}
#endif

#define LOGONCE(...) do { static int _once; if (!_once) { _once = 1; host_log(__VA_ARGS__); } } while (0)

// ---------------------------------------------------------------- context, present
extern SDL_Window *g_window;
static SDL_GLContext g_ctx;
static int gl_es = 1, has_s3tc, has_aniso;
static GLuint present_prog, present_vbo, gamma_tex, g_vao;
static GLint present_u_tex, present_u_gamma, present_u_usegamma;
static int use_gamma;
static int vp_x, vp_y, vp_w, vp_h;      // letterbox in window pixels
static GLuint last_prog;                // program of the last draw
static int enabled[16];                 // vertex attribute arrays enabled
static void invalidate_gl_state(void);

static GLuint compile_shader(GLenum type, const char *body) {
    static char src[65536];
    if (gl_es) snprintf(src, sizeof src, "#version 300 es\nprecision highp float;\nprecision highp int;\n%s", body);
    else snprintf(src, sizeof src, "#version 330 core\n%s", body);
    GLuint s = glCreateShader(type);
    const char *p = src;
    glShaderSource(s, 1, &p, NULL);
    glCompileShader(s);
    GLint ok; glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[4096]; glGetShaderInfoLog(s, sizeof log, NULL, log);
        host_log("shader compile failed: %s\n---\n%s\n", log, src);
    }
    return s;
}
static GLuint link_program(const char *vs, const char *fs) {
    GLuint p = glCreateProgram();
    GLuint v = compile_shader(GL_VERTEX_SHADER, vs), f = compile_shader(GL_FRAGMENT_SHADER, fs);
    glAttachShader(p, v); glAttachShader(p, f);
    glBindAttribLocation(p, A_POS, "a_pos"); glBindAttribLocation(p, A_NORMAL, "a_normal");
    glBindAttribLocation(p, A_DIFFUSE, "a_diffuse"); glBindAttribLocation(p, A_SPECULAR, "a_specular");
    for (int i = 0; i < 8; i++) { char n[16]; snprintf(n, sizeof n, "a_tex%d", i); glBindAttribLocation(p, A_TEX0 + i, n); }
    glLinkProgram(p);
    GLint ok; glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) { char log[4096]; glGetProgramInfoLog(p, sizeof log, NULL, log); host_log("link failed: %s\n", log); }
    glDeleteShader(v); glDeleteShader(f);
    return p;
}

static int has_ext(const char *name) {
    GLint n = 0;
    glGetIntegerv(GL_NUM_EXTENSIONS, &n);
    for (GLint i = 0; i < n; i++) { const char *e = (const char *)glGetStringi(GL_EXTENSIONS, (GLuint)i); if (e && strstr(e, name)) return 1; }
    return 0;
}

void gfx_window_created(SDL_Window *w) {
    g_ctx = SDL_GL_CreateContext(w);
    if (!g_ctx) {
        host_log("GLES 3 context failed (%s), trying desktop GL 3.3 core\n", SDL_GetError());
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
        g_ctx = SDL_GL_CreateContext(w);
        gl_es = 0;
    }
    if (!g_ctx) host_fatal("cannot create GL context: %s", SDL_GetError());
    SDL_GL_MakeCurrent(w, g_ctx);
    if (!GL_LOAD_ALL()) host_fatal("GL functions missing");
    { extern int ff_debug_solid; ff_debug_solid = getenv("PORT_D3DSOLID") != NULL; }
    const char *ver = (const char *)glGetString(GL_VERSION);
    host_log("GL: %s / %s\n", ver, (const char *)glGetString(GL_RENDERER));
#ifdef __EMSCRIPTEN__
    // WebGL: DXT textures and anisotropic filtering are extensions the page has to switch on
    {
        EMSCRIPTEN_WEBGL_CONTEXT_HANDLE c = emscripten_webgl_get_current_context();
        has_s3tc = emscripten_webgl_enable_extension(c, "WEBGL_compressed_texture_s3tc");
        has_aniso = emscripten_webgl_enable_extension(c, "EXT_texture_filter_anisotropic");
    }
#else
    if (ver && !strstr(ver, "OpenGL ES")) gl_es = 0;
    has_s3tc = has_ext("texture_compression_s3tc") || has_ext("WEBGL_compressed_texture_s3tc");
    has_aniso = has_ext("texture_filter_anisotropic");
#endif
    host_log("GL: %s, S3TC %s, anisotropic filtering %s\n", gl_es ? "ES 3" : "desktop core", has_s3tc ? "yes" : "no (decoded)", has_aniso ? "yes" : "no");
#ifndef __EMSCRIPTEN__
    SDL_GL_SetSwapInterval(1);
#endif
    glGenVertexArrays(1, &g_vao);
    glBindVertexArray(g_vao);
    present_prog = link_program(
        "in vec4 a_pos; out vec2 v_uv;\n"
        "void main() { gl_Position = vec4(a_pos.xy, 0.0, 1.0); v_uv = a_pos.zw; }\n",
        "in vec2 v_uv; uniform sampler2D u_tex; uniform sampler2D u_gamma; uniform float u_usegamma; out vec4 o_color;\n"
        "void main() { vec4 c = texture(u_tex, v_uv);\n"
        "  if (u_usegamma > 0.5) { c.r = texture(u_gamma, vec2(c.r * 255.0 / 256.0 + 0.5 / 256.0, 0.5)).r;\n"
        "    c.g = texture(u_gamma, vec2(c.g * 255.0 / 256.0 + 0.5 / 256.0, 0.5)).g;\n"
        "    c.b = texture(u_gamma, vec2(c.b * 255.0 / 256.0 + 0.5 / 256.0, 0.5)).b; }\n"
        "  o_color = vec4(c.rgb, 1.0); }\n");
    present_u_tex = glGetUniformLocation(present_prog, "u_tex");
    present_u_gamma = glGetUniformLocation(present_prog, "u_gamma");
    present_u_usegamma = glGetUniformLocation(present_prog, "u_usegamma");
    glGenBuffers(1, &present_vbo);
    glGenTextures(1, &gamma_tex);
    glBindTexture(GL_TEXTURE_2D, gamma_tex);
    uint8_t ident[256 * 4];
    for (int i = 0; i < 256; i++) { ident[i * 4] = ident[i * 4 + 1] = ident[i * 4 + 2] = (uint8_t)i; ident[i * 4 + 3] = 255; }
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 256, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, ident);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
}

// ---------------------------------------------------------------- formats
#define FMT_R8G8B8 20
#define FMT_A8R8G8B8 21
#define FMT_X8R8G8B8 22
#define FMT_R5G6B5 23
#define FMT_X1R5G5B5 24
#define FMT_A1R5G5B5 25
#define FMT_A4R4G4B4 26
#define FMT_A8 28
#define FMT_X4R4G4B4 30
#define FMT_P8 41
#define FMT_L8 50
#define FMT_A8L8 51
#define FMT_D32 71
#define FMT_D15S1 73
#define FMT_D24S8 75
#define FMT_D16 80
#define FMT_D24X8 77
#define FMT_D24X4S4 79
#define FMT_INDEX16 101
#define FMT_INDEX32 102
#define FOURCC(a, b, c, d) ((uint32_t)(a) | ((uint32_t)(b) << 8) | ((uint32_t)(c) << 16) | ((uint32_t)(d) << 24))
#define FMT_DXT1 FOURCC('D', 'X', 'T', '1')
#define FMT_DXT2 FOURCC('D', 'X', 'T', '2')
#define FMT_DXT3 FOURCC('D', 'X', 'T', '3')
#define FMT_DXT4 FOURCC('D', 'X', 'T', '4')
#define FMT_DXT5 FOURCC('D', 'X', 'T', '5')

static int is_dxt(uint32_t f) { return f == FMT_DXT1 || f == FMT_DXT2 || f == FMT_DXT3 || f == FMT_DXT4 || f == FMT_DXT5; }
static int is_depth(uint32_t f) { return f >= 70 && f <= 80; }
static uint32_t fmt_bpp(uint32_t f) {
    switch (f) {
    case FMT_A8R8G8B8: case FMT_X8R8G8B8: case FMT_D32: case FMT_D24S8: case FMT_D24X8: case FMT_D24X4S4: return 4;
    case FMT_R8G8B8: return 3;
    case FMT_R5G6B5: case FMT_X1R5G5B5: case FMT_A1R5G5B5: case FMT_A4R4G4B4: case FMT_X4R4G4B4:
    case FMT_A8L8: case FMT_D16: case FMT_D15S1: return 2;
    case FMT_A8: case FMT_P8: case FMT_L8: return 1;
    }
    return 4;
}
static uint32_t fmt_pitch(uint32_t f, uint32_t w) {
    if (f == FMT_DXT1) return ((w + 3) / 4) * 8;
    if (is_dxt(f)) return ((w + 3) / 4) * 16;
    return w * fmt_bpp(f);
}
static uint32_t fmt_rows(uint32_t f, uint32_t h) { return is_dxt(f) ? (h + 3) / 4 : h; }
static int fmt_texture_ok(uint32_t f) {
    switch (f) {
    case FMT_A8R8G8B8: case FMT_X8R8G8B8: case FMT_R5G6B5: case FMT_X1R5G5B5: case FMT_A1R5G5B5:
    case FMT_A4R4G4B4: case FMT_X4R4G4B4: case FMT_A8: case FMT_L8: case FMT_A8L8: case FMT_R8G8B8:
    case FMT_DXT1: case FMT_DXT2: case FMT_DXT3: case FMT_DXT4: case FMT_DXT5: return 1;
    }
    return 0;
}
static const char *fmt_name(uint32_t f) {
    static char b[16];
    if (f > 0xffff) { memcpy(b, &f, 4); b[4] = 0; return b; }
    snprintf(b, sizeof b, "%u", f);
    return b;
}

static void c565(uint16_t c, uint8_t *o) {
    o[0] = (uint8_t)(((c >> 11) & 31) * 255 / 31); o[1] = (uint8_t)(((c >> 5) & 63) * 255 / 63); o[2] = (uint8_t)((c & 31) * 255 / 31);
}
static void dxt_color_block(const uint8_t *b, uint8_t out[16][4], int dxt1) {
    uint16_t c0 = b[0] | (b[1] << 8), c1 = b[2] | (b[3] << 8);
    uint8_t p[4][4];
    c565(c0, p[0]); c565(c1, p[1]); p[0][3] = p[1][3] = 255;
    if (c0 > c1 || !dxt1) {
        for (int k = 0; k < 3; k++) { p[2][k] = (uint8_t)((2 * p[0][k] + p[1][k]) / 3); p[3][k] = (uint8_t)((p[0][k] + 2 * p[1][k]) / 3); }
        p[2][3] = p[3][3] = 255;
    } else {
        for (int k = 0; k < 3; k++) { p[2][k] = (uint8_t)((p[0][k] + p[1][k]) / 2); p[3][k] = 0; }
        p[2][3] = 255; p[3][3] = 0;
    }
    uint32_t idx = b[4] | (b[5] << 8) | (b[6] << 16) | ((uint32_t)b[7] << 24);
    for (int i = 0; i < 16; i++) memcpy(out[i], p[(idx >> (2 * i)) & 3], 4);
}
static void dxt_to_rgba(uint32_t f, const uint8_t *src, uint32_t pitch, uint32_t w, uint32_t h, uint8_t *dst) {
    for (uint32_t by = 0; by < (h + 3) / 4; by++)
        for (uint32_t bx = 0; bx < (w + 3) / 4; bx++) {
            const uint8_t *b = src + by * pitch + bx * (f == FMT_DXT1 ? 8 : 16);
            uint8_t px[16][4];
            if (f == FMT_DXT1) dxt_color_block(b, px, 1);
            else {
                dxt_color_block(b + 8, px, 0);
                if (f == FMT_DXT2 || f == FMT_DXT3) {
                    for (int i = 0; i < 16; i++) { int a = (b[i / 2] >> ((i & 1) * 4)) & 15; px[i][3] = (uint8_t)(a * 17); }
                } else {
                    uint8_t a[8]; a[0] = b[0]; a[1] = b[1];
                    if (a[0] > a[1]) for (int i = 0; i < 6; i++) a[2 + i] = (uint8_t)(((6 - i) * a[0] + (1 + i) * a[1]) / 7);
                    else { for (int i = 0; i < 4; i++) a[2 + i] = (uint8_t)(((4 - i) * a[0] + (1 + i) * a[1]) / 5); a[6] = 0; a[7] = 255; }
                    uint64_t bits = 0;
                    for (int i = 0; i < 6; i++) bits |= (uint64_t)b[2 + i] << (8 * i);
                    for (int i = 0; i < 16; i++) px[i][3] = a[(bits >> (3 * i)) & 7];
                }
            }
            for (int y = 0; y < 4; y++) for (int x = 0; x < 4; x++) {
                uint32_t X = bx * 4 + x, Y = by * 4 + y;
                if (X < w && Y < h) memcpy(dst + (Y * w + X) * 4, px[y * 4 + x], 4);
            }
        }
}

// How a D3D format goes to GL. conv: 0 direct copy, 1 to RGBA8, 2 rotate 16-bit (5551 / 4444), 3 DXT compressed
typedef struct { GLenum ifmt, fmt, type; int conv, bpp_out; } GLFmt;
static GLFmt gl_format(uint32_t f) {
    GLFmt g = { GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, 1, 4 };
    switch (f) {
    case FMT_R5G6B5: g.ifmt = GL_RGB565; g.fmt = GL_RGB; g.type = GL_UNSIGNED_SHORT_5_6_5; g.conv = 0; g.bpp_out = 2; break;
    case FMT_A1R5G5B5: case FMT_X1R5G5B5: g.ifmt = GL_RGB5_A1; g.fmt = GL_RGBA; g.type = GL_UNSIGNED_SHORT_5_5_5_1; g.conv = 2; g.bpp_out = 2; break;
    case FMT_A4R4G4B4: case FMT_X4R4G4B4: g.ifmt = GL_RGBA4; g.fmt = GL_RGBA; g.type = GL_UNSIGNED_SHORT_4_4_4_4; g.conv = 2; g.bpp_out = 2; break;
    case FMT_DXT1: if (has_s3tc) { g.ifmt = GL_COMPRESSED_RGBA_S3TC_DXT1_EXT; g.conv = 3; } break;
    case FMT_DXT2: case FMT_DXT3: if (has_s3tc) { g.ifmt = GL_COMPRESSED_RGBA_S3TC_DXT3_EXT; g.conv = 3; } break;
    case FMT_DXT4: case FMT_DXT5: if (has_s3tc) { g.ifmt = GL_COMPRESSED_RGBA_S3TC_DXT5_EXT; g.conv = 3; } break;
    }
    return g;
}
// convert w x h pixels of D3D format f (source pitch) into GL upload layout (tightly packed)
static void convert_pixels(uint32_t f, const uint8_t *src, uint32_t pitch, uint32_t w, uint32_t h, uint8_t *dst, const GLFmt *g) {
    if (g->conv == 3) { for (uint32_t y = 0; y < fmt_rows(f, h); y++) memcpy(dst + y * fmt_pitch(f, w), src + y * pitch, fmt_pitch(f, w)); return; }
    if (is_dxt(f)) { dxt_to_rgba(f, src, pitch, w, h, dst); return; }
    for (uint32_t y = 0; y < h; y++) {
        const uint8_t *s = src + y * pitch;
        uint8_t *d = dst + y * w * g->bpp_out;
        uint16_t *d16 = (uint16_t *)d;
        switch (f) {
        case FMT_A8R8G8B8: for (uint32_t x = 0; x < w; x++, s += 4, d += 4) { d[0] = s[2]; d[1] = s[1]; d[2] = s[0]; d[3] = s[3]; } break;
        case FMT_X8R8G8B8: for (uint32_t x = 0; x < w; x++, s += 4, d += 4) { d[0] = s[2]; d[1] = s[1]; d[2] = s[0]; d[3] = 255; } break;
        case FMT_R8G8B8: for (uint32_t x = 0; x < w; x++, s += 3, d += 4) { d[0] = s[2]; d[1] = s[1]; d[2] = s[0]; d[3] = 255; } break;
        case FMT_R5G6B5: memcpy(d, s, w * 2); break;
        case FMT_A1R5G5B5: case FMT_X1R5G5B5:
            for (uint32_t x = 0; x < w; x++) {
                uint16_t c = (uint16_t)(s[2 * x] | (s[2 * x + 1] << 8));
                d16[x] = (uint16_t)((c << 1) | (f == FMT_X1R5G5B5 ? 1 : (c >> 15)));
            }
            break;
        case FMT_A4R4G4B4: case FMT_X4R4G4B4:
            for (uint32_t x = 0; x < w; x++) {
                uint16_t c = (uint16_t)(s[2 * x] | (s[2 * x + 1] << 8));
                d16[x] = (uint16_t)((c << 4) | (f == FMT_X4R4G4B4 ? 15 : (c >> 12)));
            }
            break;
        case FMT_A8: for (uint32_t x = 0; x < w; x++, s++, d += 4) { d[0] = d[1] = d[2] = 255; d[3] = s[0]; } break;
        case FMT_L8: case FMT_P8: for (uint32_t x = 0; x < w; x++, s++, d += 4) { d[0] = d[1] = d[2] = s[0]; d[3] = 255; } break;
        case FMT_A8L8: for (uint32_t x = 0; x < w; x++, s += 2, d += 4) { d[0] = d[1] = d[2] = s[0]; d[3] = s[1]; } break;
        default: memset(d, 255, (size_t)w * 4);
        }
    }
}
// RGBA8 rows (GL read-back) to a D3D format
static void from_rgba(uint32_t f, const uint8_t *src, uint32_t w, uint32_t h, uint8_t *dst, uint32_t pitch) {
    for (uint32_t y = 0; y < h; y++) {
        const uint8_t *s = src + y * w * 4;
        uint8_t *d = dst + y * pitch;
        for (uint32_t x = 0; x < w; x++, s += 4) {
            switch (f) {
            case FMT_R5G6B5: { uint16_t c = (uint16_t)(((s[0] >> 3) << 11) | ((s[1] >> 2) << 5) | (s[2] >> 3)); d[0] = (uint8_t)c; d[1] = (uint8_t)(c >> 8); d += 2; break; }
            case FMT_X1R5G5B5: case FMT_A1R5G5B5: { uint16_t c = (uint16_t)(0x8000 | ((s[0] >> 3) << 10) | ((s[1] >> 3) << 5) | (s[2] >> 3)); d[0] = (uint8_t)c; d[1] = (uint8_t)(c >> 8); d += 2; break; }
            default: d[0] = s[2]; d[1] = s[1]; d[2] = s[0]; d[3] = f == FMT_X8R8G8B8 ? 255 : s[3]; d += 4; break;
            }
        }
    }
}

// ---------------------------------------------------------------- objects
enum { T_D3D = 1, T_DEV, T_TEX, T_CUBE, T_SURF, T_VB, T_IB };
typedef struct Tex Tex;
typedef struct Surf {
    int type, ref; uint32_t g;
    int role;                 // 0 image (system memory), 1 texture level, 2 back buffer, 3 depth, 4 render target
    uint32_t fmt, w, h, pitch, usage, pool;
    Tex *tex; int level, face;
    GLuint gltex;             // standalone colour surfaces (back buffer, render targets)
    GLuint rb;                // depth renderbuffer
    GLuint fbo;               // framebuffer when this is a colour target
    GLuint fbo_depth;         // depth renderbuffer currently attached to fbo
    uint32_t mem;             // persistent guest pixels (image surfaces)
    uint32_t lockbuf, lockpitch; int lx, ly, lw, lh, lflags, locked;
} Surf;
struct Tex {
    int type, ref; uint32_t g;
    uint32_t fmt, w, h, levels, usage, pool;
    GLenum target; GLuint gl; GLFmt gf;
    Surf *lv[6][16];
    int nfaces;
};
typedef struct Buf {
    int type, ref; uint32_t g;
    uint32_t len, usage, fmt, pool, mem;
    GLuint gl; uint32_t gl_len;
    uint32_t lock_off, lock_len, lock_flags; int locked;
} Buf;

#define MAXOBJ 65536
static void *objs[MAXOBJ];
static int obj_free_hint = 1;
static uint32_t vt_d3d, vt_dev, vt_tex, vt_cube, vt_surf, vt_vb, vt_ib;

static uint32_t make_obj(void *o, uint32_t vt) {
    int i = obj_free_hint;
    for (int n = 0; n < MAXOBJ; n++, i = (i + 1) % MAXOBJ) if (i && !objs[i]) break;
    if (objs[i] || !i) host_fatal("out of D3D objects");
    obj_free_hint = i + 1;
    objs[i] = o;
    uint32_t g = com_new(vt, 16);
    W32(g + 4, (uint32_t)i);
    W32(g + 8, 0xD3D00000u);
    ((int *)o)[2] = (int)g;   // the g field (after type, ref)
    return g;
}
static void *get_obj(uint32_t g, int type) {
    if (!g) return NULL;
    uint32_t i = R32(g + 4);
    if (i >= MAXOBJ || !objs[i]) { host_log("d3d: bad object %08x\n", g); return NULL; }
    if (type && *(int *)objs[i] != type) { host_log("d3d: object %08x type %d != %d\n", g, *(int *)objs[i], type); return NULL; }
    return objs[i];
}
static void free_obj(uint32_t g) {
    uint32_t i = R32(g + 4);
    if (i < MAXOBJ) objs[i] = NULL;
    if (i < (uint32_t)obj_free_hint) obj_free_hint = (int)i;
    W32(g, 0);
    heap_free(g);
}

// ---------------------------------------------------------------- device state
typedef struct { float m[16]; } Mat;
typedef struct { uint8_t stream, reg, type, offset; } VElem;
typedef struct { int used; int n; VElem el[24]; } VDecl;
#define MAXDECL 512
static VDecl decls[MAXDECL];
typedef struct {
    uint32_t type; float d[4], s[4], a[4], pos[3], dir[3], range, falloff, att[3], theta, phi;
    int enabled;
} Light;
#define MAXLIGHTS 128
#define MAXSB 256
typedef struct SBlock SBlock;
static struct {
    int type, ref; uint32_t g;
    uint32_t bb_w, bb_h, bb_fmt, autodepth, depth_fmt;
    Surf *bb, *depth, *cur_rt, *cur_ds;
    uint32_t rs[256];
    uint32_t tss[8][32];
    Tex *tex[8];
    Mat world, view, proj, texm[8];
    uint32_t vp[4]; float vp_minz, vp_maxz;
    uint32_t shader;            // FVF code, or a declaration handle (odd)
    Buf *vb[16]; uint32_t stride[16];
    Buf *ib; uint32_t base_vertex;
    float mat[17];              // D3DMATERIAL8
    Light lights[MAXLIGHTS];
    float clip[6][4];
    GLuint up_buf, up_ibuf, samplers[8];
    int in_scene;
    uint32_t frames;
    SBlock *recording;
} D;
static uint32_t g_d3d;      // IDirect3D8 object

static Mat ident_mat(void) { Mat m; memset(&m, 0, sizeof m); m.m[0] = m.m[5] = m.m[10] = m.m[15] = 1; return m; }
static Mat mat_mul(const Mat *a, const Mat *b) {   // D3D row-vector convention: a then b
    Mat r;
    for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) {
        float s = 0;
        for (int k = 0; k < 4; k++) s += a->m[i * 4 + k] * b->m[k * 4 + j];
        r.m[i * 4 + j] = s;
    }
    return r;
}
// inverse transpose of the upper 3x3 (normal matrix), as a 4x4
static Mat normal_mat(const Mat *a) {
    const float *m = a->m;
    float c00 = m[5] * m[10] - m[6] * m[9], c01 = m[6] * m[8] - m[4] * m[10], c02 = m[4] * m[9] - m[5] * m[8];
    float c10 = m[2] * m[9] - m[1] * m[10], c11 = m[0] * m[10] - m[2] * m[8], c12 = m[1] * m[8] - m[0] * m[9];
    float c20 = m[1] * m[6] - m[2] * m[5], c21 = m[2] * m[4] - m[0] * m[6], c22 = m[0] * m[5] - m[1] * m[4];
    float det = m[0] * c00 + m[1] * c01 + m[2] * c02;
    float id = fabsf(det) > 1e-20f ? 1.0f / det : 0.0f;
    Mat r = ident_mat();
    // (M^-1)^T = cofactor matrix / det
    r.m[0] = c00 * id; r.m[1] = c01 * id; r.m[2] = c02 * id;
    r.m[4] = c10 * id; r.m[5] = c11 * id; r.m[6] = c12 * id;
    r.m[8] = c20 * id; r.m[9] = c21 * id; r.m[10] = c22 * id;
    return r;
}

static void reset_states(void) {
    memset(D.rs, 0, sizeof D.rs);
    D.rs[7] = D.autodepth ? 1 : 0;   // ZENABLE
    D.rs[8] = 3;    // FILLMODE solid
    D.rs[9] = 2;    // SHADEMODE gouraud
    D.rs[14] = 1;   // ZWRITEENABLE
    D.rs[16] = 1;   // LASTPIXEL
    D.rs[19] = 2;   // SRCBLEND one
    D.rs[20] = 1;   // DESTBLEND zero
    D.rs[22] = 3;   // CULLMODE ccw
    D.rs[23] = 4;   // ZFUNC lessequal
    D.rs[25] = 8;   // ALPHAFUNC always
    D.rs[26] = 1;   // DITHER
    { float one = 1.0f; memcpy(&D.rs[37], &one, 4); memcpy(&D.rs[38], &one, 4); }    // FOGEND, FOGDENSITY
    D.rs[56] = 8;   // STENCILFUNC always
    D.rs[53] = D.rs[54] = D.rs[55] = 1;     // stencil ops keep
    D.rs[58] = D.rs[59] = 0xffffffffu;      // stencil masks
    D.rs[60] = 0xffffffffu;  // TEXTUREFACTOR
    D.rs[136] = 1;  // CLIPPING
    D.rs[137] = 1;  // LIGHTING
    D.rs[141] = 1;  // COLORVERTEX
    D.rs[145] = 1;  // DIFFUSEMATERIALSOURCE color1
    D.rs[146] = 2;  // SPECULARMATERIALSOURCE color2
    D.rs[168] = 0xf; // COLORWRITEENABLE
    D.rs[171] = 1;  // BLENDOP add
    for (int s = 0; s < 8; s++) {
        memset(D.tss[s], 0, sizeof D.tss[s]);
        D.tss[s][1] = s == 0 ? 4 : 1; D.tss[s][2] = 2; D.tss[s][3] = 1;    // COLOROP, ARG1 texture, ARG2 current
        D.tss[s][4] = s == 0 ? 2 : 1; D.tss[s][5] = 2; D.tss[s][6] = 1;    // ALPHAOP
        D.tss[s][11] = (uint32_t)s;                                         // TEXCOORDINDEX
        D.tss[s][13] = 1; D.tss[s][14] = 1; D.tss[s][25] = 1;               // ADDRESSU/V/W wrap
        D.tss[s][16] = 1; D.tss[s][17] = 1; D.tss[s][18] = 0;               // MAG/MIN point, MIP none
        D.tss[s][21] = 1;                                                   // MAXANISOTROPY
        D.tss[s][26] = 1; D.tss[s][27] = 1; D.tss[s][28] = 1;               // ARG0s current, RESULTARG current
        D.texm[s] = ident_mat();
    }
    D.world = D.view = D.proj = ident_mat();
    memset(D.mat, 0, sizeof D.mat);
}

// ---------------------------------------------------------------- program cache
typedef struct { FFKey k; FFProg p; uint32_t serial; int used; } ProgEnt;
#define PROG_HASH 2048
static ProgEnt progcache[PROG_HASH];
static uint32_t key_hash(const FFKey *k) {
    const uint8_t *b = (const uint8_t *)k; uint32_t h = 2166136261u;
    for (size_t i = 0; i < sizeof *k; i++) { h ^= b[i]; h *= 16777619u; }
    return h;
}
static FFProg *get_prog(const FFKey *k) {
    uint32_t h = key_hash(k);
    for (int i = 0; i < 16; i++) {
        ProgEnt *e = &progcache[(h + i) & (PROG_HASH - 1)];
        if (e->used && !memcmp(&e->k, k, sizeof *k)) return &e->p;
        if (!e->used) {
            static char vs[32768], fs[32768];
            ff_make_sources(k, vs, sizeof vs, fs, sizeof fs);
            FFProg *p = &e->p;
            memset(p, 0, sizeof *p);
            p->prog = link_program(vs, fs);
            GLuint pr = p->prog;
#define U(f, n) p->f = glGetUniformLocation(pr, n)
            U(wv, "u_wv"); U(proj, "u_proj"); U(nmat, "u_nmat"); U(world, "u_world"); U(rt, "u_rt"); U(off, "u_off");
            U(mat_d, "u_mat_d"); U(mat_a, "u_mat_a"); U(mat_s, "u_mat_s"); U(mat_e, "u_mat_e"); U(mat_p, "u_mat_p"); U(amb, "u_amb");
            U(l_d, "u_l_d"); U(l_s, "u_l_s"); U(l_a, "u_l_a"); U(l_pos, "u_l_pos"); U(l_dir, "u_l_dir"); U(l_att, "u_l_att"); U(l_spot, "u_l_spot");
            U(tf, "u_tf"); U(aref, "u_aref"); U(fogc, "u_fogc"); U(fogp, "u_fogp"); U(clip, "u_clip");
#undef U
            for (int s = 0; s < FF_STAGES; s++) {
                char nm[16];
                snprintf(nm, sizeof nm, "u_tm[%d]", s); p->tm[s] = glGetUniformLocation(pr, nm);
                snprintf(nm, sizeof nm, "u_s%d", s); p->samp[s] = glGetUniformLocation(pr, nm);
            }
            glUseProgram(pr);
            for (int s = 0; s < FF_STAGES; s++) if (p->samp[s] >= 0) glUniform1i(p->samp[s], s);
            e->k = *k; e->used = 1;
            static int nprog;
            if (++nprog % 50 == 0) host_log("d3d: %d shader programs\n", nprog);
            return p;
        }
    }
    // table full around this hash: reuse the first slot
    ProgEnt *e = &progcache[h & (PROG_HASH - 1)];
    glDeleteProgram(e->p.prog);
    e->used = 0;
    return get_prog(k);
}

// ---------------------------------------------------------------- textures
static Surf *new_surf(int role, uint32_t fmt, uint32_t w, uint32_t h, Tex *t, int level, int face);
static void tex_alloc_gl(Tex *t) {
    glGenTextures(1, &t->gl);
    glBindTexture(t->target, t->gl);
    t->gf = gl_format(t->fmt);
    if (t->usage & 1) t->gf = (GLFmt){ GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, 1, 4 };    // render target
    for (int f = 0; f < t->nfaces; f++) {
        GLenum tgt = t->target == GL_TEXTURE_CUBE_MAP ? GL_TEXTURE_CUBE_MAP_POSITIVE_X + f : GL_TEXTURE_2D;
        for (uint32_t l = 0; l < t->levels; l++) {
            uint32_t w = t->w >> l, h = t->h >> l;
            if (!w) w = 1; if (!h) h = 1;
            if (t->gf.conv == 3) {
                uint32_t sz = fmt_pitch(t->fmt, w) * fmt_rows(t->fmt, h);
                void *z = calloc(1, sz);
                glCompressedTexImage2D(tgt, (GLint)l, t->gf.ifmt, (GLsizei)w, (GLsizei)h, 0, (GLsizei)sz, z);
                free(z);
            } else {
                glTexImage2D(tgt, (GLint)l, (GLint)t->gf.ifmt, (GLsizei)w, (GLsizei)h, 0, t->gf.fmt, t->gf.type, NULL);
            }
        }
    }
    glTexParameteri(t->target, GL_TEXTURE_MAX_LEVEL, (GLint)t->levels - 1);
}
static uint32_t create_texture(int cube, uint32_t w, uint32_t h, uint32_t levels, uint32_t usage, uint32_t fmt, uint32_t pool) {
    Tex *t = calloc(1, sizeof *t);
    t->type = cube ? T_CUBE : T_TEX; t->ref = 1;
    t->fmt = fmt; t->w = w; t->h = h; t->usage = usage; t->pool = pool;
    if (!levels) { levels = 1; for (uint32_t m = w > h ? w : h; m > 1; m >>= 1) levels++; }
    if (levels > 16) levels = 16;
    t->levels = levels;
    t->nfaces = cube ? 6 : 1;
    t->target = cube ? GL_TEXTURE_CUBE_MAP : GL_TEXTURE_2D;
    make_obj(t, cube ? vt_cube : vt_tex);
    tex_alloc_gl(t);
    for (int f = 0; f < t->nfaces; f++)
        for (uint32_t l = 0; l < levels; l++) {
            uint32_t lw = w >> l, lh = h >> l;
            t->lv[f][l] = new_surf(1, fmt, lw ? lw : 1, lh ? lh : 1, t, (int)l, f);
            t->lv[f][l]->ref = 0;
        }
    return t->g;
}

// upload a locked rectangle of a texture level from guest memory
static void tex_upload_rect(Surf *s, uint32_t src, uint32_t pitch, int x, int y, int w, int h) {
    Tex *t = s->tex;
    GLenum tgt = t->target == GL_TEXTURE_CUBE_MAP ? GL_TEXTURE_CUBE_MAP_POSITIVE_X + s->face : GL_TEXTURE_2D;
    glBindTexture(t->target, t->gl);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    if (t->gf.conv == 3) {
        // compressed: whole 4x4 blocks
        uint32_t bw = (uint32_t)((w + 3) / 4) * 4, bh = (uint32_t)((h + 3) / 4) * 4;
        if (x == 0 && y == 0 && (uint32_t)w >= s->w && (uint32_t)h >= s->h) { bw = s->w; bh = s->h; }
        uint32_t rp = fmt_pitch(t->fmt, bw), rows = fmt_rows(t->fmt, bh);
        uint8_t *tmp = malloc((size_t)rp * rows);
        for (uint32_t r = 0; r < rows; r++) memcpy(tmp + r * rp, G + src + r * pitch, rp);
        glCompressedTexSubImage2D(tgt, s->level, x, y, (GLsizei)bw, (GLsizei)bh, t->gf.ifmt, (GLsizei)(rp * rows), tmp);
        free(tmp);
        return;
    }
    uint8_t *tmp = malloc((size_t)w * h * 4 + 16);
    convert_pixels(t->fmt, G + src, pitch, (uint32_t)w, (uint32_t)h, tmp, &t->gf);
    glTexSubImage2D(tgt, s->level, x, y, w, h, t->gf.fmt, t->gf.type, tmp);
    free(tmp);
}

// ---------------------------------------------------------------- framebuffers
static GLuint cur_fbo;
static void bind_fbo(GLuint f) { if (f != cur_fbo) { glBindFramebuffer(GL_FRAMEBUFFER, f); cur_fbo = f; } }
static void surf_color_attach(Surf *s) {
    if (s->tex) glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                       s->tex->target == GL_TEXTURE_CUBE_MAP ? GL_TEXTURE_CUBE_MAP_POSITIVE_X + s->face : GL_TEXTURE_2D,
                                       s->tex->gl, s->level);
    else glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, s->gltex, 0);
}
static void bind_rt(void) {
    Surf *s = D.cur_rt ? D.cur_rt : D.bb;
    if (!s->fbo) {
        glGenFramebuffers(1, &s->fbo);
        bind_fbo(s->fbo);
        surf_color_attach(s);
    } else bind_fbo(s->fbo);
    GLuint want = D.cur_ds ? D.cur_ds->rb : 0;
    if (s->fbo_depth != want) {
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, want);
        s->fbo_depth = want;
        GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        if (st != GL_FRAMEBUFFER_COMPLETE) host_log("d3d: framebuffer incomplete %x (%ux%u)\n", st, s->w, s->h);
    }
}
static void rt_size(uint32_t *w, uint32_t *h) {
    Surf *s = D.cur_rt ? D.cur_rt : D.bb;
    *w = s->w; *h = s->h;
}

static void create_color_surface(Surf *s) {
    glGenTextures(1, &s->gltex);
    glBindTexture(GL_TEXTURE_2D, s->gltex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, (GLsizei)s->w, (GLsizei)s->h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
}
static void create_depth_surface(Surf *s) {
    glGenRenderbuffers(1, &s->rb);
    glBindRenderbuffer(GL_RENDERBUFFER, s->rb);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, (GLsizei)s->w, (GLsizei)s->h);
}

// read back a colour surface (rectangle) into RGBA8
static uint8_t *read_surface(Surf *s, int x, int y, int w, int h) {
    GLuint keep = cur_fbo;
    GLuint tmp = 0;
    if (s->fbo) bind_fbo(s->fbo);
    else {
        glGenFramebuffers(1, &tmp);
        bind_fbo(tmp);
        surf_color_attach(s);
    }
    uint8_t *px = malloc((size_t)w * h * 4);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(x, y, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px);
    if (tmp) { bind_fbo(keep); glDeleteFramebuffers(1, &tmp); }
    else bind_fbo(keep);
    return px;
}

// ---------------------------------------------------------------- present
static void present_to_window(void) {
    int ww, wh;
    SDL_GL_GetDrawableSize(g_window, &ww, &wh);
    float sx = (float)ww / D.bb_w, sy = (float)wh / D.bb_h, sc = sx < sy ? sx : sy;
    int w = (int)(D.bb_w * sc), h = (int)(D.bb_h * sc);
    vp_x = (ww - w) / 2; vp_y = (wh - h) / 2; vp_w = w; vp_h = h;
    bind_fbo(0);
    glDisable(GL_SCISSOR_TEST); glDisable(GL_DEPTH_TEST); glDisable(GL_BLEND); glDisable(GL_CULL_FACE); glDisable(GL_STENCIL_TEST);
    glColorMask(1, 1, 1, 1);
    glViewport(0, 0, ww, wh);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glViewport(vp_x, vp_y, w, h);
    glUseProgram(present_prog);
    glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D, gamma_tex); glBindSampler(1, 0);
    glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, D.bb->gltex); glBindSampler(0, 0);
    glUniform1i(present_u_tex, 0); glUniform1i(present_u_gamma, 1); glUniform1f(present_u_usegamma, use_gamma ? 1.0f : 0.0f);
    // the back buffer's first row is the top: flip for the window
    float q[16] = { -1, -1, 0, 1,  1, -1, 1, 1,  -1, 1, 0, 0,  1, 1, 1, 0 };
    glBindBuffer(GL_ARRAY_BUFFER, present_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof q, q, GL_STREAM_DRAW);
    for (int i = 0; i < 16; i++) glDisableVertexAttribArray((GLuint)i);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 16, 0);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glDisableVertexAttribArray(0);
    SDL_GL_SwapWindow(g_window);
    invalidate_gl_state();
    last_prog = 0;
    memset(enabled, 0, sizeof enabled);
}
// window coordinates -> back buffer coordinates
void gfx_map_mouse(int wx, int wy, int *gx, int *gy) {
    int ww, wh, dw, dh;
    SDL_GetWindowSize(g_window, &ww, &wh);
    SDL_GL_GetDrawableSize(g_window, &dw, &dh);
    float px = ww ? (float)wx * dw / ww : (float)wx, py = wh ? (float)wy * dh / wh : (float)wy;
    if (!vp_w || !vp_h) { *gx = wx; *gy = wy; return; }
    *gx = (int)((px - vp_x) * D.bb_w / vp_w);
    *gy = (int)((py - vp_y) * D.bb_h / vp_h);
}

// ---------------------------------------------------------------- generic method helpers
#define UNIMPL(cls, idx, nargs) static void cls##_m##idx(void) { LOGONCE("d3d: " #cls " method %d not implemented\n", idx); SRET(0x80004001u, nargs); }
static void com_QueryInterface(void) { if (ARG(2)) W32(ARG(2), 0); SRET(0x80004002u, 3); }
static void ok1(void) { SRET(0, 1); }
static void ok2(void) { SRET(0, 2); }
static void ok3(void) { SRET(0, 3); }
static void ok4(void) { SRET(0, 4); }
static void ok5(void) { SRET(0, 5); }
static void ok6(void) { SRET(0, 6); }

// ---------------------------------------------------------------- IDirect3D8
static void d3d_AddRef(void) { SRET(2, 1); }
static void d3d_Release(void) { SRET(1, 1); }
static void d3d_GetAdapterCount(void) { SRET(1, 1); }
static void d3d_GetAdapterIdentifier(void) {
    uint32_t p = ARG(3);
    memset(G + p, 0, 1068);
    strcpy(GSTR(p), "gl.dll");
    strcpy(GSTR(p + 512), "OpenGL ES 3 / WebGL 2");
    W32(p + 1024, 0x00010000); W32(p + 1028, 0x00060000);
    W32(p + 1032, 0x10DE); W32(p + 1036, 0x0300);
    SRET(0, 4);
}
static const uint32_t modes[][2] = { {640, 480}, {800, 600}, {1024, 768}, {1280, 720}, {1280, 960}, {1280, 1024}, {1600, 900}, {1600, 1200}, {1920, 1080} };
#define NMODES ((int)(sizeof modes / sizeof *modes))
static void d3d_GetAdapterModeCount(void) { SRET(NMODES, 2); }
static void d3d_EnumAdapterModes(void) {
    uint32_t i = ARG(2), p = ARG(3);
    if (i >= (uint32_t)NMODES) SRET(0x8876086cu, 4);
    W32(p, modes[i][0]); W32(p + 4, modes[i][1]); W32(p + 8, 60); W32(p + 12, FMT_X8R8G8B8);
    SRET(0, 4);
}
static void d3d_GetAdapterDisplayMode(void) {
    uint32_t p = ARG(2);
    W32(p, 1920); W32(p + 4, 1080); W32(p + 8, 60); W32(p + 12, FMT_X8R8G8B8);
    SRET(0, 3);
}
static void d3d_CheckDeviceType(void) { SRET(0, 6); }
static void d3d_CheckDeviceFormat(void) {
    uint32_t usage = ARG(4), rtype = ARG(5), f = ARG(6);
    int ok;
    if (usage & 2) ok = f == FMT_D24S8 || f == FMT_D16 || f == FMT_D24X8 || f == FMT_D32;   // DEPTHSTENCIL
    else if (usage & 1) ok = f == FMT_A8R8G8B8 || f == FMT_X8R8G8B8 || f == FMT_R5G6B5;    // RENDERTARGET
    else if (rtype == 3 || rtype == 5) ok = fmt_texture_ok(f);    // TEXTURE / CUBETEXTURE
    else if (rtype == 1) ok = fmt_texture_ok(f) || is_depth(f);   // SURFACE
    else ok = 0;                                                  // volumes
    static int logged;
    if (logged++ < 40) host_log("CheckDeviceFormat usage=%x rtype=%u fmt=%s -> %s\n", usage, rtype, fmt_name(f), ok ? "ok" : "no");
    SRET(ok ? 0 : 0x8876086au, 7);
}
static void d3d_CheckDeviceMultiSampleType(void) { SRET(ARG(5) == 0 ? 0 : 0x8876086au, 6); }
static void d3d_CheckDepthStencilMatch(void) { SRET(0, 6); }
static void fill_caps(uint32_t p) {
    memset(G + p, 0, 212);
    W32(p + 0, 1);                 // HAL
    W32(p + 8, 0x20000);           // Caps: READ_SCANLINE
    W32(p + 12, 0x20000 | 0x80000000u);   // Caps2: FULLSCREENGAMMA, NO2DDURING3DSCENE
    W32(p + 20, 0x8000000f);       // presentation intervals
    W32(p + 28, 0x1ffff0 & ~0x100000u);   // DevCaps: HW T&L, no pure device
    W32(p + 32, 0x2ff2);           // PrimitiveMiscCaps (incl. COLORWRITEENABLE, CULL*)
    W32(p + 36, 0x336191);         // RasterCaps: dither, ztest, fog vertex/table, mip LOD bias, z bias, range fog, anisotropy, w fog, z fog
    W32(p + 40, 0xff); W32(p + 44, 0x1fff); W32(p + 48, 0x1fff); W32(p + 52, 0xff);
    W32(p + 56, 0x1ffff);          // ShadeCaps
    W32(p + 60, 0x1 | 0x4 | 0x400 | 0x800 | 0x4000 | 0x10000);   // TextureCaps: perspective, alpha, projected, cube, mipmap, mipcube
    W32(p + 64, 0x03030303); W32(p + 68, 0x03030303); W32(p + 72, 0x03030303);
    W32(p + 76, 0x3f); W32(p + 80, 0x3f); W32(p + 84, 0x1f);
    W32(p + 88, 4096); W32(p + 92, 4096); W32(p + 96, 256); W32(p + 100, 8192); W32(p + 104, 4096); W32(p + 108, has_aniso ? 8 : 1);
    WF32(p + 112, 1e10); WF32(p + 116, -1e8); WF32(p + 120, -1e8); WF32(p + 124, 1e8); WF32(p + 128, 1e8);
    W32(p + 136, 0xff); W32(p + 140, 0x80008);   // StencilCaps, FVFCaps: 8 tex coords
    W32(p + 144, 0x3ffffff & ~(0x200000u | 0x400000u));   // TextureOpCaps (no bump mapping)
    W32(p + 148, 8); W32(p + 152, 8);              // stages, simultaneous textures
    W32(p + 156, 0x3b); W32(p + 160, 8); W32(p + 164, 3); W32(p + 168, 0);   // vertex processing, 8 lights, 3 clip planes, no blending
    WF32(p + 176, 64); W32(p + 180, 0xfffff); W32(p + 184, 0xffffff); W32(p + 188, 16); W32(p + 192, 255);
    W32(p + 196, 0xfffe0000);      // VS 0.0 (none)
    W32(p + 204, 0xffff0000);      // PS 0.0 (none)
}
static void d3d_GetDeviceCaps(void) { fill_caps(ARG(3)); SRET(0, 4); }
static void d3d_GetAdapterMonitor(void) { SRET(1, 2); }

static void apply_pp(uint32_t pp) {
    D.bb_w = R32(pp); D.bb_h = R32(pp + 4); D.bb_fmt = R32(pp + 8);
    if (!D.bb_w) D.bb_w = g_win_w;
    if (!D.bb_h) D.bb_h = g_win_h;
    if (!D.bb_fmt) D.bb_fmt = FMT_X8R8G8B8;
    D.autodepth = R32(pp + 32); D.depth_fmt = R32(pp + 36);
    host_log("d3d: present params %ux%u fmt=%s count=%u swap=%u windowed=%u autodepth=%u(%s) flags=%x interval=%x\n",
             D.bb_w, D.bb_h, fmt_name(D.bb_fmt), R32(pp + 12), R32(pp + 20), R32(pp + 28), D.autodepth, fmt_name(D.depth_fmt), R32(pp + 40), R32(pp + 48));
    g_win_w = D.bb_w; g_win_h = D.bb_h;
    if (!D.bb) { D.bb = new_surf(2, D.bb_fmt, D.bb_w, D.bb_h, NULL, 0, 0); D.bb->ref = 1000000; }
    else {
        if (D.bb->gltex) glDeleteTextures(1, &D.bb->gltex);
        if (D.bb->fbo) { if (cur_fbo == D.bb->fbo) bind_fbo(0); glDeleteFramebuffers(1, &D.bb->fbo); }
        D.bb->gltex = D.bb->fbo = D.bb->fbo_depth = 0;
    }
    D.bb->w = D.bb_w; D.bb->h = D.bb_h; D.bb->fmt = D.bb_fmt; D.bb->pitch = fmt_pitch(D.bb_fmt, D.bb_w);
    create_color_surface(D.bb);
    if (D.autodepth) {
        if (!D.depth) { D.depth = new_surf(3, D.depth_fmt, D.bb_w, D.bb_h, NULL, 0, 0); D.depth->ref = 1000000; }
        else if (D.depth->rb) glDeleteRenderbuffers(1, &D.depth->rb);
        D.depth->w = D.bb_w; D.depth->h = D.bb_h;
        create_depth_surface(D.depth);
    }
    D.cur_rt = D.bb; D.cur_ds = D.autodepth ? D.depth : NULL;
    D.vp[0] = 0; D.vp[1] = 0; D.vp[2] = D.bb_w; D.vp[3] = D.bb_h; D.vp_minz = 0; D.vp_maxz = 1;
    reset_states();
    bind_rt();
    glViewport(0, 0, (GLsizei)D.bb_w, (GLsizei)D.bb_h);
    glClearColor(0, 0, 0, 1); glClearDepthf(1.0f); glClearStencil(0);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
}

static void d3d_CreateDevice(void) {
    uint32_t pp = ARG(5), out = ARG(6);
    host_log("d3d: CreateDevice adapter=%u type=%u behavior=%x\n", ARG(1), ARG(2), ARG(4));
    user32_set_viewport(R32(pp + 24) ? R32(pp + 24) : ARG(3), (int)R32(pp), (int)R32(pp + 4));
    if (!D.g) {
        D.type = T_DEV; D.ref = 1;
        make_obj(&D, vt_dev);
        glGenBuffers(1, &D.up_buf); glGenBuffers(1, &D.up_ibuf);
        glGenSamplers(8, D.samplers);
    }
    apply_pp(pp);
    D.ref = 1;
    W32(out, D.g);
    SRET(0, 7);
}

// ---------------------------------------------------------------- IDirect3DDevice8: misc
static void dev_AddRef(void) { SRET(++D.ref, 1); }
static void dev_Release(void) { SRET(D.ref > 0 ? --D.ref : 0, 1); }
static void dev_TestCooperativeLevel(void) { SRET(0, 1); }
static void dev_GetAvailableTextureMem(void) { SRET(512u << 20, 1); }
static void dev_GetDirect3D(void) { W32(ARG(1), g_d3d); SRET(0, 2); }
static void dev_GetDeviceCaps(void) { fill_caps(ARG(1)); SRET(0, 2); }
static void dev_GetDisplayMode(void) {
    uint32_t p = ARG(1);
    W32(p, D.bb_w); W32(p + 4, D.bb_h); W32(p + 8, 60); W32(p + 12, D.bb_fmt);
    SRET(0, 2);
}
static void dev_GetCreationParameters(void) { uint32_t p = ARG(1); W32(p, 0); W32(p + 4, 1); W32(p + 8, g_hwnd); W32(p + 12, 0x40); SRET(0, 2); }
static void dev_Reset(void) { host_log("d3d: Reset\n"); user32_set_viewport(0, (int)R32(ARG(1)), (int)R32(ARG(1) + 4)); apply_pp(ARG(1)); SRET(0, 2); }

// screenshots for testing: PORT_SHOT=frame,frame,... writes shot_NNNNN.bmp
static void save_bmp(const char *path, const uint8_t *rgba, int w, int h) {
    FILE *f = fopen(path, "wb");
    if (!f) return;
    uint32_t rs = (uint32_t)w * 3, pad = (4 - rs % 4) % 4, sz = (rs + pad) * h;
    uint8_t hdr[54] = { 'B', 'M' };
    *(uint32_t *)(hdr + 2) = 54 + sz; *(uint32_t *)(hdr + 10) = 54; *(uint32_t *)(hdr + 14) = 40;
    *(int32_t *)(hdr + 18) = w; *(int32_t *)(hdr + 22) = h; *(uint16_t *)(hdr + 26) = 1; *(uint16_t *)(hdr + 28) = 24;
    *(uint32_t *)(hdr + 34) = sz;
    fwrite(hdr, 1, 54, f);
    uint8_t *row = malloc(rs + pad);
    memset(row, 0, rs + pad);
    for (int y = h - 1; y >= 0; y--) {      // BMP rows go bottom-up; our row 0 is the top
        for (int x = 0; x < w; x++) { const uint8_t *p = rgba + ((size_t)y * w + x) * 4; row[x * 3] = p[2]; row[x * 3 + 1] = p[1]; row[x * 3 + 2] = p[0]; }
        fwrite(row, 1, rs + pad, f);
    }
    free(row);
    fclose(f);
}
static void maybe_screenshot(void) {
    static const char *spec; static int init;
    if (!init) { init = 1; spec = getenv("PORT_SHOT"); }
    if (!spec) return;
    char want[16]; snprintf(want, sizeof want, "%u", D.frames);
    const char *p = spec; size_t wl = strlen(want);
    while ((p = strstr(p, want))) {
        if ((p == spec || p[-1] == ',') && (p[wl] == ',' || !p[wl])) {
            uint8_t *px = read_surface(D.bb, 0, 0, (int)D.bb_w, (int)D.bb_h);
            char path[64]; snprintf(path, sizeof path, "shot_%05u.bmp", D.frames);
            save_bmp(path, px, (int)D.bb_w, (int)D.bb_h);
            free(px);
            host_log("screenshot %s\n", path);
            break;
        }
        p += wl;
    }
}
static void dev_Present(void) {
    D.frames++;
    { GLenum e = glGetError(); static int n; if (e && n++ < 20) host_log("d3d: GL error %x before present of frame %u\n", e, D.frames); }
    maybe_screenshot();
    present_to_window();
    SRET(0, 5);
}
static void dev_GetBackBuffer(void) { D.bb->ref++; W32(ARG(3), D.bb->g); SRET(0, 4); }
static void dev_GetRasterStatus(void) { W32(ARG(1), 0); W32(ARG(1) + 4, 0); SRET(0, 2); }
static void dev_SetGammaRamp(void) {
    uint32_t r = ARG(2);
    uint8_t tab[256 * 4];
    int ident = 1;
    for (int i = 0; i < 256; i++) {
        tab[i * 4] = (uint8_t)(R16(r + i * 2) >> 8); tab[i * 4 + 1] = (uint8_t)(R16(r + 512 + i * 2) >> 8);
        tab[i * 4 + 2] = (uint8_t)(R16(r + 1024 + i * 2) >> 8); tab[i * 4 + 3] = 255;
        if (tab[i * 4] != i || tab[i * 4 + 1] != i || tab[i * 4 + 2] != i) ident = 0;
    }
    glBindTexture(GL_TEXTURE_2D, gamma_tex);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 256, 1, GL_RGBA, GL_UNSIGNED_BYTE, tab);
    use_gamma = !ident;
    SRET(0, 3);
}
static void dev_GetGammaRamp(void) {
    for (int c = 0; c < 3; c++) for (int i = 0; i < 256; i++) W16(ARG(1) + c * 512 + i * 2, (uint32_t)i * 257);
    SRET(0, 2);
}

// ---------------------------------------------------------------- resource creation
static Surf *new_surf(int role, uint32_t fmt, uint32_t w, uint32_t h, Tex *t, int level, int face) {
    Surf *s = calloc(1, sizeof *s);
    s->type = T_SURF; s->ref = 1; s->role = role;
    s->fmt = fmt; s->w = w; s->h = h; s->pitch = fmt_pitch(fmt, w);
    s->tex = t; s->level = level; s->face = face;
    make_obj(s, vt_surf);
    return s;
}
static void dev_CreateTexture(void) {
    uint32_t w = ARG(1), h = ARG(2), levels = ARG(3), usage = ARG(4), fmt = ARG(5), pool = ARG(6), out = ARG(7);
    if (!fmt_texture_ok(fmt) && !(usage & 1)) { host_log("d3d: CreateTexture format %s unsupported\n", fmt_name(fmt)); SRET(0x8876086cu, 8); }
    uint32_t g = create_texture(0, w, h, levels, usage, fmt, pool);
    static int nlog;
    if (nlog++ < 20) host_log("d3d: CreateTexture %ux%u levels=%u usage=%x fmt=%s pool=%u\n", w, h, levels, usage, fmt_name(fmt), pool);
    W32(out, g);
    SRET(0, 8);
}
static void dev_CreateCubeTexture(void) {
    uint32_t e = ARG(1), levels = ARG(2), usage = ARG(3), fmt = ARG(4), pool = ARG(5), out = ARG(6);
    if (!fmt_texture_ok(fmt) && !(usage & 1)) SRET(0x8876086cu, 7);
    host_log("d3d: CreateCubeTexture %u levels=%u usage=%x fmt=%s\n", e, levels, usage, fmt_name(fmt));
    W32(out, create_texture(1, e, e, levels, usage, fmt, pool));
    SRET(0, 7);
}
static Buf *new_buf(int type, uint32_t len, uint32_t usage, uint32_t fmt, uint32_t pool) {
    Buf *b = calloc(1, sizeof *b);
    b->type = type; b->ref = 1; b->len = len; b->usage = usage; b->fmt = fmt; b->pool = pool;
    b->mem = heap_alloc(len ? len : 4);
    if (!b->mem) host_fatal("out of guest memory for a %u byte buffer", len);
    memset(G + b->mem, 0, len);
    glGenBuffers(1, &b->gl);
    make_obj(b, type == T_VB ? vt_vb : vt_ib);
    return b;
}
static void dev_CreateVertexBuffer(void) {
    Buf *b = new_buf(T_VB, ARG(1), ARG(2), ARG(3), ARG(4));
    W32(ARG(5), b->g);
    SRET(0, 6);
}
static void dev_CreateIndexBuffer(void) {
    Buf *b = new_buf(T_IB, ARG(1), ARG(2), ARG(3), ARG(4));
    W32(ARG(5), b->g);
    SRET(0, 6);
}
static void dev_CreateRenderTarget(void) {
    Surf *s = new_surf(4, ARG(3), ARG(1), ARG(2), NULL, 0, 0);
    create_color_surface(s);
    W32(ARG(6), s->g);
    SRET(0, 7);
}
static void dev_CreateDepthStencilSurface(void) {
    Surf *s = new_surf(3, ARG(3), ARG(1), ARG(2), NULL, 0, 0);
    create_depth_surface(s);
    W32(ARG(5), s->g);
    SRET(0, 6);
}
static void dev_CreateImageSurface(void) {
    Surf *s = new_surf(0, ARG(3), ARG(1), ARG(2), NULL, 0, 0);
    s->mem = heap_alloc(s->pitch * fmt_rows(s->fmt, s->h) + 16);
    memset(G + s->mem, 0, s->pitch * fmt_rows(s->fmt, s->h));
    W32(ARG(4), s->g);
    SRET(0, 5);
}

// CopyRects(src, rects, n, dst, points): system-memory image <-> texture level, or GPU surface -> image
static void dev_CopyRects(void) {
    Surf *src = get_obj(ARG(1), T_SURF), *dst = get_obj(ARG(4), T_SURF);
    if (!src || !dst) SRET(0x8876086cu, 6);
    uint32_t rects = ARG(2), n = ARG(3), pts = ARG(5);
    if (!rects || !n) n = 1;
    for (uint32_t i = 0; i < n; i++) {
        int x0 = 0, y0 = 0, x1 = (int)src->w, y1 = (int)src->h;
        if (rects) { x0 = (int32_t)R32(rects + i * 16); y0 = (int32_t)R32(rects + i * 16 + 4); x1 = (int32_t)R32(rects + i * 16 + 8); y1 = (int32_t)R32(rects + i * 16 + 12); }
        int dx = x0, dy = y0;
        if (pts) { dx = (int32_t)R32(pts + i * 8); dy = (int32_t)R32(pts + i * 8 + 4); }
        int w = x1 - x0, h = y1 - y0;
        if (w <= 0 || h <= 0) continue;
        if (src->mem && dst->tex) {
            uint32_t bpp = fmt_bpp(src->fmt);
            tex_upload_rect(dst, src->mem + (uint32_t)y0 * src->pitch + (uint32_t)x0 * bpp, src->pitch, dx, dy, w, h);
        } else if (dst->mem) {
            uint8_t *px = read_surface(src, x0, y0, w, h);
            uint32_t bpp = fmt_bpp(dst->fmt);
            from_rgba(dst->fmt, px, (uint32_t)w, (uint32_t)h, G + dst->mem + (uint32_t)dy * dst->pitch + (uint32_t)dx * bpp, dst->pitch);
            free(px);
        } else {
            // GPU to GPU (render target to texture level)
            GLuint keep = cur_fbo, tmp;
            glGenFramebuffers(1, &tmp);
            bind_fbo(src->fbo ? src->fbo : tmp);
            if (!src->fbo) surf_color_attach(src);
            Tex *t = dst->tex;
            if (t) {
                glBindTexture(t->target, t->gl);
                glCopyTexSubImage2D(t->target == GL_TEXTURE_CUBE_MAP ? GL_TEXTURE_CUBE_MAP_POSITIVE_X + dst->face : GL_TEXTURE_2D,
                                    dst->level, dx, dy, x0, y0, w, h);
            } else LOGONCE("d3d: CopyRects between these surfaces is not supported\n");
            bind_fbo(keep);
            glDeleteFramebuffers(1, &tmp);
        }
    }
    SRET(0, 6);
}
static void dev_UpdateTexture(void) { LOGONCE("d3d: UpdateTexture ignored\n"); SRET(0, 3); }
static void dev_GetFrontBuffer(void) {
    Surf *dst = get_obj(ARG(1), T_SURF);
    if (dst && dst->mem) {
        uint8_t *px = read_surface(D.bb, 0, 0, (int)D.bb_w, (int)D.bb_h);
        from_rgba(FMT_A8R8G8B8, px, D.bb_w, D.bb_h, G + dst->mem, dst->pitch);
        free(px);
    }
    SRET(0, 2);
}
static void dev_SetRenderTarget(void) {
    Surf *rt = ARG(1) ? get_obj(ARG(1), T_SURF) : NULL;
    if (rt) D.cur_rt = rt;
    D.cur_ds = ARG(2) ? get_obj(ARG(2), T_SURF) : NULL;
    uint32_t w, h; rt_size(&w, &h);
    D.vp[0] = 0; D.vp[1] = 0; D.vp[2] = w; D.vp[3] = h;
    SRET(0, 3);
}
static void dev_GetRenderTarget(void) { Surf *s = D.cur_rt ? D.cur_rt : D.bb; s->ref++; W32(ARG(1), s->g); SRET(0, 2); }
static void dev_GetDepthStencilSurface(void) {
    if (!D.cur_ds) { W32(ARG(1), 0); SRET(0x8876086cu, 2); }
    D.cur_ds->ref++; W32(ARG(1), D.cur_ds->g); SRET(0, 2);
}
static void dev_BeginScene(void) { D.in_scene = 1; SRET(0, 1); }
static void dev_EndScene(void) { D.in_scene = 0; SRET(0, 1); }

// ---------------------------------------------------------------- GL state cache
static struct { int init; uint32_t blend, sf, df, bop, zt, zw, zf, cull, cmask, stencil, sfn, sref, smask, swmask, sfail, szfail, spass, scissor; int32_t bias; } gs;
static GLenum gl_blend(uint32_t b, int src) {
    switch (b) {
    case 1: return GL_ZERO; case 2: return GL_ONE; case 3: return GL_SRC_COLOR; case 4: return GL_ONE_MINUS_SRC_COLOR;
    case 5: return GL_SRC_ALPHA; case 6: return GL_ONE_MINUS_SRC_ALPHA; case 7: return GL_DST_ALPHA; case 8: return GL_ONE_MINUS_DST_ALPHA;
    case 9: return GL_DST_COLOR; case 10: return GL_ONE_MINUS_DST_COLOR; case 11: return GL_SRC_ALPHA_SATURATE;
    case 12: return src ? GL_SRC_ALPHA : GL_ONE_MINUS_SRC_ALPHA; case 13: return src ? GL_ONE_MINUS_SRC_ALPHA : GL_SRC_ALPHA;
    }
    return GL_ONE;
}
static GLenum gl_cmp(uint32_t c) {
    switch (c) { case 1: return GL_NEVER; case 2: return GL_LESS; case 3: return GL_EQUAL; case 4: return GL_LEQUAL;
    case 5: return GL_GREATER; case 6: return GL_NOTEQUAL; case 7: return GL_GEQUAL; }
    return GL_ALWAYS;
}
static GLenum gl_sop(uint32_t o) {
    switch (o) { case 2: return GL_ZERO; case 3: return GL_REPLACE; case 4: return GL_INCR; case 5: return GL_DECR;
    case 6: return GL_INVERT; case 7: return GL_INCR_WRAP; case 8: return GL_DECR_WRAP; }
    return GL_KEEP;
}
static void apply_raster_state(void) {
    uint32_t *rs = D.rs;
    if (!gs.init) { memset(&gs, 0xff, sizeof gs); gs.init = 1; }
    uint32_t blend = rs[27];
    if (blend != gs.blend) { gs.blend = blend; if (blend) glEnable(GL_BLEND); else glDisable(GL_BLEND); }
    if (blend) {
        uint32_t sf = rs[19], df = rs[20];
        if (sf == 12) df = 6; if (sf == 13) df = 5;       // BOTH(INV)SRCALPHA override the destination
        if (sf != gs.sf || df != gs.df) { gs.sf = sf; gs.df = df; glBlendFuncSeparate(gl_blend(sf, 1), gl_blend(df, 0), gl_blend(sf, 1), gl_blend(df, 0)); }
        uint32_t bop = rs[171];
        if (bop != gs.bop) {
            gs.bop = bop;
            glBlendEquation(bop == 2 ? GL_FUNC_SUBTRACT : bop == 3 ? GL_FUNC_REVERSE_SUBTRACT : bop == 4 ? GL_MIN : bop == 5 ? GL_MAX : GL_FUNC_ADD);
        }
    }
    uint32_t zt = (rs[7] && D.cur_ds) ? 1 : 0;
    if (zt != gs.zt) { gs.zt = zt; if (zt) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST); }
    uint32_t zw = rs[14] && D.cur_ds ? 1 : 0;
    if (zw != gs.zw) { gs.zw = zw; glDepthMask(zw ? GL_TRUE : GL_FALSE); }
    if (rs[23] != gs.zf) { gs.zf = rs[23]; glDepthFunc(gl_cmp(rs[23])); }
    uint32_t cull = rs[22];
    if (cull != gs.cull) {
        gs.cull = cull;
        if (cull == 1) glDisable(GL_CULL_FACE);
        else { glEnable(GL_CULL_FACE); glFrontFace(GL_CCW); glCullFace(cull == 3 ? GL_BACK : GL_FRONT); }
    }
    uint32_t cm = rs[168] & 15;
    if (cm != gs.cmask) { gs.cmask = cm; glColorMask(cm & 1, (cm >> 1) & 1, (cm >> 2) & 1, (cm >> 3) & 1); }
    int32_t bias = (int32_t)rs[47];
    if (bias != gs.bias) {
        gs.bias = bias;
        if (bias) { glEnable(GL_POLYGON_OFFSET_FILL); glPolygonOffset(-(float)bias * 0.25f, -(float)bias * 4.0f); }
        else glDisable(GL_POLYGON_OFFSET_FILL);
    }
    uint32_t st = rs[52] && D.cur_ds;
    if (st != gs.stencil) { gs.stencil = st; if (st) glEnable(GL_STENCIL_TEST); else glDisable(GL_STENCIL_TEST); }
    if (st) {
        if (rs[56] != gs.sfn || rs[57] != gs.sref || rs[58] != gs.smask) { gs.sfn = rs[56]; gs.sref = rs[57]; gs.smask = rs[58]; glStencilFunc(gl_cmp(rs[56]), (GLint)rs[57], rs[58]); }
        if (rs[53] != gs.sfail || rs[54] != gs.szfail || rs[55] != gs.spass) { gs.sfail = rs[53]; gs.szfail = rs[54]; gs.spass = rs[55]; glStencilOp(gl_sop(rs[53]), gl_sop(rs[54]), gl_sop(rs[55])); }
        if (rs[59] != gs.swmask) { gs.swmask = rs[59]; glStencilMask(rs[59]); }
    }
    // the viewport also clips (D3D never draws outside it)
    uint32_t w, h; rt_size(&w, &h);
    glViewport((GLint)D.vp[0], (GLint)D.vp[1], (GLsizei)D.vp[2], (GLsizei)D.vp[3]);
    glDepthRangef(D.vp_minz, D.vp_maxz);
    if (gs.scissor != 1) { gs.scissor = 1; glEnable(GL_SCISSOR_TEST); }
    glScissor((GLint)D.vp[0], (GLint)D.vp[1], (GLsizei)D.vp[2], (GLsizei)D.vp[3]);
}
static void invalidate_gl_state(void) { gs.init = 0; }

static void dev_Clear(void) {
    uint32_t count = ARG(1), rects = ARG(2), flags = ARG(3), color = ARG(4), stencil = ARG(6);
    float z = (float)F32(R.esp + 4 + 5 * 4);
    bind_rt();
    GLbitfield m = 0;
    if (flags & 1) m |= GL_COLOR_BUFFER_BIT;
    if ((flags & 2) && D.cur_ds) m |= GL_DEPTH_BUFFER_BIT;
    if ((flags & 4) && D.cur_ds) m |= GL_STENCIL_BUFFER_BIT;
    if (!m) SRET(0, 7);
    glClearColor(((color >> 16) & 255) / 255.0f, ((color >> 8) & 255) / 255.0f, (color & 255) / 255.0f, (color >> 24) / 255.0f);
    { extern int ff_debug_solid; if (ff_debug_solid) glClearColor(0, 0, 1, 1); }
    glClearDepthf(z); glClearStencil((GLint)stencil);
    glColorMask(1, 1, 1, 1); glDepthMask(GL_TRUE); glStencilMask(0xffffffffu);
    invalidate_gl_state();
    glEnable(GL_SCISSOR_TEST);
    if (!count || !rects) {
        glScissor((GLint)D.vp[0], (GLint)D.vp[1], (GLsizei)D.vp[2], (GLsizei)D.vp[3]);
        glClear(m);
    } else {
        for (uint32_t i = 0; i < count; i++) {
            int32_t x0 = (int32_t)R32(rects + i * 16), y0 = (int32_t)R32(rects + i * 16 + 4), x1 = (int32_t)R32(rects + i * 16 + 8), y1 = (int32_t)R32(rects + i * 16 + 12);
            glScissor(x0, y0, x1 - x0, y1 - y0);
            glClear(m);
        }
    }
    SRET(0, 7);
}

// ---------------------------------------------------------------- state setters
static Mat *transform_slot(uint32_t t) {
    if (t == 2) return &D.view;
    if (t == 3) return &D.proj;
    if (t >= 16 && t < 24) return &D.texm[t - 16];
    if (t == 256) return &D.world;
    return NULL;
}
static void dev_SetTransform(void) {
    Mat *m = transform_slot(ARG(1));
    if (m) memcpy(m->m, G + ARG(2), 64);
    SRET(0, 3);
}
static void dev_GetTransform(void) {
    Mat *m = transform_slot(ARG(1));
    Mat id = ident_mat();
    memcpy(G + ARG(2), m ? m->m : id.m, 64);
    SRET(0, 3);
}
static void dev_MultiplyTransform(void) {
    Mat *m = transform_slot(ARG(1));
    if (m) { Mat b; memcpy(b.m, G + ARG(2), 64); *m = mat_mul(m, &b); }
    SRET(0, 3);
}
static void dev_SetViewport(void) {
    uint32_t p = ARG(1);
    D.vp[0] = R32(p); D.vp[1] = R32(p + 4); D.vp[2] = R32(p + 8); D.vp[3] = R32(p + 12);
    D.vp_minz = (float)F32(p + 16); D.vp_maxz = (float)F32(p + 20);
    SRET(0, 2);
}
static void dev_GetViewport(void) {
    uint32_t p = ARG(1);
    W32(p, D.vp[0]); W32(p + 4, D.vp[1]); W32(p + 8, D.vp[2]); W32(p + 12, D.vp[3]);
    WF32(p + 16, D.vp_minz); WF32(p + 20, D.vp_maxz);
    SRET(0, 2);
}
static void dev_SetMaterial(void) { memcpy(D.mat, G + ARG(1), 68); SRET(0, 2); }
static void dev_GetMaterial(void) { memcpy(G + ARG(1), D.mat, 68); SRET(0, 2); }
static void dev_SetLight(void) {
    uint32_t i = ARG(1), p = ARG(2);
    if (i >= MAXLIGHTS) SRET(0x8876086cu, 3);
    Light *l = &D.lights[i];
    int en = l->enabled;
    l->type = R32(p);
    memcpy(l->d, G + p + 4, 16); memcpy(l->s, G + p + 20, 16); memcpy(l->a, G + p + 36, 16);
    memcpy(l->pos, G + p + 52, 12); memcpy(l->dir, G + p + 64, 12);
    l->range = (float)F32(p + 76); l->falloff = (float)F32(p + 80);
    l->att[0] = (float)F32(p + 84); l->att[1] = (float)F32(p + 88); l->att[2] = (float)F32(p + 92);
    l->theta = (float)F32(p + 96); l->phi = (float)F32(p + 100);
    l->enabled = en;
    SRET(0, 3);
}
static void dev_GetLight(void) {
    uint32_t i = ARG(1), p = ARG(2);
    if (i >= MAXLIGHTS) SRET(0x8876086cu, 3);
    Light *l = &D.lights[i];
    W32(p, l->type);
    memcpy(G + p + 4, l->d, 16); memcpy(G + p + 20, l->s, 16); memcpy(G + p + 36, l->a, 16);
    memcpy(G + p + 52, l->pos, 12); memcpy(G + p + 64, l->dir, 12);
    WF32(p + 76, l->range); WF32(p + 80, l->falloff); WF32(p + 84, l->att[0]); WF32(p + 88, l->att[1]); WF32(p + 92, l->att[2]);
    WF32(p + 96, l->theta); WF32(p + 100, l->phi);
    SRET(0, 3);
}
static void dev_LightEnable(void) {
    uint32_t i = ARG(1);
    if (i < MAXLIGHTS) { D.lights[i].enabled = ARG(2) != 0; if (!D.lights[i].type) { D.lights[i].type = 3; D.lights[i].d[0] = D.lights[i].d[1] = D.lights[i].d[2] = 1; D.lights[i].dir[2] = 1; } }
    SRET(0, 3);
}
static void dev_GetLightEnable(void) { uint32_t i = ARG(1); W32(ARG(2), i < MAXLIGHTS ? (uint32_t)D.lights[i].enabled : 0); SRET(0, 3); }
static void dev_SetClipPlane(void) { if (ARG(1) < 6) memcpy(D.clip[ARG(1)], G + ARG(2), 16); SRET(0, 3); }
static void dev_GetClipPlane(void) { if (ARG(1) < 6) memcpy(G + ARG(2), D.clip[ARG(1)], 16); SRET(0, 3); }
static void dev_SetRenderState(void) {
    uint32_t s = ARG(1), v = ARG(2);
    if (s < 256) D.rs[s] = v;
    SRET(0, 3);
}
static void dev_GetRenderState(void) { W32(ARG(2), ARG(1) < 256 ? D.rs[ARG(1)] : 0); SRET(0, 3); }
static void dev_GetTexture(void) {
    uint32_t st = ARG(1);
    Tex *t = st < 8 ? D.tex[st] : NULL;
    if (t) t->ref++;
    W32(ARG(2), t ? t->g : 0);
    SRET(0, 3);
}
static void dev_SetTexture(void) {
    uint32_t st = ARG(1);
    if (st < 8) D.tex[st] = ARG(2) ? get_obj(ARG(2), 0) : NULL;
    if (D.tex[st] && D.tex[st]->type != T_TEX && D.tex[st]->type != T_CUBE) D.tex[st] = NULL;
    SRET(0, 3);
}
static void dev_GetTextureStageState(void) { uint32_t s = ARG(1), t = ARG(2); W32(ARG(3), s < 8 && t < 32 ? D.tss[s][t] : 0); SRET(0, 4); }
static void dev_SetTextureStageState(void) { uint32_t s = ARG(1), t = ARG(2); if (s < 8 && t < 32) D.tss[s][t] = ARG(3); SRET(0, 4); }
static void dev_ValidateDevice(void) { if (ARG(1)) W32(ARG(1), 1); SRET(0, 2); }
static void dev_GetInfo(void) { SRET(1, 4); }     // S_FALSE: no info
static void dev_GetCurrentTexturePalette(void) { W32(ARG(1), 0); SRET(0, 2); }

// state blocks: a full snapshot of the settable state
struct SBlock {
    uint32_t rs[256], tss[8][32]; Tex *tex[8]; Mat world, view, proj, texm[8];
    uint32_t vp[4]; float vp_minz, vp_maxz, mat[17], clip[6][4];
    uint32_t shader; Buf *vb[16]; uint32_t stride[16]; Buf *ib; uint32_t base_vertex;
    Light lights[MAXLIGHTS];
};
static SBlock *sblocks[MAXSB];
static void sb_capture(SBlock *b) {
    memcpy(b->rs, D.rs, sizeof b->rs); memcpy(b->tss, D.tss, sizeof b->tss); memcpy(b->tex, D.tex, sizeof b->tex);
    b->world = D.world; b->view = D.view; b->proj = D.proj; memcpy(b->texm, D.texm, sizeof b->texm);
    memcpy(b->vp, D.vp, sizeof b->vp); b->vp_minz = D.vp_minz; b->vp_maxz = D.vp_maxz;
    memcpy(b->mat, D.mat, sizeof b->mat); memcpy(b->clip, D.clip, sizeof b->clip);
    b->shader = D.shader; memcpy(b->vb, D.vb, sizeof b->vb); memcpy(b->stride, D.stride, sizeof b->stride);
    b->ib = D.ib; b->base_vertex = D.base_vertex;
    memcpy(b->lights, D.lights, sizeof b->lights);
}
static void sb_apply(const SBlock *b) {
    memcpy(D.rs, b->rs, sizeof b->rs); memcpy(D.tss, b->tss, sizeof b->tss); memcpy(D.tex, b->tex, sizeof b->tex);
    D.world = b->world; D.view = b->view; D.proj = b->proj; memcpy(D.texm, b->texm, sizeof b->texm);
    memcpy(D.vp, b->vp, sizeof b->vp); D.vp_minz = b->vp_minz; D.vp_maxz = b->vp_maxz;
    memcpy(D.mat, b->mat, sizeof b->mat); memcpy(D.clip, b->clip, sizeof b->clip);
    D.shader = b->shader; memcpy(D.vb, b->vb, sizeof b->vb); memcpy(D.stride, b->stride, sizeof b->stride);
    D.ib = b->ib; D.base_vertex = b->base_vertex;
    memcpy(D.lights, b->lights, sizeof b->lights);
}
static uint32_t sb_new(void) {
    for (int i = 1; i < MAXSB; i++) if (!sblocks[i]) { sblocks[i] = calloc(1, sizeof(SBlock)); sb_capture(sblocks[i]); return (uint32_t)i; }
    host_fatal("too many state blocks");
}
static void dev_CreateStateBlock(void) { W32(ARG(2), sb_new()); SRET(0, 3); }
static void dev_BeginStateBlock(void) { SRET(0, 1); }
static void dev_EndStateBlock(void) { W32(ARG(1), sb_new()); SRET(0, 2); }
static void dev_ApplyStateBlock(void) { uint32_t t = ARG(1); if (t < MAXSB && sblocks[t]) sb_apply(sblocks[t]); SRET(0, 2); }
static void dev_CaptureStateBlock(void) { uint32_t t = ARG(1); if (t < MAXSB && sblocks[t]) sb_capture(sblocks[t]); SRET(0, 2); }
static void dev_DeleteStateBlock(void) { uint32_t t = ARG(1); if (t < MAXSB && sblocks[t]) { free(sblocks[t]); sblocks[t] = NULL; } SRET(0, 2); }

// ---------------------------------------------------------------- vertex shaders (declarations)
// handles are odd so they never look like FVF codes
static void dev_CreateVertexShader(void) {
    uint32_t decl = ARG(1), func = ARG(2), ph = ARG(3);
    if (func) LOGONCE("d3d: programmable vertex shader created (not supported, the declaration is used)\n");
    int slot = -1;
    for (int i = 1; i < MAXDECL; i++) if (!decls[i].used) { slot = i; break; }
    if (slot < 0) host_fatal("too many vertex shaders");
    VDecl *d = &decls[slot];
    memset(d, 0, sizeof *d); d->used = 1;
    static const uint8_t tsize[8] = { 4, 8, 12, 16, 4, 4, 4, 8 };
    int stream = 0, off = 0;
    for (uint32_t p = decl; p; p += 4) {
        uint32_t t = R32(p);
        if (t == 0xffffffffu) break;
        uint32_t tt = t >> 29;
        if (tt == 1) { stream = (int)(t & 0xf); off = 0; }
        else if (tt == 2) {
            if (t & 0x10000000u) off += (int)((t >> 16) & 0xf) * 4;     // SKIP
            else if (d->n < 24) {
                uint32_t type = (t >> 16) & 0xf, reg = t & 0x1f;
                d->el[d->n].stream = (uint8_t)stream; d->el[d->n].reg = (uint8_t)reg;
                d->el[d->n].type = (uint8_t)type; d->el[d->n].offset = (uint8_t)off;
                d->n++;
                off += type < 8 ? tsize[type] : 4;
            }
        } else if (tt == 4) { p += 4 * ((t >> 25) & 0xf) * 4; }          // constants: skip their data
        else if (tt == 3) { /* tessellator */ }
    }
    W32(ph, (uint32_t)slot * 2 + 1);
    SRET(0, 5);
}
static void dev_SetVertexShader(void) { D.shader = ARG(1); SRET(0, 2); }
static void dev_GetVertexShader(void) { W32(ARG(1), D.shader); SRET(0, 2); }
static void dev_DeleteVertexShader(void) { uint32_t h = ARG(1); if ((h & 1) && h / 2 < MAXDECL) decls[h / 2].used = 0; SRET(0, 2); }
static void dev_SetStreamSource(void) {
    uint32_t s = ARG(1);
    if (s < 16) { D.vb[s] = ARG(2) ? get_obj(ARG(2), T_VB) : NULL; D.stride[s] = ARG(3); }
    SRET(0, 4);
}
static void dev_GetStreamSource(void) {
    uint32_t s = ARG(1);
    Buf *b = s < 16 ? D.vb[s] : NULL;
    if (b) b->ref++;
    W32(ARG(2), b ? b->g : 0); W32(ARG(3), s < 16 ? D.stride[s] : 0);
    SRET(0, 4);
}
static void dev_SetIndices(void) { D.ib = ARG(1) ? get_obj(ARG(1), T_IB) : NULL; D.base_vertex = ARG(2); SRET(0, 3); }
static void dev_GetIndices(void) { if (D.ib) D.ib->ref++; W32(ARG(1), D.ib ? D.ib->g : 0); W32(ARG(2), D.base_vertex); SRET(0, 3); }
static void dev_SetPixelShader(void) { if (ARG(1)) LOGONCE("d3d: pixel shader %x set (unsupported)\n", ARG(1)); SRET(0, 2); }

// ---------------------------------------------------------------- drawing
// the vertex layout of the current shader: one element per attribute
typedef struct { int n; struct { int stream, reg, type, offset; } el[24]; int rhw; } Layout;
static void fvf_layout(uint32_t fvf, Layout *L) {
    L->n = 0; L->rhw = 0;
    int off = 0;
#define EL(r, t, sz) do { L->el[L->n].stream = 0; L->el[L->n].reg = (r); L->el[L->n].type = (t); L->el[L->n].offset = off; L->n++; off += (sz); } while (0)
    uint32_t pos = fvf & 0xe;
    if (pos == 4) { EL(0, 3, 16); L->rhw = 1; }
    else if (pos) {
        EL(0, 2, 12);
        int nb = pos == 2 ? 0 : (int)(pos - 4) / 2;   // XYZB1..B5: blend weights follow
        off += nb * 4;
    }
    if (fvf & 0x10) EL(3, 2, 12);
    if (fvf & 0x20) off += 4;                         // point size
    if (fvf & 0x40) EL(5, 4, 4);
    if (fvf & 0x80) EL(6, 4, 4);
    int nt = (int)((fvf >> 8) & 15);
    for (int i = 0; i < nt && i < 8; i++) {
        uint32_t f = (fvf >> (16 + 2 * i)) & 3;
        int comps = f == 0 ? 2 : f == 1 ? 3 : f == 2 ? 4 : 1;
        EL(7 + i, comps - 1, comps * 4);
    }
#undef EL
}
static void decl_layout(const VDecl *d, Layout *L) {
    L->n = 0; L->rhw = 0;
    for (int i = 0; i < d->n; i++) {
        L->el[L->n].stream = d->el[i].stream; L->el[L->n].reg = d->el[i].reg;
        L->el[L->n].type = d->el[i].type; L->el[L->n].offset = d->el[i].offset; L->n++;
    }
}
static int cur_layout(Layout *L) {
    if (D.shader & 1) {
        uint32_t i = D.shader / 2;
        if (i >= MAXDECL || !decls[i].used) return 0;
        decl_layout(&decls[i], L);
    } else fvf_layout(D.shader, L);
    return 1;
}
static int attr_of_reg(int reg) {
    switch (reg) {
    case 0: return A_POS; case 3: return A_NORMAL; case 5: return A_DIFFUSE; case 6: return A_SPECULAR;
    }
    if (reg >= 7 && reg <= 14) return A_TEX0 + (reg - 7);
    return -1;
}
static const struct { int comps; GLenum type; int norm; } vtypes[8] = {
    {1, GL_FLOAT, 0}, {2, GL_FLOAT, 0}, {3, GL_FLOAT, 0}, {4, GL_FLOAT, 0},
    {4, GL_UNSIGNED_BYTE, 1}, {4, GL_UNSIGNED_BYTE, 0}, {2, GL_SHORT, 0}, {4, GL_SHORT, 0}
};

static void build_key(const Layout *L, FFKey *k) {
    memset(k, 0, sizeof *k);
    uint32_t *rs = D.rs;
    k->rhw = (uint8_t)L->rhw;
    for (int i = 0; i < L->n; i++) {
        int r = L->el[i].reg;
        if (r == 3) k->has_normal = 1;
        else if (r == 5) k->has_diffuse = 1;
        else if (r == 6) k->has_specular = 1;
        else if (r >= 7 && r <= 14) k->tex_dim[r - 7] = (uint8_t)vtypes[L->el[i].type & 7].comps;
    }
    if (rs[137] && !k->rhw) {
        k->lighting = 1;
        k->localviewer = rs[142] != 0; k->normalize = rs[143] != 0; k->colorvertex = rs[141] != 0;
        int src[4] = { (int)rs[145], (int)rs[147], (int)rs[146], (int)rs[148] };    // diffuse, ambient, specular, emissive
        for (int i = 0; i < 4; i++) {
            if (src[i] == 1 && !k->has_diffuse) src[i] = 0;
            if (src[i] == 2 && !k->has_specular) src[i] = 0;
            if (src[i] > 2) src[i] = 0;
        }
        k->diffuse_src = (uint8_t)src[0]; k->ambient_src = (uint8_t)src[1]; k->specular_src = (uint8_t)src[2]; k->emissive_src = (uint8_t)src[3];
        for (int i = 0; i < MAXLIGHTS && k->nlights < FF_LIGHTS; i++)
            if (D.lights[i].enabled) k->light_type[k->nlights++] = (uint8_t)(D.lights[i].type ? D.lights[i].type : 3);
    }
    k->specular = rs[29] != 0;
    if (rs[28]) {
        if (rs[35]) { k->fog = 2; k->fog_mode = (uint8_t)rs[35]; }
        else if (rs[140] && !k->rhw) { k->fog = 1; k->fog_mode = (uint8_t)rs[140]; k->range_fog = rs[48] != 0; }
        else k->fog = 3;
    }
    if (rs[15]) k->alpha_func = (uint8_t)rs[25];
    if (!k->rhw) k->clip_planes = (uint8_t)(rs[152] & 7);
    int ns = 0;
    for (int s = 0; s < FF_STAGES; s++) {
        uint32_t *t = D.tss[s];
        if (t[1] == 1) break;
        FFStage *st = &k->st[s];
        st->cop = (uint8_t)t[1]; st->ca1 = (uint8_t)t[2]; st->ca2 = (uint8_t)t[3];
        st->aop = (uint8_t)t[4]; st->aa1 = (uint8_t)t[5]; st->aa2 = (uint8_t)t[6];
        st->ca0 = (uint8_t)t[26]; st->aa0 = (uint8_t)t[27];
        st->result_temp = t[28] == 5;
        st->tex = D.tex[s] ? (D.tex[s]->type == T_CUBE ? 2 : 1) : 0;
        st->tc_src = (uint8_t)(t[11] & 7);
        st->tc_gen = (uint8_t)((t[11] >> 16) & 3);
        if (k->rhw && st->tc_gen) st->tc_gen = 0;
        st->tc_count = (uint8_t)(t[24] & 7);
        st->tc_proj = (t[24] & 0x100) != 0;
        ns++;
    }
    k->nstages = (uint8_t)ns;
}

static void put_color(GLint loc, uint32_t c) {
    if (loc >= 0) glUniform4f(loc, ((c >> 16) & 255) / 255.0f, ((c >> 8) & 255) / 255.0f, (c & 255) / 255.0f, (c >> 24) / 255.0f);
}
static void set_uniforms(const FFKey *k, const FFProg *p) {
    uint32_t *rs = D.rs;
    uint32_t w, h; rt_size(&w, &h);
    if (k->rhw) {
        glUniform4f(p->rt, (float)D.vp[0], (float)D.vp[1], (float)D.vp[2], (float)D.vp[3]);
        glUniform4f(p->off, D.vp_minz, D.vp_maxz, 0, 0);
    } else {
        Mat wv = mat_mul(&D.world, &D.view);
        glUniformMatrix4fv(p->wv, 1, GL_FALSE, wv.m);
        glUniformMatrix4fv(p->proj, 1, GL_FALSE, D.proj.m);
        glUniform4f(p->rt, (float)D.vp[0], (float)D.vp[1], (float)D.vp[2], (float)D.vp[3]);
        if (k->has_normal) { Mat nm = normal_mat(&wv); glUniformMatrix4fv(p->nmat, 1, GL_FALSE, nm.m); }
        if (k->clip_planes) {
            glUniformMatrix4fv(p->world, 1, GL_FALSE, D.world.m);
            glUniform4fv(p->clip, 3, &D.clip[0][0]);
        }
    }
    if (k->lighting) {
        glUniform4fv(p->mat_d, 1, &D.mat[0]); glUniform4fv(p->mat_a, 1, &D.mat[4]);
        glUniform4fv(p->mat_s, 1, &D.mat[8]); glUniform4fv(p->mat_e, 1, &D.mat[12]);
        glUniform1f(p->mat_p, D.mat[16]);
        put_color(p->amb, rs[139]);
        float ld[FF_LIGHTS * 4], ls[FF_LIGHTS * 4], la[FF_LIGHTS * 4], lp[FF_LIGHTS * 4], ldir[FF_LIGHTS * 4], latt[FF_LIGHTS * 4], lsp[FF_LIGHTS * 4];
        int n = 0;
        for (int i = 0; i < MAXLIGHTS && n < k->nlights; i++) {
            Light *l = &D.lights[i];
            if (!l->enabled) continue;
            memcpy(&ld[n * 4], l->d, 16); memcpy(&ls[n * 4], l->s, 16); memcpy(&la[n * 4], l->a, 16);
            // to camera space with the current view matrix
            const float *v = D.view.m;
            float px = l->pos[0], py = l->pos[1], pz = l->pos[2];
            lp[n * 4 + 0] = px * v[0] + py * v[4] + pz * v[8] + v[12];
            lp[n * 4 + 1] = px * v[1] + py * v[5] + pz * v[9] + v[13];
            lp[n * 4 + 2] = px * v[2] + py * v[6] + pz * v[10] + v[14];
            lp[n * 4 + 3] = l->range > 0 ? l->range : 1e30f;
            float dx = l->dir[0], dy = l->dir[1], dz = l->dir[2];
            float ex = dx * v[0] + dy * v[4] + dz * v[8], ey = dx * v[1] + dy * v[5] + dz * v[9], ez = dx * v[2] + dy * v[6] + dz * v[10];
            float len = sqrtf(ex * ex + ey * ey + ez * ez); if (len < 1e-12f) len = 1;
            ldir[n * 4 + 0] = ex / len; ldir[n * 4 + 1] = ey / len; ldir[n * 4 + 2] = ez / len; ldir[n * 4 + 3] = l->falloff;
            latt[n * 4 + 0] = l->att[0]; latt[n * 4 + 1] = l->att[1]; latt[n * 4 + 2] = l->att[2]; latt[n * 4 + 3] = 0;
            lsp[n * 4 + 0] = cosf(l->theta * 0.5f); lsp[n * 4 + 1] = cosf(l->phi * 0.5f); lsp[n * 4 + 2] = lsp[n * 4 + 3] = 0;
            n++;
        }
        if (n) {
            glUniform4fv(p->l_d, n, ld); glUniform4fv(p->l_s, n, ls); glUniform4fv(p->l_a, n, la);
            glUniform4fv(p->l_pos, n, lp); glUniform4fv(p->l_dir, n, ldir); glUniform4fv(p->l_att, n, latt); glUniform4fv(p->l_spot, n, lsp);
        }
    }
    for (int s = 0; s < k->nstages; s++) if (k->st[s].tc_count && p->tm[s] >= 0) glUniformMatrix4fv(p->tm[s], 1, GL_FALSE, D.texm[s].m);
    put_color(p->tf, rs[60]);
    if (k->alpha_func) glUniform1f(p->aref, (float)(rs[24] & 255));
    if (k->fog) {
        put_color(p->fogc, rs[34]);
        float fs, fe, fd; memcpy(&fs, &rs[36], 4); memcpy(&fe, &rs[37], 4); memcpy(&fd, &rs[38], 4);
        glUniform4f(p->fogp, fs, fe, fe != fs ? 1.0f / (fe - fs) : 0.0f, fd);
    }
}

static GLenum gl_addr(uint32_t a) {
    switch (a) { case 2: case 5: return GL_MIRRORED_REPEAT; case 3: case 4: return GL_CLAMP_TO_EDGE; }
    return GL_REPEAT;
}
static void bind_textures(const FFKey *k) {
    static uint32_t samp_state[8][6];
    for (int s = 0; s < k->nstages; s++) {
        if (!k->st[s].tex) continue;
        Tex *t = D.tex[s];
        glActiveTexture(GL_TEXTURE0 + s);
        glBindTexture(t->target, t->gl);
        uint32_t *ts = D.tss[s];
        uint32_t mip = t->levels > 1 ? ts[18] : 0;
        uint32_t want[6] = { ts[13], ts[14], ts[25], ts[16], ts[17] | (mip << 8), ts[21] };
        if (memcmp(want, samp_state[s], sizeof want)) {
            memcpy(samp_state[s], want, sizeof want);
            GLuint sm = D.samplers[s];
            glSamplerParameteri(sm, GL_TEXTURE_WRAP_S, (GLint)gl_addr(ts[13]));
            glSamplerParameteri(sm, GL_TEXTURE_WRAP_T, (GLint)gl_addr(ts[14]));
            glSamplerParameteri(sm, GL_TEXTURE_WRAP_R, (GLint)gl_addr(ts[25]));
            glSamplerParameteri(sm, GL_TEXTURE_MAG_FILTER, ts[16] == 1 ? GL_NEAREST : GL_LINEAR);
            int lin = ts[17] != 1;
            GLint minf = !mip ? (lin ? GL_LINEAR : GL_NEAREST)
                       : mip == 1 ? (lin ? GL_LINEAR_MIPMAP_NEAREST : GL_NEAREST_MIPMAP_NEAREST)
                       : (lin ? GL_LINEAR_MIPMAP_LINEAR : GL_NEAREST_MIPMAP_LINEAR);
            glSamplerParameteri(sm, GL_TEXTURE_MIN_FILTER, minf);
            if (has_aniso) glSamplerParameterf(sm, GL_TEXTURE_MAX_ANISOTROPY_EXT, (ts[16] == 3 || ts[17] == 3) ? (float)(ts[21] ? ts[21] : 1) : 1.0f);
        }
        glBindSampler((GLuint)s, D.samplers[s]);
    }
}

// set up attributes for the current layout; base = first vertex (DrawPrimitive start or BaseVertexIndex)
static uint32_t buf_upload(Buf *b, GLenum target);
static int setup_attribs(const Layout *L, uint32_t base, const uint8_t *up_data, uint32_t up_stride) {

    int want[16] = {0};
    GLuint bound = 0xffffffffu;
    for (int i = 0; i < L->n; i++) {
        int a = attr_of_reg(L->el[i].reg);
        if (a < 0) continue;
        int st = L->el[i].stream;
        uint32_t stride; uintptr_t off;
        if (up_data) { stride = up_stride; off = (uintptr_t)L->el[i].offset; if (bound != D.up_buf) { glBindBuffer(GL_ARRAY_BUFFER, D.up_buf); bound = D.up_buf; } }
        else {
            Buf *b = D.vb[st];
            if (!b) continue;
            stride = D.stride[st];
            buf_upload(b, GL_ARRAY_BUFFER);
            if (bound != b->gl) { glBindBuffer(GL_ARRAY_BUFFER, b->gl); bound = b->gl; }
            off = (uintptr_t)base * stride + (uintptr_t)L->el[i].offset;
        }
        int t = L->el[i].type & 7;
        if (a == A_POS && L->rhw) glVertexAttribPointer((GLuint)a, 4, GL_FLOAT, GL_FALSE, (GLsizei)stride, (const void *)off);
        else glVertexAttribPointer((GLuint)a, vtypes[t].comps, vtypes[t].type, vtypes[t].norm ? GL_TRUE : GL_FALSE, (GLsizei)stride, (const void *)off);
        want[a] = 1;
    }
    for (int a = 0; a < 16; a++) {
        if (want[a] && !enabled[a]) { glEnableVertexAttribArray((GLuint)a); enabled[a] = 1; }
        else if (!want[a] && enabled[a]) { glDisableVertexAttribArray((GLuint)a); enabled[a] = 0; }
    }
    glVertexAttrib4f(A_NORMAL, 0, 0, 0, 0);
    return 1;
}

static GLenum gl_prim(uint32_t t, uint32_t n, uint32_t *count) {
    switch (t) {
    case 1: *count = n; return GL_POINTS;
    case 2: *count = n * 2; return GL_LINES;
    case 3: *count = n + 1; return GL_LINE_STRIP;
    case 4: *count = n * 3; return GL_TRIANGLES;
    case 5: *count = n + 2; return GL_TRIANGLE_STRIP;
    case 6: *count = n + 2; return GL_TRIANGLE_FAN;
    }
    *count = 0; return GL_TRIANGLES;
}

// PORT_D3DTRACE=<frame>: log every draw of that frame with its state
static int trace_frame = -2;
static void trace_draw(const char *what, const Layout *L, const FFKey *k, uint32_t count, uint32_t first_vtx, const uint8_t *up) {
    if (trace_frame == -2) trace_frame = getenv("PORT_D3DTRACE") ? atoi(getenv("PORT_D3DTRACE")) : -1;
    if ((int)D.frames != trace_frame) return;
    uint32_t *rs = D.rs;
    host_log("draw %s n=%u shader=%x rhw=%d L%d(%d lights) fog%d stages=%d rt=%s vp=%u,%u %ux%u z%d%d blend%d(%u,%u) atest%d cull%u\n",
             what, count, D.shader, k->rhw, k->lighting, k->nlights, k->fog, k->nstages, D.cur_rt == D.bb ? "bb" : "rt",
             D.vp[0], D.vp[1], D.vp[2], D.vp[3], rs[7], rs[14], rs[27], rs[19], rs[20], k->alpha_func, rs[22]);
    for (int s = 0; s < k->nstages; s++)
        host_log("   stage %d: cop %u (%x,%x) aop %u (%x,%x) tex %d %ux%u fmt %s tc %u gen %u xf %u\n", s, k->st[s].cop, k->st[s].ca1, k->st[s].ca2,
                 k->st[s].aop, k->st[s].aa1, k->st[s].aa2, k->st[s].tex, D.tex[s] ? D.tex[s]->w : 0, D.tex[s] ? D.tex[s]->h : 0,
                 D.tex[s] ? fmt_name(D.tex[s]->fmt) : "-", k->st[s].tc_src, k->st[s].tc_gen, k->st[s].tc_count);
    // the first vertex's position
    for (int i = 0; i < L->n; i++) if (L->el[i].reg == 0) {
        const uint8_t *p = NULL;
        if (up) p = up + L->el[i].offset;
        else if (D.vb[L->el[i].stream]) p = G + D.vb[L->el[i].stream]->mem + first_vtx * D.stride[L->el[i].stream] + L->el[i].offset;
        if (p) { float f[4]; memcpy(f, p, 16); host_log("   v0 = %g %g %g %g\n", f[0], f[1], f[2], f[3]); }
    }
    if (!k->rhw) {
        const float *w = D.world.m, *v = D.view.m, *pr = D.proj.m;
        host_log("   world t=%g %g %g view t=%g %g %g proj %g %g %g %g / %g %g\n", w[12], w[13], w[14], v[12], v[13], v[14], pr[0], pr[5], pr[10], pr[11], pr[14], pr[15]);
    }
}

static FFKey trace_key;
static int prepare_draw(Layout *L) {
    if (!cur_layout(L)) return 0;
    bind_rt();
    apply_raster_state();
    FFKey k;
    build_key(L, &k);
    trace_key = k;
    FFProg *p = get_prog(&k);
    if (p->prog != last_prog) { glUseProgram(p->prog); last_prog = p->prog; }
    set_uniforms(&k, p);
    bind_textures(&k);
    return 1;
}

static void dev_DrawPrimitive(void) {
    uint32_t type = ARG(1), start = ARG(2), n = ARG(3), count;
    GLenum mode = gl_prim(type, n, &count);
    Layout L;
    if (count && prepare_draw(&L)) {
        setup_attribs(&L, start, NULL, 0);
        trace_draw("DP", &L, &trace_key, count, start, NULL);
        glDrawArrays(mode, 0, (GLsizei)count);
    }
    SRET(0, 4);
}
static void dev_DrawIndexedPrimitive(void) {
    uint32_t type = ARG(1), startidx = ARG(4), n = ARG(5), count;
    GLenum mode = gl_prim(type, n, &count);
    Layout L;
    if (count && D.ib && prepare_draw(&L)) {
        setup_attribs(&L, D.base_vertex, NULL, 0);
        buf_upload(D.ib, GL_ELEMENT_ARRAY_BUFFER);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, D.ib->gl);
        int i32 = D.ib->fmt == FMT_INDEX32;
        trace_draw("DIP", &L, &trace_key, count, D.base_vertex, NULL);
        glDrawElements(mode, (GLsizei)count, i32 ? GL_UNSIGNED_INT : GL_UNSIGNED_SHORT, (const void *)(uintptr_t)(startidx * (i32 ? 4u : 2u)));
    }
    SRET(0, 6);
}
static void dev_DrawPrimitiveUP(void) {
    uint32_t type = ARG(1), n = ARG(2), data = ARG(3), stride = ARG(4), count;
    GLenum mode = gl_prim(type, n, &count);
    Layout L;
    if (count && prepare_draw(&L)) {
        glBindBuffer(GL_ARRAY_BUFFER, D.up_buf);
        glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)count * stride, G + data, GL_STREAM_DRAW);
        setup_attribs(&L, 0, G + data, stride);
        trace_draw("DPUP", &L, &trace_key, count, 0, G + data);
        glDrawArrays(mode, 0, (GLsizei)count);
    }
    SRET(0, 5);
}
static void dev_DrawIndexedPrimitiveUP(void) {
    uint32_t type = ARG(1), minidx = ARG(2), nverts = ARG(3), n = ARG(4), idx = ARG(5), ifmt = ARG(6), vdata = ARG(7), stride = ARG(8), count;
    GLenum mode = gl_prim(type, n, &count);
    Layout L;
    if (count && prepare_draw(&L)) {
        glBindBuffer(GL_ARRAY_BUFFER, D.up_buf);
        glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(minidx + nverts) * stride, G + vdata, GL_STREAM_DRAW);
        setup_attribs(&L, 0, G + vdata, stride);
        int i32 = ifmt == FMT_INDEX32;
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, D.up_ibuf);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)count * (i32 ? 4 : 2), G + idx, GL_STREAM_DRAW);
        glDrawElements(mode, (GLsizei)count, i32 ? GL_UNSIGNED_INT : GL_UNSIGNED_SHORT, 0);
    }
    SRET(0, 9);
}

// ---------------------------------------------------------------- buffers
// the guest copy is the truth; GL gets the bytes that changed since the last draw that used the buffer
static uint32_t buf_upload(Buf *b, GLenum target) {
    if (b->lock_len == 0xffffffffu) return b->gl;     // nothing pending
    glBindBuffer(target, b->gl);
    if (b->gl_len != b->len || (b->lock_flags & 0x2000) || (b->lock_off == 0 && b->lock_len >= b->len)) {
        glBufferData(target, (GLsizeiptr)b->len, G + b->mem, (b->usage & 0x200) ? GL_STREAM_DRAW : GL_STATIC_DRAW);   // DYNAMIC
        b->gl_len = b->len;
    } else {
        glBufferSubData(target, (GLintptr)b->lock_off, (GLsizeiptr)b->lock_len, G + b->mem + b->lock_off);
    }
    b->lock_len = 0xffffffffu; b->lock_off = 0; b->lock_flags = 0;
    return b->gl;
}
static void buf_Lock(void) {
    Buf *b = get_obj(ARG(0), 0);
    uint32_t off = ARG(1), size = ARG(2), pp = ARG(3), flags = ARG(4);
    if (!b) SRET(0x8876086cu, 5);
    if (!size || off + size > b->len) size = b->len - (off < b->len ? off : b->len);
    // pending range: union with what is already waiting for upload
    if (b->lock_len == 0xffffffffu || !b->gl_len) { b->lock_off = off; b->lock_len = size; b->lock_flags = flags; }
    else {
        uint32_t lo = b->lock_off < off ? b->lock_off : off;
        uint32_t hi = b->lock_off + b->lock_len > off + size ? b->lock_off + b->lock_len : off + size;
        b->lock_off = lo; b->lock_len = hi - lo; b->lock_flags |= flags;
    }
    W32(pp, b->mem + off);
    SRET(0, 5);
}
static void buf_Unlock(void) { SRET(0, 1); }
static void buf_GetDesc(void) {
    Buf *b = get_obj(ARG(0), 0);
    uint32_t p = ARG(1);
    if (b) { W32(p, b->type == T_VB ? 0 : b->fmt); W32(p + 4, b->type == T_VB ? 6 : 7); W32(p + 8, b->usage); W32(p + 12, b->pool); W32(p + 16, b->len); if (b->type == T_VB) W32(p + 20, b->fmt); }
    SRET(0, 2);
}
static void buf_AddRef(void) { Buf *b = get_obj(ARG(0), 0); SRET(b ? ++b->ref : 0, 1); }
static void buf_Release(void) {
    uint32_t g = ARG(0);
    Buf *b = get_obj(g, 0);
    if (!b) SRET(0, 1);
    if (--b->ref > 0) SRET(b->ref, 1);
    for (int i = 0; i < 16; i++) if (D.vb[i] == b) D.vb[i] = NULL;
    if (D.ib == b) D.ib = NULL;
    glDeleteBuffers(1, &b->gl);
    heap_free(b->mem);
    free_obj(g);
    free(b);
    SRET(0, 1);
}
static void res_GetDevice(void) { W32(ARG(1), D.g); SRET(0, 2); }
static void res_GetType(void) { void *o = get_obj(ARG(0), 0); int t = o ? *(int *)o : 0; SRET(t == T_TEX ? 3 : t == T_CUBE ? 5 : t == T_VB ? 6 : t == T_IB ? 7 : 1, 1); }
static void res_GetPrivateData(void) { SRET(0x88760870u, 4); }

// ---------------------------------------------------------------- textures
static void tex_AddRef(void) { Tex *t = get_obj(ARG(0), 0); SRET(t ? ++t->ref : 0, 1); }
static void tex_Release(void) {
    uint32_t g = ARG(0);
    Tex *t = get_obj(g, 0);
    if (!t) SRET(0, 1);
    if (--t->ref > 0) SRET(t->ref, 1);
    for (int i = 0; i < 8; i++) if (D.tex[i] == t) D.tex[i] = NULL;
    for (int f = 0; f < t->nfaces; f++)
        for (uint32_t l = 0; l < t->levels; l++) {
            Surf *s = t->lv[f][l];
            if (!s) continue;
            if (D.cur_rt == s) D.cur_rt = D.bb;
            if (s->fbo) { if (cur_fbo == s->fbo) bind_fbo(0); glDeleteFramebuffers(1, &s->fbo); }
            if (s->lockbuf) heap_free(s->lockbuf);
            free_obj(s->g); free(s);
        }
    glDeleteTextures(1, &t->gl);
    free_obj(g);
    free(t);
    SRET(0, 1);
}
static void tex_GetLevelCount(void) { Tex *t = get_obj(ARG(0), 0); SRET(t ? t->levels : 0, 1); }
static void tex_SetLOD(void) { SRET(0, 2); }
static void tex_GetLOD(void) { SRET(0, 1); }
static void put_desc(uint32_t p, Surf *s) {
    // D3DSURFACE_DESC: Format, Type, Usage, Pool, Size, MultiSampleType, Width, Height
    uint32_t usage = s->tex ? s->tex->usage : (s->role == 4 || s->role == 2) ? 1 : s->role == 3 ? 2 : 0;
    W32(p, s->fmt); W32(p + 4, 1); W32(p + 8, usage); W32(p + 12, s->tex ? s->tex->pool : 0);
    W32(p + 16, s->pitch * fmt_rows(s->fmt, s->h)); W32(p + 20, 0); W32(p + 24, s->w); W32(p + 28, s->h);
}
static Surf *tex_level(uint32_t g, int face, uint32_t level) {
    Tex *t = get_obj(g, 0);
    if (!t || level >= t->levels || face >= t->nfaces) return NULL;
    return t->lv[face][level];
}
static void tex_GetLevelDesc(void) {
    Surf *s = tex_level(ARG(0), 0, ARG(1));
    if (!s) SRET(0x8876086cu, 3);
    put_desc(ARG(2), s);
    SRET(0, 3);
}
static void tex_GetSurfaceLevel(void) {
    Surf *s = tex_level(ARG(0), 0, ARG(1));
    if (!s) SRET(0x8876086cu, 3);
    s->tex->ref++;                     // the level's references belong to its texture
    W32(ARG(2), s->g);
    SRET(0, 3);
}
static void cube_GetCubeMapSurface(void) {
    Surf *s = tex_level(ARG(0), (int)ARG(1), ARG(2));
    if (!s) SRET(0x8876086cu, 4);
    s->tex->ref++;
    W32(ARG(3), s->g);
    SRET(0, 4);
}
// lock a surface: a texture level or render target gets a temporary buffer; images their own memory
static void lock_surface(Surf *s, uint32_t plr, uint32_t prect, uint32_t flags) {
    int x = 0, y = 0, w = (int)s->w, h = (int)s->h;
    if (prect) { x = (int32_t)R32(prect); y = (int32_t)R32(prect + 4); w = (int32_t)R32(prect + 8) - x; h = (int32_t)R32(prect + 12) - y; }
    s->lx = x; s->ly = y; s->lw = w; s->lh = h; s->lflags = (int)flags; s->locked = 1;
    if (s->mem) {
        W32(plr, s->pitch); W32(plr + 4, s->mem + (uint32_t)y * s->pitch + (uint32_t)x * fmt_bpp(s->fmt));
        return;
    }
    uint32_t pitch = fmt_pitch(s->fmt, (uint32_t)w), rows = fmt_rows(s->fmt, (uint32_t)h);
    if (is_dxt(s->fmt) && x == 0 && y == 0 && (uint32_t)w >= s->w) pitch = fmt_pitch(s->fmt, s->w);
    if (s->lockbuf) heap_free(s->lockbuf);
    s->lockbuf = heap_alloc(pitch * rows + 16);
    s->lockpitch = pitch;
    if (!s->tex || (s->tex->usage & 1)) {
        // a colour target: the caller may read it. A 1x1 lock is UE2's GPU sync (ReduceMouseLag): skip the read.
        if (w * h > 1 && !(flags & 0x2000)) {
            uint8_t *px = read_surface(s, x, y, w, h);
            from_rgba(s->fmt, px, (uint32_t)w, (uint32_t)h, G + s->lockbuf, pitch);
            free(px);
        }
    }
    W32(plr, pitch); W32(plr + 4, s->lockbuf);
}
static void unlock_surface(Surf *s) {
    if (!s->locked) return;
    s->locked = 0;
    if (s->mem) return;
    if (s->tex && !(s->lflags & 0x10))        // not D3DLOCK_READONLY
        tex_upload_rect(s, s->lockbuf, s->lockpitch, s->lx, s->ly, s->lw, s->lh);
    heap_free(s->lockbuf);
    s->lockbuf = 0;
}
static void tex_LockRect(void) {
    Surf *s = tex_level(ARG(0), 0, ARG(1));
    if (!s) SRET(0x8876086cu, 5);
    lock_surface(s, ARG(2), ARG(3), ARG(4));
    SRET(0, 5);
}
static void tex_UnlockRect(void) {
    Surf *s = tex_level(ARG(0), 0, ARG(1));
    if (s) unlock_surface(s);
    SRET(0, 2);
}
static void cube_GetLevelDesc(void) { Surf *s = tex_level(ARG(0), 0, ARG(1)); if (!s) SRET(0x8876086cu, 3); put_desc(ARG(2), s); SRET(0, 3); }
static void cube_LockRect(void) {
    Surf *s = tex_level(ARG(0), (int)ARG(1), ARG(2));
    if (!s) SRET(0x8876086cu, 6);
    lock_surface(s, ARG(3), ARG(4), ARG(5));
    SRET(0, 6);
}
static void cube_UnlockRect(void) {
    Surf *s = tex_level(ARG(0), (int)ARG(1), ARG(2));
    if (s) unlock_surface(s);
    SRET(0, 3);
}

// ---------------------------------------------------------------- surfaces
static void surf_AddRef(void) {
    Surf *s = get_obj(ARG(0), T_SURF);
    if (!s) SRET(0, 1);
    if (s->tex) SRET(++s->tex->ref, 1);
    SRET(++s->ref, 1);
}
static void surf_Release(void) {
    uint32_t g = ARG(0);
    Surf *s = get_obj(g, T_SURF);
    if (!s) SRET(0, 1);
    if (s->tex) {
        // forward to the texture: GetSurfaceLevel took a texture reference
        W32(R.esp + 4, s->tex->g);
        tex_Release();
        return;
    }
    if (--s->ref > 0) SRET(s->ref, 1);
    if (s == D.bb || s == D.depth) SRET(0, 1);
    if (D.cur_rt == s) D.cur_rt = D.bb;
    if (D.cur_ds == s) D.cur_ds = NULL;
    if (s->fbo) { if (cur_fbo == s->fbo) bind_fbo(0); glDeleteFramebuffers(1, &s->fbo); }
    if (s->gltex) glDeleteTextures(1, &s->gltex);
    if (s->rb) glDeleteRenderbuffers(1, &s->rb);
    if (s->mem) heap_free(s->mem);
    if (s->lockbuf) heap_free(s->lockbuf);
    free_obj(g);
    free(s);
    SRET(0, 1);
}
static void surf_GetDesc(void) {
    Surf *s = get_obj(ARG(0), T_SURF);
    if (s) put_desc(ARG(1), s);
    SRET(0, 2);
}
static void surf_LockRect(void) {
    Surf *s = get_obj(ARG(0), T_SURF);
    if (!s) SRET(0x8876086cu, 4);
    lock_surface(s, ARG(1), ARG(2), ARG(3));
    SRET(0, 4);
}
static void surf_UnlockRect(void) {
    Surf *s = get_obj(ARG(0), T_SURF);
    if (s) unlock_surface(s);
    SRET(0, 1);
}
static void surf_GetContainer(void) {
    Surf *s = get_obj(ARG(0), T_SURF);
    if (!s || !s->tex) { W32(ARG(2), 0); SRET(0x80004002u, 3); }
    s->tex->ref++;
    W32(ARG(2), s->tex->g);
    SRET(0, 3);
}

// ---------------------------------------------------------------- vtables
UNIMPL(dev, 10, 4) UNIMPL(dev, 13, 3) UNIMPL(dev, 21, 9) UNIMPL(dev, 58, 2) UNIMPL(dev, 59, 2)
UNIMPL(dev, 74, 6) UNIMPL(dev, 79, 4) UNIMPL(dev, 80, 4) UNIMPL(dev, 81, 4) UNIMPL(dev, 82, 4) UNIMPL(dev, 87, 3)
UNIMPL(dev, 89, 2) UNIMPL(dev, 90, 2) UNIMPL(dev, 91, 4) UNIMPL(dev, 92, 4) UNIMPL(dev, 93, 4) UNIMPL(dev, 94, 4)
UNIMPL(dev, 95, 4) UNIMPL(dev, 96, 2) UNIMPL(d3d, 3, 2)

void shim_Direct3DCreate8(void) {
    host_log("Direct3DCreate8(%u)\n", ARG(0));
    if (!g_d3d) {
        GFunc m3d[16] = { com_QueryInterface, d3d_AddRef, d3d_Release, d3d_m3, d3d_GetAdapterCount, d3d_GetAdapterIdentifier,
                          d3d_GetAdapterModeCount, d3d_EnumAdapterModes, d3d_GetAdapterDisplayMode, d3d_CheckDeviceType,
                          d3d_CheckDeviceFormat, d3d_CheckDeviceMultiSampleType, d3d_CheckDepthStencilMatch, d3d_GetDeviceCaps,
                          d3d_GetAdapterMonitor, d3d_CreateDevice };
        vt_d3d = com_vtable(16, m3d, "IDirect3D8");
        GFunc md[97] = {
            com_QueryInterface, dev_AddRef, dev_Release, dev_TestCooperativeLevel, dev_GetAvailableTextureMem, ok2,
            dev_GetDirect3D, dev_GetDeviceCaps, dev_GetDisplayMode, dev_GetCreationParameters, dev_m10, ok4, ok2, dev_m13,
            dev_Reset, dev_Present, dev_GetBackBuffer, dev_GetRasterStatus, dev_SetGammaRamp, dev_GetGammaRamp,
            dev_CreateTexture, dev_m21, dev_CreateCubeTexture, dev_CreateVertexBuffer, dev_CreateIndexBuffer,
            dev_CreateRenderTarget, dev_CreateDepthStencilSurface, dev_CreateImageSurface, dev_CopyRects, dev_UpdateTexture,
            dev_GetFrontBuffer, dev_SetRenderTarget, dev_GetRenderTarget, dev_GetDepthStencilSurface, dev_BeginScene,
            dev_EndScene, dev_Clear, dev_SetTransform, dev_GetTransform, dev_MultiplyTransform, dev_SetViewport,
            dev_GetViewport, dev_SetMaterial, dev_GetMaterial, dev_SetLight, dev_GetLight, dev_LightEnable, dev_GetLightEnable,
            dev_SetClipPlane, dev_GetClipPlane, dev_SetRenderState, dev_GetRenderState, dev_BeginStateBlock, dev_EndStateBlock,
            dev_ApplyStateBlock, dev_CaptureStateBlock, dev_DeleteStateBlock, dev_CreateStateBlock, dev_m58, dev_m59,
            dev_GetTexture, dev_SetTexture, dev_GetTextureStageState, dev_SetTextureStageState, dev_ValidateDevice, dev_GetInfo,
            ok3, ok3, ok2, dev_GetCurrentTexturePalette, dev_DrawPrimitive, dev_DrawIndexedPrimitive, dev_DrawPrimitiveUP,
            dev_DrawIndexedPrimitiveUP, dev_m74, dev_CreateVertexShader, dev_SetVertexShader, dev_GetVertexShader,
            dev_DeleteVertexShader, dev_m79, dev_m80, dev_m81, dev_m82, dev_SetStreamSource, dev_GetStreamSource, dev_SetIndices,
            dev_GetIndices, dev_m87, dev_SetPixelShader, dev_m89, dev_m90, dev_m91, dev_m92, dev_m93, dev_m94, dev_m95, dev_m96 };
        vt_dev = com_vtable(97, md, "IDirect3DDevice8");
        GFunc mt[19] = { com_QueryInterface, tex_AddRef, tex_Release, res_GetDevice, ok5, res_GetPrivateData, ok2, ok2, ok1, ok1,
                         res_GetType, tex_SetLOD, tex_GetLOD, tex_GetLevelCount, tex_GetLevelDesc, tex_GetSurfaceLevel,
                         tex_LockRect, tex_UnlockRect, ok2 };
        vt_tex = com_vtable(19, mt, "IDirect3DTexture8");
        GFunc mc[19] = { com_QueryInterface, tex_AddRef, tex_Release, res_GetDevice, ok5, res_GetPrivateData, ok2, ok2, ok1, ok1,
                         res_GetType, tex_SetLOD, tex_GetLOD, tex_GetLevelCount, cube_GetLevelDesc, cube_GetCubeMapSurface,
                         cube_LockRect, cube_UnlockRect, ok3 };
        vt_cube = com_vtable(19, mc, "IDirect3DCubeTexture8");
        GFunc ms[11] = { com_QueryInterface, surf_AddRef, surf_Release, res_GetDevice, ok5, res_GetPrivateData, ok2,
                         surf_GetContainer, surf_GetDesc, surf_LockRect, surf_UnlockRect };
        vt_surf = com_vtable(11, ms, "IDirect3DSurface8");
        GFunc mb[14] = { com_QueryInterface, buf_AddRef, buf_Release, res_GetDevice, ok5, res_GetPrivateData, ok2, ok2, ok1, ok1,
                         res_GetType, buf_Lock, buf_Unlock, buf_GetDesc };
        vt_vb = com_vtable(14, mb, "IDirect3DVertexBuffer8");
        vt_ib = com_vtable(14, mb, "IDirect3DIndexBuffer8");
        static struct { int type, ref; uint32_t g; } d3dobj = { T_D3D, 1, 0 };
        g_d3d = make_obj(&d3dobj, vt_d3d);
    }
    SRET(g_d3d, 1);
}

void gfx_shutdown(void) {}
