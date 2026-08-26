/*
 * Tests for the RDP mode-word decoder.
 *
 * Every combiner here is assembled with the SDK's own gsDPSetCombine macros
 * rather than a hand-written bit pattern, so the tests check the decoder
 * against the authoritative encoder instead of against my reading of the
 * packing. If a field ever moves, these fail rather than quietly decoding the
 * wrong operand.
 *
 * The case that matters most is slot ambiguity. The raw operand values mean
 * different things depending on which of a, b, c or d they land in, and
 * nothing about a wrong answer there looks like an error -- the picture is
 * just subtly wrong. So there is a test that puts the *same* raw value in all
 * four slots and checks it decodes four different ways.
 */
#include "gfx_rdp.h"
#include "platform.h"

#include <stdio.h>
#include <string.h>

#include <PR/ultratypes.h>
#include <PR/mbi.h>
#include <PR/gbi.h>

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

static void decode_gfx(const Gfx *g, gfx_combiner *cc)
{
    gfxCombineDecode((unsigned)g->words.w0, (unsigned)g->words.w1, cc);
}

/*
 * G_CC_MODULATEIA is (TEXEL0 - 0) * SHADE + 0 in both colour and alpha, which
 * is the workhorse for lit textured geometry and what the old shader's
 * "texture * shade" was approximating.
 */
static void test_modulate(void)
{
    const Gfx dl[] = { gsDPSetCombineMode(G_CC_MODULATEIA, G_CC_MODULATEIA) };
    gfx_combiner cc;

    printf("G_CC_MODULATEIA\n");
    decode_gfx(&dl[0], &cc);

    CHECK(cc.color[0][0] == GFX_CC_TEXEL0);
    CHECK(cc.color[0][1] == GFX_CC_ZERO);
    CHECK(cc.color[0][2] == GFX_CC_SHADE);
    CHECK(cc.color[0][3] == GFX_CC_ZERO);

    CHECK(cc.alpha[0][0] == GFX_CC_TEXEL0_ALPHA);
    CHECK(cc.alpha[0][1] == GFX_CC_ZERO);
    CHECK(cc.alpha[0][2] == GFX_CC_SHADE_ALPHA);
    CHECK(cc.alpha[0][3] == GFX_CC_ZERO);

    CHECK(gfxCombineUsesTexel(&cc, 0) == 1);
    CHECK(gfxCombineUsesTexel(&cc, 1) == 0);
}

/* Flat shaded, no texture at all: d picks SHADE and everything else is zero. */
static void test_shade_only(void)
{
    const Gfx dl[] = { gsDPSetCombineMode(G_CC_SHADE, G_CC_SHADE) };
    gfx_combiner cc;

    printf("G_CC_SHADE\n");
    decode_gfx(&dl[0], &cc);

    CHECK(cc.color[0][0] == GFX_CC_ZERO);
    CHECK(cc.color[0][3] == GFX_CC_SHADE);
    CHECK(cc.alpha[0][3] == GFX_CC_SHADE_ALPHA);
    CHECK(gfxCombineUsesTexel(&cc, 0) == 0);
}

/* Texture straight through, unlit -- signs, HUD art, muzzle flashes. */
static void test_decal(void)
{
    const Gfx dl[] = { gsDPSetCombineMode(G_CC_DECALRGBA, G_CC_DECALRGBA) };
    gfx_combiner cc;

    printf("G_CC_DECALRGBA\n");
    decode_gfx(&dl[0], &cc);

    CHECK(cc.color[0][3] == GFX_CC_TEXEL0);
    CHECK(cc.alpha[0][3] == GFX_CC_TEXEL0_ALPHA);
    CHECK(gfxCombineUsesTexel(&cc, 0) == 1);
}

/*
 * The reason this decoder exists. Raw 6 is 1 in the a slot, CENTER in b,
 * SCALE in c and 1 again in d; raw 7 is NOISE in a, K4 in b, COMBINED_ALPHA
 * in c and 0 in d. A decoder using one table for every slot passes every test
 * above and still gets these wrong.
 */
static void test_slot_dependent_operands(void)
{
    const Gfx six[]   = { gsDPSetCombineLERP(1, CENTER, SCALE, 1,
                                             1, 1, 1, 1,
                                             1, CENTER, SCALE, 1,
                                             1, 1, 1, 1) };
    const Gfx seven[] = { gsDPSetCombineLERP(NOISE, K4, COMBINED_ALPHA, 0,
                                             0, 0, 0, 0,
                                             NOISE, K4, COMBINED_ALPHA, 0,
                                             0, 0, 0, 0) };
    gfx_combiner cc;

    printf("slot-dependent operands\n");

    decode_gfx(&six[0], &cc);
    CHECK(cc.color[0][0] == GFX_CC_ONE);      /* raw 6 in a */
    CHECK(cc.color[0][1] == GFX_CC_CENTER);   /* raw 6 in b */
    CHECK(cc.color[0][2] == GFX_CC_SCALE);    /* raw 6 in c */
    CHECK(cc.color[0][3] == GFX_CC_ONE);      /* raw 6 in d */

    decode_gfx(&seven[0], &cc);
    CHECK(cc.color[0][0] == GFX_CC_NOISE);            /* raw 7 in a */
    CHECK(cc.color[0][1] == GFX_CC_K4);               /* raw 7 in b */
    CHECK(cc.color[0][2] == GFX_CC_COMBINED_ALPHA);   /* raw 7 in c */
    CHECK(cc.color[0][3] == GFX_CC_ZERO);             /* raw 7 in d */

    /* Same trap on the alpha side: raw 0 is COMBINED in a, b and d but
     * LOD_FRACTION in c. Spelling the names out matters here -- writing a
     * literal 0 in an alpha slot means G_ACMUX_0, which is raw 7, not raw 0. */
    {
        const Gfx zero[] = { gsDPSetCombineLERP(0, 0, 0, 0,
                                                COMBINED, COMBINED,
                                                LOD_FRACTION, COMBINED,
                                                0, 0, 0, 0,
                                                COMBINED, COMBINED,
                                                LOD_FRACTION, COMBINED) };
        gfx_combiner a;
        decode_gfx(&zero[0], &a);
        CHECK(a.alpha[0][0] == GFX_CC_COMBINED_ALPHA);   /* raw 0 in a */
        CHECK(a.alpha[0][1] == GFX_CC_COMBINED_ALPHA);   /* raw 0 in b */
        CHECK(a.alpha[0][2] == GFX_CC_LOD_FRACTION);     /* raw 0 in c */
        CHECK(a.alpha[0][3] == GFX_CC_COMBINED_ALPHA);   /* raw 0 in d */

        /* And a literal 0 really does mean "the constant zero". */
        CHECK(cc.alpha[0][0] == GFX_CC_ZERO);
        CHECK(cc.alpha[0][2] == GFX_CC_ZERO);
    }
}

