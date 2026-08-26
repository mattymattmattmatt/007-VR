/*
 * gfx_gl.h - an OpenGL 3.3 backend for gfx_backend.h.
 *
 * Deliberately the *only* part of the renderer that needs a GPU. Everything
 * with a right answer -- display-list decoding, the matrix stack, texture
 * format conversion -- lives in gfx_state.c and gfx_texture.c and is tested
 * headlessly. This file is plumbing.
 */
#ifndef GEPC_GFX_GL_H
#define GEPC_GFX_GL_H

#include "gfx_backend.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Requires a current GL 3.3 core context. Returns NULL on failure, with the
 * reason available from gfxGLLastError(). The returned backend stays valid
 * until gfxGLDestroy. */
gfx_backend *gfxGLCreate(void);
void         gfxGLDestroy(gfx_backend *be);

const char  *gfxGLLastError(void);

/*
 * The game issues viewports and scissors in its own 320x240 framebuffer
 * coordinates, with y measured from the top. Tell the backend how big the
 * output actually is and it maps them across, flipping y for GL's
 * bottom-left origin. Without this the whole picture renders into a 320x240
 * patch in the corner of the window.
 */
void gfxGLSetOutputSize(gfx_backend *be, int fb_w, int fb_h,
                        int out_w, int out_h);

/* Drops every cached texture. Needed when the game reloads a level, since
 * source addresses are reused for different data. */
void gfxGLFlushTextureCache(gfx_backend *be);

/* Diagnostics for the debug overlay. */
unsigned gfxGLDrawCallCount(const gfx_backend *be);
unsigned gfxGLTextureCount(const gfx_backend *be);

#ifdef __cplusplus
}
#endif

#endif /* GEPC_GFX_GL_H */
