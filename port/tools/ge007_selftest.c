/*
 * ge007-selftest - drives the whole port stack end to end.
 *
 * Not a unit test. This builds a display list in the RDRAM arena exactly as
 * the game would, submits it through the real SP task interception, lets it
 * flow through gfx_state into the GL backend, then reads the framebuffer back
 * and checks pixels actually changed. It does the same for a frame of audio
 * through the microcode and the output ring.
 *
 * The point is to exercise the seams unit tests cannot: real GL, real segment
 * resolution, real pointer round trips.
 */
#include "audio.h"
#include "audio_abi.h"
#include "gbi_walk.h"
#include "gfxhook.h"
#include "platform.h"
#include "rdram.h"
#include "video.h"

#include <SDL2/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <PR/ultratypes.h>
#include <PR/os.h>
#include <PR/sptask.h>
#include <PR/mbi.h>
#include <PR/gbi.h>
#include <gbi_extension.h>
#include <PR/abi.h>

static int g_fail;

static void ok(int cond, const char *what)
{
    printf("  [%s] %s\n", cond ? " ok " : "FAIL", what);
    if (!cond) {
        g_fail++;
    }
}

static void stage_rdram(void)
{
    unsigned long long base, end;

    printf("\n== RDRAM ==\n");

    ok(rdramInit() == 0, "arena reserved");
    if (!rdramIsReady()) {
        return;
    }

    base = (unsigned long long)(size_t)rdramBase();
    end = base + rdramSize();
    printf("       base=0x%08llx end=0x%08llx\n", base, end);

    ok(end <= 0x100000000ULL, "arena below 4 GB");

    /* Segmented addressing puts the segment index in bits 24-27. On hardware
     * RDRAM is physical 0x00000000-0x007fffff, so a direct pointer always has
     * segment index 0 and cannot be confused with a segmented one. The arena
     * must preserve that, or every direct address in a display list gets
     * mistaken for a segment reference. */
    {
        unsigned seg_lo = (unsigned)((base >> 24) & 0x0Fu);
        unsigned seg_hi = (unsigned)(((end - 1) >> 24) & 0x0Fu);
        printf("       segment index of arena: %u..%u\n", seg_lo, seg_hi);
        ok(seg_lo == 0 && seg_hi == 0,
           "arena addresses carry segment index 0");
    }

    {
        void *p = rdramAlloc(64, 16);
        ok(p != NULL && rdramContains(p), "arena allocation works");
        ok(osPhysicalToVirtual(osVirtualToPhysical(p)) == p,
           "pointer survives osVirtualToPhysical");
    }
}

static Vtx *g_verts;
static Gfx *g_dl;

static void build_display_list(void)
{
    Gfx *p;
    int i;

    g_verts = (Vtx *)rdramAlloc(sizeof(Vtx) * 8, 16);
    g_dl = (Gfx *)rdramAlloc(sizeof(Gfx) * 64, 16);
    if (!g_verts || !g_dl) {
        return;
    }

    memset(g_verts, 0, sizeof(Vtx) * 8);
    for (i = 0; i < 3; i++) {
        g_verts[i].v.cn[0] = 255;
        g_verts[i].v.cn[1] = (unsigned char)(i * 100);
        g_verts[i].v.cn[2] = 0;
        g_verts[i].v.cn[3] = 255;
    }
    g_verts[0].v.ob[0] = -1; g_verts[0].v.ob[1] = -1;
    g_verts[1].v.ob[0] =  1; g_verts[1].v.ob[1] = -1;
    g_verts[2].v.ob[0] =  0; g_verts[2].v.ob[1] =  1;

    p = g_dl;
    gDPPipeSync(p++);
    gSPSetGeometryMode(p++, G_SHADE | G_SHADING_SMOOTH);
    gSPTexture(p++, 0xFFFF, 0xFFFF, 0, G_TX_RENDERTILE, G_OFF);
    gSPVertex(p++, g_verts, 3, 0);
    gSP1Triangle(p++, 0, 1, 2, 0);
    gSPEndDisplayList(p++);
}

