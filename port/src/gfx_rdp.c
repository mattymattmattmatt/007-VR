#include "gfx_rdp.h"

#include <string.h>

#include <PR/ultratypes.h>
#include <PR/mbi.h>
#include <PR/gbi.h>

/* ------------------------------------------------------------- combiner */

/*
 * One table per slot, because the raw values are only meaningful in context.
 * These follow the RDP's own decoding: an index past the end of a table reads
 * as zero, which is why the wide slots (a and b are 4 bits, c is 5) have far
 * more encodings than useful operands.
 */
static const unsigned char k_color_a[16] = {
    GFX_CC_COMBINED, GFX_CC_TEXEL0, GFX_CC_TEXEL1, GFX_CC_PRIMITIVE,
    GFX_CC_SHADE,    GFX_CC_ENVIRONMENT, GFX_CC_ONE, GFX_CC_NOISE,
    GFX_CC_ZERO, GFX_CC_ZERO, GFX_CC_ZERO, GFX_CC_ZERO,
    GFX_CC_ZERO, GFX_CC_ZERO, GFX_CC_ZERO, GFX_CC_ZERO
};

static const unsigned char k_color_b[16] = {
    GFX_CC_COMBINED, GFX_CC_TEXEL0, GFX_CC_TEXEL1, GFX_CC_PRIMITIVE,
    GFX_CC_SHADE,    GFX_CC_ENVIRONMENT, GFX_CC_CENTER, GFX_CC_K4,
    GFX_CC_ZERO, GFX_CC_ZERO, GFX_CC_ZERO, GFX_CC_ZERO,
    GFX_CC_ZERO, GFX_CC_ZERO, GFX_CC_ZERO, GFX_CC_ZERO
};

static const unsigned char k_color_c[32] = {
    GFX_CC_COMBINED, GFX_CC_TEXEL0, GFX_CC_TEXEL1, GFX_CC_PRIMITIVE,
    GFX_CC_SHADE,    GFX_CC_ENVIRONMENT, GFX_CC_SCALE, GFX_CC_COMBINED_ALPHA,
    GFX_CC_TEXEL0_ALPHA, GFX_CC_TEXEL1_ALPHA, GFX_CC_PRIMITIVE_ALPHA,
    GFX_CC_SHADE_ALPHA,  GFX_CC_ENV_ALPHA, GFX_CC_LOD_FRACTION,
    GFX_CC_PRIM_LOD_FRAC, GFX_CC_K5,
    GFX_CC_ZERO, GFX_CC_ZERO, GFX_CC_ZERO, GFX_CC_ZERO,
    GFX_CC_ZERO, GFX_CC_ZERO, GFX_CC_ZERO, GFX_CC_ZERO,
    GFX_CC_ZERO, GFX_CC_ZERO, GFX_CC_ZERO, GFX_CC_ZERO,
    GFX_CC_ZERO, GFX_CC_ZERO, GFX_CC_ZERO, GFX_CC_ZERO
};

static const unsigned char k_color_d[8] = {
    GFX_CC_COMBINED, GFX_CC_TEXEL0, GFX_CC_TEXEL1, GFX_CC_PRIMITIVE,
    GFX_CC_SHADE,    GFX_CC_ENVIRONMENT, GFX_CC_ONE, GFX_CC_ZERO
};

/* a, b and d share an alpha table; c does not -- 0 is LOD_FRACTION there
 * rather than COMBINED, and 6 is PRIM_LOD_FRAC rather than 1. */
static const unsigned char k_alpha_abd[8] = {
    GFX_CC_COMBINED_ALPHA, GFX_CC_TEXEL0_ALPHA, GFX_CC_TEXEL1_ALPHA,
    GFX_CC_PRIMITIVE_ALPHA, GFX_CC_SHADE_ALPHA, GFX_CC_ENV_ALPHA,
    GFX_CC_ONE, GFX_CC_ZERO
};

static const unsigned char k_alpha_c[8] = {
    GFX_CC_LOD_FRACTION, GFX_CC_TEXEL0_ALPHA, GFX_CC_TEXEL1_ALPHA,
    GFX_CC_PRIMITIVE_ALPHA, GFX_CC_SHADE_ALPHA, GFX_CC_ENV_ALPHA,
    GFX_CC_PRIM_LOD_FRAC, GFX_CC_ZERO
};

