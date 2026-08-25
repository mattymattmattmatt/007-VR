/*
 * Tests for the renderer state machine, driven through a recording backend so
 * they need no GPU.
 *
 * As with the walker tests, every display list is built with the game's own
 * GBI macros. That is what makes these meaningful: the triangle index
 * encodings differ between GBI variants, and the whole point is to verify the
 * decoder against the encoding this build actually produces.
 */
#include "gfx_state.h"
#include "gbi_walk.h"
#include "platform.h"

#include <stdio.h>
#include <string.h>

#include <PR/ultratypes.h>
#include <PR/mbi.h>
#include <PR/gbi.h>
#include <gbi_extension.h>

static int g_failures;
static int g_checks;

static void check(int cond, const char *what, const char *file, int line)
{
    g_checks++;
    if (!cond) {
        g_failures++;
        printf("  FAIL %s:%d: %s\n", file, line, what);
    }
}

static void check_near(float got, float want, float tol, const char *what,
                       const char *file, int line)
{
    float d = got - want;
    if (d < 0.0f) { d = -d; }
    g_checks++;
    if (d > tol) {
        g_failures++;
        printf("  FAIL %s:%d: %s (got %f want %f)\n", file, line, what, got, want);
    }
}

#define CHECK(c)            check((c), #c, __FILE__, __LINE__)
#define CHECK_NEAR(g,w,t)   check_near((g),(w),(t), #g " ~= " #w, __FILE__, __LINE__)

/* ------------------------------------------------------ recording backend */

#define MAX_REC 256

typedef struct {
    int        tri_count;
    gfx_vertex tri[MAX_REC][3];
    int        unsupported_count;
    unsigned char unsupported_ops[MAX_REC];
    int        geometry_mode_calls;
    unsigned   last_geometry_mode;
    int        prim_color_calls;
    int        tex_scale_calls;
    float      last_tex_s, last_tex_t;
    int        last_tex_tile, last_tex_on;
    int        frames_begun;
    int        frames_ended;
} recorder;

static void rec_draw(void *user, const gfx_vertex *a, const gfx_vertex *b,
                     const gfx_vertex *c)
{
    recorder *r = (recorder *)user;
    if (r->tri_count < MAX_REC) {
        r->tri[r->tri_count][0] = *a;
        r->tri[r->tri_count][1] = *b;
        r->tri[r->tri_count][2] = *c;
    }
    r->tri_count++;
}

static void rec_unsupported(void *user, unsigned char op, unsigned w0, unsigned w1)
{
    recorder *r = (recorder *)user;
    (void)w0; (void)w1;
    if (r->unsupported_count < MAX_REC) {
        r->unsupported_ops[r->unsupported_count] = op;
    }
    r->unsupported_count++;
}

static void rec_geom(void *user, unsigned mode)
{
    recorder *r = (recorder *)user;
    r->geometry_mode_calls++;
    r->last_geometry_mode = mode;
}

static void rec_prim(void *user, unsigned char a, unsigned char b,
                     unsigned char c, unsigned char d)
{
    recorder *r = (recorder *)user;
    (void)a; (void)b; (void)c; (void)d;
    r->prim_color_calls++;
}

static void rec_tex_scale(void *user, float s, float t, int level, int tile, int on)
{
    recorder *r = (recorder *)user;
    (void)level;
    r->tex_scale_calls++;
    r->last_tex_s = s;
    r->last_tex_t = t;
    r->last_tex_tile = tile;
    r->last_tex_on = on;
}

static void rec_begin(void *user) { ((recorder *)user)->frames_begun++; }
static void rec_end(void *user)   { ((recorder *)user)->frames_ended++; }

static gfx_state *make_state(recorder *r)
{
    gfx_backend be;

    memset(r, 0, sizeof(*r));
    memset(&be, 0, sizeof(be));
    be.user = r;
    be.draw_triangle = rec_draw;
    be.unsupported = rec_unsupported;
    be.set_geometry_mode = rec_geom;
    be.set_prim_color = rec_prim;
    be.set_texture_scale = rec_tex_scale;
    be.begin_frame = rec_begin;
    be.end_frame = rec_end;
    return gfxStateCreate(&be);
}