static void stage_display_list(void)
{
    gbi_stats st;

    printf("\n== display list ==\n");

    build_display_list();
    ok(g_verts != NULL && g_dl != NULL, "list and vertices allocated in RDRAM");
    if (!g_dl) {
        return;
    }

    /* Direct addresses must resolve without any gSPSegment having been
     * issued, exactly as on hardware. */
    gbiResetSegments();
    {
        const void *r = gbiResolve((unsigned)(size_t)g_verts);
        printf("       gbiResolve(%p) -> %p\n", (void *)g_verts, (void *)r);
        ok(r == g_verts, "a direct pointer resolves to itself");
    }

    ok(gbiWalk(g_dl, 64, &st) == 0, "list walks cleanly");
    printf("       commands=%u triangles=%u vertices=%u\n",
           st.commands, st.triangles, st.vertices);
    ok(st.triangles == 1, "one triangle counted");
    ok(st.vertices == 3, "three vertices counted");
}

static void stage_render(void)
{
    OSTask task;
    unsigned char *pixels;
    int w = 320, h = 240;
    int nonblack = 0;
    int i;
    void (*p_glReadPixels)(int, int, int, int, unsigned, unsigned, void *);

    printf("\n== render ==\n");

    if (videoInit(w, h, "ge007 selftest") != 0) {
        ok(0, "video came up");
        return;
    }
    ok(videoIsReady(), "video came up");

    /* Keep the finished frame in the back buffer so it can be read back. */
    videoSetPresentEnabled(0);

    memset(&task, 0, sizeof(task));
    task.t.type = M_GFXTASK;
    task.t.data_ptr = (u64 *)g_dl;
    task.t.data_size = 6 * (u32)sizeof(Gfx);

    osSpTaskLoad(&task);
    osSpTaskStartGo(&task);

    ok(videoFrameCount() == 1, "one frame rendered");
    printf("       triangles=%u drawcalls=%u\n",
           videoTriangleCount(), videoDrawCallCount());
    ok(videoTriangleCount() == 1, "the triangle reached the backend");
    ok(videoDrawCallCount() >= 1, "a draw call was issued");

    p_glReadPixels = (void (*)(int, int, int, int, unsigned, unsigned, void *))
                     SDL_GL_GetProcAddress("glReadPixels");
    if (!p_glReadPixels) {
        ok(0, "glReadPixels available");
        return;
    }

    pixels = (unsigned char *)calloc((size_t)w * (size_t)h, 4);
    if (!pixels) {
        return;
    }
    p_glReadPixels(0, 0, w, h, 0x1908, 0x1401, pixels);

    for (i = 0; i < w * h; i++) {
        if (pixels[i * 4] || pixels[i * 4 + 1] || pixels[i * 4 + 2]) {
            nonblack++;
        }
    }
    printf("       non-black pixels: %d of %d\n", nonblack, w * h);
    ok(nonblack > 100, "pixels were actually rasterised");

    free(pixels);
}

/* ------------------------------------------------------------ stage 3b */

static unsigned count_nonblack(int w, int h, int *out_b)
{
    void (*p_glReadPixels)(int, int, int, int, unsigned, unsigned, void *);
    unsigned char *pixels;
    unsigned n = 0;
    long btotal = 0;
    int i;

    p_glReadPixels = (void (*)(int, int, int, int, unsigned, unsigned, void *))
                     SDL_GL_GetProcAddress("glReadPixels");
    if (!p_glReadPixels) {
        return 0;
    }
    pixels = (unsigned char *)calloc((size_t)w * (size_t)h, 4);
    if (!pixels) {
        return 0;
    }
    p_glReadPixels(0, 0, w, h, 0x1908, 0x1401, pixels);

    for (i = 0; i < w * h; i++) {
        if (pixels[i * 4] || pixels[i * 4 + 1] || pixels[i * 4 + 2]) {
            n++;
            btotal += pixels[i * 4 + 2];
        }
    }
    if (out_b) {
        *out_b = n ? (int)(btotal / (long)n) : 0;
    }
    free(pixels);
    return n;
}

