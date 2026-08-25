/*
 * Tests for the display-list walker.
 *
 * Every list here is built with the game's own GBI macros rather than
 * hand-written words. That matters: it means the tests validate the walker
 * against whatever encoding the build actually selects, instead of against my
 * reading of which preprocessor branch wins. If a define changes and G_VTX
 * starts packing its count somewhere else, these fail rather than silently
 * decoding nonsense.
 */
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

#define CHECK(c) check((c), #c, __FILE__, __LINE__)

static Vtx g_verts[32];

/* --------------------------------------------------------------- basics */

static void test_empty_list(void)
{
    Gfx dl[4];
    Gfx *p = dl;
    gbi_stats st;

    printf("walk: a bare end-of-list terminates\n");

    gSPEndDisplayList(p++);

    CHECK(gbiWalk(dl, 64, &st) == 0);
    CHECK(st.commands == 1);
    CHECK(st.triangles == 0);
    CHECK(st.max_depth == 0);
    CHECK(st.unsupported == 0);

    /* A null list is an error, not a crash. */
    CHECK(gbiWalk(NULL, 64, &st) == -1);

    /* stats is optional. */
    CHECK(gbiWalk(dl, 64, NULL) == 0);
}

static void test_unterminated(void)
{
    static Gfx dl[64];
    gbi_stats st;
    int i;

    printf("walk: an unterminated list is refused, not chased\n");

    /* Fill with pipe syncs and never end it. Bounded to exactly the array's
     * length, so the walker stops at the last valid element rather than
     * reading past it -- which is the only way this can be tested safely. */
    for (i = 0; i < 64; i++) {
        Gfx *p = &dl[i];
        gDPPipeSync(p);
    }

    CHECK(gbiWalk(dl, 64, &st) == -1);
    CHECK(st.commands == 64);
}

/* ------------------------------------------------------------ geometry */

static void test_tri1(void)
{
    Gfx dl[8];
    Gfx *p = dl;
    gbi_stats st;

    printf("walk: G_TRI1 counts one triangle\n");

    gSP1Triangle(p++, 0, 1, 2, 0);
    gSP1Triangle(p++, 3, 4, 5, 0);
    gSPEndDisplayList(p++);

    CHECK(gbiWalk(dl, 64, &st) == 0);
    CHECK(st.triangles == 2);
    CHECK(st.commands == 3);
}

static void test_tri4_padding(void)
{
    Gfx dl[8];
    Gfx *p = dl;
    gbi_stats st;

    printf("walk: G_TRI4 counts real triangles and skips padding\n");

    /* Four real triangles. */
    gSP4Triangles(p++, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12);
    gSPEndDisplayList(p++);

    CHECK(gbiWalk(dl, 64, &st) == 0);
    CHECK(st.triangles == 4);

    /* gSP2Triangles is redefined in terms of G_TRI4 with two empty slots.
     * Those slots are padding, not degenerate geometry, and counting them
     * would inflate every model in the game by half. */
    p = dl;
    gSP2Triangles(p++, 1, 2, 3, 0, 4, 5, 6, 0);
    gSPEndDisplayList(p++);

    CHECK(gbiWalk(dl, 64, &st) == 0);
    CHECK(st.triangles == 2);

    /* A single triangle packed into a tri4 slot. */
    p = dl;
    gSP4Triangles(p++, 1, 2, 3, 0, 0, 0, 0, 0, 0, 0, 0, 0);
    gSPEndDisplayList(p++);

    CHECK(gbiWalk(dl, 64, &st) == 0);
    CHECK(st.triangles == 1);
}

static void test_vertices(void)
{
    Gfx dl[8];
    Gfx *p = dl;
    gbi_stats st;

    printf("walk: G_VTX count decodes to what the macro encoded\n");

    /* Built with the real macro, so this checks the walker's field decode
     * against the build's actual encoding rather than an assumption. */
    gSPVertex(p++, g_verts, 8, 0);
    gSPEndDisplayList(p++);

    CHECK(gbiWalk(dl, 64, &st) == 0);
    CHECK(st.vertices == 8);

    p = dl;
    gSPVertex(p++, g_verts, 16, 0);
    gSPVertex(p++, g_verts, 1, 0);
    gSPEndDisplayList(p++);

    CHECK(gbiWalk(dl, 64, &st) == 0);
    CHECK(st.vertices == 17);
}