/* --------------------------------------------------------------- matrix */

/* Packs floats the way guMtxF2L does, so the decode can be round-tripped. */
static void float_to_mtx(const float in[16], s32 out[16])
{
    int k;

    memset(out, 0, sizeof(s32) * 16);
    for (k = 0; k < 16; k++) {
        s32 fixed = (s32)(in[k] * 65536.0f);
        u16 ip = (u16)((fixed >> 16) & 0xFFFF);
        u16 fp = (u16)(fixed & 0xFFFF);

        if (k & 1) {
            out[k >> 1]       |= (s32)ip;
            out[8 + (k >> 1)] |= (s32)fp;
        } else {
            out[k >> 1]       |= (s32)((u32)ip << 16);
            out[8 + (k >> 1)] |= (s32)((u32)fp << 16);
        }
    }
}

static void test_matrix_decode(void)
{
    float in[16], out[16];
    s32 packed[16];
    int k;

    printf("gfx: fixed-point matrix decode round trips\n");

    for (k = 0; k < 16; k++) {
        in[k] = 0.0f;
    }
    in[0] = in[5] = in[10] = in[15] = 1.0f;

    float_to_mtx(in, packed);
    gfxMtxToFloat(packed, out);
    for (k = 0; k < 16; k++) {
        CHECK_NEAR(out[k], in[k], 0.001f);
    }

    /* Negative and fractional values, which is where a sign or shift error in
     * the split-halves format shows up. */
    for (k = 0; k < 16; k++) {
        in[k] = (float)(k - 8) * 1.5f;
    }
    float_to_mtx(in, packed);
    gfxMtxToFloat(packed, out);
    for (k = 0; k < 16; k++) {
        CHECK_NEAR(out[k], in[k], 0.001f);
    }

    /* A translation, the case the vertex transform depends on. */
    memset(in, 0, sizeof(in));
    in[0] = in[5] = in[10] = in[15] = 1.0f;
    in[12] = 10.0f; in[13] = -20.0f; in[14] = 30.5f;
    float_to_mtx(in, packed);
    gfxMtxToFloat(packed, out);
    CHECK_NEAR(out[12], 10.0f, 0.001f);
    CHECK_NEAR(out[13], -20.0f, 0.001f);
    CHECK_NEAR(out[14], 30.5f, 0.001f);
}

/* -------------------------------------------------------- index decoding */

static void test_tri1_indices(void)
{
    Gfx dl[4];
    Gfx *p = dl;
    int idx[3];

    printf("gfx: G_TRI1 indices decode to what the macro encoded\n");

    p = dl;
    gSP1Triangle(p, 3, 7, 11, 0);

    gfxDecodeTri1((unsigned)dl[0].words.w0, (unsigned)dl[0].words.w1, idx);
    CHECK(idx[0] == 3);
    CHECK(idx[1] == 7);
    CHECK(idx[2] == 11);

    /* Indices above 15, which G_TRI4's 4-bit fields cannot reach. */
    p = dl;
    gSP1Triangle(p, 0, 20, 25, 0);
    gfxDecodeTri1((unsigned)dl[0].words.w0, (unsigned)dl[0].words.w1, idx);
    CHECK(idx[0] == 0);
    CHECK(idx[1] == 20);
    CHECK(idx[2] == 25);
}

