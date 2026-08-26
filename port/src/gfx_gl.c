/*
 * OpenGL 3.3 backend. Entry points are resolved through SDL rather than linked
 * against libGL, for the same reason the VR layer does it: Windows only
 * exports GL 1.1, so anything newer has to come from wglGetProcAddress anyway.
 */
#include "gfx_gl.h"
#include "gfx_rdp.h"
#include "gfx_texture.h"
#include "platform.h"

#include <SDL2/SDL.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <PR/ultratypes.h>
#include <PR/mbi.h>
#include <PR/gbi.h>

typedef unsigned  GLenum;
typedef unsigned  GLuint;
typedef int       GLint;
typedef int       GLsizei;
typedef char      GLchar;
typedef float     GLfloat;
typedef unsigned  GLbitfield;
typedef unsigned char GLboolean;
typedef ptrdiff_t GLsizeiptr;

#define GL_FALSE                  0
#define GL_TRUE                   1
#define GL_TRIANGLES              0x0004
#define GL_DEPTH_BUFFER_BIT       0x00000100
#define GL_COLOR_BUFFER_BIT       0x00004000
#define GL_DEPTH_TEST             0x0B71
#define GL_BLEND                  0x0BE2
#define GL_CULL_FACE              0x0B44
#define GL_SRC_ALPHA              0x0302
#define GL_ONE_MINUS_SRC_ALPHA    0x0303
#define GL_TEXTURE_2D             0x0DE1
#define GL_TEXTURE0               0x84C0
#define GL_FLOAT                  0x1406
#define GL_UNSIGNED_BYTE          0x1401
#define GL_RGBA                   0x1908
#define GL_RGBA8                  0x8058
#define GL_ARRAY_BUFFER           0x8892
#define GL_DYNAMIC_DRAW           0x88E8
#define GL_VERTEX_SHADER          0x8B31
#define GL_FRAGMENT_SHADER        0x8B30
#define GL_COMPILE_STATUS         0x8B81
#define GL_LINK_STATUS            0x8B82
#define GL_TEXTURE_MIN_FILTER     0x2801
#define GL_TEXTURE_MAG_FILTER     0x2800
#define GL_TEXTURE_WRAP_S         0x2802
#define GL_TEXTURE_WRAP_T         0x2803
#define GL_LINEAR                 0x2601
#define GL_NEAREST                0x2600
#define GL_REPEAT                 0x2901
#define GL_CLAMP_TO_EDGE          0x812F
#define GL_MIRRORED_REPEAT        0x8370
#define GL_SCISSOR_TEST           0x0C11
#define GL_UNPACK_ALIGNMENT       0x0CF5
#define GL_ZERO                   0
#define GL_ONE                    1
#define GL_DST_ALPHA              0x0304
#define GL_LEQUAL                 0x0203
#define GL_POLYGON_OFFSET_FILL    0x8037

#define GL_FUNCS(X)                                                           \
    X(void,   glViewport,        (GLint, GLint, GLsizei, GLsizei))            \
    X(void,   glScissor,         (GLint, GLint, GLsizei, GLsizei))            \
    X(void,   glClearColor,      (GLfloat, GLfloat, GLfloat, GLfloat))        \
    X(void,   glClear,           (GLbitfield))                                \
    X(void,   glEnable,          (GLenum))                                    \
    X(void,   glDisable,         (GLenum))                                    \
    X(void,   glBlendFunc,       (GLenum, GLenum))                            \
    X(void,   glDepthMask,       (GLboolean))                              \
    X(void,   glDepthFunc,       (GLenum))                                 \
    X(void,   glPolygonOffset,   (GLfloat, GLfloat))                       \
    X(void,   glDrawArrays,      (GLenum, GLint, GLsizei))                    \
    X(void,   glActiveTexture,   (GLenum))                                    \
    X(void,   glBindTexture,     (GLenum, GLuint))                            \
    X(void,   glGenTextures,     (GLsizei, GLuint *))                         \
    X(void,   glDeleteTextures,  (GLsizei, const GLuint *))                   \
    X(void,   glTexImage2D,      (GLenum, GLint, GLint, GLsizei, GLsizei,     \
                                  GLint, GLenum, GLenum, const void *))       \
    X(void,   glTexParameteri,   (GLenum, GLenum, GLint))                     \
    X(void,   glPixelStorei,     (GLenum, GLint))                             \
    X(GLuint, glCreateShader,    (GLenum))                                    \
    X(void,   glShaderSource,    (GLuint, GLsizei, const GLchar *const *,     \
                                  const GLint *))                             \
    X(void,   glCompileShader,   (GLuint))                                    \
    X(void,   glGetShaderiv,     (GLuint, GLenum, GLint *))                   \
    X(void,   glGetShaderInfoLog,(GLuint, GLsizei, GLsizei *, GLchar *))      \
    X(void,   glDeleteShader,    (GLuint))                                    \
    X(GLuint, glCreateProgram,   (void))                                      \
    X(void,   glAttachShader,    (GLuint, GLuint))                            \
    X(void,   glLinkProgram,     (GLuint))                                    \
    X(void,   glGetProgramiv,    (GLuint, GLenum, GLint *))                   \
    X(void,   glGetProgramInfoLog,(GLuint, GLsizei, GLsizei *, GLchar *))     \
    X(void,   glDeleteProgram,   (GLuint))                                    \
    X(void,   glUseProgram,      (GLuint))                                    \
    X(GLint,  glGetUniformLocation,(GLuint, const GLchar *))                  \
    X(void,   glUniform1i,       (GLint, GLint))                              \
    X(void,   glUniform1f,       (GLint, GLfloat))                         \
    X(void,   glUniform4i,       (GLint, GLint, GLint, GLint, GLint))      \
    X(void,   glUniform2f,       (GLint, GLfloat, GLfloat))                   \
    X(void,   glUniform4f,       (GLint, GLfloat, GLfloat, GLfloat, GLfloat)) \
    X(void,   glGenVertexArrays, (GLsizei, GLuint *))                         \
    X(void,   glDeleteVertexArrays,(GLsizei, const GLuint *))                 \
    X(void,   glBindVertexArray, (GLuint))                                    \
    X(void,   glGenBuffers,      (GLsizei, GLuint *))                         \
    X(void,   glDeleteBuffers,   (GLsizei, const GLuint *))                   \
    X(void,   glBindBuffer,      (GLenum, GLuint))                            \
    X(void,   glBufferData,      (GLenum, GLsizeiptr, const void *, GLenum))  \
    X(void,   glVertexAttribPointer,(GLuint, GLint, GLenum, GLboolean,        \
                                  GLsizei, const void *))                     \
    X(void,   glEnableVertexAttribArray,(GLuint))