/* --------------------------------------------------------- nested lists */

static Gfx g_child[8];
static Gfx g_grandchild[8];

static void test_nested_and_branch(void)
{
    Gfx dl[16];
    Gfx *p;
    gbi_stats st;

    printf("walk: nested lists return, branches do not\n");

    gbiResetSegments();
    /* Bind segment 1 to the child list, then reference it segmented, exactly
     * as the game does through gSPSegment. */
    gbiSetSegment(1, g_child);

    p = g_grandchild;
    gSP1Triangle(p++, 1, 2, 3, 0);
    gSPEndDisplayList(p++);

    p = g_child;
    gSP1Triangle(p++, 1, 2, 3, 0);
    gSPEndDisplayList(p++);

    p = dl;
    gSP1Triangle(p++, 1, 2, 3, 0);
    gSPDisplayList(p++, 0x01000000u);   /* call into segment 1 */
    gSP1Triangle(p++, 4, 5, 6, 0);
    gSPEndDisplayList(p++);

    CHECK(gbiWalk(dl, 64, &st) == 0);
    /* One before the call, one inside it, one after: the call must return. */
    CHECK(st.triangles == 3);
    CHECK(st.display_lists == 1);
    CHECK(st.max_depth == 1);
    CHECK(st.branches == 0);

    /* A branch does not return, so the triangle after it is never reached. */
    p = dl;
    gSP1Triangle(p++, 1, 2, 3, 0);
    gSPBranchList(p++, 0x01000000u);
    gSP1Triangle(p++, 4, 5, 6, 0);      /* unreachable */
    gSPEndDisplayList(p++);

    CHECK(gbiWalk(dl, 64, &st) == 0);
    CHECK(st.triangles == 2);
    CHECK(st.branches == 1);
    CHECK(st.display_lists == 0);
    CHECK(st.max_depth == 0);
}

static void test_unresolvable_segment(void)
{
    Gfx dl[8];
    Gfx *p = dl;
    gbi_stats st;

    printf("walk: an unbound segment is reported, not followed\n");

    gbiResetSegments();

    p = dl;
    gSPDisplayList(p++, 0x07000000u);   /* segment 7, never bound */
    gSPEndDisplayList(p++);

    /* Following a null here would dereference garbage. */
    CHECK(gbiWalk(dl, 64, &st) == -1);
}

static void test_depth_limit(void)
{
    static Gfx chain[GEPC_GBI_MAX_DEPTH + 4][4];
    gbi_stats st;
    unsigned i;

    printf("walk: runaway nesting is bounded\n");

    gbiResetSegments();

    /* Each list calls the next, deeper than the walker's stack. */
    for (i = 0; i < GEPC_GBI_MAX_DEPTH + 3; i++) {
        Gfx *p = chain[i];
        gbiSetSegment(1, chain[i + 1]);
        gSPDisplayList(p++, 0x01000000u);
        gSPEndDisplayList(p++);
    }

    /* Every level resolves to the same segment binding, so this recurses
     * until the depth limit rather than terminating. */
    gbiSetSegment(1, chain[0]);
    {
        Gfx *p = chain[0];
        gSPDisplayList(p++, 0x01000000u);
        gSPEndDisplayList(p++);
    }

    CHECK(gbiWalk(chain[0], 4096, &st) == -1);
    CHECK(st.max_depth <= GEPC_GBI_MAX_DEPTH);
}

/* -------------------------------------------------- segment resolution */

static void test_segments(void)
{
    unsigned char base[256];

    printf("segments: resolution and offsets\n");

    gbiResetSegments();
    CHECK(gbiResolve(0x01000000u) == NULL);

    gbiSetSegment(1, base);
    CHECK(gbiResolve(0x01000000u) == base);
    CHECK(gbiResolve(0x01000010u) == base + 0x10);
    CHECK(gbiResolve(0x01FFFFFFu) == base + 0xFFFFFF);

    /* Different segment index, same offset field. */
    gbiSetSegment(5, base + 8);
    CHECK(gbiResolve(0x05000004u) == base + 12);

    /* Out-of-range segment indices must not write past the table. */
    gbiSetSegment(99, base);
    CHECK(gbiResolve(0x01000000u) == base);

    gbiResetSegments();
    CHECK(gbiResolve(0x05000000u) == NULL);
}

