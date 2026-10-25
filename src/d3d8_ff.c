// Direct3D 8 fixed-function vertex and pixel processing as GLSL ES 3.00, generated per pipeline-state key.
//
// Conventions (d3d8.c sets the uniforms to match):
// - D3D matrices are uploaded as they are in memory (row-major, row vectors); GLSL then reads the transpose, so
//   "M * v" computes D3D's "v * M". u_wv = World * View, u_nmat = its inverse transpose (normals).
// - Everything renders into framebuffers whose first row is the top of the image (D3D's layout): clip-space y is
//   negated, which also turns D3D's clockwise front faces counter-clockwise, as GL expects. Pixel centers move by
//   half a pixel (D3D samples at integer coordinates). Present flips once for the window.
// - Lighting happens in camera space with D3D's equations; lights arrive already transformed by the view matrix.
// - The texture stages follow D3DTSS_*: CURRENT starts as the diffuse color, each stage clamps its result.
#include "d3d8_ff.h"
#include <stdio.h>
#include <string.h>

#define APP(...) do { int _n = snprintf(o + n, (size_t)(cap - n), __VA_ARGS__); if (_n > 0) n += _n; if (n > cap) n = cap; } while (0)

static const char *arg(int a, int stage, char *buf) {
    const char *e;
    switch (a & 15) {
    case 0: e = "dif"; break;                       // D3DTA_DIFFUSE
    case 1: e = stage == 0 ? "dif" : "cur"; break;  // D3DTA_CURRENT
    case 2: e = "tx"; break;                        // D3DTA_TEXTURE
    case 3: e = "u_tf"; break;                      // D3DTA_TFACTOR
    case 4: e = "spc"; break;                       // D3DTA_SPECULAR
    case 5: e = "tmp"; break;                       // D3DTA_TEMP
    default: e = "vec4(0.0)"; break;
    }
    char t[64];
    if (a & 0x20) snprintf(t, sizeof t, "vec4(%s.a)", e); else snprintf(t, sizeof t, "%s", e);   // ALPHAREPLICATE
    if (a & 0x10) snprintf(buf, 96, "(vec4(1.0) - %s)", t); else snprintf(buf, 96, "%s", t);    // COMPLEMENT
    return buf;
}

// D3DTOP_* as an expression of the stage arguments
static void op(char *o, int top, const char *a0, const char *a1, const char *a2) {
    switch (top) {
    case 2: sprintf(o, "%s", a1); break;                                        // SELECTARG1
    case 3: sprintf(o, "%s", a2); break;                                        // SELECTARG2
    case 4: sprintf(o, "%s * %s", a1, a2); break;                               // MODULATE
    case 5: sprintf(o, "%s * %s * 2.0", a1, a2); break;                         // MODULATE2X
    case 6: sprintf(o, "%s * %s * 4.0", a1, a2); break;                         // MODULATE4X
    case 7: sprintf(o, "%s + %s", a1, a2); break;                               // ADD
    case 8: sprintf(o, "%s + %s - vec4(0.5)", a1, a2); break;                   // ADDSIGNED
    case 9: sprintf(o, "(%s + %s - vec4(0.5)) * 2.0", a1, a2); break;           // ADDSIGNED2X
    case 10: sprintf(o, "%s - %s", a1, a2); break;                              // SUBTRACT
    case 11: sprintf(o, "%s + %s - %s * %s", a1, a2, a1, a2); break;            // ADDSMOOTH
    case 12: sprintf(o, "mix(%s, %s, dif.a)", a2, a1); break;                   // BLENDDIFFUSEALPHA
    case 13: sprintf(o, "mix(%s, %s, tx.a)", a2, a1); break;                    // BLENDTEXTUREALPHA
    case 14: sprintf(o, "mix(%s, %s, u_tf.a)", a2, a1); break;                  // BLENDFACTORALPHA
    case 15: sprintf(o, "%s + %s * (1.0 - tx.a)", a1, a2); break;               // BLENDTEXTUREALPHAPM
    case 16: sprintf(o, "mix(%s, %s, cur.a)", a2, a1); break;                   // BLENDCURRENTALPHA
    case 17: sprintf(o, "%s", a1); break;                                       // PREMODULATE (approximation)
    case 18: sprintf(o, "%s + vec4(%s.a) * %s", a1, a1, a2); break;             // MODULATEALPHA_ADDCOLOR
    case 19: sprintf(o, "%s * %s + vec4(%s.a)", a1, a2, a1); break;             // MODULATECOLOR_ADDALPHA
    case 20: sprintf(o, "vec4(1.0 - %s.a) * %s + %s", a1, a2, a1); break;      // MODULATEINVALPHA_ADDCOLOR
    case 21: sprintf(o, "(vec4(1.0) - %s) * %s + vec4(%s.a)", a1, a2, a1); break; // MODULATEINVCOLOR_ADDALPHA
    case 22: case 23: sprintf(o, "%s", a2); break;                              // BUMPENVMAP(LUMINANCE): no bump
    case 24: sprintf(o, "vec4(dot((%s).rgb * 2.0 - 1.0, (%s).rgb * 2.0 - 1.0))", a1, a2); break; // DOTPRODUCT3
    case 25: sprintf(o, "%s + %s * %s", a0, a1, a2); break;                     // MULTIPLYADD
    case 26: sprintf(o, "mix(%s, %s, %s)", a2, a1, a0); break;                  // LERP
    default: sprintf(o, "%s", a1); break;
    }
}

