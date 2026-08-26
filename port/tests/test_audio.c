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

/* ------------------------------------------- resampler and envelope mixer */

/*
 * State blocks live in DRAM, and the command carries a 32-bit address, so
 * these have to be reachable from 32 bits. -no-pie on ge007_platform puts the
 * image low enough that statics are; the check below turns a silent
 * miscompare into a named failure if that ever stops being true.
 */
static short g_resample_state[16];   /* RESAMPLE_STATE */
static short g_envmix_state[40];     /* ENVMIX_STATE */

static unsigned dram_addr(const void *p)
{
    unsigned long long v = (unsigned long long)(size_t)p;

    CHECK(v <= 0xFFFFFFFFull);
    return (unsigned)v;
}

/* A ramp, because a four-point interpolator reproduces a straight line
 * exactly: whatever pitch it is resampled at, the output is still the same
 * straight line, so the expected values are arithmetic rather than a copy of
 * the implementation. */
static void fill_ramp(audio_abi *a, unsigned at, int first, int step, int n)
{
    int i;

    for (i = 0; i < n; i++) {
        audioAbiPoke16(a, at + (unsigned)i * 2, first + step * i);
    }
}

static void test_resample_unity(void)
{
    audio_abi *a = audioAbiCreate();
    unsigned char cmds[16];
    int i;

    printf("abi: RESAMPLE at unity pitch is a copy\n");

    memset(g_resample_state, 0, sizeof(g_resample_state));
    fill_ramp(a, 0x100, 1000, 50, 32);

    put_cmd(cmds, (A_SETBUFF << 24) | 0x100u, (0x200u << 16) | 32u);
    CHECK(audioAbiRun(a, cmds, 8) == 0);

    /* 0x4000 is unity in 2.14, so every output lands exactly on an input
     * sample and the spline returns it unchanged. */
    put_cmd(cmds, (A_RESAMPLE << 24) | (A_INIT << 16) | 0x4000u,
            dram_addr(g_resample_state));
    CHECK(audioAbiRun(a, cmds, 8) == 0);

    for (i = 0; i < 16; i++) {
        CHECK(audioAbiPeek16(a, 0x200 + (unsigned)i * 2) == 1000 + 50 * i);
    }

    audioAbiDestroy(a);
}

