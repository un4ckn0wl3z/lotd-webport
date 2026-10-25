// Direct3D 8 fixed-function pipeline as generated GLSL (d3d8_ff.c), keyed by the state that changes the code.
#pragma once
#include <stdint.h>

#define FF_STAGES 8
#define FF_LIGHTS 8

// vertex attribute locations
enum { A_POS = 0, A_NORMAL, A_DIFFUSE, A_SPECULAR, A_TEX0 /* .. A_TEX0 + 7 */ };

typedef struct {
    uint8_t cop, ca0, ca1, ca2, aop, aa0, aa1, aa2;
    uint8_t tex;            // 0 none bound, 1 2D, 2 cube
    uint8_t result_temp;    // D3DTSS_RESULTARG == D3DTA_TEMP
    uint8_t tc_src, tc_gen; // texture coordinate set and generation mode (0 pass-through, 1 normal, 2 position, 3 reflection)
    uint8_t tc_count;       // D3DTSS_TEXTURETRANSFORMFLAGS count (0 = no transform)
    uint8_t tc_proj;        // ... | D3DTTFF_PROJECTED
    uint8_t pad[2];
} FFStage;

typedef struct {
    uint8_t rhw;                        // pre-transformed vertices (XYZRHW)
    uint8_t has_normal, has_diffuse, has_specular;
    uint8_t tex_dim[8];                 // components of each texture coordinate input, 0 = not present
    uint8_t lighting, nlights, localviewer, normalize, specular, colorvertex;
    uint8_t diffuse_src, ambient_src, specular_src, emissive_src;
    uint8_t light_type[FF_LIGHTS];      // 1 point, 2 spot, 3 directional
    uint8_t fog;                        // 0 off, 1 vertex fog, 2 pixel (table) fog, 3 fog factor from specular alpha
    uint8_t fog_mode;                   // 1 EXP, 2 EXP2, 3 LINEAR
    uint8_t range_fog;
    uint8_t alpha_func;                 // D3DCMP_*, 0 = alpha test off
    uint8_t clip_planes;                // mask of enabled user clip planes (0..3)
    uint8_t nstages;
    uint8_t pad[2];
    FFStage st[FF_STAGES];
} FFKey;

// uniform locations of one generated program
typedef struct {
    uint32_t prog;
    int wv, proj, nmat, world, rt, off, tm[FF_STAGES], samp[FF_STAGES];
    int mat_d, mat_a, mat_s, mat_e, mat_p, amb;
    int l_d, l_s, l_a, l_pos, l_dir, l_att, l_spot;
    int tf, aref, fogc, fogp, clip;
} FFProg;

void ff_make_sources(const FFKey *k, char *vs, int vsz, char *fs, int fsz);
