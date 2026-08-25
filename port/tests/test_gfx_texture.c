/*
 * Tests for the N64 texture decoders.
 *
 * Every texel conversion has exactly one right answer, so these check against
 * hand-computed values rather than against the implementation's own output.
 * The bit-expansion cases matter most: a decoder that shifts without
 * replicating never reaches full white, which reads in game as a whole palette
 * that is slightly dark and is very hard to spot.
 */
#include "gfx_texture.h"
#include "platform.h"

#include <stdio.h>
#include <string.h>

#include <PR/ultratypes.h>
#include <PR/mbi.h>
#include <PR/gbi.h>

static int g_failures;
static int g_checks;

static void check(int cond, const char *what, const char *file, int line)
{
    g_checks++;
    if (!cond) {
        g_failures++;
        printf("  FAIL %s:%d: %s\n", file, line, what);
    }
}

#define CHECK(c) check((c), #c, __FILE__, __LINE__)

#define PX(p, r, g, b, a) ((p)[0]==(r) && (p)[1]==(g) && (p)[2]==(b) && (p)[3]==(a))

/* ------------------------------------------------------------ expansion */

static void test_bit_expansion(void)
{
    unsigned char px[4];

    printf("texture: small fields expand to the full 0..255 range\n");

    /* RGBA16 white with alpha: every 5-bit field at max must give 255, not
     * 248. This is the case a shift-only expansion gets wrong. */
    gfxTexelRGBA16(0xFFFF, px);
    CHECK(PX(px, 255, 255, 255, 255));

    /* All zero stays zero. */
    gfxTexelRGBA16(0x0000, px);
    CHECK(PX(px, 0, 0, 0, 0));

    /* Pure red: r=31, g=0, b=0, a=1 -> 0xF801 */
    gfxTexelRGBA16(0xF801, px);
    CHECK(PX(px, 255, 0, 0, 255));

    /* Pure green: g=31 -> bits 10..6 -> 0x07C0, alpha 1 */
    gfxTexelRGBA16(0x07C1, px);
    CHECK(PX(px, 0, 255, 0, 255));

    /* Pure blue: b=31 -> bits 5..1 -> 0x003E, alpha 1 */
    gfxTexelRGBA16(0x003F, px);
    CHECK(PX(px, 0, 0, 255, 255));

    /* The 1-bit alpha is all or nothing. */
    gfxTexelRGBA16(0xFFFE, px);
    CHECK(px[3] == 0);

    /* 4-bit expansion reaches both ends. */
    gfxTexelI4(0x0, px);
    CHECK(PX(px, 0, 0, 0, 0));
    gfxTexelI4(0xF, px);
    CHECK(PX(px, 255, 255, 255, 255));

    /* Mid grey stays near mid, not skewed by the replication. */
    gfxTexelI4(0x8, px);
    CHECK(px[0] == 0x88);

    /* 3-bit intensity in IA4. */
    gfxTexelIA4(0x0F, px);   /* i=7, a=1 */
    CHECK(PX(px, 255, 255, 255, 255));
    gfxTexelIA4(0x0E, px);   /* i=7, a=0 */
    CHECK(PX(px, 255, 255, 255, 0));
    gfxTexelIA4(0x00, px);
    CHECK(PX(px, 0, 0, 0, 0));
}

static void test_intensity_drives_alpha(void)
{
    unsigned char px[4];

    printf("texture: I formats drive alpha from intensity\n");

    /* An I texture used with alpha compare relies on this. */
    gfxTexelI8(0x40, px);
    CHECK(PX(px, 0x40, 0x40, 0x40, 0x40));

    gfxTexelIA8(0xF0, px);   /* i=15, a=0 */
    CHECK(PX(px, 255, 255, 255, 0));
    gfxTexelIA8(0x0F, px);   /* i=0, a=15 */
    CHECK(PX(px, 0, 0, 0, 255));

    gfxTexelIA16(0x80FF, px);
    CHECK(PX(px, 0x80, 0x80, 0x80, 0xFF));
}

/* ------------------------------------------------------------ row sizes */

static void test_row_bytes(void)
{
    printf("texture: packed row sizes\n");

    CHECK(gfxTextureRowBytes(G_IM_SIZ_4b, 8) == 4);
    CHECK(gfxTextureRowBytes(G_IM_SIZ_8b, 8) == 8);
    CHECK(gfxTextureRowBytes(G_IM_SIZ_16b, 8) == 16);
    CHECK(gfxTextureRowBytes(G_IM_SIZ_32b, 8) == 32);

    /* An odd width in a 4-bit format still needs a whole trailing byte. */
    CHECK(gfxTextureRowBytes(G_IM_SIZ_4b, 7) == 4);
    CHECK(gfxTextureRowBytes(G_IM_SIZ_4b, 1) == 1);
}

/* -------------------------------------------------------------- decode */