#define X(ret, name, args) static ret (*p_##name) args;
GL_FUNCS(X)
#undef X

static char s_error[512];

const char *gfxGLLastError(void) { return s_error; }

/* ------------------------------------------------------------- batching */

/* One batch flushes on any state change that the shader cannot express, so
 * bigger is only better up to the point the game changes state anyway. */
#define BATCH_VERTS 4096
#define TEX_CACHE   512

typedef struct batch_vertex {
    float x, y, z, w;
    float s, t;
    unsigned char r, g, b, a;
} batch_vertex;

typedef struct tex_entry {
    const void *src;
    int         fmt, siz, width, height;
    GLuint      tex;
} tex_entry;

typedef struct tile_state {
    int fmt, siz, line, tmem, palette;
    int cms, cmt, masks, maskt, shifts, shiftt;
    int uls, ult, lrs, lrt;
} tile_state;

typedef struct gl_backend {
    gfx_backend iface;   /* must be first: the public handle is this */

    GLuint prog, vao, vbo;
    GLint  u_tex, u_texsize, u_texscale, u_textured, u_prim, u_env;
    GLint  u_cc_c0, u_cc_a0, u_cc_c1, u_cc_a1, u_two_cycle;
    GLint  u_prim_lod, u_alpha_test, u_alpha_ref;

    batch_vertex batch[BATCH_VERTS];
    unsigned     batch_count;

    tex_entry tex_cache[TEX_CACHE];
    unsigned  tex_count;

    tile_state tiles[8];
    const void *timg;
    int         timg_fmt, timg_siz, timg_width;
    const void *tlut;
    unsigned    tlut_entries;

    int   current_tile;
    int   textured;
    float tex_scale_s, tex_scale_t;
    GLuint bound_tex;
    int   bound_w, bound_h;

    unsigned geometry_mode;
    unsigned char prim[4], env[4], fog[4];
    unsigned fill_color;
    float    prim_lod;

    gfx_combiner   cc;
    gfx_rendermode rm;

    /* Framebuffer the game thinks it is drawing to, and the window it
     * actually lands in. */
    int fb_w, fb_h, out_w, out_h;

    unsigned draw_calls;
} gl_backend;

static const char *k_vs =
    "#version 330 core\n"
    "layout(location=0) in vec4 a_pos;\n"
    "layout(location=1) in vec2 a_uv;\n"
    "layout(location=2) in vec4 a_color;\n"
    "uniform vec2 u_texsize;\n"
    "uniform vec2 u_texscale;\n"
    "out vec2 v_uv;\n"
    "out vec4 v_color;\n"
    "void main() {\n"
    /* Vertex texture coords are S10.5 -- 1/32 of a texel -- so they are
     * divided by 32 before being scaled by gSPTexture and normalised. */
    "    vec2 texel = (a_uv / 32.0) * u_texscale;\n"
    "    v_uv = (u_texsize.x > 0.0) ? texel / u_texsize : texel;\n"
    "    v_color = a_color;\n"
    "    gl_Position = a_pos;\n"
    "}\n";

/*
 * The colour combiner, evaluated per fragment.
 *
 * The RDP computes (a - b) * c + d, twice over, for colour and alpha
 * separately, with each operand chosen from a small set. gfx_rdp.c resolves
 * the raw mux -- whose meaning depends on which slot it sits in -- into the
 * unambiguous operand numbers this switches on, so the numbering here has to
 * stay in step with gfx_cc_operand.
 *
 * Passing the selectors as uniforms rather than compiling a shader per
 * combiner keeps this to a single program. The branches are uniform-controlled
 * so they cost almost nothing, and the geometry counts here are from 1997.
 */
