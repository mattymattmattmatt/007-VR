#include "audio_abi.h"

#include "platform.h"

#include <stdlib.h>
#include <string.h>

#include <PR/ultratypes.h>
#include <PR/abi.h>

#define DMEM_MASK (GEPC_DMEM_SIZE - 1u)

struct audio_abi {
    unsigned char dmem[GEPC_DMEM_SIZE];

    /* Registers the microcode carries between commands.
     *
     * `out` is the main-left output; A_SETBUFF with A_AUX sets the other
     * three buses in one go, which is why they are separate fields rather
     * than a second in/out/count triple. */
    unsigned in, out, count;
    unsigned out_right, aux_left, aux_right;

    /* Envelope registers, per channel: 0 is left, 1 is right. vol is the
     * live 16.16 accumulator, rate the signed 16.16 step added once per
     * eight samples. dry and wet are Q15 sends to the main and aux buses. */
    int      vol[2], target[2], rate[2];
    int      dry, wet;

    unsigned loop_addr;
    short   *book;
    int      book_entries, book_order, book_npredictors;
    short    adpcm_state[2];

    unsigned segments[16];

    unsigned commands;
    unsigned unknown;
};

/* ------------------------------------------------------------ DMEM I/O */

void *audioAbiDmem(audio_abi *a, unsigned offset)
{
    if (!a || offset >= GEPC_DMEM_SIZE) {
        return NULL;
    }
    return a->dmem + offset;
}

const void *audioAbiDmemConst(const audio_abi *a, unsigned offset)
{
    if (!a || offset >= GEPC_DMEM_SIZE) {
        return NULL;
    }
    return a->dmem + offset;
}

/* DMEM holds big-endian 16-bit samples, because that is how LOADBUFF DMAs
 * them out of RDRAM. Reading them arithmetically keeps the host's byte order
 * out of it. */
int audioAbiPeek16(const audio_abi *a, unsigned offset)
{
    unsigned o = offset & DMEM_MASK;
    int v;

    if (!a) {
        return 0;
    }
    v = ((int)a->dmem[o] << 8) | (int)a->dmem[(o + 1) & DMEM_MASK];
    return (short)v;
}

void audioAbiPoke16(audio_abi *a, unsigned offset, int value)
{
    unsigned o = offset & DMEM_MASK;

    if (!a) {
        return;
    }
    a->dmem[o] = (unsigned char)((value >> 8) & 0xFF);
    a->dmem[(o + 1) & DMEM_MASK] = (unsigned char)(value & 0xFF);
}

static short clamp16(int v)
{
    if (v > 32767)  { return (short)32767; }
    if (v < -32768) { return (short)-32768; }
    return (short)v;
}

/* ---------------------------------------------------------------- ADPCM */

void audioAbiDecodeAdpcmFrame(const unsigned char frame[9],
                              const short *book, int order, int npredictors,
                              short state[2], short out[16])
{
    int scale;
    int pred;
    int half;
    const short *b1, *b2;

    if (!frame || !out || !state) {
        return;
    }

    scale = (frame[0] >> 4) & 0x0F;
    pred  = frame[0] & 0x0F;

    if (npredictors > 0 && pred >= npredictors) {
        pred = npredictors - 1;
    }
    if (order < 1) {
        order = 2;
    }

    if (book) {
        /* The codebook holds `order` rows of eight coefficients per
         * predictor. */
        b1 = book + (unsigned)pred * (unsigned)order * 8u;
        b2 = b1 + 8;
    } else {
        b1 = NULL;
        b2 = NULL;
    }

    for (half = 0; half < 2; half++) {
        int raw[8];
        int i;

        for (i = 0; i < 8; i++) {
            int byte_index = 1 + half * 4 + (i >> 1);
            int nibble = (i & 1) ? (frame[byte_index] & 0x0F)
                                 : (frame[byte_index] >> 4);
            /* Nibbles are signed. */
            if (nibble > 7) {
                nibble -= 16;
            }
            /* Multiply rather than shift: nibble is negative half the time,
             * and left-shifting a negative value is undefined behaviour. It
             * happens to work on gcc today, which is exactly what makes it
             * the kind of bug that appears years later under a different
             * compiler or optimisation level. */
            raw[i] = nibble * (1 << scale);
        }

        for (i = 0; i < 8; i++) {
            long acc;
            int j;

            if (b1) {
                acc = (long)b1[i] * (long)state[1] + (long)b2[i] * (long)state[0];
                for (j = 0; j < i; j++) {
                    acc += (long)b2[i - 1 - j] * (long)out[half * 8 + j];
                }
            } else {
                /* No codebook loaded: fall back to plain PCM so a missing
                 * A_LOADADPCM produces quiet audio rather than noise. */
                acc = 0;
            }
            acc += (long)raw[i] * 2048L;   /* same reason: raw[i] may be negative */
            out[half * 8 + i] = clamp16((int)(acc >> 11));
        }

        state[0] = out[half * 8 + 6];
        state[1] = out[half * 8 + 7];
    }
}