static void submit(const Gfx *dl, unsigned commands)
{
    OSTask task;

    memset(&task, 0, sizeof(task));
    task.t.type = M_GFXTASK;
    task.t.data_ptr = (u64 *)(uintptr_t)dl;
    task.t.data_size = commands * (u32)sizeof(Gfx);
    osSpTaskLoad(&task);
    osSpTaskStartGo(&task);
}

/* ------------------------------------------------------------ stage 3c */

/*
 * Averages the colour of everything that got rasterised. Which pixels the
 * triangle lands on depends on the transform, so sampling one is fragile;
 * what matters is that the shape came out the colour the combiner asked for.
 */
static void average_drawn_colour(int w, int h, int *r, int *g, int *b)
{
    void (*p_glReadPixels)(int, int, int, int, unsigned, unsigned, void *);
    unsigned char *px;
    long sr = 0, sg = 0, sb = 0, n = 0;
    int i;

    *r = *g = *b = -1;

    p_glReadPixels = (void (*)(int, int, int, int, unsigned, unsigned, void *))
                     SDL_GL_GetProcAddress("glReadPixels");
    if (!p_glReadPixels) {
        return;
    }
    px = (unsigned char *)calloc((size_t)w * (size_t)h, 4);
    if (!px) {
        return;
    }
    p_glReadPixels(0, 0, w, h, 0x1908, 0x1401, px);

    for (i = 0; i < w * h; i++) {
        int pr = px[i * 4], pg = px[i * 4 + 1], pb = px[i * 4 + 2];
        if (pr || pg || pb) {
            sr += pr; sg += pg; sb += pb; n++;
        }
    }
    if (n) {
        *r = (int)(sr / n);
        *g = (int)(sg / n);
        *b = (int)(sb / n);
    }
    free(px);
}

/*
 * The colour combiner, checked by looking at the pixels rather than at the
 * decoder.
 *
 * Both cases below draw a triangle whose shade is pure white while the
 * primitive colour is pure red, and differ only in which the combiner
 * selects. That is deliberately the one thing the old "texture * shade"
 * approximation could not get right: it ignored the combiner entirely and
 * would render both cases white. A decoder test alone would not have caught
 * that, in the same way the earlier five bugs all passed their unit tests
 * while the screen stayed black.
 */
static void stage_combiner(void)
{
    Vtx *v;
    Gfx *dl;
    Gfx *p;
    int i, r, g, b;
    const int w = 320, h = 240;

    printf("\n== colour combiner ==\n");

    v  = (Vtx *)rdramAlloc(sizeof(Vtx) * 4, 16);
    dl = (Gfx *)rdramAlloc(sizeof(Gfx) * 16, 16);
    if (!v || !dl) {
        ok(0, "combiner test allocated");
        return;
    }

    memset(v, 0, sizeof(Vtx) * 4);
    for (i = 0; i < 3; i++) {
        v[i].v.cn[0] = 255; v[i].v.cn[1] = 255;   /* white shade */
        v[i].v.cn[2] = 255; v[i].v.cn[3] = 255;
    }
    v[0].v.ob[0] = -1; v[0].v.ob[1] = -1;
    v[1].v.ob[0] =  1; v[1].v.ob[1] = -1;
    v[2].v.ob[0] =  0; v[2].v.ob[1] =  1;

    /* Case one: the combiner selects PRIMITIVE, which is red. */
    p = dl;
    gDPPipeSync(p++);
    gSPSetGeometryMode(p++, G_SHADE | G_SHADING_SMOOTH);
    gSPTexture(p++, 0xFFFF, 0xFFFF, 0, G_TX_RENDERTILE, G_OFF);
    gDPSetCombineMode(p++, G_CC_PRIMITIVE, G_CC_PRIMITIVE);
    gDPSetPrimColor(p++, 0, 0, 255, 0, 0, 255);
    gSPVertex(p++, v, 3, 0);
    gSP1Triangle(p++, 0, 1, 2, 0);
    gSPEndDisplayList(p++);
    submit(dl, 8);

    average_drawn_colour(w, h, &r, &g, &b);
    printf("       G_CC_PRIMITIVE with white shade -> rgb(%d,%d,%d)\n", r, g, b);
    ok(r > 200 && g < 60 && b < 60,
       "the combiner selected the primitive colour, not shade");

    /* Case two: same geometry, same primitive colour, but the combiner picks
     * SHADE. If the first case passed by accident -- by always using prim --
     * this one fails. */
    for (i = 0; i < 3; i++) {
        v[i].v.cn[0] = 0; v[i].v.cn[1] = 255; v[i].v.cn[2] = 0;
    }
    p = dl;
    gDPPipeSync(p++);
    gSPSetGeometryMode(p++, G_SHADE | G_SHADING_SMOOTH);
    gSPTexture(p++, 0xFFFF, 0xFFFF, 0, G_TX_RENDERTILE, G_OFF);
    gDPSetCombineMode(p++, G_CC_SHADE, G_CC_SHADE);
    gDPSetPrimColor(p++, 0, 0, 255, 0, 0, 255);
    gSPVertex(p++, v, 3, 0);
    gSP1Triangle(p++, 0, 1, 2, 0);
    gSPEndDisplayList(p++);
    submit(dl, 8);

    average_drawn_colour(w, h, &r, &g, &b);
    printf("       G_CC_SHADE with green shade      -> rgb(%d,%d,%d)\n", r, g, b);
    ok(g > 200 && r < 60 && b < 60,
       "the combiner selected shade, not the primitive colour");
}