static const char *k_fs =
    "#version 330 core\n"
    "in vec2 v_uv;\n"
    "in vec4 v_color;\n"
    "uniform sampler2D u_tex;\n"
    "uniform int   u_textured;\n"
    "uniform ivec4 u_cc_c0;\n"
    "uniform ivec4 u_cc_a0;\n"
    "uniform ivec4 u_cc_c1;\n"
    "uniform ivec4 u_cc_a1;\n"
    "uniform int   u_two_cycle;\n"
    "uniform vec4  u_prim;\n"
    "uniform vec4  u_env;\n"
    "uniform float u_prim_lod;\n"
    "uniform int   u_alpha_test;\n"
    "uniform float u_alpha_ref;\n"
    "out vec4 frag;\n"
    "vec4 g_tex0;\n"
    "vec4 g_tex1;\n"
    "vec4 g_comb;\n"
    "vec3 cc_rgb(int s) {\n"
    "    if (s ==  0) return g_comb.rgb;\n"
    "    if (s ==  1) return g_tex0.rgb;\n"
    "    if (s ==  2) return g_tex1.rgb;\n"
    "    if (s ==  3) return u_prim.rgb;\n"
    "    if (s ==  4) return v_color.rgb;\n"
    "    if (s ==  5) return u_env.rgb;\n"
    "    if (s ==  6) return vec3(0.5);\n"
    "    if (s ==  7) return vec3(1.0);\n"
    "    if (s ==  8) return vec3(g_comb.a);\n"
    "    if (s ==  9) return vec3(g_tex0.a);\n"
    "    if (s == 10) return vec3(g_tex1.a);\n"
    "    if (s == 11) return vec3(u_prim.a);\n"
    "    if (s == 12) return vec3(v_color.a);\n"
    "    if (s == 13) return vec3(u_env.a);\n"
    "    if (s == 14) return vec3(0.0);\n"
    "    if (s == 15) return vec3(u_prim_lod);\n"
    "    if (s == 16) return vec3(0.5);\n"
    "    if (s == 17) return vec3(0.0);\n"
    "    if (s == 18) return vec3(0.0);\n"
    "    if (s == 19) return vec3(1.0);\n"
    "    return vec3(0.0);\n"
    "}\n"
    "float cc_a(int s) {\n"
    "    if (s ==  8) return g_comb.a;\n"
    "    if (s ==  9) return g_tex0.a;\n"
    "    if (s == 10) return g_tex1.a;\n"
    "    if (s == 11) return u_prim.a;\n"
    "    if (s == 12) return v_color.a;\n"
    "    if (s == 13) return u_env.a;\n"
    "    if (s == 14) return 0.0;\n"
    "    if (s == 15) return u_prim_lod;\n"
    "    if (s == 19) return 1.0;\n"
    "    return 0.0;\n"
    "}\n"
    "void main() {\n"
    "    g_tex0 = (u_textured != 0) ? texture(u_tex, v_uv) : vec4(1.0);\n"
    "    g_tex1 = g_tex0;\n"
    "    g_comb = vec4(0.0);\n"
    "    vec3  c = (cc_rgb(u_cc_c0.x) - cc_rgb(u_cc_c0.y))\n"
    "            * cc_rgb(u_cc_c0.z) + cc_rgb(u_cc_c0.w);\n"
    "    float a = (cc_a(u_cc_a0.x) - cc_a(u_cc_a0.y))\n"
    "            * cc_a(u_cc_a0.z) + cc_a(u_cc_a0.w);\n"
    "    g_comb = vec4(c, a);\n"
    "    if (u_two_cycle != 0) {\n"
    "        c = (cc_rgb(u_cc_c1.x) - cc_rgb(u_cc_c1.y))\n"
    "          * cc_rgb(u_cc_c1.z) + cc_rgb(u_cc_c1.w);\n"
    "        a = (cc_a(u_cc_a1.x) - cc_a(u_cc_a1.y))\n"
    "          * cc_a(u_cc_a1.z) + cc_a(u_cc_a1.w);\n"
    "    }\n"
    "    if (u_alpha_test == 1 && a < u_alpha_ref) { discard; }\n"
    "    if (u_alpha_test == 2 && a < 0.125) { discard; }\n"
    "    frag = vec4(clamp(c, 0.0, 1.0), clamp(a, 0.0, 1.0));\n"
    "}\n";

static GLuint compile(GLenum stage, const char *src)
{
    GLuint sh = p_glCreateShader(stage);
    GLint ok = 0;

    p_glShaderSource(sh, 1, &src, NULL);
    p_glCompileShader(sh);
    p_glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        GLsizei n = 0;
        p_glGetShaderInfoLog(sh, (GLsizei)sizeof(s_error), &n, s_error);
        p_glDeleteShader(sh);
        return 0;
    }
    return sh;
}

/* ------------------------------------------------------------- textures */