static int uses_tex(const FFStage *s) {
    if ((s->ca1 & 15) == 2 || (s->ca2 & 15) == 2 || (s->ca0 & 15) == 2) return 1;
    if (s->aop != 1 && ((s->aa1 & 15) == 2 || (s->aa2 & 15) == 2 || (s->aa0 & 15) == 2)) return 1;
    if (s->cop >= 12 && s->cop <= 15 && s->cop != 14 && s->cop != 12) return 1;   // texture alpha blends
    return 0;
}

int ff_debug_solid;     // PORT_D3DSOLID: every pixel red (is anything rasterised?)
void ff_make_sources(const FFKey *k, char *vso, int vsz, char *fso, int fsz) {
    char *o; int n, cap;
    // ------------------------------------------------------------ vertex shader
    o = vso; n = 0; cap = vsz;
    APP("in vec4 a_pos;\nin vec3 a_normal;\nin vec4 a_diffuse;\nin vec4 a_specular;\n");
    for (int i = 0; i < 8; i++) if (k->tex_dim[i]) APP("in vec4 a_tex%d;\n", i);
    APP("uniform mat4 u_wv, u_proj, u_nmat, u_world;\nuniform vec4 u_rt;\nuniform vec4 u_off;\n");
    APP("uniform mat4 u_tm[%d];\n", FF_STAGES);
    APP("uniform vec4 u_mat_d, u_mat_a, u_mat_s, u_mat_e, u_amb;\nuniform float u_mat_p;\n");
    APP("uniform vec4 u_l_d[%d], u_l_s[%d], u_l_a[%d], u_l_pos[%d], u_l_dir[%d], u_l_att[%d], u_l_spot[%d];\n",
        FF_LIGHTS, FF_LIGHTS, FF_LIGHTS, FF_LIGHTS, FF_LIGHTS, FF_LIGHTS, FF_LIGHTS);
    APP("uniform vec4 u_fogp;\nuniform vec4 u_clip[3];\n");
    APP("out vec4 v_col;\nout vec4 v_spc;\nout float v_fog;\nout vec3 v_clipd;\n");
    for (int s = 0; s < k->nstages; s++) APP("out vec4 v_tc%d;\n", s);
    APP("void main() {\n");
    APP("  vec4 vdif = %s;\n  vec4 vspc = %s;\n", k->has_diffuse ? "a_diffuse.zyxw" : "vec4(1.0)", k->has_specular ? "a_specular.zyxw" : "vec4(0.0)");
    if (k->rhw) {
        // screen-space vertices: u_rt = viewport x, y, w, h; u_off = min z, max z
        APP("  float w = a_pos.w != 0.0 ? 1.0 / a_pos.w : 1.0;\n");
        APP("  vec2 ndc = (a_pos.xy + 0.5 - u_rt.xy) / u_rt.zw * 2.0 - 1.0;\n");
        APP("  float z = (a_pos.z - u_off.x) / max(u_off.y - u_off.x, 1e-6) * 2.0 - 1.0;\n");
        APP("  gl_Position = vec4(ndc * w, z * w, w);\n");
        APP("  vec3 pe = vec3(0.0, 0.0, w);\n  vec3 ne = vec3(0.0, 0.0, -1.0);\n");
        APP("  v_clipd = vec3(1.0);\n");
    } else {
        APP("  vec4 pe4 = u_wv * vec4(a_pos.xyz, 1.0);\n  vec3 pe = pe4.xyz;\n");
        APP("  vec4 p = u_proj * pe4;\n");
        APP("  p.y = -p.y;\n  p.xy += p.w / u_rt.zw * vec2(1.0, 1.0);\n  p.z = p.z * 2.0 - p.w;\n  gl_Position = p;\n");
        if (k->has_normal) {
            APP("  vec3 ne = mat3(u_nmat) * a_normal;\n");
            if (k->normalize || k->lighting) APP("  ne = normalize(ne);\n");
        } else APP("  vec3 ne = vec3(0.0, 0.0, 0.0);\n");
        if (k->clip_planes) {
            APP("  vec4 pw = u_world * vec4(a_pos.xyz, 1.0);\n");
            APP("  v_clipd = vec3(dot(u_clip[0], pw), dot(u_clip[1], pw), dot(u_clip[2], pw));\n");
        } else APP("  v_clipd = vec3(1.0);\n");
    }
    if (k->lighting && !k->rhw) {
        const char *src[3] = { "u_mat_%c", "vdif", "vspc" };
        char md[32], ma[32], ms[32], me[32];
        snprintf(md, sizeof md, k->colorvertex && k->diffuse_src ? src[k->diffuse_src] : "u_mat_d");
        snprintf(ma, sizeof ma, k->colorvertex && k->ambient_src ? src[k->ambient_src] : "u_mat_a");
        snprintf(ms, sizeof ms, k->colorvertex && k->specular_src ? src[k->specular_src] : "u_mat_s");
        snprintf(me, sizeof me, k->colorvertex && k->emissive_src ? src[k->emissive_src] : "u_mat_e");
        APP("  vec3 la = vec3(0.0), ld = vec3(0.0), ls = vec3(0.0);\n");
        APP("  vec3 V = %s;\n", k->localviewer ? "normalize(-pe)" : "vec3(0.0, 0.0, -1.0)");
        for (int i = 0; i < k->nlights; i++) {
            APP("  {\n");
            if (k->light_type[i] == 3) {
                APP("    vec3 L = -u_l_dir[%d].xyz; float at = 1.0;\n", i);
            } else {
                APP("    vec3 dv = u_l_pos[%d].xyz - pe; float d = length(dv); vec3 L = dv / max(d, 1e-6);\n", i);
                APP("    float at = d > u_l_pos[%d].w ? 0.0 : 1.0 / max(u_l_att[%d].x + u_l_att[%d].y * d + u_l_att[%d].z * d * d, 1e-6);\n", i, i, i, i);
                if (k->light_type[i] == 2) {
                    APP("    float rho = dot(-L, u_l_dir[%d].xyz);\n", i);
                    APP("    float sp = rho > u_l_spot[%d].x ? 1.0 : (rho <= u_l_spot[%d].y ? 0.0 : pow(max((rho - u_l_spot[%d].y) / max(u_l_spot[%d].x - u_l_spot[%d].y, 1e-6), 0.0), u_l_dir[%d].w));\n", i, i, i, i, i, i);
                    APP("    at *= sp;\n");
                }
            }
            APP("    la += u_l_a[%d].rgb * at;\n", i);
            APP("    float nl = max(dot(ne, L), 0.0);\n    ld += u_l_d[%d].rgb * nl * at;\n", i);
            if (k->specular) {
                APP("    vec3 H = normalize(L + V);\n");
                APP("    if (nl > 0.0) ls += u_l_s[%d].rgb * pow(max(dot(ne, H), 0.0), u_mat_p) * at;\n", i);
            }
            APP("  }\n");
        }
        APP("  v_col = vec4(clamp(%s.rgb + %s.rgb * (u_amb.rgb + la) + %s.rgb * ld, 0.0, 1.0), %s.a);\n", me, ma, md, md);
        APP("  v_spc = vec4(clamp(%s.rgb * ls, 0.0, 1.0), vspc.a);\n", k->specular ? ms : "vec4(0.0)");
    } else {
        APP("  v_col = vdif;\n  v_spc = vspc;\n");
    }
    // fog factor: 1 = no fog
    if (k->fog == 1) {
        APP("  float fd = %s;\n", k->range_fog ? "length(pe)" : "abs(pe.z)");
        if (k->fog_mode == 3) APP("  v_fog = clamp((u_fogp.y - fd) * u_fogp.z, 0.0, 1.0);\n");
        else if (k->fog_mode == 1) APP("  v_fog = clamp(exp(-u_fogp.w * fd), 0.0, 1.0);\n");
        else APP("  v_fog = clamp(exp(-(u_fogp.w * fd) * (u_fogp.w * fd)), 0.0, 1.0);\n");
    } else if (k->fog == 2) {
        APP("  v_fog = %s;\n", k->rhw ? "w" : "abs(pe.z)");      // depth for the per-pixel formula
    } else if (k->fog == 3) {
        APP("  v_fog = vspc.a;\n");
    } else APP("  v_fog = 1.0;\n");
    // texture coordinates
    for (int s = 0; s < k->nstages; s++) {
        const FFStage *st = &k->st[s];
        int src = st->tc_src & 7, dim = k->tex_dim[src];
        switch (st->tc_gen) {
        case 1: APP("  vec4 tc%d = vec4(ne, 1.0);\n", s); dim = 3; break;
        case 2: APP("  vec4 tc%d = vec4(pe, 1.0);\n", s); dim = 3; break;
        case 3: APP("  vec4 tc%d = vec4(reflect(normalize(pe), ne), 1.0);\n", s); dim = 3; break;
        default:
            if (!dim) { APP("  vec4 tc%d = vec4(0.0, 0.0, 0.0, 1.0);\n", s); dim = 2; }
            else if (dim == 1) APP("  vec4 tc%d = vec4(a_tex%d.x, 1.0, 0.0, 0.0);\n", s, src);
            else if (dim == 2) APP("  vec4 tc%d = vec4(a_tex%d.xy, 1.0, 0.0);\n", s, src);
            else if (dim == 3) APP("  vec4 tc%d = vec4(a_tex%d.xyz, 1.0);\n", s, src);
            else APP("  vec4 tc%d = a_tex%d;\n", s, src);
        }
        if (st->tc_count) APP("  v_tc%d = u_tm[%d] * tc%d;\n", s, s, s);
        else APP("  v_tc%d = tc%d;\n", s, s);
    }
    APP("}\n");

    // ------------------------------------------------------------ fragment shader
    o = fso; n = 0; cap = fsz;
    APP("in vec4 v_col;\nin vec4 v_spc;\nin float v_fog;\nin vec3 v_clipd;\n");
    for (int s = 0; s < k->nstages; s++) APP("in vec4 v_tc%d;\n", s);
    for (int s = 0; s < k->nstages; s++)
        if (k->st[s].tex) APP("uniform %s u_s%d;\n", k->st[s].tex == 2 ? "samplerCube" : "sampler2D", s);
    APP("uniform vec4 u_tf;\nuniform float u_aref;\nuniform vec4 u_fogc;\nuniform vec4 u_fogp;\n");
    APP("out vec4 o_color;\n");
    APP("void main() {\n");
    for (int i = 0; i < 3; i++) if (k->clip_planes & (1 << i)) APP("  if (v_clipd[%d] < 0.0) discard;\n", i);
    APP("  vec4 dif = v_col, spc = v_spc, cur = v_col, tmp = vec4(0.0), tx = vec4(1.0);\n");
    for (int s = 0; s < k->nstages; s++) {
        const FFStage *st = &k->st[s];
        if (st->cop == 1) break;
        if (st->tex) {
            char coord[96];
            int cnt = st->tc_count ? st->tc_count : (st->tc_gen ? 3 : 2);
            if (st->tex == 2) snprintf(coord, sizeof coord, "v_tc%d.xyz", s);
            else if (st->tc_proj && cnt >= 2) snprintf(coord, sizeof coord, "v_tc%d.xy / v_tc%d[%d]", s, s, cnt - 1);
            else snprintf(coord, sizeof coord, "v_tc%d.xy", s);
            APP("  tx = texture(u_s%d, %s);\n", s, coord);
        } else if (uses_tex(st)) APP("  tx = vec4(1.0);\n");
        char b0[96], b1[96], b2[96], c[512], a[512];
        op(c, st->cop, arg(st->ca0, s, b0), arg(st->ca1, s, b1), arg(st->ca2, s, b2));
        const char *dst = st->result_temp ? "tmp" : "cur";
        if (st->cop == 24) {
            APP("  %s = clamp(%s, 0.0, 1.0);\n", dst, c);   // DOTPRODUCT3 goes to all four channels
        } else {
            if (st->aop == 1) snprintf(a, sizeof a, "%s", s == 0 ? "dif" : "cur");
            else op(a, st->aop, arg(st->aa0, s, b0), arg(st->aa1, s, b1), arg(st->aa2, s, b2));
            APP("  %s = clamp(vec4((%s).rgb, (%s).a), 0.0, 1.0);\n", dst, c, a);
        }
    }
    if (k->specular) APP("  cur.rgb = clamp(cur.rgb + spc.rgb, 0.0, 1.0);\n");
    if (k->alpha_func && k->alpha_func != 8) {
        static const char *cmp[] = { "", "false", "<", "==", "<=", ">", "!=", ">=", "true" };
        APP("  float av = floor(cur.a * 255.0 + 0.5);\n");
        if (k->alpha_func == 1) APP("  discard;\n");
        else APP("  if (!(av %s u_aref)) discard;\n", cmp[k->alpha_func]);
    }
    if (k->fog == 2) {
        if (k->fog_mode == 3) APP("  float f = clamp((u_fogp.y - v_fog) * u_fogp.z, 0.0, 1.0);\n");
        else if (k->fog_mode == 1) APP("  float f = clamp(exp(-u_fogp.w * v_fog), 0.0, 1.0);\n");
        else APP("  float f = clamp(exp(-(u_fogp.w * v_fog) * (u_fogp.w * v_fog)), 0.0, 1.0);\n");
        APP("  cur.rgb = mix(u_fogc.rgb, cur.rgb, f);\n");
    } else if (k->fog) {
        APP("  cur.rgb = mix(u_fogc.rgb, cur.rgb, clamp(v_fog, 0.0, 1.0));\n");
    }
    if (ff_debug_solid) APP("  o_color = vec4(1.0, 0.0, 0.0, 1.0);\n}\n");
    else APP("  o_color = cur;\n}\n");
}
