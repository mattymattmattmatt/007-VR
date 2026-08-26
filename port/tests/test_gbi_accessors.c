/*
 * Tests for the portable display-list accessors in include/gbi_extension.h.
 *
 * gbi.h overlays Gdma/Gtri/Gloadtile on the command words as bit-fields, but
 * only on a big-endian 32-bit target -- it excludes them everywhere else and
 * says so in a comment there. Three routines in the game read a display list
 * from the CPU (bg.c bounds room vertices, lightfixture.c resolves the
 * vertices behind a triangle, unk_092E50.c animates tile origins in place), so
 * they now go through the GFX_* accessors instead.
 *
 * That is a change to code the ROM build also compiles, so unlike the
 * GEPC-guarded changes it cannot be justified by "the token stream is
 * identical". Instead every case here builds its command with the SDK's own
 * gs* encoder and checks the accessor recovers what was encoded. The encoder
 * is the authority: if a define flips and a field moves, these fail rather
 * than quietly decoding the wrong bits.
 */
#include "platform.h"

#include <stdio.h>
#include <string.h>

#include <PR/ultratypes.h>
#include <PR/mbi.h>
#include <PR/gbi.h>
#include <gbi_extension.h>

static int g_failures;
static int g_checks;

static void check_impl(int cond, const char *what, const char *file, int line)
{
    g_checks++;
    if (!cond) {
        g_failures++;
        printf("  FAIL %s:%d: %s\n", file, line, what);
    }
}

#define CHECK(cond) check_impl((cond) ? 1 : 0, #cond, __FILE__, __LINE__)

/* Standing in for real vertex data; only its address is ever read back. */
static Vtx g_vertices[32];

/*
 * The premise of the GEPC branch of Gwords. A display list command is 64 bits
 * on the N64, and every list the game reads out of the ROM is laid out that
 * way, so a 16-byte Gfx would desynchronise the walker on the first command.
 */
static void test_command_is_eight_bytes(void)
{
    printf("command size\n");
    CHECK(sizeof(Gfx) == 8);
    CHECK(sizeof(Gwords) == 8);
}

/*
 * The sign-extension case, and the reason GFX_CMD casts through s8. gbi.h
 * declares the opcode as `int cmd:8` and gives the immediate-mode commands
 * negative values -- G_IMMFIRST is -65, so G_ENDDL is -72. An accessor that
 * returned a plain unsigned byte would compare 184 against -72, never match,
 * and walk off the end of every list bg.c scans.
 */
static void test_opcode_sign_extends(void)
{
    /* Not static: a command holding a pointer cannot be a load-time constant
     * once the words are 32 bits wide, because narrowing an address is not an
     * address constant in C.
     *
     * The game does have one such list -- debugmenu.c builds a static Gfx[]
     * around gsDPLoadTextureBlock(&g_DebugMenuTexture, ...) -- so this is a
     * real constraint the port still owes an answer to, not a curiosity.
     * These tests build their lists on the stack to stay clear of it. */
    const Gfx enddl[] = { gsSPEndDisplayList() };
    const Gfx vtx[]   = { gsSPVertex(g_vertices, 4, 0) };

    printf("opcode sign extension\n");

    CHECK(GFX_CMD(&enddl[0]) == G_ENDDL);
    CHECK(GFX_CMD(&vtx[0]) == G_VTX);

    /* Guard the property rather than the constant: if G_ENDDL stopped being
     * negative the cast would be pointless and this test would be lying. */
    CHECK(G_ENDDL < 0);
    CHECK(GFX_CMD(&enddl[0]) < 0);
}

/*
 * bg.c reads the vertex count as ((par >> 4) & 0xf) + 1 and the address
 * through SEGMENT_OFFSET, so those are the invariants worth pinning.
 */
static void test_vertex_command(void)
{
    const Gfx dl[] = { gsSPVertex(g_vertices, 7, 0) };
    u32 count;

    printf("gsSPVertex\n");

    CHECK(GFX_CMD(&dl[0]) == G_VTX);

    count = ((GFX_DMA_PAR(&dl[0]) >> 4) & 0xf) + 1u;
    CHECK(count == 7u);

    CHECK(GFX_DMA_ADDR(&dl[0]) == (u32)(uintptr_t)g_vertices);
}

/*
 * lightfixture.c divides each index by 10, which is F3DEX's vertex stride.
 * Encoding through gsSP1Triangle keeps that assumption honest.
 */
static void test_triangle_indices(void)
{
    const Gfx dl[] = { gsSP1Triangle(3, 8, 15, 0) };

    printf("gsSP1Triangle\n");

    CHECK(GFX_CMD(&dl[0]) == G_TRI1);
    CHECK(GFX_TRI_V(&dl[0], 0) / 10u == 3u);
    CHECK(GFX_TRI_V(&dl[0], 1) / 10u == 8u);
    CHECK(GFX_TRI_V(&dl[0], 2) / 10u == 15u);
}

/*
 * unk_092E50.c writes sl and tl on an existing command every frame, so the
 * setters have to land in the right place *and* leave the rest of the command
 * alone -- a setter that clobbered the tile number or the lower-right corner
 * would still animate, just wrongly.
 */
static void test_tile_origin_round_trip(void)
{
    Gfx dl[] = { gsDPSetTileSize(2, 40, 56, 100, 120) };
    Gfx original = dl[0];

    printf("tile origin setters\n");

    CHECK(GFX_TILE_SL(&dl[0]) == 40u);
    CHECK(GFX_TILE_TL(&dl[0]) == 56u);

    GFX_SET_TILE_SL(&dl[0], 90);
    GFX_SET_TILE_TL(&dl[0], 150);

    CHECK(GFX_TILE_SL(&dl[0]) == 90u);
    CHECK(GFX_TILE_TL(&dl[0]) == 150u);

    /* Opcode and the whole second word (tile, sh, th) must be untouched. */
    CHECK(GFX_CMD(&dl[0]) == GFX_CMD(&original));
    CHECK(dl[0].words.w1 == original.words.w1);

    /* The call sites assign floats; truncation must go through s32, since
     * float-to-unsigned is undefined for a negative value. */
    GFX_SET_TILE_SL(&dl[0], 12.75f);
    CHECK(GFX_TILE_SL(&dl[0]) == 12u);

    /* Values wider than the field wrap, exactly as a 12-bit bit-field does. */
    GFX_SET_TILE_TL(&dl[0], 0x1234);
    CHECK(GFX_TILE_TL(&dl[0]) == 0x234u);
}

int main(void)
{
    printf("gbi accessor tests\n");

    test_command_is_eight_bytes();
    test_opcode_sign_extends();
    test_vertex_command();
    test_triangle_indices();
    test_tile_origin_round_trip();

    printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