/* ------------------------------------------------------------ lifecycle */

audio_abi *audioAbiCreate(void)
{
    audio_abi *a = (audio_abi *)calloc(1, sizeof(*a));

    if (!a) {
        return NULL;
    }
    a->book_order = 2;
    a->book_npredictors = 1;
    return a;
}

void audioAbiDestroy(audio_abi *a)
{
    if (!a) {
        return;
    }
    free(a->book);
    free(a);
}

unsigned audioAbiCommandCount(const audio_abi *a) { return a ? a->commands : 0; }
unsigned audioAbiUnknownCount(const audio_abi *a) { return a ? a->unknown : 0; }

/* ---------------------------------------------------------- DRAM access */

/* Command addresses are what osVirtualToPhysical produced, which the RDRAM
 * arena guarantees fits in 32 bits and maps straight back to a host pointer.
 * A_SEGMENT rebases them the same way display lists are segmented. */
/*
 * The state blocks the resampler and the envelope mixer carry between calls
 * live in DRAM and are written as well as read, so they need a resolver that
 * does not hand back a const pointer.
 *
 * Their *contents* are this port's own business. The game allocates the block,
 * passes its address, and never looks inside -- only the microcode does, and
 * the microcode never runs here. So the layouts below only have to round-trip
 * with themselves, and they hold what this implementation needs in whatever
 * order suits it rather than matching the hardware's. The SDK's sizes are
 * still honoured, because the game sized its allocation from them:
 * RESAMPLE_STATE is 16 shorts, ENVMIX_STATE is 40 and POLEF_STATE is 4, and
 * writing past those would corrupt whatever the game put next to them.
 */
static unsigned char *resolve_rw(audio_abi *a, unsigned addr)
{
    unsigned seg = (addr >> 24) & 0x0Fu;

    if (a->segments[seg]) {
        addr = a->segments[seg] + (addr & 0x00FFFFFFu);
    }
    if (!addr) {
        return NULL;
    }
    return (unsigned char *)(size_t)addr;
}

/* Big-endian, like everything else the microcode touches. */
static int state_get16(const unsigned char *st, int i)
{
    return (short)(((unsigned)st[i * 2] << 8) | st[i * 2 + 1]);
}

static void state_put16(unsigned char *st, int i, int v)
{
    st[i * 2]     = (unsigned char)((v >> 8) & 0xFF);
    st[i * 2 + 1] = (unsigned char)(v & 0xFF);
}

static unsigned state_get32(const unsigned char *st, int i)
{
    return ((unsigned)(state_get16(st, i) & 0xFFFF) << 16)
         | (unsigned)(state_get16(st, i + 1) & 0xFFFF);
}

static void state_put32(unsigned char *st, int i, unsigned v)
{
    state_put16(st, i, (int)((v >> 16) & 0xFFFFu));
    state_put16(st, i + 1, (int)(v & 0xFFFFu));
}

static const unsigned char *resolve(const audio_abi *a, unsigned addr)
{
    unsigned seg = (addr >> 24) & 0x0Fu;

    if (a->segments[seg]) {
        addr = a->segments[seg] + (addr & 0x00FFFFFFu);
    }
    if (!addr) {
        return NULL;
    }
    return (const unsigned char *)(size_t)addr;
}

/* --------------------------------------------------------------- commands */

static void cmd_clearbuff(audio_abi *a, unsigned dmem, unsigned count)
{
    unsigned i;

    for (i = 0; i < count; i++) {
        a->dmem[(dmem + i) & DMEM_MASK] = 0;
    }
}