static void test_tri4_indices(void)
{
    Gfx dl[4];
    Gfx *p = dl;
    int idx[4][3];
    int n;

    printf("gfx: G_TRI4 indices decode, padding skipped\n");

    p = dl;
    gSP4Triangles(p, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12);
    n = gfxDecodeTri4((unsigned)dl[0].words.w0, (unsigned)dl[0].words.w1, idx);
    CHECK(n == 4);
    CHECK(idx[0][0] == 1 && idx[0][1] == 2 && idx[0][2] == 3);
    CHECK(idx[3][0] == 10 && idx[3][1] == 11 && idx[3][2] == 12);

    /* gSP2Triangles is G_TRI4 with two empty slots. */
    p = dl;
    gSP2Triangles(p, 1, 2, 3, 0, 4, 5, 6, 0);
    n = gfxDecodeTri4((unsigned)dl[0].words.w0, (unsigned)dl[0].words.w1, idx);
    CHECK(n == 2);
    CHECK(idx[0][0] == 1 && idx[0][2] == 3);
    CHECK(idx[1][0] == 4 && idx[1][2] == 6);
}

/* --------------------------------------------------------------- vertex */

static Vtx g_verts[8];
static Gfx g_dl[32];

static void build_verts(void)
{
    int i;

    memset(g_verts, 0, sizeof(g_verts));
    for (i = 0; i < 8; i++) {
        g_verts[i].v.ob[0] = (short)(i * 10);
        g_verts[i].v.ob[1] = (short)(i * 20);
        g_verts[i].v.ob[2] = (short)(i * 30);
        g_verts[i].v.tc[0] = (short)(i * 32);
        g_verts[i].v.tc[1] = (short)(i * 64);
        g_verts[i].v.cn[0] = (unsigned char)i;
        g_verts[i].v.cn[1] = (unsigned char)(i + 1);
        g_verts[i].v.cn[2] = (unsigned char)(i + 2);
        g_verts[i].v.cn[3] = 255;
    }
}

static void test_vertex_load_and_draw(void)
{
    recorder r;
    gfx_state *st;
    Gfx *p;

    printf("gfx: vertices load, transform and reach the backend\n");

    build_verts();
    gbiResetSegments();
    gbiSetSegment(1, g_verts);

    st = make_state(&r);
    CHECK(st != NULL);
    gfxStateBeginFrame(st);

    p = g_dl;
    gSPVertex(p++, 0x01000000u, 8, 0);
    gSP1Triangle(p++, 0, 1, 2, 0);
    gSPEndDisplayList(p++);

    CHECK(gfxStateRun(st, g_dl, 64) == 0);
    CHECK(gfxStateVertexCount(st) == 8);
    CHECK(gfxStateTriangleCount(st) == 1);
    CHECK(r.tri_count == 1);

    /* With an identity transform the positions pass through unchanged, so a
     * transposed or mis-scaled matrix decode would show up immediately. */
    CHECK_NEAR(r.tri[0][1].x, 10.0f, 0.01f);
    CHECK_NEAR(r.tri[0][1].y, 20.0f, 0.01f);
    CHECK_NEAR(r.tri[0][1].z, 30.0f, 0.01f);
    CHECK_NEAR(r.tri[0][2].x, 20.0f, 0.01f);

    /* Texture coordinates and shade colour survive the trip. */
    CHECK_NEAR(r.tri[0][1].s, 32.0f, 0.01f);
    CHECK_NEAR(r.tri[0][1].t, 64.0f, 0.01f);
    CHECK(r.tri[0][1].r == 1);
    CHECK(r.tri[0][1].a == 255);

    gfxStateEndFrame(st);
    CHECK(r.frames_begun == 1);
    CHECK(r.frames_ended == 1);
    gfxStateDestroy(st);
}

static void test_vertex_translation(void)
{
    recorder r;
    gfx_state *st;
    Gfx *p;
    float m[16];
    static s32 packed[16];

    printf("gfx: a loaded matrix actually transforms vertices\n");

    build_verts();
    memset(m, 0, sizeof(m));
    m[0] = m[5] = m[10] = m[15] = 1.0f;
    m[12] = 100.0f;   /* translate x */
    float_to_mtx(m, packed);

    gbiResetSegments();
    gbiSetSegment(1, g_verts);
    gbiSetSegment(2, packed);

    st = make_state(&r);
    gfxStateBeginFrame(st);

    p = g_dl;
    gSPMatrix(p++, 0x02000000u, G_MTX_MODELVIEW | G_MTX_LOAD | G_MTX_NOPUSH);
    gSPVertex(p++, 0x01000000u, 8, 0);
    gSP1Triangle(p++, 0, 1, 2, 0);
    gSPEndDisplayList(p++);

    CHECK(gfxStateRun(st, g_dl, 64) == 0);
    CHECK(r.tri_count == 1);

    /* Vertex 0 is at the origin, so it lands exactly on the translation. */
    CHECK_NEAR(r.tri[0][0].x, 100.0f, 0.01f);
    CHECK_NEAR(r.tri[0][0].y, 0.0f, 0.01f);

    gfxStateDestroy(st);
}

