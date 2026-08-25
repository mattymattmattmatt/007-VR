/*
 * Tests for ROM loading, byte-order normalisation, address translation and
 * the PI DMA path.
 *
 * No real ROM is involved: the load tests build a synthetic image with a valid
 * N64 header and a recognisable payload, which exercises every code path
 * without any game data.
 */
#include "platform.h"
#include "romdata.h"
#include "sha1.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <PR/ultratypes.h>
#include <PR/os.h>

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

/* ------------------------------------------------------------------ sha1 */

static void test_sha1(void)
{
    char hex[41];

    printf("sha1: published test vectors\n");

    gepc_sha1_hex("", 0, hex);
    CHECK(strcmp(hex, "da39a3ee5e6b4b0d3255bfef95601890afd80709") == 0);

    gepc_sha1_hex("abc", 3, hex);
    CHECK(strcmp(hex, "a9993e364706816aba3e25717850c26c9cd0d89d") == 0);

    gepc_sha1_hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", 56, hex);
    CHECK(strcmp(hex, "84983e441c3bd26ebaae4aa1f95129e5e54670f1") == 0);

    /* Longer than one block and not a block multiple, which is where a
     * padding bug shows up. */
    {
        char *buf = (char *)malloc(1000000);
        memset(buf, 'a', 1000000);
        gepc_sha1_hex(buf, 1000000, hex);
        CHECK(strcmp(hex, "34aa973cd4c4daa4f61eeb2bdbad27316534016f") == 0);
        free(buf);
    }
}

/* ----------------------------------------------------------- byte order */

static void test_format_detection(void)
{
    unsigned char z64[8] = { 0x80, 0x37, 0x12, 0x40, 0, 0, 0, 0 };
    unsigned char v64[8] = { 0x37, 0x80, 0x40, 0x12, 0, 0, 0, 0 };
    unsigned char n64[8] = { 0x40, 0x12, 0x37, 0x80, 0, 0, 0, 0 };
    unsigned char junk[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };

    printf("rom: dump format detection\n");

    CHECK(romdataDetectFormat(z64, 8) == GEPC_ROM_FORMAT_Z64);
    CHECK(romdataDetectFormat(v64, 8) == GEPC_ROM_FORMAT_V64);
    CHECK(romdataDetectFormat(n64, 8) == GEPC_ROM_FORMAT_N64);
    CHECK(romdataDetectFormat(junk, 8) == GEPC_ROM_FORMAT_INVALID);

    /* Too short to hold a magic must not read past the buffer. */
    CHECK(romdataDetectFormat(z64, 2) == GEPC_ROM_FORMAT_INVALID);
    CHECK(romdataDetectFormat(NULL, 8) == GEPC_ROM_FORMAT_INVALID);
}

static void test_normalise(void)
{
    unsigned char v64[8] = { 0x37, 0x80, 0x40, 0x12, 0xCD, 0xAB, 0x01, 0xEF };
    /* n64 format reverses each whole 32-bit word, so the z64 pair
     * AB CD EF 01 appears on disk as 01 EF CD AB. */
    unsigned char n64[8] = { 0x40, 0x12, 0x37, 0x80, 0x01, 0xEF, 0xCD, 0xAB };
    unsigned char z64[8] = { 0x80, 0x37, 0x12, 0x40, 0xAB, 0xCD, 0xEF, 0x01 };
    unsigned char keep[8];

    printf("rom: byte order normalisation\n");

    memcpy(keep, z64, 8);

    romdataNormalise(v64, 8, GEPC_ROM_FORMAT_V64);
    CHECK(memcmp(v64, keep, 8) == 0);

    romdataNormalise(n64, 8, GEPC_ROM_FORMAT_N64);
    CHECK(memcmp(n64, keep, 8) == 0);

    /* A z64 image must be left exactly alone. */
    romdataNormalise(z64, 8, GEPC_ROM_FORMAT_Z64);
    CHECK(memcmp(z64, keep, 8) == 0);

    /* Odd trailing bytes must not be read past. */
    {
        unsigned char odd[6] = { 0x37, 0x80, 0x40, 0x12, 0xAA, 0xBB };
        romdataNormalise(odd, 6, GEPC_ROM_FORMAT_V64);
        CHECK(odd[0] == 0x80 && odd[4] == 0xBB && odd[5] == 0xAA);
    }
}

