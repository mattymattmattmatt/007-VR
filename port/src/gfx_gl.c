/*
 * OpenGL 3.3 backend. Entry points are resolved through SDL rather than linked
 * against libGL, for the same reason the VR layer does it: Windows only
 * exports GL 1.1, so anything newer has to come from wglGetProcAddress anyway.
 */
#include "gfx_gl.h"
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

#define GL_FUNCS(X)                                                           \
    X(void,   glViewport,        (GLint, GLint, GLsizei, GLsizei))            \
    X(void,   glScissor,         (GLint, GLint, GLsizei, GLsizei))            \
    X(void,   glClearColor,      (GLfloat, GLfloat, GLfloat, GLfloat))        \
    X(void,   glClear,           (GLbitfield))                                \
    X(void,   glEnable,          (GLenum))                                    \
    X(void,   glDisable,         (GLenum))                                    \
    X(void,   glBlendFunc,       (GLenum, GLenum))                            \
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
    unsigned char prim[4], env[4];

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

static const char *k_fs =
    "#version 330 core\n"
    "in vec2 v_uv;\n"
    "in vec4 v_color;\n"
    "uniform sampler2D u_tex;\n"
    "uniform int u_textured;\n"
    "uniform vec4 u_prim;\n"
    "uniform vec4 u_env;\n"
    "out vec4 frag;\n"
    "void main() {\n"
    "    vec4 c = v_color;\n"
    "    if (u_textured != 0) { c *= texture(u_tex, v_uv); }\n"
    "    if (c.a < 0.01) { discard; }\n"
    "    frag = c;\n"
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

    p_glActiveTexture(GL_TEXTURE0);
    p_glBindTexture(GL_TEXTURE_2D, b->bound_tex);

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
    flush((gl_backend *)user);
    p_glViewport(x, y, w, h);
}

static void be_scissor(void *user, int x, int y, int w, int h)
{
    flush((gl_backend *)user);
    p_glEnable(GL_SCISSOR_TEST);
    p_glScissor(x, y, w, h);
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
