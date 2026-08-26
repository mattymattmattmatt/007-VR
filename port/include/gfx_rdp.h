/*
 * gfx_rdp.h - decoding the RDP's two mode words.
 *
 * G_SETCOMBINE and G_SETOTHERMODE carry the pixel pipeline's entire
 * configuration packed into bit fields, and both need untangling before a
 * shader can act on them. That untangling has exactly one right answer, so it
 * lives here where it can be tested headlessly, in the same spirit as the
 * display-list decoder: gfx_gl.c should only have to know how to draw.
 *
 * The combiner is the part worth being careful about. Its raw operand values
 * are *slot-dependent* -- the same number means different things depending on
 * which of a, b, c or d it appears in. The value 6 is 1 in the a slot, CENTER
 * in b, SCALE in c, and 1 again in d; the value 7 is NOISE in a, K4 in b,
 * COMBINED_ALPHA in c and 0 in d. Reading those with one table, the way the
 * G_CCMUX_ names in gbi.h tempt you to, silently produces the wrong picture
 * rather than an error. gfxCombineDecode resolves each slot against its own
 * table and hands back unambiguous operands.
 */
#ifndef GEPC_GFX_RDP_H
#define GEPC_GFX_RDP_H

#ifdef __cplusplus
extern "C" {
#endif

/* Canonical combiner operands. The numbering is this port's own; it is what
 * the fragment shader switches on, so keep it in step with the GLSL in
 * gfx_gl.c. */
typedef enum gfx_cc_operand {
    GFX_CC_COMBINED = 0,
    GFX_CC_TEXEL0,
    GFX_CC_TEXEL1,
    GFX_CC_PRIMITIVE,
    GFX_CC_SHADE,
    GFX_CC_ENVIRONMENT,
    GFX_CC_CENTER,
    GFX_CC_SCALE,
    GFX_CC_COMBINED_ALPHA,
    GFX_CC_TEXEL0_ALPHA,
    GFX_CC_TEXEL1_ALPHA,
    GFX_CC_PRIMITIVE_ALPHA,
    GFX_CC_SHADE_ALPHA,
    GFX_CC_ENV_ALPHA,
    GFX_CC_LOD_FRACTION,
    GFX_CC_PRIM_LOD_FRAC,
    GFX_CC_NOISE,
    GFX_CC_K4,
    GFX_CC_K5,
    GFX_CC_ONE,
    GFX_CC_ZERO,
    GFX_CC_OPERAND_COUNT
} gfx_cc_operand;

/* Both cycles of the colour and alpha combiners. Each is (a - b) * c + d.
 * Index [cycle][slot], slot 0..3 being a, b, c, d. */
typedef struct gfx_combiner {
    unsigned char color[2][4];
    unsigned char alpha[2][4];
} gfx_combiner;

/* Splits G_SETCOMBINE's two command words. Never fails: every bit pattern
 * denotes some operand, since the wide slots read anything past their table
 * as zero, which is what the hardware does. */
void gfxCombineDecode(unsigned w0, unsigned w1, gfx_combiner *out);

/* True when the combiner never references a texel, so the backend can skip
 * binding a texture rather than trusting gSPTexture's enable flag alone. */
int gfxCombineUsesTexel(const gfx_combiner *cc, int which);

/* ---------------------------------------------------------- other modes */

typedef enum gfx_blend_factor {
    GFX_BLEND_ZERO = 0,
    GFX_BLEND_ONE,
    GFX_BLEND_SRC_ALPHA,
    GFX_BLEND_ONE_MINUS_SRC_ALPHA,
    GFX_BLEND_DST_ALPHA
} gfx_blend_factor;

typedef struct gfx_rendermode {
    /* Raw accumulated words, so a partial G_SETOTHERMODE can update a field
     * without disturbing the rest -- which is how the game uses them. */
    unsigned hi, lo;

    int cycle_type;      /* 0 = 1CYCLE, 1 = 2CYCLE, 2 = COPY, 3 = FILL */
    int tex_filter;      /* 0 = point, 2 = bilerp, 3 = average */
    int tex_lut;         /* 0 = none, 2 = RGBA16, 3 = IA16 */
    int tex_persp;

    int z_compare;
    int z_update;
    int z_mode;          /* 0 = opaque, 1 = interpenetrating, 2 = xlu, 3 = decal */
    int alpha_compare;   /* 0 = none, 1 = threshold, 3 = dither */
    int cvg_x_alpha;
    int alpha_cvg_sel;
    int force_blend;

    /* What the blender's second (or only) cycle works out to in GL terms.
     * The RDP computes P*A + M*B with its own coverage rules; these are the
     * closest fixed-function equivalent, which covers what GoldenEye uses. */
    int blend_enabled;
    int src_factor;
    int dst_factor;
} gfx_rendermode;

/* Puts a rendermode into the state the RDP powers up in. */
void gfxRenderModeInit(gfx_rendermode *rm);

/* Applies one G_SETOTHERMODE_H / _L command. `shift` and `len` are the
 * command's own fields, so a write to a single field leaves the others
 * alone. */
void gfxRenderModeSetH(gfx_rendermode *rm, unsigned shift, unsigned len,
                       unsigned data);
void gfxRenderModeSetL(gfx_rendermode *rm, unsigned shift, unsigned len,
                       unsigned data);

#ifdef __cplusplus
}
#endif

#endif /* GEPC_GFX_RDP_H */