static GLuint upload_texture(gl_backend *b, const void *src, int fmt, int siz,
                             int width, int height, int *out_w, int *out_h)
{
    unsigned char *rgba;
    GLuint tex = 0;
    unsigned i;
    unsigned src_bytes;

    if (!src || width <= 0 || height <= 0) {
        return 0;
    }

    for (i = 0; i < b->tex_count; i++) {
        tex_entry *e = &b->tex_cache[i];
        if (e->src == src && e->fmt == fmt && e->siz == siz &&
            e->width == width && e->height == height) {
            *out_w = e->width;
            *out_h = e->height;
            return e->tex;
        }
    }

    src_bytes = gfxTextureRowBytes(siz, width) * (unsigned)height;
    rgba = (unsigned char *)malloc(GEPC_TEX_RGBA8_BYTES(width, height));
    if (!rgba) {
        return 0;
    }

    if (gfxTextureDecode(src, src_bytes, fmt, siz, width, height, 0,
                         b->tlut, b->tlut_entries, rgba) != 0) {
        /* An undecodable texture is a data or state-tracking bug. Draw it
         * untextured rather than binding whatever was last uploaded, which
         * would smear an unrelated image across the surface. */
        free(rgba);
        return 0;
    }

    p_glGenTextures(1, &tex);
    p_glBindTexture(GL_TEXTURE_2D, tex);
    p_glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    p_glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0,
                   GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    /* Nearest by default: the N64 look depends on it, and bilinear on a
     * 32x32 texture stretched over a wall reads as mush. */
    p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    free(rgba);

    if (b->tex_count < TEX_CACHE) {
        tex_entry *e = &b->tex_cache[b->tex_count++];
        e->src = src;
        e->fmt = fmt;
        e->siz = siz;
        e->width = width;
        e->height = height;
        e->tex = tex;
    }

    *out_w = width;
    *out_h = height;
    return tex;
}

/* --------------------------------------------------------------- batch */

/*
 * Turn the decoded render mode into GL state. Called from flush, so the state
 * that applies is always the state in force when the batch was built.
 */
static GLenum blend_factor(int f)
{
    switch (f) {
    case GFX_BLEND_ZERO:                 return GL_ZERO;
    case GFX_BLEND_ONE:                  return GL_ONE;
    case GFX_BLEND_SRC_ALPHA:            return GL_SRC_ALPHA;
    case GFX_BLEND_ONE_MINUS_SRC_ALPHA:  return GL_ONE_MINUS_SRC_ALPHA;
    case GFX_BLEND_DST_ALPHA:            return GL_DST_ALPHA;
    default:                             return GL_ONE;
    }
}

/*
 * Filtering and wrapping belong to the *tile*, not the texture, so they are
 * set when the texture is bound rather than when it is uploaded. The same
 * image is routinely used clamped in one place and wrapped in another, and
 * baking either choice into the cached object gets one of them wrong.
 */
static GLenum wrap_mode(int cm)
{
    if (cm & G_TX_CLAMP) {
        return GL_CLAMP_TO_EDGE;
    }
    return (cm & G_TX_MIRROR) ? GL_MIRRORED_REPEAT : GL_REPEAT;
}

/*
 * Maps a rectangle from the game's framebuffer coordinates into the window,
 * flipping y on the way: the N64 measures from the top edge and GL from the
 * bottom.
 */
static void map_rect(const gl_backend *b, int x, int y, int w, int h,
                     GLint *ox, GLint *oy, GLsizei *ow, GLsizei *oh)
{
    float sx = (b->fb_w > 0) ? (float)b->out_w / (float)b->fb_w : 1.0f;
    float sy = (b->fb_h > 0) ? (float)b->out_h / (float)b->fb_h : 1.0f;
    int flipped = (b->fb_h > 0) ? (b->fb_h - (y + h)) : y;

    *ox = (GLint)(x * sx + 0.5f);
    *oy = (GLint)(flipped * sy + 0.5f);
    *ow = (GLsizei)(w * sx + 0.5f);
    *oh = (GLsizei)(h * sy + 0.5f);
}

static void apply_texture_params(gl_backend *b)
{
    const tile_state *t;
    GLenum filter;

    if (!b->bound_tex) {
        return;
    }
    t = &b->tiles[(b->current_tile >= 0 && b->current_tile <= 7)
                  ? b->current_tile : 0];

    /* G_TF_POINT is 0 and G_TF_BILERP is 2 once shifted down. Point sampling
     * is what the console did for most surfaces and what the art was drawn
     * for; honouring the game's choice rather than forcing either. */
    filter = (b->rm.tex_filter == (G_TF_BILERP >> G_MDSFT_TEXTFILT))
                 ? GL_LINEAR : GL_NEAREST;

    p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, (GLint)filter);
    p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, (GLint)filter);
    p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, (GLint)wrap_mode(t->cms));
    p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, (GLint)wrap_mode(t->cmt));
}

static void apply_rendermode(gl_backend *b)
{
    const gfx_rendermode *rm = &b->rm;

    /* Depth testing is the intersection of two things: the geometry mode's
     * G_ZBUFFER and the render mode's Z_CMP. The game turns the first off for
     * the HUD and the second off for sky and muzzle flashes. */
    if (rm->z_compare && (b->geometry_mode & (unsigned)G_ZBUFFER)) {
        p_glEnable(GL_DEPTH_TEST);
        p_glDepthFunc(GL_LEQUAL);
    } else {
        p_glDisable(GL_DEPTH_TEST);
    }

    /* Writing depth is separate from testing it, and getting this wrong is
     * very visible: a translucent surface that writes depth hides everything
     * drawn behind it afterwards. */
    p_glDepthMask(rm->z_update ? GL_TRUE : GL_FALSE);

    /* ZMODE_DEC is the RDP's decal mode -- coplanar geometry such as bullet
     * holes and floor markings, which would otherwise z-fight with the
     * surface they sit on. A small depth offset is the standard equivalent. */
    if (rm->z_mode == 3) {
        p_glEnable(GL_POLYGON_OFFSET_FILL);
        p_glPolygonOffset(-1.0f, -1.0f);
    } else {
        p_glDisable(GL_POLYGON_OFFSET_FILL);
    }

    if (rm->blend_enabled) {
        p_glEnable(GL_BLEND);
        p_glBlendFunc(blend_factor(rm->src_factor), blend_factor(rm->dst_factor));
    } else {
        p_glDisable(GL_BLEND);
    }
}