static void cmd_dmemmove(audio_abi *a, unsigned src, unsigned dst, unsigned count)
{
    unsigned i;

    /* Copy forwards through a temporary so overlapping ranges behave the way
     * a DMA would rather than smearing. */
    for (i = 0; i < count; i++) {
        unsigned char v = a->dmem[(src + i) & DMEM_MASK];
        a->dmem[(dst + i) & DMEM_MASK] = v;
    }
}

static void cmd_loadbuff(audio_abi *a, unsigned addr, unsigned dmem,
                         unsigned count)
{
    const unsigned char *src = resolve(a, addr);
    unsigned i;

    if (!src) {
        return;
    }
    for (i = 0; i < count; i++) {
        a->dmem[(dmem + i) & DMEM_MASK] = src[i];
    }
}

static void cmd_savebuff(audio_abi *a, unsigned addr, unsigned dmem,
                         unsigned count)
{
    unsigned char *dst = (unsigned char *)(size_t)addr;
    unsigned i;

    if (!addr) {
        return;
    }
    for (i = 0; i < count; i++) {
        dst[i] = a->dmem[(dmem + i) & DMEM_MASK];
    }
}

static void cmd_mixer(audio_abi *a, unsigned in, unsigned out, unsigned count,
                      int gain)
{
    unsigned i;

    /* count is in bytes; samples are 16-bit. */
    for (i = 0; i + 1 < count; i += 2) {
        int s = audioAbiPeek16(a, in + i);
        int d = audioAbiPeek16(a, out + i);
        audioAbiPoke16(a, out + i, clamp16(d + ((s * gain) >> 15)));
    }
}

static void cmd_interleave(audio_abi *a, unsigned out, unsigned inL,
                           unsigned inR, unsigned count)
{
    unsigned i;

    /* Two mono buffers become one stereo buffer, which is the last thing the
     * microcode does before the frame is handed to the DAC. */
    for (i = 0; i + 1 < count; i += 2) {
        int l = audioAbiPeek16(a, inL + i);
        int r = audioAbiPeek16(a, inR + i);
        audioAbiPoke16(a, out + i * 2, l);
        audioAbiPoke16(a, out + i * 2 + 2, r);
    }
}

/*
 * A_RESAMPLE: pitch-shift the input into the output.
 *
 * The command carries a state address, and ignoring it -- which this used to
 * do -- is audible. The filter needs the samples either side of the point it
 * is interpolating, and the fractional read position has to survive to the
 * next call. Restarting both at zero every buffer means a discontinuity at
 * every buffer boundary: a click, several times a second, on every pitched
 * voice. That is a worse artefact than any amount of interpolation error.
 *
 * pitch is 2.14 fixed point, so unity is 0x4000 and shifting up by two gives
 * a 16.16 step -- 0x4000 << 2 == 0x10000, one input sample per output sample.
 *
 * The interpolation is a four-point Catmull-Rom spline. The hardware uses a
 * 64-phase filter table which is not in this repository and which I am not
 * going to reproduce from memory, since a wrong table would be both hard to
 * hear and impossible to attribute. A cubic through four points is the right
 * shape for a four-tap interpolator and reproduces a straight line exactly.
 * It is still not the same filter, so this is the place to look if pitched
 * voices sound subtly off -- but continuity is no longer the problem.
 */
static int catmull_rom(int sm1, int s0, int s1, int s2, unsigned frac)
{
    /* frac is 0..0xFFFF across the span between s0 and s1. */
    float t = (float)frac / 65536.0f;
    float a = -0.5f * sm1 + 1.5f * s0 - 1.5f * s1 + 0.5f * s2;
    float b = sm1 - 2.5f * s0 + 2.0f * s1 - 0.5f * s2;
    float c = -0.5f * sm1 + 0.5f * s1;
    float v = ((a * t + b) * t + c) * t + (float)s0;

    return (int)(v >= 0.0f ? v + 0.5f : v - 0.5f);
}

