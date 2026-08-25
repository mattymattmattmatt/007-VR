/*
 * audio_abi.h - the RSP audio microcode, in software.
 *
 * The mirror image of what gfx_state.c does for graphics. alAudioFrame() in
 * src/libultra/audio/synthesizer.c builds a list of Acmd commands; on hardware
 * the RSP audio microcode executes them, reading and writing a 4 KB scratchpad
 * (DMEM) and DMAing to and from RDRAM, and leaves a frame of PCM behind. This
 * interprets that same list on the CPU.
 *
 * Scope, stated honestly. The commands that are exact integer operations --
 * buffer clears, moves, DMA, mixing, interleave and ADPCM decode -- are
 * implemented properly and tested against hand-computed values. The resampler
 * and the envelope mixer are approximations: the hardware's exact filter
 * coefficients and ramp behaviour are not reproduced here, so pitch-shifted
 * voices and volume ramps will be close rather than sample-accurate. That is a
 * real limitation, not a placeholder, and it wants checking against real
 * output before anyone calls the audio finished.
 */
#ifndef GEPC_AUDIO_ABI_H
#define GEPC_AUDIO_ABI_H

#ifdef __cplusplus
extern "C" {
#endif

/* The audio microcode's scratchpad. */
#define GEPC_DMEM_SIZE 0x1000

typedef struct audio_abi audio_abi;

audio_abi *audioAbiCreate(void);
void       audioAbiDestroy(audio_abi *a);

/* Executes a command list. `bytes` is the task's data_size, so the run is
 * bounded exactly rather than trusting a terminator. Returns 0 on a clean
 * run, -1 if a command was malformed. */
int audioAbiRun(audio_abi *a, const void *cmds, unsigned bytes);

/* Counters for diagnostics and tests. */
unsigned audioAbiCommandCount(const audio_abi *a);
unsigned audioAbiUnknownCount(const audio_abi *a);

/* DMEM access, for tests. Returns NULL if out of range. */
void       *audioAbiDmem(audio_abi *a, unsigned offset);
const void *audioAbiDmemConst(const audio_abi *a, unsigned offset);

/* Reads a signed 16-bit sample out of DMEM, honouring the big-endian layout
 * the microcode works in. */
int audioAbiPeek16(const audio_abi *a, unsigned offset);
void audioAbiPoke16(audio_abi *a, unsigned offset, int value);

/* Decodes one 9-byte N64 ADPCM frame into 16 samples. Exposed because it is
 * self-contained and worth testing directly.
 *
 * `book` is the predictor codebook loaded by A_LOADADPCM: `order` * 8 entries
 * of signed 16-bit coefficients per predictor. `state` carries the two
 * previous samples across frames and is updated. */
void audioAbiDecodeAdpcmFrame(const unsigned char frame[9],
                              const short *book, int order, int npredictors,
                              short state[2], short out[16]);

#ifdef __cplusplus
}
#endif

#endif /* GEPC_AUDIO_ABI_H */