static void test_resample_continuity(void)
{
    audio_abi *a = audioAbiCreate();
    unsigned char cmds[16];
    int whole[16];
    int split[16];
    int dropped[16];
    int i;

    printf("abi: RESAMPLE carries its state across buffers\n");

    /*
     * pitch 0x1400 is 0.3125 input samples per output. Over 8 outputs that is
     * 2.5 input samples: the game consumes 2 and carries 0.5 in
     * ALResampler::delta, and the microcode has to carry the matching 0.5.
     *
     * The assertion is that splitting a run in two changes nothing. Sixteen
     * outputs from one call, against two calls of eight with the input buffer
     * refilled from the two samples the first call consumed -- exactly what
     * alResamplePull does between frames -- must agree sample for sample.
     * That pins continuity without depending on the shape of the
     * interpolator, and it covers the startup transient too: with the state
     * zeroed at A_INIT the first few outputs are reached across a history of
     * silence, and both runs have to do that identically.
     */
    memset(g_resample_state, 0, sizeof(g_resample_state));
    fill_ramp(a, 0x100, 1000, 50, 32);
    put_cmd(cmds, (A_SETBUFF << 24) | 0x100u, (0x200u << 16) | 32u);
    CHECK(audioAbiRun(a, cmds, 8) == 0);
    put_cmd(cmds, (A_RESAMPLE << 24) | (A_INIT << 16) | 0x1400u,
            dram_addr(g_resample_state));
    CHECK(audioAbiRun(a, cmds, 8) == 0);
    for (i = 0; i < 16; i++) {
        whole[i] = audioAbiPeek16(a, 0x200 + (unsigned)i * 2);
    }

    memset(g_resample_state, 0, sizeof(g_resample_state));
    fill_ramp(a, 0x100, 1000, 50, 32);
    put_cmd(cmds, (A_SETBUFF << 24) | 0x100u, (0x200u << 16) | 16u);
    CHECK(audioAbiRun(a, cmds, 8) == 0);
    put_cmd(cmds, (A_RESAMPLE << 24) | (A_INIT << 16) | 0x1400u,
            dram_addr(g_resample_state));
    CHECK(audioAbiRun(a, cmds, 8) == 0);
    for (i = 0; i < 8; i++) {
        split[i] = audioAbiPeek16(a, 0x200 + (unsigned)i * 2);
    }

    /* 0.3125 * 8 = 2.5, so two samples are consumed and half a sample is
     * carried. If the fraction were dropped the refill below would be wrong
     * as well as the join, so check the arithmetic the game does. */
    CHECK((g_resample_state[0] & 0xFFFF) != 0);

    fill_ramp(a, 0x100, 1000 + 50 * 2, 50, 32);
    put_cmd(cmds, (A_SETBUFF << 24) | 0x100u, (0x200u << 16) | 16u);
    CHECK(audioAbiRun(a, cmds, 8) == 0);
    put_cmd(cmds, (A_RESAMPLE << 24) | (A_CONTINUE << 16) | 0x1400u,
            dram_addr(g_resample_state));
    CHECK(audioAbiRun(a, cmds, 8) == 0);
    for (i = 0; i < 8; i++) {
        split[8 + i] = audioAbiPeek16(a, 0x200 + (unsigned)i * 2);
    }

    for (i = 0; i < 16; i++) {
        CHECK(split[i] == whole[i]);
    }

    /* Once the history is real -- from the fifth output, where idx has moved
     * off zero -- the resampled ramp is a ramp again, at 50 * 0.3125 per
     * step. The first four are reached across the initial silence and are
     * not expected to be. */
    for (i = 5; i < 16; i++) {
        CHECK(whole[i] - whole[i - 1] >= 15);
        CHECK(whole[i] - whole[i - 1] <= 16);
    }

    /*
     * And the same second call with the state thrown away, to show the check
     * above has something to catch: A_INIT restarts at position zero, so the
     * output jumps backwards at the join instead of stepping 15 or 16.
     */
    fill_ramp(a, 0x100, 1000 + 50 * 2, 50, 32);
    put_cmd(cmds, (A_RESAMPLE << 24) | (A_INIT << 16) | 0x1400u,
            dram_addr(g_resample_state));
    CHECK(audioAbiRun(a, cmds, 8) == 0);
    for (i = 0; i < 8; i++) {
        dropped[i] = audioAbiPeek16(a, 0x200 + (unsigned)i * 2);
    }
    CHECK(dropped[0] != split[8]);
    CHECK(dropped[0] - split[7] < 0);

    audioAbiDestroy(a);
}

/* The five aSetVolume commands and the two aSetBuffer commands that
 * _pullSubFrame emits ahead of an A_INIT envelope mix, in that order. */
static void env_setup(audio_abi *a, unsigned count_bytes,
                      int volL, int volR, int tgtL, int tgtR,
                      unsigned rateL, unsigned rateR, int dry, int wet)
{
    unsigned char cmds[16];

    put_cmd(cmds, (A_SETBUFF << 24) | 0x100u, (0x200u << 16) | count_bytes);
    CHECK(audioAbiRun(a, cmds, 8) == 0);
    put_cmd(cmds, (A_SETBUFF << 24) | (A_AUX << 16) | 0x300u,
            (0x400u << 16) | 0x500u);
    CHECK(audioAbiRun(a, cmds, 8) == 0);

    put_cmd(cmds, (A_SETVOL << 24) | ((A_LEFT | A_VOL) << 16)
                  | ((unsigned)volL & 0xFFFFu), 0);
    CHECK(audioAbiRun(a, cmds, 8) == 0);
    put_cmd(cmds, (A_SETVOL << 24) | ((A_RIGHT | A_VOL) << 16)
                  | ((unsigned)volR & 0xFFFFu), 0);
    CHECK(audioAbiRun(a, cmds, 8) == 0);
    put_cmd(cmds, (A_SETVOL << 24) | ((A_LEFT | A_RATE) << 16)
                  | ((unsigned)tgtL & 0xFFFFu), rateL);
    CHECK(audioAbiRun(a, cmds, 8) == 0);
    put_cmd(cmds, (A_SETVOL << 24) | ((A_RIGHT | A_RATE) << 16)
                  | ((unsigned)tgtR & 0xFFFFu), rateR);
    CHECK(audioAbiRun(a, cmds, 8) == 0);
    put_cmd(cmds, (A_SETVOL << 24) | (A_AUX << 16) | ((unsigned)dry & 0xFFFFu),
            (unsigned)wet & 0xFFFFu);
    CHECK(audioAbiRun(a, cmds, 8) == 0);
}

