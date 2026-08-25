#include "audio_abi.h"

#include "platform.h"

#include <stdlib.h>
#include <string.h>

#include <PR/ultratypes.h>
#include <PR/abi.h>

#define DMEM_MASK (GEPC_DMEM_SIZE - 1u)

struct audio_abi {
    unsigned char dmem[GEPC_DMEM_SIZE];

    /* Registers the microcode carries between commands. */
    unsigned in, out, count;
    short    vol[2], target[2];
    int      rate[2];
    unsigned loop_addr;
    short   *book;
    int      book_order, book_npredictors;
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

/* Approximation: linear interpolation rather than the hardware's filter.
 * Pitch-shifted voices will be close but not sample-accurate, and this is the
 * first place to look if the audio sounds subtly wrong. */
static void cmd_resample(audio_abi *a, unsigned in, unsigned out,
                         unsigned count, unsigned pitch)
{
    unsigned pos = 0;
    unsigned i;
    unsigned step = pitch << 2;   /* pitch is 1.15 fixed point */

    for (i = 0; i + 1 < count; i += 2) {
        unsigned idx = (pos >> 16) * 2u;
        unsigned frac = pos & 0xFFFFu;
        int s0 = audioAbiPeek16(a, in + idx);
        int s1 = audioAbiPeek16(a, in + idx + 2);
        int v = s0 + (int)(((long)(s1 - s0) * (long)frac) >> 16);

        audioAbiPoke16(a, out + i, clamp16(v));
        pos += step;
    }
}

/* Approximation: a straight linear ramp between the current and target
 * volumes across the buffer, rather than the hardware's per-sample rate
 * registers. */
static void cmd_envmixer(audio_abi *a, unsigned in, unsigned outL,
                         unsigned outR, unsigned count)
{
    unsigned i;
    unsigned samples = count / 2u;

    if (!samples) {
        return;
    }

    for (i = 0; i < samples; i++) {
        int s = audioAbiPeek16(a, in + i * 2);
        int t = (int)i;
        int vl = a->vol[0] + (int)(((long)(a->target[0] - a->vol[0]) * t) / (long)samples);
        int vr = a->vol[1] + (int)(((long)(a->target[1] - a->vol[1]) * t) / (long)samples);
        int dl = audioAbiPeek16(a, outL + i * 2);
        int dr = audioAbiPeek16(a, outR + i * 2);

        audioAbiPoke16(a, outL + i * 2, clamp16(dl + ((s * vl) >> 15)));
        audioAbiPoke16(a, outR + i * 2, clamp16(dr + ((s * vr) >> 15)));
    }

    a->vol[0] = a->target[0];
    a->vol[1] = a->target[1];
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
        a->book_npredictors = 0;
        return;
    }
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
            a->in = w0 & 0xFFFFu;
            a->out = (w1 >> 16) & 0xFFFFu;
            a->count = w1 & 0xFFFFu;
            break;

        case A_SETVOL:
            /* Which pair of registers this writes depends on the flags; the
             * common cases are the two channel volumes and their targets. */
            if (flags & A_VOL) {
                a->vol[0] = (short)((w0 >> 0) & 0xFFFFu);
                a->vol[1] = (short)((w1 >> 16) & 0xFFFFu);
            } else {
                a->target[0] = (short)((w0 >> 0) & 0xFFFFu);
                a->target[1] = (short)((w1 >> 16) & 0xFFFFu);
                a->rate[0] = (int)(w1 & 0xFFFFu);
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
            cmd_resample(a, a->in, a->out, a->count, w0 & 0xFFFFu);
            break;

        case A_ENVMIXER:
            cmd_envmixer(a, a->in, a->out, a->out + a->count, a->count);
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
            /* A one-pole filter the game may never use. Counted so it shows
             * up if it does, rather than silently doing nothing. */
            a->unknown++;
            break;

        default:
            a->unknown++;
            break;
        }
    }
    return 0;
}