static void test_rgba16_image(void)
{
    unsigned char src[8];
    unsigned char out[16];

    printf("texture: RGBA16 image decode, big-endian source\n");

    /* 2x2: white, red, green, transparent-black. Source is big endian. */
    src[0] = 0xFF; src[1] = 0xFF;   /* white  */
    src[2] = 0xF8; src[3] = 0x01;   /* red    */
    src[4] = 0x07; src[5] = 0xC1;   /* green  */
    src[6] = 0x00; src[7] = 0x00;   /* clear  */

    CHECK(gfxTextureDecode(src, sizeof(src), G_IM_FMT_RGBA, G_IM_SIZ_16b,
                           2, 2, 0, NULL, 0, out) == 0);
    CHECK(PX(out + 0,  255, 255, 255, 255));
    CHECK(PX(out + 4,  255, 0, 0, 255));
    CHECK(PX(out + 8,  0, 255, 0, 255));
    CHECK(PX(out + 12, 0, 0, 0, 0));
}

static void test_rgba32_image(void)
{
    unsigned char src[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    unsigned char out[8];

    printf("texture: RGBA32 passes through untouched\n");

    CHECK(gfxTextureDecode(src, sizeof(src), G_IM_FMT_RGBA, G_IM_SIZ_32b,
                           2, 1, 0, NULL, 0, out) == 0);
    CHECK(PX(out + 0, 1, 2, 3, 4));
    CHECK(PX(out + 4, 5, 6, 7, 8));
}

static void test_nibble_order(void)
{
    unsigned char src[1];
    unsigned char out[8];

    printf("texture: 4-bit texels are high nibble first\n");

    /* One byte, two texels. Getting the order backwards mirrors every 4-bit
     * texture in the game horizontally, which looks plausible enough to miss. */
    src[0] = 0xF0;
    CHECK(gfxTextureDecode(src, 1, G_IM_FMT_I, G_IM_SIZ_4b,
                           2, 1, 0, NULL, 0, out) == 0);
    CHECK(out[0] == 255);   /* first texel is the high nibble */
    CHECK(out[4] == 0);     /* second is the low nibble */

    src[0] = 0x0F;
    CHECK(gfxTextureDecode(src, 1, G_IM_FMT_I, G_IM_SIZ_4b,
                           2, 1, 0, NULL, 0, out) == 0);
    CHECK(out[0] == 0);
    CHECK(out[4] == 255);
}

static void test_ci_palette(void)
{
    unsigned char src[2];
    unsigned char pal[8];
    unsigned char out[16];

    printf("texture: CI formats index the TLUT\n");

    /* Four RGBA16 palette entries: white, red, green, clear. */
    pal[0] = 0xFF; pal[1] = 0xFF;
    pal[2] = 0xF8; pal[3] = 0x01;
    pal[4] = 0x07; pal[5] = 0xC1;
    pal[6] = 0x00; pal[7] = 0x00;

    /* CI8, two texels. */
    src[0] = 1; src[1] = 2;
    CHECK(gfxTextureDecode(src, 2, G_IM_FMT_CI, G_IM_SIZ_8b,
                           2, 1, 0, pal, 4, out) == 0);
    CHECK(PX(out + 0, 255, 0, 0, 255));
    CHECK(PX(out + 4, 0, 255, 0, 255));

    /* CI4, two texels in one byte, indices 0 and 3. */
    src[0] = 0x03;
    CHECK(gfxTextureDecode(src, 1, G_IM_FMT_CI, G_IM_SIZ_4b,
                           2, 1, 0, pal, 4, out) == 0);
    CHECK(PX(out + 0, 255, 255, 255, 255));
    CHECK(PX(out + 4, 0, 0, 0, 0));

    /* An index past the end of the palette is a data error; it must not read
     * past the TLUT buffer. */
    src[0] = 200;
    CHECK(gfxTextureDecode(src, 1, G_IM_FMT_CI, G_IM_SIZ_8b,
                           1, 1, 0, pal, 4, out) == 0);
    CHECK(PX(out + 0, 0, 0, 0, 0));

    /* CI without a palette is refused rather than guessed at. */
    CHECK(gfxTextureDecode(src, 1, G_IM_FMT_CI, G_IM_SIZ_8b,
                           1, 1, 0, NULL, 0, out) == -1);
}

static void test_stride(void)
{
    unsigned char src[8];
    unsigned char out[16];

    printf("texture: an explicit row stride is honoured\n");

    /* 2x2 of I8 with 4-byte rows: two texels then two bytes of padding. */
    src[0] = 0x11; src[1] = 0x22; src[2] = 0xAA; src[3] = 0xBB;
    src[4] = 0x33; src[5] = 0x44; src[6] = 0xCC; src[7] = 0xDD;

    CHECK(gfxTextureDecode(src, sizeof(src), G_IM_FMT_I, G_IM_SIZ_8b,
                           2, 2, 4, NULL, 0, out) == 0);
    CHECK(out[0] == 0x11);
    CHECK(out[4] == 0x22);
    /* Second row must start after the padding, not at byte 2. */
    CHECK(out[8] == 0x33);
    CHECK(out[12] == 0x44);
}

static void test_bounds_and_bad_args(void)
{
    unsigned char src[4] = { 0, 0, 0, 0 };
    unsigned char out[64];

    printf("texture: short buffers and bad formats are refused\n");

    /* Asking for more texels than the source holds. */
    CHECK(gfxTextureDecode(src, 4, G_IM_FMT_RGBA, G_IM_SIZ_16b,
                           4, 4, 0, NULL, 0, out) == -1);

    /* Exactly enough is fine: the last row only needs its own bytes, not a
     * full stride, so a tightly trailing buffer must not be rejected. */
    {
        unsigned char big[8];
        memset(big, 0, sizeof(big));
        CHECK(gfxTextureDecode(big, 8, G_IM_FMT_RGBA, G_IM_SIZ_16b,
                               2, 2, 0, NULL, 0, out) == 0);
        /* One byte short must fail. */
        CHECK(gfxTextureDecode(big, 7, G_IM_FMT_RGBA, G_IM_SIZ_16b,
                               2, 2, 0, NULL, 0, out) == -1);
    }

    CHECK(gfxTextureDecode(NULL, 4, G_IM_FMT_I, G_IM_SIZ_8b, 1, 1, 0, NULL, 0, out) == -1);
    CHECK(gfxTextureDecode(src, 4, G_IM_FMT_I, G_IM_SIZ_8b, 1, 1, 0, NULL, 0, NULL) == -1);
    CHECK(gfxTextureDecode(src, 4, G_IM_FMT_I, G_IM_SIZ_8b, 0, 1, 0, NULL, 0, out) == -1);
    CHECK(gfxTextureDecode(src, 4, G_IM_FMT_I, G_IM_SIZ_8b, 1, -1, 0, NULL, 0, out) == -1);

    /* Combinations the hardware has no encoding for. */
    CHECK(gfxTextureDecode(src, 4, G_IM_FMT_RGBA, G_IM_SIZ_4b, 1, 1, 0, NULL, 0, out) == -1);
    CHECK(gfxTextureDecode(src, 4, G_IM_FMT_I, G_IM_SIZ_16b, 1, 1, 0, NULL, 0, out) == -1);

    /* A stride narrower than a row is nonsense. */
    CHECK(gfxTextureDecode(src, 4, G_IM_FMT_I, G_IM_SIZ_8b, 4, 1, 2, NULL, 0, out) == -1);
}

static void test_all_formats_run(void)
{
    unsigned char src[256];
    unsigned char pal[512];
    unsigned char out[8 * 8 * 4];
    unsigned i;

    printf("texture: every format GoldenEye uses decodes an 8x8\n");

    for (i = 0; i < sizeof(src); i++) {
        src[i] = (unsigned char)i;
    }
    for (i = 0; i < sizeof(pal); i++) {
        pal[i] = (unsigned char)(i * 3);
    }

    CHECK(gfxTextureDecode(src, sizeof(src), G_IM_FMT_RGBA, G_IM_SIZ_16b, 8, 8, 0, NULL, 0, out) == 0);
    CHECK(gfxTextureDecode(src, sizeof(src), G_IM_FMT_RGBA, G_IM_SIZ_32b, 8, 8, 0, NULL, 0, out) == 0);
    CHECK(gfxTextureDecode(src, sizeof(src), G_IM_FMT_IA,   G_IM_SIZ_16b, 8, 8, 0, NULL, 0, out) == 0);
    CHECK(gfxTextureDecode(src, sizeof(src), G_IM_FMT_IA,   G_IM_SIZ_8b,  8, 8, 0, NULL, 0, out) == 0);
    CHECK(gfxTextureDecode(src, sizeof(src), G_IM_FMT_IA,   G_IM_SIZ_4b,  8, 8, 0, NULL, 0, out) == 0);
    CHECK(gfxTextureDecode(src, sizeof(src), G_IM_FMT_I,    G_IM_SIZ_8b,  8, 8, 0, NULL, 0, out) == 0);
    CHECK(gfxTextureDecode(src, sizeof(src), G_IM_FMT_I,    G_IM_SIZ_4b,  8, 8, 0, NULL, 0, out) == 0);
    CHECK(gfxTextureDecode(src, sizeof(src), G_IM_FMT_CI,   G_IM_SIZ_8b,  8, 8, 0, pal, 256, out) == 0);
    CHECK(gfxTextureDecode(src, sizeof(src), G_IM_FMT_CI,   G_IM_SIZ_4b,  8, 8, 0, pal, 16, out) == 0);
}

int main(void)
{
    platformInit();

    printf("ge007 port: texture decoder tests\n\n");

    test_bit_expansion();
    test_intensity_drives_alpha();
    test_row_bytes();
    test_rgba16_image();
    test_rgba32_image();
    test_nibble_order();
    test_ci_palette();
    test_stride();
    test_bounds_and_bad_args();
    test_all_formats_run();

    printf("\n%d checks, %d failure(s)\n", g_checks, g_failures);
    platformShutdown();
    return g_failures ? 1 : 0;
}
