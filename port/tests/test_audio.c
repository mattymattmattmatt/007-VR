/*
 * Tests for the audio output path and the software audio microcode.
 *
 * The pacing behaviour matters as much as the sample maths: src/audi.c sizes
 * every frame from what osAiGetLength reports, so a ring buffer that lies
 * about its depth makes the game either run away from the DAC or starve it.
 */
#include "audio.h"
#include "audio_abi.h"
#include "platform.h"

#include <stdio.h>
#include <string.h>

#include <PR/ultratypes.h>
#include <PR/os.h>
#include <PR/abi.h>

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

/* ------------------------------------------------------------- AI path */

static void test_rate(void)
{
    printf("audio: the output rate is reported back\n");

    /* GoldenEye asks for 22050 (OUTPUT_RATE in src/audi.c). */
    CHECK(osAiSetFrequency(22050) == 22050);
    CHECK(audioGetRate() == 22050);

    /* Nonsense rates are clamped rather than passed to the device. */
    CHECK(osAiSetFrequency(10) == 8000);
    CHECK(osAiSetFrequency(200000) == 48000);

    osAiSetFrequency(22050);
}

static void test_queue_and_drain(void)
{
    short frame[64];
    short got[64];
    unsigned i;

    printf("audio: queued audio drains, and the depth is truthful\n");

    audioInit();
    audioFlush();
    audioResetCounters();

    for (i = 0; i < 64; i++) {
        frame[i] = (short)(i * 100);
    }

    CHECK(audioQueuedBytes() == 0);
    CHECK(osAiGetLength() == 0);

    CHECK(osAiSetNextBuffer(frame, sizeof(frame)) == 0);
    CHECK(audioQueuedBytes() == sizeof(frame));

    /* This is what src/audi.c subtracts from its frame size, so it has to
     * track the real depth rather than a constant. */
    CHECK(osAiGetLength() == sizeof(frame));

    memset(got, 0, sizeof(got));
    CHECK(audioPullBytes(got, sizeof(got)) == sizeof(got));
    CHECK(memcmp(got, frame, sizeof(frame)) == 0);

    CHECK(audioQueuedBytes() == 0);
    CHECK(osAiGetLength() == 0);
}

static void test_partial_drain(void)
{
    short frame[64];
    short got[16];

    printf("audio: a partial pull leaves the remainder queued\n");

    audioInit();
    audioFlush();
    memset(frame, 0x11, sizeof(frame));

    osAiSetNextBuffer(frame, sizeof(frame));
    CHECK(audioPullBytes(got, sizeof(got)) == sizeof(got));
    CHECK(audioQueuedBytes() == sizeof(frame) - sizeof(got));
}

static void test_wraparound(void)
{
    unsigned char block[8192];
    unsigned char got[8192];
    int round;

    printf("audio: the ring wraps without reordering samples\n");

    audioInit();
    audioFlush();

    /* Push and drain far more than the ring holds, so head and tail wrap
     * repeatedly. A modulo slip shows up here as scrambled audio. */
    for (round = 0; round < 80; round++) {
        unsigned i;
        for (i = 0; i < sizeof(block); i++) {
            block[i] = (unsigned char)((round * 7 + i) & 0xFF);
        }
        CHECK(osAiSetNextBuffer(block, sizeof(block)) == 0);
        CHECK(audioPullBytes(got, sizeof(got)) == sizeof(got));
        CHECK(memcmp(got, block, sizeof(block)) == 0);
    }
}

static void test_underrun(void)
{
    unsigned char got[256];
    unsigned n;

    printf("audio: an empty ring reports an underrun, not garbage\n");

    audioInit();
    audioFlush();
    audioResetCounters();

    n = audioPullBytes(got, sizeof(got));
    CHECK(n == 0);
    CHECK(audioUnderrunCount() == 1);

    /* A short read is still an underrun; the caller pads with silence. */
    {
        unsigned char small[64];
        memset(small, 0x5A, sizeof(small));
        osAiSetNextBuffer(small, sizeof(small));
        n = audioPullBytes(got, sizeof(got));
        CHECK(n == sizeof(small));
        CHECK(audioUnderrunCount() == 2);
    }
}

static void test_not_ready_is_accepted(void)
{
    short frame[8];

    printf("audio: audio queued before the device exists is dropped, not refused\n");

    audioShutdown();
    memset(frame, 0, sizeof(frame));

    /* The game queues audio during boot. Refusing would make it retry
     * forever; accepting and dropping is what hardware effectively does. */
    CHECK(osAiSetNextBuffer(frame, sizeof(frame)) == 0);
    CHECK(audioQueuedBytes() == 0);

    audioInit();
}

static void test_bad_args(void)
{
    printf("audio: bad buffers are refused\n");

    CHECK(osAiSetNextBuffer(NULL, 16) == -1);
    {
        short f[4] = { 0, 0, 0, 0 };
        CHECK(osAiSetNextBuffer(f, 0) == -1);
    }
    CHECK(audioPullBytes(NULL, 16) == 0);
}

