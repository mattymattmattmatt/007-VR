#include "gbi_walk.h"

#include "platform.h"

#include <stddef.h>
#include <string.h>

#include <PR/ultratypes.h>
#include <PR/gbi.h>
/* Defines TRI4_Ext itself, which is what makes G_TRI4 visible. Without it a
 * decoder sees Rare's packed four-triangle command as an unknown opcode and
 * draws almost no geometry at all. */
#include <gbi_extension.h>

/* GoldenEye builds the F3DEX (GBI 1) branch of gbi.h without F3DEX_GBI set,
 * so G_MODIFYVTX never gets defined even though the opcode slot exists. It is
 * declared here purely so the walker can recognise and report it: the only
 * gSPModifyVertex in the game source is commented out, but a backend should
 * still name the command rather than fall through to "unknown". */
#ifndef G_MODIFYVTX
#  define G_MODIFYVTX (G_IMMFIRST - 13)
#endif

/* gbi_extension.h also defines G_SETTEX as 0xc0, which collides with G_NOOP.
 * The macro that would emit it (gsSPUseTexture) has no callers in the game, so
 * treating 0xc0 as G_NOOP is safe today -- but a backend that starts seeing
 * 0xc0 with a non-zero payload is looking at G_SETTEX, not a no-op. */

/* The opcode constants come straight from the game's own gbi.h rather than a
 * hardcoded table, so this stays correct for whichever GBI variant the build
 * selects. GoldenEye builds the F3DEX (GBI 1) branch. */
#define OP_OF(w0) ((unsigned char)(((w0) >> 24) & 0xFFu))

static const void *g_segments[GEPC_GBI_MAX_SEGMENTS];

void gbiSetSegment(unsigned segment, const void *base)
{
    if (segment < GEPC_GBI_MAX_SEGMENTS) {
        g_segments[segment] = base;
    }
}

void gbiResetSegments(void)
{
    memset(g_segments, 0, sizeof(g_segments));
}

const void *gbiResolve(unsigned addr)
{
    unsigned segment = (addr >> 24) & 0x0Fu;
    unsigned offset = addr & 0x00FFFFFFu;

    /* Segment 0 is conventionally the identity mapping, but only if nothing
     * has been bound to it; the game does use it as a real segment. */
    if (!g_segments[segment]) {
        return NULL;
    }
    return (const unsigned char *)g_segments[segment] + offset;
}

const char *gbiOpName(unsigned char op)
{
    switch (op) {
    case (unsigned char)G_SPNOOP:            return "G_SPNOOP";
    case (unsigned char)G_MTX:               return "G_MTX";
    case (unsigned char)G_MOVEMEM:           return "G_MOVEMEM";
    case (unsigned char)G_VTX:               return "G_VTX";
    case (unsigned char)G_DL:                return "G_DL";
    case (unsigned char)G_TRI1:              return "G_TRI1";
    case (unsigned char)G_TRI4:              return "G_TRI4";
    case (unsigned char)G_CULLDL:            return "G_CULLDL";
    case (unsigned char)G_POPMTX:            return "G_POPMTX";
    case (unsigned char)G_MOVEWORD:          return "G_MOVEWORD";
    case (unsigned char)G_TEXTURE:           return "G_TEXTURE";
    case (unsigned char)G_SETOTHERMODE_H:    return "G_SETOTHERMODE_H";
    case (unsigned char)G_SETOTHERMODE_L:    return "G_SETOTHERMODE_L";
    case (unsigned char)G_ENDDL:             return "G_ENDDL";
    case (unsigned char)G_SETGEOMETRYMODE:   return "G_SETGEOMETRYMODE";
    case (unsigned char)G_CLEARGEOMETRYMODE: return "G_CLEARGEOMETRYMODE";
    case (unsigned char)G_LINE3D:            return "G_LINE3D";
    case (unsigned char)G_RDPHALF_1:         return "G_RDPHALF_1";
    case (unsigned char)G_RDPHALF_2:         return "G_RDPHALF_2";
    case (unsigned char)G_NOOP:              return "G_NOOP";
    case (unsigned char)G_SETCIMG:           return "G_SETCIMG";
    case (unsigned char)G_SETZIMG:           return "G_SETZIMG";
    case (unsigned char)G_SETTIMG:           return "G_SETTIMG";
    case (unsigned char)G_SETCOMBINE:        return "G_SETCOMBINE";
    case (unsigned char)G_SETENVCOLOR:       return "G_SETENVCOLOR";
    case (unsigned char)G_SETPRIMCOLOR:      return "G_SETPRIMCOLOR";
    case (unsigned char)G_SETBLENDCOLOR:     return "G_SETBLENDCOLOR";
    case (unsigned char)G_SETFOGCOLOR:       return "G_SETFOGCOLOR";
    case (unsigned char)G_SETFILLCOLOR:      return "G_SETFILLCOLOR";
    case (unsigned char)G_FILLRECT:          return "G_FILLRECT";
    case (unsigned char)G_SETTILE:           return "G_SETTILE";
    case (unsigned char)G_LOADTILE:          return "G_LOADTILE";
    case (unsigned char)G_LOADBLOCK:         return "G_LOADBLOCK";
    case (unsigned char)G_SETTILESIZE:       return "G_SETTILESIZE";
    case (unsigned char)G_LOADTLUT:          return "G_LOADTLUT";
    case (unsigned char)G_RDPSETOTHERMODE:   return "G_RDPSETOTHERMODE";
    case (unsigned char)G_SETPRIMDEPTH:      return "G_SETPRIMDEPTH";
    case (unsigned char)G_SETSCISSOR:        return "G_SETSCISSOR";
    case (unsigned char)G_SETCONVERT:        return "G_SETCONVERT";
    case (unsigned char)G_RDPFULLSYNC:       return "G_RDPFULLSYNC";
    case (unsigned char)G_RDPTILESYNC:       return "G_RDPTILESYNC";
    case (unsigned char)G_RDPPIPESYNC:       return "G_RDPPIPESYNC";
    case (unsigned char)G_RDPLOADSYNC:       return "G_RDPLOADSYNC";
    case (unsigned char)G_TEXRECT:           return "G_TEXRECT";
    case (unsigned char)G_TEXRECTFLIP:       return "G_TEXRECTFLIP";
    default:                                 return "?";
    }
}

