/*
 * gbi_walk.h - walk a GoldenEye display list without executing it.
 *
 * Why this exists before the renderer does: the renderer's whole viability
 * rests on the claim that GoldenEye's display lists are in a format an
 * existing Fast3D-style translator can decode. This walks a real list,
 * resolves segmented addresses the way the RSP would, counts the geometry and
 * reports any command a translator would not recognise — so that claim can be
 * checked rather than assumed.
 *
 * It also becomes the interception point: port/src/video.c hands the finished
 * list here, and the renderer backend consumes what it classifies.
 *
 * Two GoldenEye-specific details it has to get right:
 *
 *   G_TRI4 - Rare extended the microcode with a packed four-triangle command
 *   and redefined gSP2Triangles in terms of it, so a stock F3DEX decoder that
 *   only knows G_TRI1 and G_TRI2 sees almost no geometry at all. Perfect Dark
 *   uses the identical opcode, which is why its renderer transfers.
 *
 *   Triangles whose three indices are all zero are padding inside a G_TRI4 and
 *   must not be drawn.
 */
#ifndef GEPC_GBI_WALK_H
#define GEPC_GBI_WALK_H

#ifdef __cplusplus
extern "C" {
#endif

#define GEPC_GBI_MAX_SEGMENTS   16
#define GEPC_GBI_MAX_DEPTH      32
#define GEPC_GBI_MAX_UNSUPPORTED 16

typedef struct gbi_stats {
    unsigned commands;        /* total commands visited                 */
    unsigned triangles;       /* actually drawn, padding excluded       */
    unsigned vertices;        /* vertices loaded by G_VTX               */
    unsigned display_lists;   /* nested lists entered                   */
    unsigned branches;        /* G_DL with the no-push flag             */
    unsigned max_depth;       /* deepest nesting reached                */
    unsigned textures;        /* G_SETTIMG                              */
    unsigned matrices;        /* G_MTX                                  */

    unsigned unsupported;     /* commands a Fast3D-style backend lacks  */
    unsigned char unsupported_ops[GEPC_GBI_MAX_UNSUPPORTED];
    unsigned unsupported_op_count;
} gbi_stats;

/* Segment table, as gSPSegment would set it. Base is a host pointer. */
void  gbiSetSegment(unsigned segment, const void *base);
void  gbiResetSegments(void);

/* Resolves a segmented N64 address to a host pointer, or NULL if the segment
 * is unset. Segment index lives in bits 24-27, offset in the low 24. */
const void *gbiResolve(unsigned addr);

/* Walks a display list. Returns 0 on a clean walk, -1 if the list was
 * malformed (unterminated, too deeply nested, or an unresolvable branch).
 * stats may be NULL.
 *
 * max_commands bounds the walk. This is not optional paranoia: a display list
 * carries no length, only a G_ENDDL terminator, so a list that is missing one
 * cannot be detected except by refusing to read forever -- and by then the
 * walker has already run off the end of the buffer. Callers that know the
 * extent of their list should pass it. Pass 0 for a large default, which is
 * appropriate for the game's own lists since those really are terminated. */
int gbiWalk(const void *dl, unsigned max_commands, gbi_stats *stats);

/* Names an opcode, for diagnostics. Never NULL. */
const char *gbiOpName(unsigned char op);

#ifdef __cplusplus
}
#endif

#endif /* GEPC_GBI_WALK_H */
