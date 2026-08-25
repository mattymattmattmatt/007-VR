#include "gfx_texture.h"

#include <string.h>

#include <PR/ultratypes.h>
#include <PR/mbi.h>
#include <PR/gbi.h>

/* Expanding a small field to 8 bits by shifting alone leaves the top of the
 * range unreachable: 5 bits of 0x1F would become 0xF8, so white is never quite
 * white. Replicating the high bits into the low ones fixes that, and is what
 * the hardware effectively does. */
static unsigned char expand5(unsigned v)
{
    v &= 0x1Fu;
    return (unsigned char)((v << 3) | (v >> 2));
}

static unsigned char expand4(unsigned v)
{
    v &= 0x0Fu;
    return (unsigned char)((v << 4) | v);
}

static unsigned char expand3(unsigned v)
{
    v &= 0x07u;
    return (unsigned char)((v << 5) | (v << 2) | (v >> 1));
}

static unsigned char expand1(unsigned v)
{
    return (unsigned char)((v & 1u) ? 0xFFu : 0x00u);
}

void gfxTexelRGBA16(unsigned short v, unsigned char *out)
{
    out[0] = expand5((unsigned)(v >> 11));
    out[1] = expand5((unsigned)(v >> 6));
    out[2] = expand5((unsigned)(v >> 1));
    out[3] = expand1((unsigned)v);
}

void gfxTexelIA16(unsigned short v, unsigned char *out)
{
    unsigned char i = (unsigned char)(v >> 8);
    out[0] = out[1] = out[2] = i;
    out[3] = (unsigned char)(v & 0xFFu);
}

void gfxTexelIA8(unsigned char v, unsigned char *out)
{
    unsigned char i = expand4((unsigned)(v >> 4));
    out[0] = out[1] = out[2] = i;
    out[3] = expand4((unsigned)v);
}

void gfxTexelIA4(unsigned char v, unsigned char *out)
{
    /* Three bits of intensity, one of alpha. */
    unsigned char i = expand3((unsigned)(v >> 1));
    out[0] = out[1] = out[2] = i;
    out[3] = expand1((unsigned)v);
}

void gfxTexelI8(unsigned char v, unsigned char *out)
{
    /* Intensity formats drive alpha from the same value, so an I texture used
     * with alpha compare behaves as the hardware does. */
    out[0] = out[1] = out[2] = out[3] = v;
}

void gfxTexelI4(unsigned char v, unsigned char *out)
{
    unsigned char i = expand4((unsigned)v);
    out[0] = out[1] = out[2] = out[3] = i;
}

unsigned gfxTextureRowBytes(int siz, int width)
{
    unsigned w = (unsigned)(width < 0 ? 0 : width);

    switch (siz) {
    case G_IM_SIZ_4b:  return (w + 1u) / 2u;
    case G_IM_SIZ_8b:  return w;
    case G_IM_SIZ_16b: return w * 2u;
    case G_IM_SIZ_32b: return w * 4u;
    default:           return 0;
    }
}

static unsigned short be16(const unsigned char *p)
{
    return (unsigned short)(((unsigned)p[0] << 8) | (unsigned)p[1]);
}

int gfxTextureDecode(const void *src, unsigned src_bytes,
                     int fmt, int siz,
                     int width, int height, unsigned stride,
                     const void *tlut, unsigned tlut_entries,
                     unsigned char *out)
{
    const unsigned char *s = (const unsigned char *)src;
    const unsigned char *pal = (const unsigned char *)tlut;
    unsigned row_bytes;
    int x, y;

    if (!s || !out || width <= 0 || height <= 0) {
        return -1;
    }

    row_bytes = gfxTextureRowBytes(siz, width);
    if (!row_bytes) {
        return -1;
    }
    if (!stride) {
        stride = row_bytes;
    }
    if (stride < row_bytes) {
        return -1;
    }

    /* The last row only needs row_bytes, not a full stride, so requiring
     * stride * height would reject legitimate tightly-trailing buffers. */
    if ((unsigned long long)stride * (unsigned long long)(height - 1) + row_bytes
        > (unsigned long long)src_bytes) {
        return -1;
    }

    if (fmt == G_IM_FMT_CI && (!pal || tlut_entries == 0)) {
        return -1;
    }

    for (y = 0; y < height; y++) {
        const unsigned char *row = s + (unsigned)y * stride;
        unsigned char *dst = out + (unsigned)y * (unsigned)width * 4u;

        for (x = 0; x < width; x++) {
            unsigned char *px = dst + (unsigned)x * 4u;

            switch (fmt) {
            case G_IM_FMT_RGBA:
                if (siz == G_IM_SIZ_16b) {
                    gfxTexelRGBA16(be16(row + x * 2), px);
                } else if (siz == G_IM_SIZ_32b) {
                    px[0] = row[x * 4 + 0];
                    px[1] = row[x * 4 + 1];
                    px[2] = row[x * 4 + 2];
                    px[3] = row[x * 4 + 3];
                } else {
                    return -1;
                }
                break;

            case G_IM_FMT_IA:
                if (siz == G_IM_SIZ_16b) {
                    gfxTexelIA16(be16(row + x * 2), px);
                } else if (siz == G_IM_SIZ_8b) {
                    gfxTexelIA8(row[x], px);
                } else if (siz == G_IM_SIZ_4b) {
                    unsigned char b = row[x >> 1];
                    /* Two texels per byte, high nibble first. */
                    gfxTexelIA4((unsigned char)((x & 1) ? (b & 0x0Fu) : (b >> 4)), px);
                } else {
                    return -1;
                }
                break;

            case G_IM_FMT_I:
                if (siz == G_IM_SIZ_8b) {
                    gfxTexelI8(row[x], px);
                } else if (siz == G_IM_SIZ_4b) {
                    unsigned char b = row[x >> 1];
                    gfxTexelI4((unsigned char)((x & 1) ? (b & 0x0Fu) : (b >> 4)), px);
                } else {
                    return -1;
                }
                break;

            case G_IM_FMT_CI: {
                unsigned idx;

                if (siz == G_IM_SIZ_8b) {
                    idx = row[x];
                } else if (siz == G_IM_SIZ_4b) {
                    unsigned char b = row[x >> 1];
                    idx = (x & 1) ? (unsigned)(b & 0x0Fu) : (unsigned)(b >> 4);
                } else {
                    return -1;
                }

                /* A palette index past the end of the TLUT is a data error,
                 * not something to read past the buffer for. */
                if (idx >= tlut_entries) {
                    px[0] = px[1] = px[2] = 0;
                    px[3] = 0;
                    break;
                }
                gfxTexelRGBA16(be16(pal + idx * 2), px);
                break;
            }

            default:
                return -1;
            }
        }
    }
    return 0;
}