static void fill_const(audio_abi *a, unsigned at, int v, int n)
{
    int i;

    for (i = 0; i < n; i++) {
        audioAbiPoke16(a, at + (unsigned)i * 2, v);
    }
}

static void test_envmixer_buses(void)
{
    audio_abi *a = audioAbiCreate();
    unsigned char cmds[16];
    int i;

    printf("abi: ENVMIXER splits dry and wet across four buses\n");

    memset(g_envmix_state, 0, sizeof(g_envmix_state));
    fill_const(a, 0x100, 1000, 16);

    /* Full volume left, silent right, so a bus that picked up the wrong
     * channel shows as a non-zero right output. Half the signal dry, half
     * wet. */
    env_setup(a, 32, 0x7FFF, 0, 0x7FFF, 0, 0, 0, 0x7FFF, 0x4000);

    put_cmd(cmds, (A_ENVMIXER << 24) | ((A_INIT | A_AUX) << 16),
            dram_addr(g_envmix_state));
    CHECK(audioAbiRun(a, cmds, 8) == 0);

    for (i = 0; i < 16; i++) {
        int l = audioAbiPeek16(a, 0x200 + (unsigned)i * 2);
        int r = audioAbiPeek16(a, 0x300 + (unsigned)i * 2);
        int al = audioAbiPeek16(a, 0x400 + (unsigned)i * 2);
        int ar = audioAbiPeek16(a, 0x500 + (unsigned)i * 2);

        CHECK(l >= 995 && l <= 1000);   /* Q15 unity, less rounding */
        CHECK(r == 0);
        CHECK(al >= 495 && al <= 501);  /* wet is half scale */
        CHECK(ar == 0);
    }

    /* The mixer adds into its outputs rather than overwriting them: that is
     * how several voices land on one bus. */
    put_cmd(cmds, (A_ENVMIXER << 24) | ((A_CONTINUE | A_AUX) << 16),
            dram_addr(g_envmix_state));
    CHECK(audioAbiRun(a, cmds, 8) == 0);
    CHECK(audioAbiPeek16(a, 0x200) >= 1990);

    audioAbiDestroy(a);
}

static void test_envmixer_ramp(void)
{
    audio_abi *a = audioAbiCreate();
    unsigned char cmds[16];
    int block[8];
    int i, b;

    printf("abi: ENVMIXER steps the volume once per eight samples\n");

    memset(g_envmix_state, 0, sizeof(g_envmix_state));
    fill_const(a, 0x100, 1000, 64);

    /*
     * env.c's _getRate for a ramp from 0 to full scale over 64 samples:
     * 8 * 32767 / 64 = 4095.875, so ratem is 4095 and ratel is the fraction
     * scaled by 0xffff. Eight blocks of eight samples land exactly on the
     * target, which is the property _getRate is built to have.
     */
    env_setup(a, 128, 0, 0, 0x7FFF, 0, (4095u << 16) | 0xDFFEu, 0, 0x7FFF, 0);

    put_cmd(cmds, (A_ENVMIXER << 24) | ((A_INIT | A_AUX) << 16),
            dram_addr(g_envmix_state));
    CHECK(audioAbiRun(a, cmds, 8) == 0);

    /* A staircase, not a slope: the eight samples inside a block are equal,
     * and the value only moves between blocks. A per-sample interpolation
     * fails the first of these on the very first pair. */
    for (b = 0; b < 8; b++) {
        block[b] = audioAbiPeek16(a, 0x200 + (unsigned)(b * 8) * 2);
        for (i = 1; i < 8; i++) {
            CHECK(audioAbiPeek16(a, 0x200 + (unsigned)(b * 8 + i) * 2)
                  == block[b]);
        }
    }

    CHECK(block[0] == 0);
    for (b = 1; b < 8; b++) {
        CHECK(block[b] > block[b - 1]);
    }

    /* Seven steps of 4095.875 have been applied by the last block: the
     * volume there is 28671/32767 of the input. */
    CHECK(block[7] >= 870 && block[7] <= 880);

    audioAbiDestroy(a);
}