static void cmd_resample(audio_abi *a, unsigned in, unsigned out,
                         unsigned count, unsigned pitch, unsigned state_addr,
                         int init)
{
    unsigned char *st = resolve_rw(a, state_addr);
    unsigned step = pitch << 2;
    unsigned pos = 0;
    int prev = 0;
    unsigned i;

    /*
     * Only two things need to survive to the next call: the fractional read
     * position, and the one input sample immediately before the start of the
     * buffer. A four-tap interpolator standing at position idx wants
     * idx-1, idx, idx+1 and idx+2, and only idx-1 can fall off the front --
     * and only while idx is still zero. Carrying four samples, as the first
     * version of this did, means three of them are written every call and
     * never read once.
     */
    if (st && !init) {
        pos  = (unsigned)state_get16(st, 0) & 0xFFFFu;
        prev = state_get16(st, 1);
    }

    for (i = 0; i + 1 < count; i += 2) {
        unsigned idx = pos >> 16;
        unsigned frac = pos & 0xFFFFu;
        int sm1, s0, s1, s2;

        sm1 = (idx >= 1) ? audioAbiPeek16(a, in + (idx - 1) * 2) : prev;
        s0  = audioAbiPeek16(a, in + idx * 2);
        s1  = audioAbiPeek16(a, in + (idx + 1) * 2);
        s2  = audioAbiPeek16(a, in + (idx + 2) * 2);

        audioAbiPoke16(a, out + i, clamp16(catmull_rom(sm1, s0, s1, s2, frac)));
        pos += step;
    }

    if (st) {
        unsigned consumed = pos >> 16;

        if (consumed >= 1) {
            prev = audioAbiPeek16(a, in + (consumed - 1) * 2);
        }
        /* consumed == 0 means this buffer did not advance past the first
         * input sample, so the sample before the next buffer's first is the
         * same one we were already carrying. */

        /* Only the fraction survives. The game refills the input buffer from
         * the start every call and tracks the integer part itself, in
         * ALResampler::delta, so storing the whole position would count it
         * twice. */
        state_put16(st, 0, (int)(pos & 0xFFFFu));
        state_put16(st, 1, prev);
    }
}

/*
 * A_ENVMIXER: scale the input by a ramping per-channel volume and add it into
 * the four output buses.
 *
 * The ramp is not guesswork. src/libultrare/audio/env.c is the code that
 * builds these commands, and its _getRate/_getVol pair say exactly what the
 * microcode on the other end does:
 *
 *     rate = ((ratem << 16) + ratel) / 65536.0      -- signed 16.16
 *     vol += rate * samples * 0.125
 *
 * The 0.125 is the tell: rate is added once per eight samples, not once per
 * sample, and _getRate produces 8 * (tgt - vol) / count so that after count
 * samples the volume has arrived exactly on target. So the envelope is a
 * staircase -- constant across each block of eight, stepping between blocks --
 * and reproducing it is a matter of doing the same arithmetic, not of
 * approximating a curve. This used to interpolate smoothly per sample from
 * vol to target across the buffer, which is a different shape and, worse,
 * ignored rate entirely: a segment meant to span several buffers completed
 * inside the first one.
 *
 * The other half of the job is the state block. Look at _pullSubFrame: on the
 * first call for a voice it emits five aSetVolume commands and then
 * aEnvMixer(A_INIT); on every call after that it emits aEnvMixer(A_CONTINUE)
 * and nothing else. The registers are never re-sent. So a continue frame has
 * to recover volume, target, rate and the dry/wet sends from the state block,
 * and taking them from the live registers -- as this used to -- means every
 * sustained voice is mixed at whatever volume and pan the *last voice in the
 * list* happened to set. With several voices per frame that is not a subtle
 * error.
 *
 * Volumes and sends are Q15: full scale is 32767 and two of them multiplied
 * and shifted back down twice come out at unity, which is what the game's own
 * value ranges imply (volume, eqpower[] and dryamt all top out at 32767).
 */

/* Our layout for ENVMIX_STATE. 12 of the 40 shorts the SDK reserves. */
#define ENV_ST_VOL_L   0    /* 16.16, two shorts */
#define ENV_ST_VOL_R   2
#define ENV_ST_TGT_L   4
#define ENV_ST_TGT_R   5
#define ENV_ST_RATE_L  6    /* 16.16, two shorts */
#define ENV_ST_RATE_R  8
#define ENV_ST_DRY     10
#define ENV_ST_WET     11

/* Steps one channel's volume accumulator by one block, stopping on the
 * target. The hardware does not clamp, but the game only ever runs a segment
 * for exactly the sample count it sized the rate for, so clamping is a no-op
 * on every well-formed ramp and stops a malformed one from sailing past the
 * target and back out the other side. */
