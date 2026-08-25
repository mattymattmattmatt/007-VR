#include "video.h"

#include "gbi_walk.h"
#include "gfx_gl.h"
#include "gfx_state.h"
#include "gfxhook.h"
#include "platform.h"

#include <SDL2/SDL.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include <PR/ultratypes.h>
#include <PR/mbi.h>
#include <PR/gbi.h>

static SDL_Window   *g_window;
static SDL_GLContext g_gl;
static gfx_backend  *g_backend;
static gfx_state    *g_state;
static int           g_ready;
static int           g_quit;

static int      g_present = 1;
static unsigned g_frames;
static unsigned g_triangles;
static unsigned g_draw_calls;

/* Called from libultra.c when the game submits a graphics task. */
static void on_gfx_task(const void *dl, unsigned bytes, void *user)
{
    unsigned commands;

    (void)user;

    if (!g_ready || !dl || !bytes) {
        return;
    }

    /* rspGfxTaskStart records the list's length in bytes, so the walk can be
     * bounded exactly rather than relying on the terminator being present.
     * A list that is somehow unterminated then stops at the right place
     * instead of reading into whatever follows it. */
    commands = bytes / (unsigned)sizeof(Gfx);
    if (!commands) {
        return;
    }

    gfxStateBeginFrame(g_state);

    if (gfxStateRun(g_state, dl, commands) != 0) {
        /* Worth saying out loud: a malformed list means the frame is
         * incomplete, and silently showing a half-drawn frame makes the cause
         * very hard to find later. */
        platformLog("video: display list run failed after %u commands",
                    gfxStateCommandCount(g_state));
    }

    g_triangles = gfxStateTriangleCount(g_state);
    gfxStateEndFrame(g_state);

    g_draw_calls = gfxGLDrawCallCount(g_backend);
    g_frames++;

    /* The game's task carries OS_SC_SWAPBUFFER, so one graphics task is one
     * frame. */
    if (g_window && g_present) {
        SDL_GL_SwapWindow(g_window);
    }
}

int videoInit(int width, int height, const char *title)
{
    if (g_ready) {
        return 0;
    }

    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        platformLog("SDL_Init failed: %s", SDL_GetError());
        return -1;
    }

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK,
                        SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);

    g_window = SDL_CreateWindow(title ? title : "GoldenEye 007",
                                SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                width, height,
                                SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE);
    if (!g_window) {
        platformLog("SDL_CreateWindow failed: %s", SDL_GetError());
        return -1;
    }

    g_gl = SDL_GL_CreateContext(g_window);
    if (!g_gl) {
        platformLog("SDL_GL_CreateContext failed: %s", SDL_GetError());
        SDL_DestroyWindow(g_window);
        g_window = NULL;
        return -1;
    }

    g_backend = gfxGLCreate();
    if (!g_backend) {
        platformLog("GL backend failed: %s", gfxGLLastError());
        SDL_GL_DeleteContext(g_gl);
        SDL_DestroyWindow(g_window);
        g_window = NULL;
        g_gl = NULL;
        return -1;
    }

    g_state = gfxStateCreate(g_backend);
    if (!g_state) {
        platformLog("out of memory creating the renderer state");
        gfxGLDestroy(g_backend);
        g_backend = NULL;
        return -1;
    }

    spSetGfxTaskHandler(on_gfx_task, NULL);
    g_ready = 1;

    platformLog("video: %dx%d, GL backend ready", width, height);
    return 0;
}

void videoShutdown(void)
{
    spSetGfxTaskHandler(NULL, NULL);
    g_ready = 0;

    if (g_state)   { gfxStateDestroy(g_state); g_state = NULL; }
    if (g_backend) { gfxGLDestroy(g_backend); g_backend = NULL; }
    if (g_gl)      { SDL_GL_DeleteContext(g_gl); g_gl = NULL; }
    if (g_window)  { SDL_DestroyWindow(g_window); g_window = NULL; }
    SDL_Quit();
}

int videoIsReady(void) { return g_ready; }

void videoSetPresentEnabled(int enabled) { g_present = enabled; }

int videoPumpEvents(void)
{
    SDL_Event ev;

    while (SDL_PollEvent(&ev)) {
        if (ev.type == SDL_QUIT) {
            g_quit = 1;
        } else if (ev.type == SDL_WINDOWEVENT &&
                   ev.window.event == SDL_WINDOWEVENT_CLOSE) {
            g_quit = 1;
        }
    }
    return g_quit;
}

unsigned videoFrameCount(void)     { return g_frames; }
unsigned videoTriangleCount(void)  { return g_triangles; }
unsigned videoDrawCallCount(void)  { return g_draw_calls; }
