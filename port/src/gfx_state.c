#include "gfx_state.h"

#include "gbi_walk.h"
#include "platform.h"

#include <stdlib.h>
#include <string.h>

#include <PR/ultratypes.h>
#include <PR/mbi.h>
#include <PR/gbi.h>
#include <gbi_extension.h>

#ifndef G_MODIFYVTX
#  define G_MODIFYVTX (G_IMMFIRST - 13)
#endif

#define OP_OF(w0) ((unsigned char)(((w0) >> 24) & 0xFFu))

struct gfx_state {
    gfx_backend backend;

    float    projection[16];
    float    modelview[GEPC_MTX_STACK][16];
    unsigned mtx_depth;
    float    combined[16];
    int      combined_valid;

    gfx_vertex vtx[GEPC_VTX_CACHE];

    unsigned geometry_mode;

    unsigned triangles;
    unsigned vertices;
    unsigned commands;
};

/* ----------------------------------------------------------------- maths */

/* Column-major multiply, out = a * b. */
static void mat_mul(float *out, const float *a, const float *b)
{
    float r[16];
    int c, i;

    for (c = 0; c < 4; c++) {
        for (i = 0; i < 4; i++) {
            r[c * 4 + i] = a[0 * 4 + i] * b[c * 4 + 0]
                         + a[1 * 4 + i] * b[c * 4 + 1]
                         + a[2 * 4 + i] * b[c * 4 + 2]
                         + a[3 * 4 + i] * b[c * 4 + 3];
        }
    }
    memcpy(out, r, sizeof(r));
}

static void mat_identity(float *m)
{
    memset(m, 0, sizeof(float) * 16);
    m[0] = m[5] = m[10] = m[15] = 1.0f;
}

void gfxMtxToFloat(const void *mtx, float out[16])
{
    const s32 *w = (const s32 *)mtx;
    int k;

    if (!mtx || !out) {
        return;
    }

    /* The N64 splits a 4x4 into two halves: the first eight words hold every
     * element's integer part, the last eight hold every fractional part, two
     * 16-bit halves per word. Reading them arithmetically out of the s32s,
     * rather than by casting to s16*, keeps this endian-independent. */
    for (k = 0; k < 16; k++) {
        s32 iw = w[k >> 1];
        s32 fw = w[8 + (k >> 1)];
        s32 ip = (k & 1) ? (s32)(s16)(iw & 0xFFFF) : (s32)(s16)((iw >> 16) & 0xFFFF);
        u32 fp = (k & 1) ? (u32)(fw & 0xFFFF) : (u32)((fw >> 16) & 0xFFFF);

        out[k] = (float)((ip << 16) | (s32)fp) / 65536.0f;
    }

    /* No transpose is needed. libultra matrices are row-vector (v * M) and
     * this layer is column-vector (M * v), which are transposes of one
     * another -- and a straight linear copy of a row-major matrix into
     * column-major storage *is* that transpose. */
}

/* ------------------------------------------------------ index decoding */

void gfxDecodeTri1(unsigned w0, unsigned w1, int out_idx[3])
{
    (void)w0;

    /* GoldenEye builds the branch where the indices are pre-multiplied by 10
     * and packed into w1. A decoder that assumes the F3DEX "times two" form
     * produces indices five times too large and draws nothing recognisable. */
    out_idx[0] = (int)((w1 >> 16) & 0xFFu) / 10;
    out_idx[1] = (int)((w1 >> 8) & 0xFFu) / 10;
    out_idx[2] = (int)(w1 & 0xFFu) / 10;
}

int gfxDecodeTri4(unsigned w0, unsigned w1, int out_idx[4][3])
{
    int drawn = 0;
    int i;

    for (i = 0; i < 4; i++) {
        int x = (int)((w1 >> (i * 8)) & 0x0Fu);
        int y = (int)((w1 >> (i * 8 + 4)) & 0x0Fu);
        int z = (int)((w0 >> (i * 4)) & 0x0Fu);

        /* All-zero slots are padding inside the packed command, not
         * degenerate triangles. Drawing them would add a stray triangle at
         * vertex 0 to a large share of the game's models. */
        if (!x && !y && !z) {
            continue;
        }
        out_idx[drawn][0] = x;
        out_idx[drawn][1] = y;
        out_idx[drawn][2] = z;
        drawn++;
    }
    return drawn;
}

/* ------------------------------------------------------------ lifecycle */

gfx_state *gfxStateCreate(const gfx_backend *backend)
{
    gfx_state *st = (gfx_state *)calloc(1, sizeof(*st));

    if (!st) {
        return NULL;
    }
    if (backend) {
        st->backend = *backend;
    }
    mat_identity(st->projection);
    mat_identity(st->modelview[0]);
    mat_identity(st->combined);
    st->combined_valid = 1;
    return st;
}

void gfxStateDestroy(gfx_state *st)
{
    free(st);
}