/* ------------------------------------------------------- unsupported */

static void test_unsupported_detection(void)
{
    Gfx dl[8];
    Gfx *p = dl;
    gbi_stats st;

    printf("walk: the three unsupported commands are flagged\n");

    /* These are the commands GoldenEye can emit that a Perfect-Dark-derived
     * Fast3D backend has no case for. The walker's job is to name them rather
     * than let them pass as ordinary traffic. */
    p = dl;
    gDPSetBlendColor(p++, 0xff, 0xff, 0xff, 0xff);
    gDPSetPrimDepth(p++, 0xffff, 0xffff);
    gSPEndDisplayList(p++);

    CHECK(gbiWalk(dl, 64, &st) == 0);
    CHECK(st.unsupported == 2);
    CHECK(st.unsupported_op_count == 2);

    /* A list with none of them must report a clean bill. */
    p = dl;
    gDPPipeSync(p++);
    gSP1Triangle(p++, 1, 2, 3, 0);
    gSPEndDisplayList(p++);

    CHECK(gbiWalk(dl, 64, &st) == 0);
    CHECK(st.unsupported == 0);
    CHECK(st.unsupported_op_count == 0);
}

static void test_op_names(void)
{
    printf("walk: opcodes have names\n");

    CHECK(strcmp(gbiOpName((unsigned char)G_TRI1), "G_TRI1") == 0);
    CHECK(strcmp(gbiOpName((unsigned char)G_TRI4), "G_TRI4") == 0);
    CHECK(strcmp(gbiOpName((unsigned char)G_VTX), "G_VTX") == 0);
    CHECK(strcmp(gbiOpName((unsigned char)G_ENDDL), "G_ENDDL") == 0);
    CHECK(strcmp(gbiOpName((unsigned char)G_DL), "G_DL") == 0);

    /* G_TRI4 must not collide with G_TRI1: if Rare's extension ever lands on
     * the same slot, geometry silently halves. */
    CHECK((unsigned char)G_TRI4 != (unsigned char)G_TRI1);

    /* Unknown opcodes still return a usable string. */
    CHECK(gbiOpName(0x7F) != NULL);
}

/* ------------------------------------------------------------ mixed */

static void test_realistic_list(void)
{
    Gfx dl[32];
    Gfx *p = dl;
    gbi_stats st;

    printf("walk: a representative model-style list\n");

    gDPPipeSync(p++);
    gSPSetGeometryMode(p++, G_SHADE | G_SHADING_SMOOTH);
    gSPVertex(p++, g_verts, 16, 0);
    gSP4Triangles(p++, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12);
    gSP4Triangles(p++, 1, 2, 3, 4, 5, 6, 0, 0, 0, 0, 0, 0);
    gSP1Triangle(p++, 13, 14, 15, 0);
    gDPPipeSync(p++);
    gSPEndDisplayList(p++);

    CHECK(gbiWalk(dl, 64, &st) == 0);
    CHECK(st.vertices == 16);
    CHECK(st.triangles == 4 + 2 + 1);
    CHECK(st.commands == 8);
    CHECK(st.unsupported == 0);
    CHECK(st.max_depth == 0);
}

int main(void)
{
    platformInit();

    printf("ge007 port: display-list walker tests\n\n");

    test_empty_list();
    test_unterminated();
    test_tri1();
    test_tri4_padding();
    test_vertices();
    test_nested_and_branch();
    test_unresolvable_segment();
    test_depth_limit();
    test_segments();
    test_unsupported_detection();
    test_op_names();
    test_realistic_list();

    printf("\n%d checks, %d failure(s)\n", g_checks, g_failures);
    platformShutdown();
    return g_failures ? 1 : 0;
}