static int env_step(int vol, int target, int rate)
{
    int tgt = target << 16;

    vol += rate;
    if (rate > 0 && vol > tgt) { return tgt; }
    if (rate < 0 && vol < tgt) { return tgt; }
    return vol;
}

static void cmd_envmixer(audio_abi *a, unsigned state_addr, int init, int aux)
{
    unsigned char *st = resolve_rw(a, state_addr);
    unsigned samples = a->count / 2u;
    int vol[2], target[2], rate[2];
    int dry, wet;
    unsigned i;

    if (!samples) {
        return;
    }

    if (init || !st) {
        vol[0] = a->vol[0];    vol[1] = a->vol[1];
        target[0] = a->target[0]; target[1] = a->target[1];
        rate[0] = a->rate[0];  rate[1] = a->rate[1];
        dry = a->dry;          wet = a->wet;
    } else {
        vol[0]    = (int)state_get32(st, ENV_ST_VOL_L);
        vol[1]    = (int)state_get32(st, ENV_ST_VOL_R);
        target[0] = state_get16(st, ENV_ST_TGT_L);
        target[1] = state_get16(st, ENV_ST_TGT_R);
        rate[0]   = (int)state_get32(st, ENV_ST_RATE_L);
        rate[1]   = (int)state_get32(st, ENV_ST_RATE_R);
        dry       = state_get16(st, ENV_ST_DRY);
        wet       = state_get16(st, ENV_ST_WET);
    }

    for (i = 0; i < samples; i++) {
        int s = audioAbiPeek16(a, a->in + i * 2);
        int l = (int)(((long)s * (long)(vol[0] >> 16)) >> 15);
        int r = (int)(((long)s * (long)(vol[1] >> 16)) >> 15);

        audioAbiPoke16(a, a->out + i * 2,
                       clamp16(audioAbiPeek16(a, a->out + i * 2)
                               + (int)(((long)l * (long)dry) >> 15)));
        audioAbiPoke16(a, a->out_right + i * 2,
                       clamp16(audioAbiPeek16(a, a->out_right + i * 2)
                               + (int)(((long)r * (long)dry) >> 15)));

        if (aux) {
            audioAbiPoke16(a, a->aux_left + i * 2,
                           clamp16(audioAbiPeek16(a, a->aux_left + i * 2)
                                   + (int)(((long)l * (long)wet) >> 15)));
            audioAbiPoke16(a, a->aux_right + i * 2,
                           clamp16(audioAbiPeek16(a, a->aux_right + i * 2)
                                   + (int)(((long)r * (long)wet) >> 15)));
        }

        /* One step per block of eight, after the eighth sample. */
        if ((i & 7u) == 7u) {
            vol[0] = env_step(vol[0], target[0], rate[0]);
            vol[1] = env_step(vol[1], target[1], rate[1]);
        }
    }

    if (st) {
        state_put32(st, ENV_ST_VOL_L, (unsigned)vol[0]);
        state_put32(st, ENV_ST_VOL_R, (unsigned)vol[1]);
        state_put16(st, ENV_ST_TGT_L, target[0]);
        state_put16(st, ENV_ST_TGT_R, target[1]);
        state_put32(st, ENV_ST_RATE_L, (unsigned)rate[0]);
        state_put32(st, ENV_ST_RATE_R, (unsigned)rate[1]);
        state_put16(st, ENV_ST_DRY, dry);
        state_put16(st, ENV_ST_WET, wet);
    }

    /* The live registers follow the accumulator so a caller that reads them
     * back -- the tests do -- sees where the ramp got to. */
    a->vol[0] = vol[0];
    a->vol[1] = vol[1];
}

static void cmd_loadadpcm(audio_abi *a, unsigned addr, unsigned count)
{
    const unsigned char *src = resolve(a, addr);
    unsigned entries = count / 2u;
    unsigned i;

    if (!src || !entries) {
        return;
    }

    free(a->book);
    a->book = (short *)malloc(entries * sizeof(short));
    if (!a->book) {
        a->book_entries = 0;
        a->book_npredictors = 0;
        return;
    }
    a->book_entries = (int)entries;
    for (i = 0; i < entries; i++) {
        a->book[i] = (short)(((int)src[i * 2] << 8) | (int)src[i * 2 + 1]);
    }
    /* Two rows of eight coefficients per predictor is the standard layout. */
    a->book_order = 2;
    a->book_npredictors = (int)(entries / 16u);
    if (a->book_npredictors < 1) {
        a->book_npredictors = 1;
    }
}