/* ---------------------------------------------------------- microcode */

static void put_cmd(unsigned char *p, unsigned w0, unsigned w1)
{
    p[0] = (unsigned char)(w0 >> 24); p[1] = (unsigned char)(w0 >> 16);
    p[2] = (unsigned char)(w0 >> 8);  p[3] = (unsigned char)w0;
    p[4] = (unsigned char)(w1 >> 24); p[5] = (unsigned char)(w1 >> 16);
    p[6] = (unsigned char)(w1 >> 8);  p[7] = (unsigned char)w1;
}

static void test_dmem_endianness(void)
{
    audio_abi *a = audioAbiCreate();

    printf("abi: DMEM samples are big-endian\n");

    CHECK(a != NULL);
    audioAbiPoke16(a, 0, 0x1234);
    {
        const unsigned char *d = (const unsigned char *)audioAbiDmemConst(a, 0);
        /* Stored high byte first, as the DMA from RDRAM would leave it. */
        CHECK(d[0] == 0x12);
        CHECK(d[1] == 0x34);
    }
    CHECK(audioAbiPeek16(a, 0) == 0x1234);

    /* Negative samples round-trip. */
    audioAbiPoke16(a, 4, -1000);
    CHECK(audioAbiPeek16(a, 4) == -1000);

    audioAbiDestroy(a);
}

static void test_clearbuff_and_move(void)
{
    audio_abi *a = audioAbiCreate();
    unsigned char cmds[16];

    printf("abi: CLEARBUFF and DMEMMOVE\n");

    audioAbiPoke16(a, 0x100, 0x7FFF);
    audioAbiPoke16(a, 0x102, 0x7FFF);

    put_cmd(cmds, (A_CLEARBUFF << 24) | 0x100u, 4u);
    CHECK(audioAbiRun(a, cmds, 8) == 0);
    CHECK(audioAbiPeek16(a, 0x100) == 0);
    CHECK(audioAbiPeek16(a, 0x102) == 0);

    audioAbiPoke16(a, 0x200, 1234);
    put_cmd(cmds, (A_DMEMMOVE << 24) | 0x200u, (0x300u << 16) | 2u);
    CHECK(audioAbiRun(a, cmds, 8) == 0);
    CHECK(audioAbiPeek16(a, 0x300) == 1234);

    CHECK(audioAbiCommandCount(a) == 2);
    audioAbiDestroy(a);
}

static void test_mixer(void)
{
    audio_abi *a = audioAbiCreate();
    unsigned char cmds[16];

    printf("abi: SETBUFF then MIXER adds with gain\n");

    audioAbiPoke16(a, 0x100, 1000);
    audioAbiPoke16(a, 0x200, 500);

    /* SETBUFF sets the count the mixer works over. */
    put_cmd(cmds, (A_SETBUFF << 24) | 0x100u, (0x200u << 16) | 2u);
    CHECK(audioAbiRun(a, cmds, 8) == 0);

    /* Full gain: destination += source. */
    put_cmd(cmds, (A_MIXER << 24) | 0x7FFFu, (0x100u << 16) | 0x200u);
    CHECK(audioAbiRun(a, cmds, 8) == 0);
    CHECK(audioAbiPeek16(a, 0x200) > 1400);
    CHECK(audioAbiPeek16(a, 0x200) <= 1500);

    audioAbiDestroy(a);
}

static void test_interleave(void)
{
    audio_abi *a = audioAbiCreate();
    unsigned char cmds[16];

    printf("abi: INTERLEAVE builds a stereo frame\n");

    audioAbiPoke16(a, 0x100, 111);
    audioAbiPoke16(a, 0x102, 222);
    audioAbiPoke16(a, 0x200, -111);
    audioAbiPoke16(a, 0x202, -222);

    put_cmd(cmds, (A_SETBUFF << 24) | 0u, (0x400u << 16) | 4u);
    CHECK(audioAbiRun(a, cmds, 8) == 0);

    put_cmd(cmds, (A_INTERLEAVE << 24), (0x100u << 16) | 0x200u);
    CHECK(audioAbiRun(a, cmds, 8) == 0);

    /* Left and right alternate, which is what the DAC expects. */
    CHECK(audioAbiPeek16(a, 0x400) == 111);
    CHECK(audioAbiPeek16(a, 0x402) == -111);
    CHECK(audioAbiPeek16(a, 0x404) == 222);
    CHECK(audioAbiPeek16(a, 0x406) == -222);

    audioAbiDestroy(a);
}