static void stage_segments(void)
{
    Gfx *sub;
    Gfx *dl;
    Gfx *p;
    unsigned before;

    printf("\n== segments ==\n");

    sub = (Gfx *)rdramAlloc(sizeof(Gfx) * 8, 16);
    dl = (Gfx *)rdramAlloc(sizeof(Gfx) * 16, 16);
    if (!sub || !dl) {
        ok(0, "allocated segment test lists");
        return;
    }

    p = sub;
    gSPVertex(p++, g_verts, 3, 0);
    gSP1Triangle(p++, 0, 1, 2, 0);
    gSPEndDisplayList(p++);

    /* gSPSegment is a G_MOVEWORD. If that command is not dispatched, the
     * segmented call below resolves to nothing and silently draws zero
     * triangles. */
    p = dl;
    gSPSegment(p++, 0x0A, sub);
    gSPDisplayList(p++, 0x0A000000u);
    gSPEndDisplayList(p++);

    before = videoTriangleCount();
    submit(dl, 3);
    printf("       triangles after segmented call: %u (was %u)\n",
           videoTriangleCount(), before);
    ok(videoTriangleCount() == 1, "gSPSegment bound and the segmented list ran");
}

static void stage_texture(void)
{
    unsigned char *tex;
    Gfx *dl;
    Gfx *p;
    int i;
    unsigned nonblack;
    int avg_blue = 0;

    printf("\n== texture ==\n");

    /* 8x8 RGBA16, solid blue: b=31, a=1 -> 0x003f, big endian. */
    tex = (unsigned char *)rdramAlloc(8 * 8 * 2, 16);
    dl = (Gfx *)rdramAlloc(sizeof(Gfx) * 32, 16);
    if (!tex || !dl) {
        ok(0, "allocated texture and list");
        return;
    }
    for (i = 0; i < 8 * 8; i++) {
        tex[i * 2] = 0x00;
        tex[i * 2 + 1] = 0x3F;
    }

    /* White shade so the texture colour survives texture * shade. */
    for (i = 0; i < 3; i++) {
        g_verts[i].v.cn[0] = 255;
        g_verts[i].v.cn[1] = 255;
        g_verts[i].v.cn[2] = 255;
        g_verts[i].v.cn[3] = 255;
        g_verts[i].v.tc[0] = 0;
        g_verts[i].v.tc[1] = 0;
    }

    p = dl;
    gDPPipeSync(p++);
    gSPTexture(p++, 0xFFFF, 0xFFFF, 0, G_TX_RENDERTILE, G_ON);
    gDPSetTextureImage(p++, G_IM_FMT_RGBA, G_IM_SIZ_16b, 8, tex);
    gDPSetTile(p++, G_IM_FMT_RGBA, G_IM_SIZ_16b, 2, 0, G_TX_RENDERTILE,
               0, 0, 0, 0, 0, 0, 0);
    gDPLoadBlock(p++, G_TX_RENDERTILE, 0, 0, 8 * 8 - 1, 0);
    gDPSetTileSize(p++, G_TX_RENDERTILE, 0, 0, (8 - 1) << 2, (8 - 1) << 2);
    gSPVertex(p++, g_verts, 3, 0);
    gSP1Triangle(p++, 0, 1, 2, 0);
    gSPEndDisplayList(p++);

    submit(dl, 9);

    nonblack = count_nonblack(320, 240, &avg_blue);
    printf("       non-black=%u  average blue=%d\n", nonblack, avg_blue);
    ok(nonblack > 100, "textured triangle rasterised");
    /* Blue texture on white shade should come out clearly blue. */
    ok(avg_blue > 150, "the texture's colour reached the framebuffer");
}