static void flush(gl_backend *b)
{
    if (!b->batch_count) {
        return;
    }

    p_glBindVertexArray(b->vao);
    p_glBindBuffer(GL_ARRAY_BUFFER, b->vbo);
    p_glBufferData(GL_ARRAY_BUFFER,
                   (GLsizeiptr)(b->batch_count * sizeof(batch_vertex)),
                   b->batch, GL_DYNAMIC_DRAW);

    p_glUseProgram(b->prog);
    p_glUniform1i(b->u_tex, 0);
    p_glUniform1i(b->u_textured, b->textured && b->bound_tex ? 1 : 0);
    p_glUniform2f(b->u_texsize, (float)b->bound_w, (float)b->bound_h);
    p_glUniform2f(b->u_texscale, b->tex_scale_s, b->tex_scale_t);
    p_glUniform4f(b->u_prim, b->prim[0] / 255.0f, b->prim[1] / 255.0f,
                  b->prim[2] / 255.0f, b->prim[3] / 255.0f);
    p_glUniform4f(b->u_env, b->env[0] / 255.0f, b->env[1] / 255.0f,
                  b->env[2] / 255.0f, b->env[3] / 255.0f);

    p_glUniform4i(b->u_cc_c0, b->cc.color[0][0], b->cc.color[0][1],
                  b->cc.color[0][2], b->cc.color[0][3]);
    p_glUniform4i(b->u_cc_a0, b->cc.alpha[0][0], b->cc.alpha[0][1],
                  b->cc.alpha[0][2], b->cc.alpha[0][3]);
    p_glUniform4i(b->u_cc_c1, b->cc.color[1][0], b->cc.color[1][1],
                  b->cc.color[1][2], b->cc.color[1][3]);
    p_glUniform4i(b->u_cc_a1, b->cc.alpha[1][0], b->cc.alpha[1][1],
                  b->cc.alpha[1][2], b->cc.alpha[1][3]);
    p_glUniform1i(b->u_two_cycle, b->rm.cycle_type == 1 ? 1 : 0);
    p_glUniform1f(b->u_prim_lod, b->prim_lod);

    /* CVG_X_ALPHA is how the game cuts out foliage and chain-link fences:
     * coverage is multiplied by alpha, so a nearly transparent texel drops
     * out entirely. Without coverage to multiply, a cutout is the honest
     * equivalent. A threshold compare is the explicit form of the same idea. */
    p_glUniform1i(b->u_alpha_test,
                  b->rm.alpha_compare == 1 ? 1 : (b->rm.cvg_x_alpha ? 2 : 0));
    p_glUniform1f(b->u_alpha_ref, b->env[3] / 255.0f);

    apply_rendermode(b);

    p_glActiveTexture(GL_TEXTURE0);
    p_glBindTexture(GL_TEXTURE_2D, b->bound_tex);
    apply_texture_params(b);

    p_glDrawArrays(GL_TRIANGLES, 0, (GLsizei)b->batch_count);
    p_glBindVertexArray(0);

    b->batch_count = 0;
    b->draw_calls++;
}

static void push_vertex(gl_backend *b, const gfx_vertex *v)
{
    batch_vertex *o = &b->batch[b->batch_count++];
    o->x = v->x; o->y = v->y; o->z = v->z; o->w = v->w;
    o->s = v->s; o->t = v->t;
    o->r = v->r; o->g = v->g; o->b = v->b; o->a = v->a;
}

/* ------------------------------------------------------- backend hooks */

static void be_begin_frame(void *user)
{
    gl_backend *b = (gl_backend *)user;
    b->batch_count = 0;
    b->draw_calls = 0;
    p_glEnable(GL_DEPTH_TEST);
    p_glEnable(GL_BLEND);
    p_glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    p_glDisable(GL_CULL_FACE);

    /* Clear colour and depth. Without this each frame composites onto the
     * last, and -- worse -- stale depth values reject the new frame's
     * geometry wherever it sits at the same depth as the old, so the picture
     * simply stops updating. The self-test caught exactly that: a textured
     * triangle drawn over an identical untextured one produced no visible
     * change at all.
     *
     * The N64 cleared by filling the framebuffer with a rectangle, and the
     * game still issues that; clearing here as well is cheap and means the
     * depth buffer is always in a known state. */
    p_glDisable(GL_SCISSOR_TEST);
    p_glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    p_glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
}

static void be_end_frame(void *user)
{
    flush((gl_backend *)user);
}

static void be_viewport(void *user, int x, int y, int w, int h)
{
    gl_backend *b = (gl_backend *)user;
    GLint ox, oy; GLsizei ow, oh;

    flush(b);
    map_rect(b, x, y, w, h, &ox, &oy, &ow, &oh);
    p_glViewport(ox, oy, ow, oh);
}

static void be_scissor(void *user, int x, int y, int w, int h)
{
    gl_backend *b = (gl_backend *)user;
    GLint ox, oy; GLsizei ow, oh;

    flush(b);
    map_rect(b, x, y, w, h, &ox, &oy, &ow, &oh);
    p_glEnable(GL_SCISSOR_TEST);
    p_glScissor(ox, oy, ow, oh);
}