static void test_matrix_stack(void)
{
    recorder r;
    gfx_state *st;
    Gfx *p;
    float m[16];
    static s32 packed[16];

    printf("gfx: matrix push and pop balance\n");

    memset(m, 0, sizeof(m));
    m[0] = m[5] = m[10] = m[15] = 1.0f;
    float_to_mtx(m, packed);

    gbiResetSegments();
    gbiSetSegment(2, packed);

    st = make_state(&r);
    gfxStateBeginFrame(st);
    CHECK(gfxStateMatrixDepth(st) == 0);

    p = g_dl;
    gSPMatrix(p++, 0x02000000u, G_MTX_MODELVIEW | G_MTX_LOAD | G_MTX_PUSH);
    gSPMatrix(p++, 0x02000000u, G_MTX_MODELVIEW | G_MTX_LOAD | G_MTX_PUSH);
    gSPEndDisplayList(p++);
    CHECK(gfxStateRun(st, g_dl, 64) == 0);
    CHECK(gfxStateMatrixDepth(st) == 2);

    p = g_dl;
    gSPPopMatrix(p++, G_MTX_MODELVIEW);
    gSPEndDisplayList(p++);
    CHECK(gfxStateRun(st, g_dl, 64) == 0);
    CHECK(gfxStateMatrixDepth(st) == 1);

    /* Popping past the bottom must clamp, not underflow into the stack. */
    p = g_dl;
    gSPPopMatrix(p++, G_MTX_MODELVIEW);
    gSPPopMatrix(p++, G_MTX_MODELVIEW);
    gSPPopMatrix(p++, G_MTX_MODELVIEW);
    gSPEndDisplayList(p++);
    CHECK(gfxStateRun(st, g_dl, 64) == 0);
    CHECK(gfxStateMatrixDepth(st) == 0);

    gfxStateDestroy(st);
}

/* --------------------------------------------------------------- misc */

static void test_tri4_through_state(void)
{
    recorder r;
    gfx_state *st;
    Gfx *p;

    printf("gfx: a G_TRI4 emits the right number of triangles\n");

    build_verts();
    gbiResetSegments();
    gbiSetSegment(1, g_verts);

    st = make_state(&r);
    gfxStateBeginFrame(st);

    p = g_dl;
    gSPVertex(p++, 0x01000000u, 8, 0);
    gSP4Triangles(p++, 1, 2, 3, 4, 5, 6, 0, 0, 0, 0, 0, 0);
    gSPEndDisplayList(p++);

    CHECK(gfxStateRun(st, g_dl, 64) == 0);
    /* Two real triangles, not four: the empty slots are padding. */
    CHECK(r.tri_count == 2);
    CHECK(gfxStateTriangleCount(st) == 2);

    gfxStateDestroy(st);
}

static void test_unsupported_reaches_backend(void)
{
    recorder r;
    gfx_state *st;
    Gfx *p;

    printf("gfx: unsupported commands are reported, not dropped\n");

    st = make_state(&r);
    gfxStateBeginFrame(st);

    p = g_dl;
    gDPSetBlendColor(p++, 0xff, 0xff, 0xff, 0xff);
    gDPSetPrimDepth(p++, 0xffff, 0xffff);
    gSPEndDisplayList(p++);

    CHECK(gfxStateRun(st, g_dl, 64) == 0);
    CHECK(r.unsupported_count == 2);

    gfxStateDestroy(st);
}

