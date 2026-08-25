/*
 * gfx_texture.h - N64 texture formats to RGBA8.
 *
 * GoldenEye uses all four format families: RGBA (157 references), I (134),
 * IA (91) and CI (22), across 4, 8, 16 and 32 bit sizes. A GPU backend wants
 * plain RGBA8, so this does the conversion.
 *
 * Kept separate from the GL backend on purpose. Format conversion is pure
 * arithmetic with exactly one right answer per texel, so it can be tested
 * exhaustively with no GPU present -- and a bug here shows up in game as
 * subtly wrong colours rather than a crash, which is the worst kind to chase.
 *
 * Source data is big-endian, as it sits in the ROM. Rows are assumed tightly
 * packed, which is what a G_LOADBLOCK produces; callers loading a sub-tile
 * pass the row stride explicitly.
 */
#ifndef GEPC_GFX_TEXTURE_H
#define GEPC_GFX_TEXTURE_H

#ifdef __cplusplus
extern "C" {
#endif

/* Bytes needed for a decoded image. */
#define GEPC_TEX_RGBA8_BYTES(w, h) ((unsigned)(w) * (unsigned)(h) * 4u)

/* Decodes one image into RGBA8.
 *
 *   fmt, siz   the G_IM_FMT_* / G_IM_SIZ_* values from the display list
 *   src_bytes  size of src, checked so a short buffer is refused rather than
 *              read past
 *   stride     bytes per source row; 0 means tightly packed
 *   tlut       palette for CI formats, 16-bit RGBA5551 entries, big-endian.
 *              May be NULL for non-CI formats.
 *
 * Returns 0 on success, -1 on a bad argument or a source too small. */
int gfxTextureDecode(const void *src, unsigned src_bytes,
                     int fmt, int siz,
                     int width, int height, unsigned stride,
                     const void *tlut, unsigned tlut_entries,
                     unsigned char *out);

/* Bytes one row of a given format occupies, tightly packed. */
unsigned gfxTextureRowBytes(int siz, int width);

/* Single-texel conversions, exposed so the bit expansions can be tested
 * directly. Each writes four bytes. */
void gfxTexelRGBA16(unsigned short v, unsigned char *out);
void gfxTexelIA16(unsigned short v, unsigned char *out);
void gfxTexelIA8(unsigned char v, unsigned char *out);
void gfxTexelIA4(unsigned char v, unsigned char *out);
void gfxTexelI8(unsigned char v, unsigned char *out);
void gfxTexelI4(unsigned char v, unsigned char *out);

#ifdef __cplusplus
}
#endif

#endif /* GEPC_GFX_TEXTURE_H */