static void be_geometry_mode(void *user, unsigned mode)
{
    gl_backend *b = (gl_backend *)user;
    if (b->geometry_mode == mode) {
        return;
    }
    flush(b);
    b->geometry_mode = mode;

    if (mode & (unsigned)G_ZBUFFER) {
        p_glEnable(GL_DEPTH_TEST);
    } else {
        p_glDisable(GL_DEPTH_TEST);
    }
}

static void be_texture_image(void *user, const void *addr, int fmt, int siz,
                             int width)
{
    gl_backend *b = (gl_backend *)user;
    flush(b);
    b->timg = addr;
    b->timg_fmt = fmt;
    b->timg_siz = siz;
    b->timg_width = width;
}

static void be_set_tile(void *user, int tile, int fmt, int siz, int line,
                        int tmem, int palette, int cms, int cmt,
                        int masks, int maskt, int shifts, int shiftt)
{
    gl_backend *b = (gl_backend *)user;
    tile_state *t;

    if (tile < 0 || tile > 7) {
        return;
    }
    flush(b);
    t = &b->tiles[tile];
    t->fmt = fmt; t->siz = siz; t->line = line; t->tmem = tmem;
    t->palette = palette; t->cms = cms; t->cmt = cmt;
    t->masks = masks; t->maskt = maskt; t->shifts = shifts; t->shiftt = shiftt;
}

static void be_tile_size(void *user, int tile, int uls, int ult, int lrs, int lrt)
{
    gl_backend *b = (gl_backend *)user;
    tile_state *t;
    int w, h;

    if (tile < 0 || tile > 7) {
        return;
    }
    flush(b);
    t = &b->tiles[tile];
    t->uls = uls; t->ult = ult; t->lrs = lrs; t->lrt = lrt;

    /* Tile coordinates are 10.2 fixed point and inclusive at both ends, so
     * the span is (lr - ul) / 4 + 1 texels. */
    w = ((lrs - uls) >> 2) + 1;
    h = ((lrt - ult) >> 2) + 1;

    if (tile == b->current_tile && b->timg && w > 0 && h > 0) {
        b->bound_tex = upload_texture(b, b->timg,
                                      t->fmt >= 0 ? t->fmt : b->timg_fmt,
                                      t->siz >= 0 ? t->siz : b->timg_siz,
                                      w, h, &b->bound_w, &b->bound_h);
    }
}

static void be_load_block(void *user, int tile, int uls, int ult, int lrs, int dxt)
{
    (void)tile; (void)uls; (void)ult; (void)lrs; (void)dxt;
    /* The image is decoded lazily from b->timg when the tile size arrives,
     * so there is nothing to copy here. Kept as a hook because a backend that
     * emulates TMEM properly would need it. */
    flush((gl_backend *)user);
}

static void be_load_tlut(void *user, int tile, int count)
{
    gl_backend *b = (gl_backend *)user;
    (void)tile;
    flush(b);
    /* The palette lives wherever the last G_SETTIMG pointed. */
    b->tlut = b->timg;
    b->tlut_entries = (unsigned)(count > 0 ? count : 0);
}

static void be_texture_scale(void *user, float s, float t, int level, int tile,
                             int on)
{
    gl_backend *b = (gl_backend *)user;
    (void)level;
    flush(b);
    b->tex_scale_s = (s > 0.0f) ? s : 1.0f;
    b->tex_scale_t = (t > 0.0f) ? t : 1.0f;
    b->textured = on;
    if (tile >= 0 && tile <= 7) {
        b->current_tile = tile;
    }
}

static void be_prim(void *user, unsigned char r, unsigned char g,
                    unsigned char bl, unsigned char a)
{
    gl_backend *b = (gl_backend *)user;
    flush(b);
    b->prim[0] = r; b->prim[1] = g; b->prim[2] = bl; b->prim[3] = a;
}

static void be_env(void *user, unsigned char r, unsigned char g,
                   unsigned char bl, unsigned char a)
{
    gl_backend *b = (gl_backend *)user;
    flush(b);
    b->env[0] = r; b->env[1] = g; b->env[2] = bl; b->env[3] = a;
}

static void be_combine(void *user, unsigned w0, unsigned w1)
{
    gl_backend *b = (gl_backend *)user;
    gfx_combiner cc;

    gfxCombineDecode(w0, w1, &cc);
    if (memcmp(&cc, &b->cc, sizeof(cc)) == 0) {
        return;
    }
    flush(b);
    b->cc = cc;
}

static void be_othermode_h(void *user, unsigned shift, unsigned len,
                           unsigned data)
{
    gl_backend *b = (gl_backend *)user;
    unsigned before = b->rm.hi;

    gfxRenderModeSetH(&b->rm, shift, len, data);
    if (b->rm.hi != before) {
        flush(b);
    }
}

static void be_othermode_l(void *user, unsigned shift, unsigned len,
                           unsigned data)
{
    gl_backend *b = (gl_backend *)user;
    unsigned before = b->rm.lo;

    gfxRenderModeSetL(&b->rm, shift, len, data);
    if (b->rm.lo != before) {
        flush(b);
    }
}