static void cmd_adpcm(audio_abi *a, unsigned addr, unsigned flags)
{
    const unsigned char *src = resolve(a, addr);
    unsigned frames;
    unsigned f;
    unsigned outpos = a->out;

    if (!src) {
        return;
    }

    if (!(flags & A_INIT)) {
        /* Continuing a voice: keep the running sample history. */
    } else {
        a->adpcm_state[0] = 0;
        a->adpcm_state[1] = 0;
    }

    /* Each 9-byte frame yields 16 samples, so 32 bytes of DMEM. */
    frames = a->count / 32u;
    for (f = 0; f < frames; f++) {
        short out[16];
        int i;

        audioAbiDecodeAdpcmFrame(src + f * 9, a->book, a->book_order,
                                 a->book_npredictors, a->adpcm_state, out);
        for (i = 0; i < 16; i++) {
            audioAbiPoke16(a, outpos, out[i]);
            outpos += 2;
        }
    }
}

/*
 * A_POLEF: the one-pole lowpass that damps the reverb tail.
 *
 * This looks opaque -- a gain in the command, a state block, and coefficients
 * that arrive through A_LOADADPCM of all things -- until you read the code
 * that fills the coefficients. init_lpfilter in src/libultrare/audio/drvrNew.c
 * writes eight zeros, then fc, then fc^2 through fc^8, all over SCALE, which
 * is 16384; and it sets fgain = SCALE - fc, which is the command's gain word.
 *
 * A table of consecutive powers of one coefficient is what you build to unroll
 *
 *     y[n] = (fgain * x[n] + fc * y[n-1]) >> 14
 *
 * across eight samples so the RSP can do them in parallel: the k-th output of
 * a block needs fc^(k+1) times the carried y. So the filter is a plain
 * one-pole, the vector table is an implementation detail of the hardware's
 * parallelism, and on a CPU the recurrence is just the recurrence. The eight
 * leading zeros are padding to make the coefficients the shape A_LOADADPCM
 * expects. fgain = SCALE - fc gives it unity gain at DC, which is the check
 * that the shift is 14 and not 15.
 *
 * What the repository does not pin down is the hardware's rounding, or what
 * the other three shorts of POLEF_STATE hold; only the carried output is
 * needed here. Skipping the command entirely -- which this used to do -- left
 * the filter as a passthrough, so reverb tails kept their high end instead of
 * being damped.
 */
static void cmd_polef(audio_abi *a, unsigned gain_word, unsigned state_addr,
                      int init)
{
    unsigned char *st = resolve_rw(a, state_addr);
    int gain = (short)(gain_word & 0xFFFFu);
    int fc;
    int y = 0;
    unsigned i;

    /* fc comes from the codebook A_LOADADPCM just filled: index 8, the first
     * entry after init_lpfilter's padding. _filterBuffer always loads 32 bytes
     * before this command, but the length is checked rather than assumed --
     * a shorter book would otherwise be read past the end. Without a usable
     * one there is nothing to filter with, and since this command runs in
     * place, leaving the samples alone is the only honest thing left to do. */
    if (!a->book || a->book_entries < 9) {
        return;
    }
    fc = a->book[8];

    if (st && !init) {
        y = state_get16(st, 0);
    }

    for (i = 0; i + 1 < a->count; i += 2) {
        int x = audioAbiPeek16(a, a->in + i);

        y = clamp16((int)(((long)gain * (long)x + (long)fc * (long)y) >> 14));
        audioAbiPoke16(a, a->out + i, y);
    }

    if (st) {
        state_put16(st, 0, y);
    }
}

/* --------------------------------------------------------------- run */