/* The three commands GoldenEye can emit that a Perfect-Dark-derived Fast3D
 * backend has no case for. All three are rare in practice -- the only
 * gSPModifyVertex in the source is commented out, and the other two occur as a
 * single adjacent pair in src/boss.c -- but a backend needs to handle or
 * deliberately ignore them rather than fall through to "unknown command". */
static int is_unsupported(unsigned char op)
{
    return op == (unsigned char)G_MODIFYVTX
        || op == (unsigned char)G_SETPRIMDEPTH
        || op == (unsigned char)G_SETBLENDCOLOR;
}

static void note_unsupported(gbi_stats *st, unsigned char op)
{
    unsigned i;

    st->unsupported++;
    for (i = 0; i < st->unsupported_op_count; i++) {
        if (st->unsupported_ops[i] == op) {
            return;
        }
    }
    if (st->unsupported_op_count < GEPC_GBI_MAX_UNSUPPORTED) {
        st->unsupported_ops[st->unsupported_op_count++] = op;
    }
}

/* One G_TRI4 packs up to four triangles, each as three 4-bit vertex indices.
 * Slots that are entirely zero are padding, not degenerate geometry, and must
 * not be counted or drawn. */
static unsigned count_tri4(unsigned w0, unsigned w1)
{
    unsigned drawn = 0;
    unsigned i;

    for (i = 0; i < 4; i++) {
        unsigned x = (w1 >> (i * 8)) & 0x0Fu;
        unsigned y = (w1 >> (i * 8 + 4)) & 0x0Fu;
        unsigned z = (w0 >> (i * 4)) & 0x0Fu;

        if (x || y || z) {
            drawn++;
        }
    }
    return drawn;
}

typedef struct walk_frame {
    const Gfx *ret;
} walk_frame;

int gbiWalk(const void *dl, unsigned max_commands, gbi_stats *stats)
{
    gbi_stats local;
    walk_frame stack[GEPC_GBI_MAX_DEPTH];
    unsigned depth = 0;
    const Gfx *pc = (const Gfx *)dl;
    /* There is no length in a display list, only a terminator, so an
     * unterminated one can only be caught by refusing to read past a bound the
     * caller supplies. Without that the walk runs off the buffer long before
     * any counter would notice. */
    unsigned long budget = max_commands ? (unsigned long)max_commands : 4000000ul;

    if (!stats) {
        stats = &local;
    }
    memset(stats, 0, sizeof(*stats));

    if (!pc) {
        return -1;
    }

    while (budget--) {
        unsigned w0 = (unsigned)pc->words.w0;
        unsigned w1 = (unsigned)pc->words.w1;
        unsigned char op = OP_OF(w0);

        stats->commands++;

        if (is_unsupported(op)) {
            note_unsupported(stats, op);
        }

        if (op == (unsigned char)G_ENDDL) {
            if (depth == 0) {
                return 0;
            }
            pc = stack[--depth].ret;
            continue;
        }

        if (op == (unsigned char)G_DL) {
            const Gfx *target = (const Gfx *)gbiResolve(w1);
            /* Bit 16 of w0 selects branch (no push) over call. */
            int branch = ((w0 >> 16) & 0xFFu) != 0;

            if (!target) {
                /* An unresolvable target means a segment the caller never
                 * bound. That is a setup bug worth reporting, not something
                 * to skip past silently. */
                return -1;
            }

            if (branch) {
                stats->branches++;
                pc = target;
                continue;
            }

            if (depth >= GEPC_GBI_MAX_DEPTH) {
                return -1;
            }
            stats->display_lists++;
            stack[depth++].ret = pc + 1;
            if (depth > stats->max_depth) {
                stats->max_depth = depth;
            }
            pc = target;
            continue;
        }

        if (op == (unsigned char)G_TRI1) {
            stats->triangles++;
        } else if (op == (unsigned char)G_TRI4) {
            stats->triangles += count_tri4(w0, w1);
        } else if (op == (unsigned char)G_VTX) {
            /* F3DEX packs the count into w0; the exact field width differs
             * between GBI revisions, so take the number of vertices the
             * command loads from the shared low nibble-pair. */
            stats->vertices += ((w0 >> 20) & 0x0Fu) + 1u;
        } else if (op == (unsigned char)G_SETTIMG) {
            stats->textures++;
        } else if (op == (unsigned char)G_MTX) {
            stats->matrices++;
        }

        pc++;
    }

    /* Ran out of budget: the list never terminated. */
    return -1;
}