static void be_fog(void *user, unsigned char r, unsigned char g,
                   unsigned char bl, unsigned char a)
{
    gl_backend *b = (gl_backend *)user;
    b->fog[0] = r; b->fog[1] = g; b->fog[2] = bl; b->fog[3] = a;
}

static void be_fill_color(void *user, unsigned value)
{
    ((gl_backend *)user)->fill_color = value;
}

/*
 * G_FILLRECT in fill mode is how the game clears the screen, and in copy mode
 * how it draws solid bars. The fill colour is two packed RGBA5551 pixels; both
 * halves are the same colour for a clear, so reading the low half is enough.
 */
static void be_fill_rect(void *user, int ulx, int uly, int lrx, int lry)
{
    gl_backend *b = (gl_backend *)user;
    unsigned px = b->fill_color & 0xFFFFu;
    float r = (float)((px >> 11) & 0x1Fu) / 31.0f;
    float g = (float)((px >>  6) & 0x1Fu) / 31.0f;
    float bl = (float)((px >>  1) & 0x1Fu) / 31.0f;

    flush(b);

    /* Scissor to the rectangle and clear, rather than drawing geometry: the
     * rectangle is in screen pixels and has no place in the transformed
     * pipeline. Depth is deliberately left alone -- a fill rect covers colour
     * only, and clearing depth here would throw away the frame's z-buffer
     * partway through drawing it. */
    {
        GLint ox, oy; GLsizei ow, oh;
        map_rect(b, ulx, uly, lrx - ulx, lry - uly, &ox, &oy, &ow, &oh);
        p_glEnable(GL_SCISSOR_TEST);
        p_glScissor(ox, oy, ow, oh);
    }
    p_glClearColor(r, g, bl, 1.0f);
    p_glClear(GL_COLOR_BUFFER_BIT);
}

static void be_tex_rect(void *user, int ulx, int uly, int lrx, int lry,
                        int tile, int s, int t, int dsdx, int dtdy, int flip)
{
    (void)user; (void)ulx; (void)uly; (void)lrx; (void)lry; (void)tile;
    (void)s; (void)t; (void)dsdx; (void)dtdy; (void)flip;
    /* GoldenEye draws its HUD and menus through ordinary triangles rather
     * than texture rectangles, so this stays a hook rather than a guess at
     * screen-space quad handling that nothing would exercise. */
}

static void be_draw_triangle(void *user, const gfx_vertex *a,
                             const gfx_vertex *b_, const gfx_vertex *c)
{
    gl_backend *b = (gl_backend *)user;

    if (b->batch_count + 3 > BATCH_VERTS) {
        flush(b);
    }
    push_vertex(b, a);
    push_vertex(b, b_);
    push_vertex(b, c);
}

static void be_unsupported(void *user, unsigned char op, unsigned w0, unsigned w1)
{
    (void)user; (void)w0; (void)w1;
    /* Logged once per opcode would be better; for now this is rare enough in
     * GoldenEye (one call site in src/boss.c) to be worth seeing every time. */
    platformLog("gfx: unhandled command 0x%02x", op);
}

/* ------------------------------------------------------------ lifecycle */