static void test_state_passthrough(void)
{
    recorder r;
    gfx_state *st;
    Gfx *p;

    printf("gfx: geometry mode and colours reach the backend\n");

    st = make_state(&r);
    gfxStateBeginFrame(st);

    p = g_dl;
    gSPSetGeometryMode(p++, G_SHADE);
    gSPSetGeometryMode(p++, G_SHADING_SMOOTH);
    gDPSetPrimColor(p++, 0, 0, 1, 2, 3, 4);
    gSPEndDisplayList(p++);

    CHECK(gfxStateRun(st, g_dl, 64) == 0);
    CHECK(r.geometry_mode_calls == 2);
    /* Set accumulates rather than replacing. */
    CHECK((r.last_geometry_mode & (unsigned)G_SHADE) != 0);
    CHECK((r.last_geometry_mode & (unsigned)G_SHADING_SMOOTH) != 0);
    CHECK(r.prim_color_calls == 1);

    /* Clear removes only the named bits. */
    p = g_dl;
    gSPClearGeometryMode(p++, G_SHADING_SMOOTH);
    gSPEndDisplayList(p++);
    CHECK(gfxStateRun(st, g_dl, 64) == 0);
    CHECK((r.last_geometry_mode & (unsigned)G_SHADE) != 0);
    CHECK((r.last_geometry_mode & (unsigned)G_SHADING_SMOOTH) == 0);

    gfxStateDestroy(st);
}

static void test_texture_scale(void)
{
    recorder r;
    gfx_state *st;
    Gfx *p;

    printf("gfx: gSPTexture scale reaches the backend\n");

    st = make_state(&r);
    gfxStateBeginFrame(st);

    p = g_dl;
    gSPTexture(p++, 0x8000, 0x8000, 0, G_TX_RENDERTILE, G_ON);
    gSPEndDisplayList(p++);

    CHECK(gfxStateRun(st, g_dl, 64) == 0);
    CHECK(r.tex_scale_calls == 1);
    /* 0x8000 is exactly a half. */
    CHECK(r.last_tex_s > 0.499f && r.last_tex_s < 0.501f);
    CHECK(r.last_tex_t > 0.499f && r.last_tex_t < 0.501f);
    CHECK(r.last_tex_tile == G_TX_RENDERTILE);
    CHECK(r.last_tex_on != 0);

    /* Turning texturing off must be visible, not just a scale of zero. */
    p = g_dl;
    gSPTexture(p++, 0xFFFF, 0xFFFF, 0, G_TX_RENDERTILE, G_OFF);
    gSPEndDisplayList(p++);
    CHECK(gfxStateRun(st, g_dl, 64) == 0);
    CHECK(r.last_tex_on == 0);
    CHECK(r.last_tex_s > 0.99f);

    gfxStateDestroy(st);
}

static void test_malformed(void)
{
    recorder r;
    gfx_state *st;
    Gfx *p;

    printf("gfx: malformed lists are refused\n");

    st = make_state(&r);
    gfxStateBeginFrame(st);

    CHECK(gfxStateRun(st, NULL, 64) == -1);

    /* Calling into an unbound segment. */
    gbiResetSegments();
    p = g_dl;
    gSPDisplayList(p++, 0x09000000u);
    gSPEndDisplayList(p++);
    CHECK(gfxStateRun(st, g_dl, 64) == -1);

    gfxStateDestroy(st);
}

int main(void)
{
    platformInit();

    printf("ge007 port: renderer state machine tests\n\n");

    test_matrix_decode();
    test_tri1_indices();
    test_tri4_indices();
    test_vertex_load_and_draw();
    test_vertex_translation();
    test_matrix_stack();
    test_tri4_through_state();
    test_unsupported_reaches_backend();
    test_state_passthrough();
    test_texture_scale();
    test_malformed();

    printf("\n%d checks, %d failure(s)\n", g_checks, g_failures);
    platformShutdown();
    return g_failures ? 1 : 0;
}
