/*
 * video.h - the port's window, GL context and frame loop.
 *
 * This is what closes the loop: the game builds a display list, the SP task
 * interception in libultra.c hands it here, gfx_state decodes it and the GL
 * backend draws it. The RSP microcode never runs.
 */
#ifndef GEPC_VIDEO_H
#define GEPC_VIDEO_H

#ifdef __cplusplus
extern "C" {
#endif

/* Creates the window and GL context and registers the graphics task handler.
 * Returns 0 on success; on failure the reason has been logged and the port can
 * still run headless, which is useful for testing the rest of the layer. */
int  videoInit(int width, int height, const char *title);
void videoShutdown(void);
int  videoIsReady(void);

/* Pumps window events. Returns 0 to keep running, 1 if the player closed the
 * window. */
int  videoPumpEvents(void);

/* Per-frame counters for the debug overlay. */
unsigned videoFrameCount(void);
unsigned videoTriangleCount(void);
unsigned videoDrawCallCount(void);

#ifdef __cplusplus
}
#endif

#endif /* GEPC_VIDEO_H */