static void put_cmd(unsigned char *p, unsigned w0, unsigned w1)
{
    p[0] = (unsigned char)(w0 >> 24); p[1] = (unsigned char)(w0 >> 16);
    p[2] = (unsigned char)(w0 >> 8);  p[3] = (unsigned char)w0;
    p[4] = (unsigned char)(w1 >> 24); p[5] = (unsigned char)(w1 >> 16);
    p[6] = (unsigned char)(w1 >> 8);  p[7] = (unsigned char)w1;
}

static void stage_audio(void)
{
    audio_abi *abi;
    unsigned char cmds[32];
    short *out;
    short pulled[64];
    int i;
    int nonzero = 0;

    printf("\n== audio ==\n");

    abi = audioAbiCreate();
    ok(abi != NULL, "microcode created");
    if (!abi) {
        return;
    }

    out = (short *)rdramAlloc(256, 16);
    ok(out != NULL, "output buffer allocated in RDRAM");
    if (!out) {
        audioAbiDestroy(abi);
        return;
    }
    memset(out, 0, 256);

    for (i = 0; i < 64; i++) {
        audioAbiPoke16(abi, (unsigned)(0x100 + i * 2), (short)(i * 400));
    }

    put_cmd(cmds, (A_SETBUFF << 24) | 0x100u, (0x100u << 16) | 128u);
    ok(audioAbiRun(abi, cmds, 8) == 0, "SETBUFF ran");

    put_cmd(cmds, (A_SAVEBUFF << 24), (unsigned)(size_t)out);
    ok(audioAbiRun(abi, cmds, 8) == 0, "SAVEBUFF ran");

    for (i = 0; i < 64; i++) {
        int v = (int)(short)(((unsigned)((unsigned char *)out)[i * 2] << 8)
                             | ((unsigned char *)out)[i * 2 + 1]);
        if (v != 0) {
            nonzero++;
        }
    }
    printf("       non-zero samples saved: %d of 64\n", nonzero);
    ok(nonzero > 50, "audio reached RDRAM through the microcode");

    audioInit();
    audioFlush();
    osAiSetFrequency(22050);
    ok(osAiSetNextBuffer(out, 256) == 0, "queued to the DAC");
    ok(osAiGetLength() == 256, "queue depth reported truthfully");
    ok(audioPullBytes(pulled, sizeof(pulled)) == sizeof(pulled), "device drained it");
    ok(osAiGetLength() == 256 - sizeof(pulled), "depth fell after draining");

    audioAbiDestroy(abi);
}

int main(void)
{
    platformInit();

    printf("ge007 port self-test\n====================\n");

    stage_rdram();
    stage_display_list();
    stage_render();
    stage_combiner();
    stage_segments();
    stage_texture();
    stage_audio();

    printf("\n====================\n%s (%d failure(s))\n",
           g_fail ? "FAILURES" : "all stages passed", g_fail);

    videoShutdown();
    audioShutdown();
    rdramShutdown();
    platformShutdown();
    return g_fail ? 1 : 0;
}