gfx_backend *gfxGLCreate(void)
{
    gl_backend *b;
    GLuint vs, fs;
    GLint ok = 0;

    s_error[0] = '\0';

#define X(ret, name, args)                                                    \
    p_##name = (ret (*) args)SDL_GL_GetProcAddress(#name);                    \
    if (!p_##name) {                                                          \
        snprintf(s_error, sizeof(s_error), "missing GL entry point: %s", #name); \
        return NULL;                                                          \
    }
    GL_FUNCS(X)
#undef X

    b = (gl_backend *)calloc(1, sizeof(*b));
    if (!b) {
        snprintf(s_error, sizeof(s_error), "out of memory");
        return NULL;
    }

    vs = compile(GL_VERTEX_SHADER, k_vs);
    if (!vs) { free(b); return NULL; }
    fs = compile(GL_FRAGMENT_SHADER, k_fs);
    if (!fs) { p_glDeleteShader(vs); free(b); return NULL; }

    b->prog = p_glCreateProgram();
    p_glAttachShader(b->prog, vs);
    p_glAttachShader(b->prog, fs);
    p_glLinkProgram(b->prog);
    p_glGetProgramiv(b->prog, GL_LINK_STATUS, &ok);
    p_glDeleteShader(vs);
    p_glDeleteShader(fs);
    if (!ok) {
        GLsizei n = 0;
        p_glGetProgramInfoLog(b->prog, (GLsizei)sizeof(s_error), &n, s_error);
        p_glDeleteProgram(b->prog);
        free(b);
        return NULL;
    }

    b->u_tex      = p_glGetUniformLocation(b->prog, "u_tex");
    b->u_texsize  = p_glGetUniformLocation(b->prog, "u_texsize");
    b->u_texscale = p_glGetUniformLocation(b->prog, "u_texscale");
    b->u_textured = p_glGetUniformLocation(b->prog, "u_textured");
    b->u_prim     = p_glGetUniformLocation(b->prog, "u_prim");
    b->u_env      = p_glGetUniformLocation(b->prog, "u_env");
    b->u_cc_c0     = p_glGetUniformLocation(b->prog, "u_cc_c0");
    b->u_cc_a0     = p_glGetUniformLocation(b->prog, "u_cc_a0");
    b->u_cc_c1     = p_glGetUniformLocation(b->prog, "u_cc_c1");
    b->u_cc_a1     = p_glGetUniformLocation(b->prog, "u_cc_a1");
    b->u_two_cycle = p_glGetUniformLocation(b->prog, "u_two_cycle");
    b->u_prim_lod  = p_glGetUniformLocation(b->prog, "u_prim_lod");
    b->u_alpha_test= p_glGetUniformLocation(b->prog, "u_alpha_test");
    b->u_alpha_ref = p_glGetUniformLocation(b->prog, "u_alpha_ref");

    /* Start from the mode the RDP powers up in, and from a combiner that
     * simply passes shade through. A list that draws before setting either
     * then produces flat-shaded geometry rather than black. */
    /* Identity until video.c says otherwise, so a backend driven directly --
     * the self-test does exactly that -- behaves as it always did. */
    b->fb_w = b->out_w = 320;
    b->fb_h = b->out_h = 240;

    gfxRenderModeInit(&b->rm);
    gfxCombineDecode(0u, 0u, &b->cc);
    b->cc.color[0][3] = b->cc.color[1][3] = (unsigned char)GFX_CC_SHADE;
    b->cc.alpha[0][3] = b->cc.alpha[1][3] = (unsigned char)GFX_CC_SHADE_ALPHA;
    b->prim_lod = 0.0f;

    p_glGenVertexArrays(1, &b->vao);
    p_glBindVertexArray(b->vao);
    p_glGenBuffers(1, &b->vbo);
    p_glBindBuffer(GL_ARRAY_BUFFER, b->vbo);

    p_glEnableVertexAttribArray(0);
    p_glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, sizeof(batch_vertex),
                            (const void *)offsetof(batch_vertex, x));
    p_glEnableVertexAttribArray(1);
    p_glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(batch_vertex),
                            (const void *)offsetof(batch_vertex, s));
    p_glEnableVertexAttribArray(2);
    p_glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_TRUE,
                            sizeof(batch_vertex),
                            (const void *)offsetof(batch_vertex, r));
    p_glBindVertexArray(0);

    b->tex_scale_s = 1.0f;
    b->tex_scale_t = 1.0f;
    b->textured = 1;
    b->current_tile = G_TX_RENDERTILE;
    memset(b->prim, 255, sizeof(b->prim));
    memset(b->env, 255, sizeof(b->env));

    b->iface.user              = b;
    b->iface.begin_frame       = be_begin_frame;
    b->iface.end_frame         = be_end_frame;
    b->iface.set_viewport      = be_viewport;
    b->iface.set_scissor       = be_scissor;
    b->iface.set_geometry_mode = be_geometry_mode;
    b->iface.set_combine       = be_combine;
    b->iface.set_othermode_h   = be_othermode_h;
    b->iface.set_othermode_l   = be_othermode_l;
    b->iface.set_fog_color     = be_fog;
    b->iface.set_fill_color    = be_fill_color;
    b->iface.fill_rect         = be_fill_rect;
    b->iface.tex_rect          = be_tex_rect;
    b->iface.set_texture_image = be_texture_image;
    b->iface.set_tile          = be_set_tile;
    b->iface.set_tile_size     = be_tile_size;
    b->iface.load_block        = be_load_block;
    b->iface.load_tlut         = be_load_tlut;
    b->iface.set_texture_scale = be_texture_scale;
    b->iface.set_prim_color    = be_prim;
    b->iface.set_env_color     = be_env;
    b->iface.draw_triangle     = be_draw_triangle;
    b->iface.unsupported       = be_unsupported;

    return &b->iface;
}

void gfxGLDestroy(gfx_backend *be)
{
    gl_backend *b = (gl_backend *)be;
    unsigned i;

    if (!b) {
        return;
    }
    for (i = 0; i < b->tex_count; i++) {
        p_glDeleteTextures(1, &b->tex_cache[i].tex);
    }
    if (b->vbo)  { p_glDeleteBuffers(1, &b->vbo); }
    if (b->vao)  { p_glDeleteVertexArrays(1, &b->vao); }
    if (b->prog) { p_glDeleteProgram(b->prog); }
    free(b);
}

void gfxGLSetOutputSize(gfx_backend *be, int fb_w, int fb_h,
                        int out_w, int out_h)
{
    gl_backend *b = (gl_backend *)be;

    if (!b || fb_w <= 0 || fb_h <= 0 || out_w <= 0 || out_h <= 0) {
        return;
    }
    flush(b);
    b->fb_w = fb_w;
    b->fb_h = fb_h;
    b->out_w = out_w;
    b->out_h = out_h;
}

void gfxGLFlushTextureCache(gfx_backend *be)
{
    gl_backend *b = (gl_backend *)be;
    unsigned i;

    if (!b) {
        return;
    }
    /* Source addresses are reused across levels, so a stale entry would show
     * the previous level's texture on this level's geometry. */
    for (i = 0; i < b->tex_count; i++) {
        p_glDeleteTextures(1, &b->tex_cache[i].tex);
    }
    b->tex_count = 0;
    b->bound_tex = 0;
}

unsigned gfxGLDrawCallCount(const gfx_backend *be)
{
    const gl_backend *b = (const gl_backend *)be;
    return b ? b->draw_calls : 0;
}

unsigned gfxGLTextureCount(const gfx_backend *be)
{
    const gl_backend *b = (const gl_backend *)be;
    return b ? b->tex_count : 0;
}