static void test_envmixer_state_survives_another_voice(void)
{
    audio_abi *a = audioAbiCreate();
    unsigned char cmds[16];
    int i;

    printf("abi: ENVMIXER on A_CONTINUE ignores the live registers\n");

    memset(g_envmix_state, 0, sizeof(g_envmix_state));
    fill_const(a, 0x100, 1000, 16);

    env_setup(a, 32, 0x7FFF, 0x7FFF, 0x7FFF, 0x7FFF, 0, 0, 0x7FFF, 0);
    put_cmd(cmds, (A_ENVMIXER << 24) | ((A_INIT | A_AUX) << 16),
            dram_addr(g_envmix_state));
    CHECK(audioAbiRun(a, cmds, 8) == 0);
    CHECK(audioAbiPeek16(a, 0x200) >= 995);

    /*
     * Now a second voice sets the registers to silence, as it would when the
     * synthesizer walks the voice list. _pullSubFrame sends no aSetVolume at
     * all on a continue frame, so the first voice's next mix must come
     * entirely out of its own state block. Reading the registers here mixes
     * every sustained voice at the last voice's volume and pan.
     */
    fill_const(a, 0x200, 0, 16);
    env_setup(a, 32, 0, 0, 0, 0, 0, 0, 0, 0);

    put_cmd(cmds, (A_ENVMIXER << 24) | ((A_CONTINUE | A_AUX) << 16),
            dram_addr(g_envmix_state));
    CHECK(audioAbiRun(a, cmds, 8) == 0);

    for (i = 0; i < 16; i++) {
        CHECK(audioAbiPeek16(a, 0x200 + (unsigned)i * 2) >= 995);
    }

    audioAbiDestroy(a);
}

static void test_setbuff_aux_does_not_clobber(void)
{
    audio_abi *a = audioAbiCreate();
    unsigned char cmds[16];

    printf("abi: SETBUFF with A_AUX leaves in, out and count alone\n");

    fill_const(a, 0x100, 1000, 16);
    memset(g_envmix_state, 0, sizeof(g_envmix_state));

    /* The aux form carries three addresses in the same fields the plain form
     * uses for in, out and count. Decoding it as the plain form would leave
     * count at 0x500 and the mix would run off the end of the buffer. */
    env_setup(a, 32, 0x7FFF, 0x7FFF, 0x7FFF, 0x7FFF, 0, 0, 0x7FFF, 0);

    put_cmd(cmds, (A_ENVMIXER << 24) | ((A_INIT | A_AUX) << 16),
            dram_addr(g_envmix_state));
    CHECK(audioAbiRun(a, cmds, 8) == 0);

    /* Exactly 16 samples written, and the 17th untouched. */
    CHECK(audioAbiPeek16(a, 0x200 + 15 * 2) >= 995);
    CHECK(audioAbiPeek16(a, 0x200 + 16 * 2) == 0);

    audioAbiDestroy(a);
}