/* Both cycles are independent, and a two-cycle combiner has to keep them
 * apart -- cycle two commonly feeds on COMBINED from cycle one. */
static void test_two_cycles_differ(void)
{
    const Gfx dl[] = { gsDPSetCombineLERP(TEXEL0, 0, SHADE, 0,
                                          0, 0, 0, TEXEL0,
                                          COMBINED, 0, PRIMITIVE, ENVIRONMENT,
                                          0, 0, 0, COMBINED) };
    gfx_combiner cc;

    printf("two-cycle combiner\n");
    decode_gfx(&dl[0], &cc);

    CHECK(cc.color[0][0] == GFX_CC_TEXEL0);
    CHECK(cc.color[0][2] == GFX_CC_SHADE);

    CHECK(cc.color[1][0] == GFX_CC_COMBINED);
    CHECK(cc.color[1][2] == GFX_CC_PRIMITIVE);
    CHECK(cc.color[1][3] == GFX_CC_ENVIRONMENT);
    CHECK(cc.alpha[1][3] == GFX_CC_COMBINED_ALPHA);
}

/* ---------------------------------------------------------- other modes */

static void apply_othermode(gfx_rendermode *rm, const Gfx *g)
{
    unsigned w0 = (unsigned)g->words.w0;
    unsigned w1 = (unsigned)g->words.w1;
    /* This build's gsSPSetOtherMode stores the shift and length directly --
     * gsDPSetCycleType comes out as w0=ba001402, shift 0x14 = 20 and length 2.
     * The other form in gbi.h, which packs 32-shift-length instead, belongs to
     * a preprocessor branch that is not live here. Decoded the same way
     * gfx_state.c does it, so the test exercises the real path's convention
     * rather than a second opinion about it. */
    unsigned shift = (w0 >> 8) & 0xFFu;
    unsigned len   = w0 & 0xFFu;
    unsigned char op = (unsigned char)((w0 >> 24) & 0xFFu);

    if ((signed char)op == (signed char)G_SETOTHERMODE_H) {
        gfxRenderModeSetH(rm, shift, len, w1);
    } else {
        gfxRenderModeSetL(rm, shift, len, w1);
    }
}

static void test_cycle_type_and_filter(void)
{
    const Gfx cyc[]  = { gsDPSetCycleType(G_CYC_2CYCLE) };
    const Gfx filt[] = { gsDPSetTextureFilter(G_TF_BILERP) };
    gfx_rendermode rm;

    printf("cycle type and texture filter\n");
    gfxRenderModeInit(&rm);
    CHECK(rm.cycle_type == 0);

    apply_othermode(&rm, &cyc[0]);
    CHECK(rm.cycle_type == 1);

    /* Setting the filter must not disturb the cycle type: both live in the
     * high word and the game writes them with separate partial commands. */
    apply_othermode(&rm, &filt[0]);
    CHECK(rm.cycle_type == 1);
    CHECK(rm.tex_filter == (G_TF_BILERP >> G_MDSFT_TEXTFILT));
}

static void test_opaque_and_translucent(void)
{
    const Gfx opa[] = { gsDPSetRenderMode(G_RM_AA_ZB_OPA_SURF, G_RM_AA_ZB_OPA_SURF2) };
    const Gfx xlu[] = { gsDPSetRenderMode(G_RM_AA_ZB_XLU_SURF, G_RM_AA_ZB_XLU_SURF2) };
    gfx_rendermode rm;

    printf("render modes\n");

    gfxRenderModeInit(&rm);
    apply_othermode(&rm, &opa[0]);
    CHECK(rm.z_compare == 1);
    CHECK(rm.z_update == 1);
    CHECK(rm.blend_enabled == 0);

    gfxRenderModeInit(&rm);
    apply_othermode(&rm, &xlu[0]);
    CHECK(rm.z_compare == 1);
    /* A translucent surface tests depth but must not write it, or whatever is
     * drawn behind it afterwards gets rejected. */
    CHECK(rm.z_update == 0);
    CHECK(rm.blend_enabled == 1);
    CHECK(rm.src_factor == GFX_BLEND_SRC_ALPHA);
    CHECK(rm.dst_factor == GFX_BLEND_ONE_MINUS_SRC_ALPHA);
}

int main(void)
{
    printf("rdp mode decoder tests\n");

    test_modulate();
    test_shade_only();
    test_decal();
    test_slot_dependent_operands();
    test_two_cycles_differ();
    test_cycle_type_and_filter();
    test_opaque_and_translucent();

    printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