/* ------------------------------------------------- address translation */

static void test_addr_translation(void)
{
    printf("rom: cartridge address translation\n");

    /* KSEG1 and KSEG0 views of the same cartridge byte. */
    CHECK(romdataAddrToOffset(0xB0000000u) == 0);
    CHECK(romdataAddrToOffset(0x90000000u) == 0);
    CHECK(romdataAddrToOffset(0x10000000u) == 0);

    CHECK(romdataAddrToOffset(0xB0001000u) == 0x1000);
    CHECK(romdataAddrToOffset(0xB0FFFFFFu) == 0x0FFFFFF);

    /* RDRAM is not cartridge space. */
    CHECK(romdataAddrToOffset(0x80000000u) == -1);
    CHECK(romdataAddrToOffset(0x00000000u) == -1);
    CHECK(romdataAddrToOffset(0x80200000u) == -1);
}

/* ---------------------------------------------------------- load + read */

#define FAKE_ROM_SIZE 4096

static char g_fake_path[1024];

static int write_fake_rom(void)
{
    unsigned char *rom = (unsigned char *)malloc(FAKE_ROM_SIZE);
    FILE *f;
    int i;
    int ok;

    if (!rom) {
        return -1;
    }

    /* Valid z64 magic, then a byte pattern we can assert on. */
    rom[0] = 0x80; rom[1] = 0x37; rom[2] = 0x12; rom[3] = 0x40;
    for (i = 4; i < FAKE_ROM_SIZE; i++) {
        rom[i] = (unsigned char)(i & 0xFF);
    }

    snprintf(g_fake_path, sizeof(g_fake_path), "%s", "fake_test_rom.z64");
    f = fopen(g_fake_path, "wb");
    if (!f) {
        free(rom);
        return -1;
    }
    ok = (fwrite(rom, 1, FAKE_ROM_SIZE, f) == FAKE_ROM_SIZE);
    fclose(f);
    free(rom);
    return ok ? 0 : -1;
}

static void test_load_and_read(void)
{
    unsigned char buf[64];

    printf("rom: load, identify and read\n");

    if (write_fake_rom() != 0) {
        printf("  (skipped: could not write a temporary ROM)\n");
        return;
    }

    CHECK(romdataLoad(g_fake_path) == 0);
    CHECK(romdataIsLoaded());
    CHECK(romdataGetSize() == FAKE_ROM_SIZE);

    /* A synthetic ROM is obviously not one of the three known releases; the
     * loader must say so and carry on rather than refuse. */
    CHECK(romdataGetVersion() == GEPC_ROM_UNKNOWN);
    CHECK(strlen(romdataGetSha1()) == 40);

    /* In-range read returns the pattern that was written. */
    CHECK(romdataRead(0x100, buf, 16) == 0);
    CHECK(buf[0] == (unsigned char)0x00);
    CHECK(buf[1] == (unsigned char)0x01);
    CHECK(buf[15] == (unsigned char)0x0F);

    /* Reads that run off the end must be refused, not truncated. */
    CHECK(romdataRead(FAKE_ROM_SIZE - 8, buf, 16) == -1);
    CHECK(romdataRead(FAKE_ROM_SIZE, buf, 1) == -1);

    /* A size large enough to wrap a 32-bit sum must still be caught. */
    CHECK(romdataRead(0x100, buf, 0xFFFFFFFFu) == -1);

    /* Exactly reaching the last byte is legal. */
    CHECK(romdataRead(FAKE_ROM_SIZE - 4, buf, 4) == 0);

    romdataUnload();
    CHECK(!romdataIsLoaded());
    CHECK(romdataRead(0, buf, 4) == -1);

    remove(g_fake_path);
}

