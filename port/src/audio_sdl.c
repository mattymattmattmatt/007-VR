/*
 * audio_sdl.c - opens the real audio device and drives the microcode.
 *
 * Kept apart from audio.c and audio_abi.c so those two stay free of SDL and
 * testable without a sound card. This file is the only part that cannot run
 * headless.
 */
#include "audio.h"
#include "audio_abi.h"
#include "gfxhook.h"
#include "platform.h"

#include <SDL2/SDL.h>
#include <string.h>

static SDL_AudioDeviceID g_device;
static audio_abi        *g_abi;

/* Called by libultra.c when the game submits an audio task. The microcode
 * writes its result back into the game's own buffer through A_SAVEBUFF; the
 * game then hands that buffer to osAiSetNextBuffer itself, so there is
 * nothing to forward here. */
static void on_aud_task(const void *data, unsigned bytes, void *user)
{
    (void)user;

    if (!g_abi || !data || !bytes) {
        return;
    }
    audioAbiRun(g_abi, data, bytes);
}

static void SDLCALL feed(void *user, Uint8 *stream, int len)
{
    unsigned got;

    (void)user;

    if (len <= 0) {
        return;
    }

    got = audioPullBytes(stream, (unsigned)len);

    /* Pad any shortfall with silence rather than leaving the device buffer
     * as it was, which repeats the previous block as a very audible buzz. */
    if (got < (unsigned)len) {
        memset(stream + got, 0, (size_t)len - got);
    }
}

int audioSdlInit(void)
{
    SDL_AudioSpec want, have;
    unsigned rate = audioGetRate();

    if (g_device) {
        return 0;
    }

    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
        platformLog("SDL audio init failed: %s", SDL_GetError());
        return -1;
    }

    g_abi = audioAbiCreate();
    if (!g_abi) {
        platformLog("out of memory creating the audio microcode");
        return -1;
    }

    audioInit();
    spSetAudTaskHandler(on_aud_task, NULL);

    memset(&want, 0, sizeof(want));
    want.freq = (int)(rate ? rate : 22050u);
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    /* Small enough to keep latency low, large enough that a hitched frame
     * does not immediately underrun. */
    want.samples = 512;
    want.callback = feed;

    g_device = SDL_OpenAudioDevice(NULL, 0, &want, &have,
                                   SDL_AUDIO_ALLOW_FREQUENCY_CHANGE);
    if (!g_device) {
        platformLog("could not open an audio device: %s", SDL_GetError());
        return -1;
    }

    if (have.freq != want.freq) {
        /* The game paces itself off osAiGetLength, which counts bytes rather
         * than time, so a device running at a different rate drifts. Worth
         * saying out loud. */
        platformLog("audio: asked for %d Hz, got %d Hz", want.freq, have.freq);
    }

    SDL_PauseAudioDevice(g_device, 0);
    platformLog("audio: %d Hz, %d channels", have.freq, have.channels);
    return 0;
}

void audioSdlShutdown(void)
{
    spSetAudTaskHandler(NULL, NULL);

    if (g_device) {
        SDL_CloseAudioDevice(g_device);
        g_device = 0;
    }
    if (g_abi) {
        audioAbiDestroy(g_abi);
        g_abi = NULL;
    }
    audioShutdown();
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
}