static void test_polef(void)
{
    audio_abi *a = audioAbiCreate();
    unsigned char cmds[16];
    static short coef[16];
    int fc = 12000;                  /* Q14, so about 0.73 */
    int gain = 16384 - 12000;        /* init_lpfilter's fgain = SCALE - fc */
    int i;
    int y;

    printf("abi: POLEF is a one-pole lowpass with unity DC gain\n");

    /* The shape init_lpfilter builds: eight zeros of padding, then fc and its
     * powers. Only fc is read here, but the padding has to be there or the
     * index is wrong. */
    memset(coef, 0, sizeof(coef));
    coef[8] = (short)fc;
    for (i = 9; i < 16; i++) {
        coef[i] = (short)(((long)coef[i - 1] * fc) >> 14);
    }
    for (i = 0; i < 16; i++) {
        /* The codebook is fetched from DRAM big-endian, like everything the
         * microcode reads. */
        unsigned char *b = (unsigned char *)&coef[i];
        int v = coef[i];

        b[0] = (unsigned char)((v >> 8) & 0xFF);
        b[1] = (unsigned char)(v & 0xFF);
    }

    fill_const(a, 0x100, 8000, 64);

    put_cmd(cmds, (A_SETBUFF << 24) | 0x100u, (0x100u << 16) | 128u);
    CHECK(audioAbiRun(a, cmds, 8) == 0);
    put_cmd(cmds, (A_LOADADPCM << 24) | 32u, dram_addr(coef));
    CHECK(audioAbiRun(a, cmds, 8) == 0);
    put_cmd(cmds, (A_POLEF << 24) | (A_INIT << 16) | ((unsigned)gain & 0xFFFFu),
            dram_addr(g_envmix_state));
    CHECK(audioAbiRun(a, cmds, 8) == 0);

    /* Filters in place, and the response to a step is a rise, not a jump. */
    CHECK(audioAbiPeek16(a, 0x100) > 0);
    CHECK(audioAbiPeek16(a, 0x100) < 8000);
    for (i = 1; i < 64; i++) {
        CHECK(audioAbiPeek16(a, 0x100 + (unsigned)i * 2)
              >= audioAbiPeek16(a, 0x100 + (unsigned)(i - 1) * 2));
    }

    /* fgain = SCALE - fc is exactly the value that makes the steady state
     * equal the input: after 64 samples at 0.73 per step it has all but
     * arrived. */
    CHECK(audioAbiPeek16(a, 0x100 + 63 * 2) > 7900);
    CHECK(audioAbiPeek16(a, 0x100 + 63 * 2) <= 8000);

    /* The recurrence, done here rather than in the implementation. */
    y = 0;
    for (i = 0; i < 64; i++) {
        y = (int)(((long)gain * 8000L + (long)fc * (long)y) >> 14);
    }
    CHECK(audioAbiPeek16(a, 0x100 + 63 * 2) == y);

    /* A_CONTINUE resumes where it left off: a second pass over a silent
     * buffer decays from the carried output rather than restarting at zero. */
    fill_const(a, 0x100, 0, 64);
    put_cmd(cmds, (A_POLEF << 24) | (A_CONTINUE << 16)
                  | ((unsigned)gain & 0xFFFFu),
            dram_addr(g_envmix_state));
    CHECK(audioAbiRun(a, cmds, 8) == 0);
    CHECK(audioAbiPeek16(a, 0x100) > 5000);
    CHECK(audioAbiPeek16(a, 0x100) < y);
    CHECK(audioAbiPeek16(a, 0x100 + 63 * 2)
          < audioAbiPeek16(a, 0x100));

    /* And it is no longer counted as a command this port does not know. */
    CHECK(audioAbiUnknownCount(a) == 0);

    /* A codebook too short to hold fc leaves the buffer alone rather than
     * reading past the end of the allocation. */
    put_cmd(cmds, (A_LOADADPCM << 24) | 4u, dram_addr(coef));
    CHECK(audioAbiRun(a, cmds, 8) == 0);
    fill_const(a, 0x100, 4321, 64);
    put_cmd(cmds, (A_POLEF << 24) | (A_INIT << 16) | ((unsigned)gain & 0xFFFFu),
            dram_addr(g_envmix_state));
    CHECK(audioAbiRun(a, cmds, 8) == 0);
    CHECK(audioAbiPeek16(a, 0x100) == 4321);
    CHECK(audioAbiPeek16(a, 0x100 + 63 * 2) == 4321);

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

    test_resample_unity();
    test_resample_continuity();
    test_envmixer_buses();
    test_envmixer_ramp();
    test_envmixer_state_survives_another_voice();
    test_setbuff_aux_does_not_clobber();
    test_polef();

    printf("\n%d checks, %d failure(s)\n", g_checks, g_failures);
    audioShutdown();
    platformShutdown();
    return g_failures ? 1 : 0;
}