static void test_bad_rom_paths(void)
{
    printf("rom: bad inputs are reported, not crashed on\n");

    CHECK(romdataLoad("this_file_does_not_exist.z64") == -1);
    CHECK(!romdataIsLoaded());

    /* A real file that is not a ROM. */
    {
        FILE *f = fopen("not_a_rom.bin", "wb");
        if (f) {
            fwrite("hello world, definitely not a rom", 1, 33, f);
            fclose(f);
            CHECK(romdataLoad("not_a_rom.bin") == -1);
            CHECK(!romdataIsLoaded());
            remove("not_a_rom.bin");
        }
    }
}

/* ------------------------------------------------------------- PI DMA */

static void test_pi_dma(void)
{
    OSIoMesg mb;
    OSMesgQueue mq;
    OSMesg msgbuf[4];
    OSMesg got;
    unsigned char dst[32];

    printf("pi: dma reads the rom and always completes\n");

    if (write_fake_rom() != 0) {
        printf("  (skipped: could not write a temporary ROM)\n");
        return;
    }
    CHECK(romdataLoad(g_fake_path) == 0);

    osCreateMesgQueue(&mq, msgbuf, 4);
    memset(&mb, 0, sizeof(mb));
    memset(dst, 0xEE, sizeof(dst));

    /* A normal read, addressed the way the game addresses the cartridge. */
    osPiStartDma(&mb, OS_MESG_PRI_NORMAL, OS_READ, 0xB0000100u, dst, 16, &mq);
    CHECK(osRecvMesg(&mq, &got, OS_MESG_NOBLOCK) == 0);
    CHECK(dst[0] == 0x00 && dst[1] == 0x01 && dst[15] == 0x0F);

    /* romCopy() blocks on the completion message unconditionally. If a failed
     * read ever skipped posting it, the game would hang forever rather than
     * show a bad texture -- so every failure path must still post. */
    memset(dst, 0xEE, sizeof(dst));
    osPiStartDma(&mb, OS_MESG_PRI_NORMAL, OS_READ, 0x80000000u, dst, 16, &mq);
    CHECK(osRecvMesg(&mq, &got, OS_MESG_NOBLOCK) == 0);
    CHECK(dst[0] == 0x00); /* zeroed, not left as 0xEE garbage */

    /* Out-of-range cartridge offset: same rule. */
    memset(dst, 0xEE, sizeof(dst));
    osPiStartDma(&mb, OS_MESG_PRI_NORMAL, OS_READ, 0xB0FFFF00u, dst, 16, &mq);
    CHECK(osRecvMesg(&mq, &got, OS_MESG_NOBLOCK) == 0);
    CHECK(dst[0] == 0x00);

    /* A write to the cartridge is accepted and dropped, but still completes. */
    osPiStartDma(&mb, OS_MESG_PRI_NORMAL, 1 /* OS_WRITE */, 0xB0000100u,
                 dst, 16, &mq);
    CHECK(osRecvMesg(&mq, &got, OS_MESG_NOBLOCK) == 0);

    /* The io message must describe the transfer that was asked for. */
    CHECK(mb.devAddr == 0xB0000100u);
    CHECK(mb.size == 16);

    /* Raw reads are big endian off the cartridge bus regardless of host. */
    {
        u32 word = 0;
        CHECK(osPiRawReadIo(0xB0000100u, &word) == 0);
        CHECK(word == 0x00010203u);
    }

    romdataUnload();
    remove(g_fake_path);
}

int main(void)
{
    platformInit();

    printf("ge007 port: romdata tests\n\n");

    test_sha1();
    test_format_detection();
    test_normalise();
    test_addr_translation();
    test_load_and_read();
    test_bad_rom_paths();
    test_pi_dma();

    printf("\n%d checks, %d failure(s)\n", g_checks, g_failures);
    platformShutdown();
    return g_failures ? 1 : 0;
}