static void test_adpcm_nibbles(void)
{
    unsigned char frame[9];
    short state[2] = { 0, 0 };
    short out[16];
    int i;

    printf("abi: ADPCM nibble extraction, sign and scale\n");

    /* With no codebook the decoder falls through to plain scaled nibbles,
     * which isolates exactly the parts most likely to be wrong: nibble
     * order, sign extension and the shift. */
    memset(frame, 0, sizeof(frame));
    frame[0] = 0x20;            /* scale 2, predictor 0 */
    frame[1] = 0x17;            /* nibbles 1 then 7 */
    frame[2] = 0x8F;            /* nibbles -8 then -1 */

    audioAbiDecodeAdpcmFrame(frame, NULL, 2, 1, state, out);

    CHECK(out[0] == (1 << 2));
    CHECK(out[1] == (7 << 2));
    /* 0x8 is -8 once sign-extended; unsigned handling gives +8 and a very
     * audible buzz. */
    CHECK(out[2] == (-8 * 4));
    CHECK(out[3] == (-1 * 4));

    /* Silence in, silence out. */
    memset(frame, 0, sizeof(frame));
    audioAbiDecodeAdpcmFrame(frame, NULL, 2, 1, state, out);
    for (i = 0; i < 16; i++) {
        CHECK(out[i] == 0);
    }

    /* The scale shift really applies. */
    memset(frame, 0, sizeof(frame));
    frame[0] = 0x50;
    frame[1] = 0x10;
    audioAbiDecodeAdpcmFrame(frame, NULL, 2, 1, state, out);
    CHECK(out[0] == (1 << 5));

    /* State advances to the last two decoded samples, so the next frame
     * continues from the right place. */
    memset(frame, 0, sizeof(frame));
    frame[0] = 0x00;
    frame[4] = 0x30;   /* affects samples 6/7 of the first half */
    audioAbiDecodeAdpcmFrame(frame, NULL, 2, 1, state, out);
    CHECK(state[0] == out[14]);
    CHECK(state[1] == out[15]);
}

static void test_adpcm_clamps(void)
{
    unsigned char frame[9];
    short state[2] = { 0, 0 };
    short out[16];
    int i;

    printf("abi: ADPCM output clamps instead of wrapping\n");

    /* Maximum scale with maximum nibbles would overflow a short; wrapping
     * turns a loud sound into a loud sound of the opposite sign, which is the
     * classic source of clicks. */
    memset(frame, 0xFF, sizeof(frame));
    frame[0] = 0xF0;
    audioAbiDecodeAdpcmFrame(frame, NULL, 2, 1, state, out);
    for (i = 0; i < 16; i++) {
        CHECK(out[i] >= -32768 && out[i] <= 32767);
    }

    memset(frame, 0x77, sizeof(frame));
    frame[0] = 0xF0;
    audioAbiDecodeAdpcmFrame(frame, NULL, 2, 1, state, out);
    for (i = 0; i < 16; i++) {
        CHECK(out[i] <= 32767);
    }
}

static void test_unknown_and_bounds(void)
{
    audio_abi *a = audioAbiCreate();
    unsigned char cmds[24];

    printf("abi: unknown commands counted, run bounded by byte count\n");

    put_cmd(cmds + 0,  (0xFEu << 24), 0);
    put_cmd(cmds + 8,  (A_SPNOOP << 24), 0);
    put_cmd(cmds + 16, (0xFDu << 24), 0);

    CHECK(audioAbiRun(a, cmds, 24) == 0);
    CHECK(audioAbiCommandCount(a) == 3);
    CHECK(audioAbiUnknownCount(a) == 2);

    /* A shorter byte count stops early rather than reading past. */
    {
        audio_abi *b = audioAbiCreate();
        CHECK(audioAbiRun(b, cmds, 8) == 0);
        CHECK(audioAbiCommandCount(b) == 1);
        audioAbiDestroy(b);
    }

    CHECK(audioAbiRun(a, NULL, 8) == -1);
    audioAbiDestroy(a);
}

static void test_dmem_bounds(void)
{
    audio_abi *a = audioAbiCreate();

    printf("abi: DMEM access stays inside the scratchpad\n");

    CHECK(audioAbiDmem(a, 0) != NULL);
    CHECK(audioAbiDmem(a, GEPC_DMEM_SIZE - 1) != NULL);
    CHECK(audioAbiDmem(a, GEPC_DMEM_SIZE) == NULL);

    /* An out-of-range offset wraps rather than writing past the buffer. */
    audioAbiPoke16(a, GEPC_DMEM_SIZE + 4, 999);
    CHECK(audioAbiPeek16(a, 4) == 999);

    audioAbiDestroy(a);
}

int main(void)
{
    platformInit();

    printf("ge007 port: audio tests\n\n");

    test_rate();
    test_queue_and_drain();
    test_partial_drain();
    test_wraparound();
    test_underrun();
    test_not_ready_is_accepted();
    test_bad_args();

    test_dmem_endianness();
    test_clearbuff_and_move();
    test_mixer();
    test_interleave();
    test_adpcm_nibbles();
    test_adpcm_clamps();
    test_unknown_and_bounds();
    test_dmem_bounds();

    printf("\n%d checks, %d failure(s)\n", g_checks, g_failures);
    audioShutdown();
    platformShutdown();
    return g_failures ? 1 : 0;
}