void gfxStateBeginFrame(gfx_state *st)
{
    if (!st) {
        return;
    }
    st->triangles = 0;
    st->vertices = 0;
    st->commands = 0;
    st->mtx_depth = 0;
    mat_identity(st->projection);
    mat_identity(st->modelview[0]);
    st->combined_valid = 0;

    if (st->backend.begin_frame) {
        st->backend.begin_frame(st->backend.user);
    }
}

void gfxStateEndFrame(gfx_state *st)
{
    if (st && st->backend.end_frame) {
        st->backend.end_frame(st->backend.user);
    }
}

unsigned gfxStateTriangleCount(const gfx_state *st) { return st ? st->triangles : 0; }
unsigned gfxStateVertexCount(const gfx_state *st)   { return st ? st->vertices : 0; }
unsigned gfxStateCommandCount(const gfx_state *st)  { return st ? st->commands : 0; }
unsigned gfxStateMatrixDepth(const gfx_state *st)   { return st ? st->mtx_depth : 0; }

/* --------------------------------------------------------------- vertex */

static const float *combined_matrix(gfx_state *st)
{
    if (!st->combined_valid) {
        mat_mul(st->combined, st->projection, st->modelview[st->mtx_depth]);
        st->combined_valid = 1;
    }
    return st->combined;
}

static void load_vertices(gfx_state *st, unsigned w0, unsigned w1)
{
    const Vtx *src = (const Vtx *)gbiResolve(w1);
    unsigned count = ((w0 >> 20) & 0x0Fu) + 1u;
    unsigned first = (w0 >> 16) & 0x0Fu;
    const float *m;
    unsigned i;

    if (!src) {
        platformLog("G_VTX from unbound segment 0x%08x", w1);
        return;
    }

    m = combined_matrix(st);

    for (i = 0; i < count; i++) {
        unsigned slot = first + i;
        const Vtx_t *v;
        float x, y, z;

        if (slot >= GEPC_VTX_CACHE) {
            break;
        }
        v = &src[i].v;
        x = (float)v->ob[0];
        y = (float)v->ob[1];
        z = (float)v->ob[2];

        st->vtx[slot].x = m[0] * x + m[4] * y + m[8]  * z + m[12];
        st->vtx[slot].y = m[1] * x + m[5] * y + m[9]  * z + m[13];
        st->vtx[slot].z = m[2] * x + m[6] * y + m[10] * z + m[14];
        st->vtx[slot].w = m[3] * x + m[7] * y + m[11] * z + m[15];

        st->vtx[slot].s = (float)v->tc[0];
        st->vtx[slot].t = (float)v->tc[1];

        st->vtx[slot].r = v->cn[0];
        st->vtx[slot].g = v->cn[1];
        st->vtx[slot].b = v->cn[2];
        st->vtx[slot].a = v->cn[3];

        st->vertices++;
    }
}

static void emit_tri(gfx_state *st, int a, int b, int c)
{
    if (a < 0 || b < 0 || c < 0 ||
        a >= GEPC_VTX_CACHE || b >= GEPC_VTX_CACHE || c >= GEPC_VTX_CACHE) {
        return;
    }
    st->triangles++;
    if (st->backend.draw_triangle) {
        st->backend.draw_triangle(st->backend.user,
                                  &st->vtx[a], &st->vtx[b], &st->vtx[c]);
    }
}

/* --------------------------------------------------------------- matrix */

static void apply_matrix(gfx_state *st, unsigned w0, unsigned w1)
{
    const void *src = gbiResolve(w1);
    unsigned params = (w0 >> 16) & 0xFFu;
    float m[16];

    if (!src) {
        platformLog("G_MTX from unbound segment 0x%08x", w1);
        return;
    }
    gfxMtxToFloat(src, m);

    if (params & G_MTX_PROJECTION) {
        if (params & G_MTX_LOAD) {
            memcpy(st->projection, m, sizeof(m));
        } else {
            mat_mul(st->projection, st->projection, m);
        }
    } else {
        if ((params & G_MTX_PUSH) && st->mtx_depth + 1 < GEPC_MTX_STACK) {
            memcpy(st->modelview[st->mtx_depth + 1],
                   st->modelview[st->mtx_depth], sizeof(m));
            st->mtx_depth++;
        }
        if (params & G_MTX_LOAD) {
            memcpy(st->modelview[st->mtx_depth], m, sizeof(m));
        } else {
            mat_mul(st->modelview[st->mtx_depth],
                    st->modelview[st->mtx_depth], m);
        }
    }
    st->combined_valid = 0;
}

static void pop_matrix(gfx_state *st)
{
    if (st->mtx_depth > 0) {
        st->mtx_depth--;
        st->combined_valid = 0;
    }
}

/* ------------------------------------------------------------ execution */

typedef struct run_frame {
    const Gfx *ret;
} run_frame;