int audioAbiRun(audio_abi *a, const void *cmds, unsigned bytes)
{
    const unsigned char *p = (const unsigned char *)cmds;
    unsigned n, i;

    if (!a || !p) {
        return -1;
    }

    n = bytes / 8u;
    for (i = 0; i < n; i++) {
        /* Commands are big-endian pairs of 32-bit words, like display list
         * commands. Reading them byte-wise keeps host order out of it. */
        const unsigned char *c = p + i * 8u;
        unsigned w0 = ((unsigned)c[0] << 24) | ((unsigned)c[1] << 16)
                    | ((unsigned)c[2] << 8)  | (unsigned)c[3];
        unsigned w1 = ((unsigned)c[4] << 24) | ((unsigned)c[5] << 16)
                    | ((unsigned)c[6] << 8)  | (unsigned)c[7];
        unsigned op = (w0 >> 24) & 0xFFu;
        unsigned flags = (w0 >> 16) & 0xFFu;

        a->commands++;

        switch (op) {
        case A_SPNOOP:
            break;

        case A_CLEARBUFF:
            cmd_clearbuff(a, w0 & 0xFFFFu, w1 & 0xFFFFu);
            break;

        case A_DMEMMOVE:
            cmd_dmemmove(a, w0 & 0xFFFFu, (w1 >> 16) & 0xFFFFu, w1 & 0xFFFFu);
            break;

        case A_LOADBUFF:
            cmd_loadbuff(a, w1, a->in, a->count);
            break;

        case A_SAVEBUFF:
            cmd_savebuff(a, w1, a->out, a->count);
            break;

        case A_SETBUFF:
            /* A_AUX makes this a different command: env.c sends
             * aSetBuffer(A_AUX, main-right, aux-left, aux-right), three
             * output addresses rather than in/out/count. Decoding it as the
             * plain form clobbers the in/out/count the *previous* SETBUFF
             * just established, which is the pair of commands that precedes
             * every single envelope mix. */
            if (flags & A_AUX) {
                a->out_right = w0 & 0xFFFFu;
                a->aux_left  = (w1 >> 16) & 0xFFFFu;
                a->aux_right = w1 & 0xFFFFu;
            } else {
                a->in = w0 & 0xFFFFu;
                a->out = (w1 >> 16) & 0xFFFFu;
                a->count = w1 & 0xFFFFu;
            }
            break;

        case A_SETVOL:
            /* Each aSetVolume touches exactly one channel, chosen by A_LEFT,
             * and which registers it writes is chosen by A_VOL and A_AUX:
             *
             *   A_AUX        -> dry = v, wet = w1 low
             *   A_VOL        -> that channel's current volume = v
             *   otherwise    -> that channel's target = v, rate = w1
             *
             * rate is a signed 16.16 assembled from the two halves of w1,
             * which is why w1 is taken whole rather than split. */
            if (flags & A_AUX) {
                a->dry = (short)(w0 & 0xFFFFu);
                a->wet = (short)(w1 & 0xFFFFu);
            } else {
                int ch = (flags & A_LEFT) ? 0 : 1;

                if (flags & A_VOL) {
                    a->vol[ch] = (int)(short)(w0 & 0xFFFFu) << 16;
                } else {
                    a->target[ch] = (short)(w0 & 0xFFFFu);
                    a->rate[ch] = (int)w1;
                }
            }
            break;

        case A_MIXER:
            cmd_mixer(a, (w1 >> 16) & 0xFFFFu, w1 & 0xFFFFu, a->count,
                      (short)(w0 & 0xFFFFu));
            break;

        case A_INTERLEAVE:
            cmd_interleave(a, a->out, (w1 >> 16) & 0xFFFFu, w1 & 0xFFFFu,
                           a->count);
            break;

        case A_RESAMPLE:
            cmd_resample(a, a->in, a->out, a->count, w0 & 0xFFFFu,
                         w1, (flags & A_INIT) != 0);
            break;

        case A_ENVMIXER:
            cmd_envmixer(a, w1, (flags & A_INIT) != 0, (flags & A_AUX) != 0);
            break;

        case A_LOADADPCM:
            cmd_loadadpcm(a, w1, w0 & 0xFFFFu);
            break;

        case A_ADPCM:
            cmd_adpcm(a, w1, flags);
            break;

        case A_SETLOOP:
            a->loop_addr = w1;
            break;

        case A_SEGMENT: {
            unsigned seg = (w1 >> 24) & 0x0Fu;
            a->segments[seg] = w1 & 0x00FFFFFFu;
            break;
        }

        case A_POLEF:
            cmd_polef(a, w0 & 0xFFFFu, w1, (flags & A_INIT) != 0);
            break;

        default:
            a->unknown++;
            break;
        }
    }
    return 0;
}
