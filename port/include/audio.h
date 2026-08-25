/*
 * audio.h - the AI output path.
 *
 * On hardware the game renders a frame of audio with the RSP, then hands the
 * resulting PCM to the audio DAC with osAiSetNextBuffer and asks how much is
 * still queued with osAiGetLength. It uses that answer to decide how many
 * samples to generate next frame, so the value has to be truthful: always
 * returning zero makes the game generate maximum-size frames forever, and
 * over-reporting starves it.
 *
 * This file models the DAC as a ring buffer and is deliberately free of SDL,
 * so the pacing behaviour can be tested without an audio device. audio_sdl.c
 * opens the real device and pulls from here.
 */
#ifndef GEPC_AUDIO_H
#define GEPC_AUDIO_H

#ifdef __cplusplus
extern "C" {
#endif

/* 16-bit stereo, which is what the N64 DAC consumes and what the game's
 * osAiSetNextBuffer sizes assume (frameSamples * 4). */
#define GEPC_AUDIO_BYTES_PER_SAMPLE 4

int  audioInit(void);
void audioShutdown(void);
int  audioIsReady(void);

/* The rate the device is actually running at, which may differ from what was
 * requested. Returns 0 before the first osAiSetFrequency. */
unsigned audioGetRate(void);

/* Called by the device backend from its callback thread. Copies up to `bytes`
 * into dst and returns how many bytes were actually available; the caller
 * fills any shortfall with silence. */
unsigned audioPullBytes(void *dst, unsigned bytes);

/* Bytes queued but not yet pulled. This is what osAiGetLength reports. */
unsigned audioQueuedBytes(void);

/* Drops everything queued, for a level change or a pause. */
void audioFlush(void);

/* Diagnostics: how often the device asked for more than was available. A
 * steadily climbing count means the game is not keeping up. */
unsigned audioUnderrunCount(void);
void     audioResetCounters(void);

/* Implemented in audio_sdl.c: opens the real device, registers the audio task
 * handler and creates the microcode instance. Separated so everything above
 * stays testable without a sound card. */
int  audioSdlInit(void);
void audioSdlShutdown(void);

#ifdef __cplusplus
}
#endif

#endif /* GEPC_AUDIO_H */