int gfxStateRun(gfx_state *st, const void *dl, unsigned max_commands)
{
    run_frame stack[GEPC_GBI_MAX_DEPTH];
    unsigned depth = 0;
    const Gfx *pc = (const Gfx *)dl;
    unsigned long budget = max_commands ? (unsigned long)max_commands : 4000000ul;

    if (!st || !pc) {
        return -1;
    }

    while (budget--) {
        unsigned w0 = (unsigned)pc->words.w0;
        unsigned w1 = (unsigned)pc->words.w1;
        unsigned char op = OP_OF(w0);

        st->commands++;

        switch (op) {
        case (unsigned char)G_ENDDL:
            if (depth == 0) {
                return 0;
            }
            pc = stack[--depth].ret;
            continue;

        case (unsigned char)G_DL: {
            const Gfx *target = (const Gfx *)gbiResolve(w1);
            int branch = ((w0 >> 16) & 0xFFu) != 0;

            if (!target) {
                return -1;
            }
            if (branch) {
                pc = target;
                continue;
            }
            if (depth >= GEPC_GBI_MAX_DEPTH) {
                return -1;
            }
            stack[depth++].ret = pc + 1;
            pc = target;
            continue;
        }

        case (unsigned char)G_VTX:
            load_vertices(st, w0, w1);
            break;

        case (unsigned char)G_TRI1: {
            int idx[3];
            gfxDecodeTri1(w0, w1, idx);
            emit_tri(st, idx[0], idx[1], idx[2]);
            break;
        }

        case (unsigned char)G_TRI4: {
            int idx[4][3];
            int n = gfxDecodeTri4(w0, w1, idx);
            int i;
            for (i = 0; i < n; i++) {
                emit_tri(st, idx[i][0], idx[i][1], idx[i][2]);
            }
            break;
        }

        case (unsigned char)G_MTX:
            apply_matrix(st, w0, w1);
            break;

        case (unsigned char)G_POPMTX:
            pop_matrix(st);
            break;

        case (unsigned char)G_SETGEOMETRYMODE:
            st->geometry_mode |= w1;
            if (st->backend.set_geometry_mode) {
                st->backend.set_geometry_mode(st->backend.user, st->geometry_mode);
            }
            break;

        case (unsigned char)G_CLEARGEOMETRYMODE:
            st->geometry_mode &= ~w1;
            if (st->backend.set_geometry_mode) {
                st->backend.set_geometry_mode(st->backend.user, st->geometry_mode);
            }
            break;

        case (unsigned char)G_SETCOMBINE:
            if (st->backend.set_combine) {
                st->backend.set_combine(st->backend.user, w0, w1);
            }
            break;

        case (unsigned char)G_SETOTHERMODE_H:
            if (st->backend.set_othermode_h) {
                st->backend.set_othermode_h(st->backend.user,
                                            (w0 >> 8) & 0xFFu, w0 & 0xFFu, w1);
            }
            break;

        case (unsigned char)G_SETOTHERMODE_L:
            if (st->backend.set_othermode_l) {
                st->backend.set_othermode_l(st->backend.user,
                                            (w0 >> 8) & 0xFFu, w0 & 0xFFu, w1);
            }
            break;

        case (unsigned char)G_SETTIMG:
            if (st->backend.set_texture_image) {
                st->backend.set_texture_image(st->backend.user, gbiResolve(w1),
                                              (int)((w0 >> 21) & 0x07u),
                                              (int)((w0 >> 19) & 0x03u),
                                              (int)((w0 & 0x0FFFu) + 1));
            }
            break;

        case (unsigned char)G_SETPRIMCOLOR:
            if (st->backend.set_prim_color) {
                st->backend.set_prim_color(st->backend.user,
                                           (unsigned char)((w1 >> 24) & 0xFF),
                                           (unsigned char)((w1 >> 16) & 0xFF),
                                           (unsigned char)((w1 >> 8) & 0xFF),
                                           (unsigned char)(w1 & 0xFF));
            }
            break;

        case (unsigned char)G_SETENVCOLOR:
            if (st->backend.set_env_color) {
                st->backend.set_env_color(st->backend.user,
                                          (unsigned char)((w1 >> 24) & 0xFF),
                                          (unsigned char)((w1 >> 16) & 0xFF),
                                          (unsigned char)((w1 >> 8) & 0xFF),
                                          (unsigned char)(w1 & 0xFF));
            }
            break;

        case (unsigned char)G_SETFOGCOLOR:
            if (st->backend.set_fog_color) {
                st->backend.set_fog_color(st->backend.user,
                                          (unsigned char)((w1 >> 24) & 0xFF),
                                          (unsigned char)((w1 >> 16) & 0xFF),
                                          (unsigned char)((w1 >> 8) & 0xFF),
                                          (unsigned char)(w1 & 0xFF));
            }
            break;

        case (unsigned char)G_MODIFYVTX:
        case (unsigned char)G_SETPRIMDEPTH:
        case (unsigned char)G_SETBLENDCOLOR:
            /* The three GoldenEye emits that a Fast3D-derived backend has no
             * case for. Reported rather than silently dropped. */
            if (st->backend.unsupported) {
                st->backend.unsupported(st->backend.user, op, w0, w1);
            }
            break;

        default:
            break;
        }

        pc++;
    }

    return -1;
}