static unsigned field(unsigned word, unsigned shift, unsigned bits)
{
    return (word >> shift) & ((1u << bits) - 1u);
}

void gfxCombineDecode(unsigned w0, unsigned w1, gfx_combiner *out)
{
    if (!out) {
        return;
    }

    /* The packing is GCCc0w0/GCCc1w0/GCCc0w1/GCCc1w1 in gbi.h. Spelled out
     * here rather than reusing those macros, because they only assemble. */
    out->color[0][0] = k_color_a[field(w0, 20, 4)];
    out->color[0][1] = k_color_b[field(w1, 28, 4)];
    out->color[0][2] = k_color_c[field(w0, 15, 5)];
    out->color[0][3] = k_color_d[field(w1, 15, 3)];

    out->alpha[0][0] = k_alpha_abd[field(w0, 12, 3)];
    out->alpha[0][1] = k_alpha_abd[field(w1, 12, 3)];
    out->alpha[0][2] = k_alpha_c  [field(w0,  9, 3)];
    out->alpha[0][3] = k_alpha_abd[field(w1,  9, 3)];

    out->color[1][0] = k_color_a[field(w0, 5, 4)];
    out->color[1][1] = k_color_b[field(w1, 24, 4)];
    out->color[1][2] = k_color_c[field(w0, 0, 5)];
    out->color[1][3] = k_color_d[field(w1, 6, 3)];

    out->alpha[1][0] = k_alpha_abd[field(w1, 21, 3)];
    out->alpha[1][1] = k_alpha_abd[field(w1,  3, 3)];
    out->alpha[1][2] = k_alpha_c  [field(w1, 18, 3)];
    out->alpha[1][3] = k_alpha_abd[field(w1,  0, 3)];
}

int gfxCombineUsesTexel(const gfx_combiner *cc, int which)
{
    int cycle, slot;
    unsigned char rgb   = (which == 1) ? GFX_CC_TEXEL1 : GFX_CC_TEXEL0;
    unsigned char alpha = (which == 1) ? GFX_CC_TEXEL1_ALPHA : GFX_CC_TEXEL0_ALPHA;

    if (!cc) {
        return 0;
    }
    for (cycle = 0; cycle < 2; cycle++) {
        for (slot = 0; slot < 4; slot++) {
            if (cc->color[cycle][slot] == rgb ||
                cc->color[cycle][slot] == alpha ||
                cc->alpha[cycle][slot] == alpha) {
                return 1;
            }
        }
    }
    return 0;
}

/* ---------------------------------------------------------- other modes */

/*
 * The blender computes P * A + M * B per cycle. Only some of the 256
 * combinations have a fixed-function equivalent, but the ones GoldenEye uses
 * are all in that set: opaque surfaces, the standard translucent blend, and
 * the "leave the framebuffer alone" form that decals rely on.
 *
 * Anything unrecognised falls back to the standard translucent blend rather
 * than to opaque. Getting it wrong that way shows up as something too faint;
 * the other way round it is an opaque black rectangle over the scene, which
 * is much harder to trace back to here.
 */
