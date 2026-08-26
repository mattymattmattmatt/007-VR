/*
 * gfx_state.h - the RSP/RDP state a display list manipulates.
 *
 * This is the half of the renderer that is GoldenEye-specific: the matrix
 * stack, the vertex cache, and the decoding rules that differ from stock
 * F3DEX. A GPU backend plugs in underneath via gfx_backend.h.
 *
 * The two decoding rules that matter most, both verified against the game's
 * own macros in the tests:
 *
 *   G_TRI1 carries its three vertex indices pre-multiplied by 10, in w1.
 *   G_TRI4 carries up to four triangles as raw 4-bit indices, and slots whose
 *   three indices are all zero are padding rather than geometry.
 *
 * Mixing those two up yields either no geometry or triangles pointing at
 * vertex 0, which is why they are pinned by tests rather than comments.
 */
#ifndef GEPC_GFX_STATE_H
#define GEPC_GFX_STATE_H

#include "gfx_backend.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Restores the pointers in a statically initialised display list.
 *
 * A display list command word is 32 bits, and narrowing an address is not an
 * address constant in C, so a list written as data in a source file cannot
 * hold a pointer -- GCC rejects the initialiser outright, whatever the
 * address turns out to be. src/debugmenu.c has one such list and
 * assets/rarewarelogo.c has nine.
 *
 * The fix is to leave the addresses out of the initialiser and supply them
 * separately as an ordinary array of pointers, which is a perfectly legal
 * static initialiser because nothing is narrowed. This then walks the list and
 * writes them into the commands that take one -- G_VTX and G_SETTIMG -- in the
 * order they appear.
 *
 * Returns the number of commands patched, so a table that has drifted out of
 * step with its list is caught at startup instead of rendering from address
 * zero. -1 if the list could not be walked.
 */
int gfxPatchListPointers(void *dl, unsigned max_commands,
                         void *const *targets, unsigned count);

/* Rare's compact vertex format fits 32 entries. G_TRI4's 4-bit indices only
 * reach the first 16; G_TRI1 reaches the rest. */
#define GEPC_VTX_CACHE      32
#define GEPC_MTX_STACK      16

typedef struct gfx_state gfx_state;

gfx_state *gfxStateCreate(const gfx_backend *backend);
void       gfxStateDestroy(gfx_state *st);

/* Resets per-frame state. Segment bindings persist, as they do on hardware. */
void gfxStateBeginFrame(gfx_state *st);
void gfxStateEndFrame(gfx_state *st);

/* Runs a display list. Returns 0 on a clean run, -1 if it was malformed.
 * max_commands bounds the walk for the same reason gbiWalk's does. */
int  gfxStateRun(gfx_state *st, const void *dl, unsigned max_commands);

/* Counters, for tests and for the on-screen debug overlay later. */
unsigned gfxStateTriangleCount(const gfx_state *st);
unsigned gfxStateVertexCount(const gfx_state *st);
unsigned gfxStateCommandCount(const gfx_state *st);
unsigned gfxStateMatrixDepth(const gfx_state *st);

/* Exposed for tests: decode the vertex indices out of a triangle command. */
void gfxDecodeTri1(unsigned w0, unsigned w1, int out_idx[3]);
int  gfxDecodeTri4(unsigned w0, unsigned w1, int out_idx[4][3]);

/* Converts an N64 fixed-point matrix into floats, column-major. */
void gfxMtxToFloat(const void *mtx, float out[16]);

#ifdef __cplusplus
}
#endif

#endif /* GEPC_GFX_STATE_H */