static void derive_blend(gfx_rendermode *rm)
{
    unsigned p, a, m, b;
    int two_cycle = (rm->cycle_type == 1);

    /* GBL_c1 puts cycle one at bits 30/26/22/18 and GBL_c2 puts cycle two at
     * 28/24/20/16. In two-cycle mode the second is what reaches memory. */
    if (two_cycle) {
        p = field(rm->lo, 28, 2);
        a = field(rm->lo, 24, 2);
        m = field(rm->lo, 20, 2);
        b = field(rm->lo, 16, 2);
    } else {
        p = field(rm->lo, 30, 2);
        a = field(rm->lo, 26, 2);
        m = field(rm->lo, 22, 2);
        b = field(rm->lo, 18, 2);
    }

    /* FORCE_BL is what actually means "blend this". IM_RD looks like the
     * same signal and is not: an opaque antialiased surface sets it too,
     * because the RDP reads memory back to blend *partially covered edge
     * pixels*. Without coverage to emulate, those surfaces are simply opaque
     * here, and keying off IM_RD would turn every antialiased wall in the
     * game translucent. */
    if (!rm->force_blend) {
        rm->blend_enabled = 0;
        rm->src_factor = GFX_BLEND_ONE;
        rm->dst_factor = GFX_BLEND_ZERO;
        return;
    }

    rm->blend_enabled = 1;

    /* P = incoming colour, M = memory. B selects how much memory survives. */
    if (p == G_BL_CLR_IN && m == G_BL_CLR_MEM) {
        if (a == G_BL_A_IN && b == G_BL_1MA) {
            rm->src_factor = GFX_BLEND_SRC_ALPHA;
            rm->dst_factor = GFX_BLEND_ONE_MINUS_SRC_ALPHA;
            return;
        }
        if (b == G_BL_1) {
            /* Additive: the incoming pixel is added on top of memory. */
            rm->src_factor = (a == G_BL_A_IN) ? GFX_BLEND_SRC_ALPHA
                                              : GFX_BLEND_ONE;
            rm->dst_factor = GFX_BLEND_ONE;
            return;
        }
        if (b == G_BL_0) {
            rm->src_factor = (a == G_BL_A_IN) ? GFX_BLEND_SRC_ALPHA
                                              : GFX_BLEND_ONE;
            rm->dst_factor = GFX_BLEND_ZERO;
            return;
        }
        if (b == G_BL_A_MEM) {
            rm->src_factor = GFX_BLEND_SRC_ALPHA;
            rm->dst_factor = GFX_BLEND_DST_ALPHA;
            return;
        }
    }

    /* Memory passed straight through: used by decals that only want the
     * depth test's effect. */
    if (p == G_BL_CLR_MEM && b == G_BL_0) {
        rm->src_factor = GFX_BLEND_ZERO;
        rm->dst_factor = GFX_BLEND_ONE;
        return;
    }

    rm->src_factor = GFX_BLEND_SRC_ALPHA;
    rm->dst_factor = GFX_BLEND_ONE_MINUS_SRC_ALPHA;
}

static void derive(gfx_rendermode *rm)
{
    rm->cycle_type = (int)field(rm->hi, G_MDSFT_CYCLETYPE, 2);
    rm->tex_filter = (int)field(rm->hi, G_MDSFT_TEXTFILT, 2);
    rm->tex_lut    = (int)field(rm->hi, G_MDSFT_TEXTLUT, 2);
    rm->tex_persp  = (int)field(rm->hi, G_MDSFT_TEXTPERSP, 1);

    rm->alpha_compare = (int)field(rm->lo, G_MDSFT_ALPHACOMPARE, 2);
    rm->z_compare     = (rm->lo & (unsigned)Z_CMP) ? 1 : 0;
    rm->z_update      = (rm->lo & (unsigned)Z_UPD) ? 1 : 0;
    rm->z_mode        = (int)field(rm->lo, 10, 2);
    rm->cvg_x_alpha   = (rm->lo & (unsigned)CVG_X_ALPHA) ? 1 : 0;
    rm->alpha_cvg_sel = (rm->lo & (unsigned)ALPHA_CVG_SEL) ? 1 : 0;
    rm->force_blend   = (rm->lo & (unsigned)FORCE_BL) ? 1 : 0;

    derive_blend(rm);
}

void gfxRenderModeInit(gfx_rendermode *rm)
{
    if (!rm) {
        return;
    }
    memset(rm, 0, sizeof(*rm));
    /* Depth testing on and one-cycle mode is what the game sets up first, and
     * starting there means a list that never issues G_SETOTHERMODE still
     * draws something sensible. */
    rm->lo = (unsigned)(Z_CMP | Z_UPD);
    derive(rm);
}

static unsigned splice(unsigned word, unsigned shift, unsigned len,
                       unsigned data)
{
    unsigned mask;

    if (len == 0 || shift >= 32) {
        return word;
    }
    if (len > 32 - shift) {
        len = 32 - shift;
    }
    mask = (len >= 32) ? 0xFFFFFFFFu : (((1u << len) - 1u) << shift);
    return (word & ~mask) | (data & mask);
}

void gfxRenderModeSetH(gfx_rendermode *rm, unsigned shift, unsigned len,
                       unsigned data)
{
    if (!rm) {
        return;
    }
    rm->hi = splice(rm->hi, shift, len, data);
    derive(rm);
}

void gfxRenderModeSetL(gfx_rendermode *rm, unsigned shift, unsigned len,
                       unsigned data)
{
    if (!rm) {
        return;
    }
    rm->lo = splice(rm->lo, shift, len, data);
    derive(rm);
}
