/*
 * gfx_backend.h - what a GPU backend has to implement.
 *
 * The state machine in gfx_state.c does the N64-specific work: segment
 * resolution, the matrix stack, the vertex cache, and turning Rare's packed
 * G_TRI4 command into triangles. It hands the result to one of these, which
 * only has to know how to draw.
 *
 * Keeping the split here means the OpenGL backend never has to understand a
 * display list, and the whole decoder can be tested against a recording
 * backend with no GPU present.
 */
#ifndef GEPC_GFX_BACKEND_H
#define GEPC_GFX_BACKEND_H

#ifdef __cplusplus
extern "C" {
#endif

/* A vertex after the state machine has transformed and lit it. Position is in
 * clip space; the backend does the perspective divide and viewport map. */
typedef struct gfx_vertex {
    float x, y, z, w;
    float s, t;                 /* texture coordinates, in texels */
    unsigned char r, g, b, a;   /* shade colour */
} gfx_vertex;

typedef struct gfx_backend {
    void *user;

    void (*begin_frame)(void *user);
    void (*end_frame)(void *user);

    void (*set_viewport)(void *user, int x, int y, int w, int h);
    void (*set_scissor)(void *user, int x, int y, int w, int h);

    /* Raw RDP state, passed through for the backend to interpret. Splitting
     * the combiner into its 16 mux fields is the backend's job, since how it
     * maps onto shaders is entirely a backend concern. */
    void (*set_combine)(void *user, unsigned w0, unsigned w1);
    void (*set_othermode_h)(void *user, unsigned shift, unsigned len, unsigned data);
    void (*set_othermode_l)(void *user, unsigned shift, unsigned len, unsigned data);
    void (*set_geometry_mode)(void *user, unsigned mode);

    /* Texture state. `addr` is already segment-resolved to a host pointer. */
    void (*set_texture_image)(void *user, const void *addr, int fmt, int siz, int width);
    void (*set_tile)(void *user, int tile, int fmt, int siz, int line,
                     int tmem, int palette, int cms, int cmt,
                     int masks, int maskt, int shifts, int shiftt);
    void (*set_tile_size)(void *user, int tile, int uls, int ult, int lrs, int lrt);

    /* gSPTexture. s and t arrive as the 16-bit fixed-point scales the command
     * carries, already converted to floats. `on` disables texturing when 0. */
    void (*set_texture_scale)(void *user, float s, float t,
                              int level, int tile, int on);
    void (*load_block)(void *user, int tile, int uls, int ult, int lrs, int dxt);
    void (*load_tlut)(void *user, int tile, int count);

    void (*set_prim_color)(void *user, unsigned char r, unsigned char g,
                           unsigned char b, unsigned char a);
    void (*set_env_color)(void *user, unsigned char r, unsigned char g,
                          unsigned char b, unsigned char a);
    void (*set_fog_color)(void *user, unsigned char r, unsigned char g,
                          unsigned char b, unsigned char a);
    void (*set_fill_color)(void *user, unsigned value);

    void (*draw_triangle)(void *user, const gfx_vertex *a,
                          const gfx_vertex *b, const gfx_vertex *c);
    void (*fill_rect)(void *user, int ulx, int uly, int lrx, int lry);
    void (*tex_rect)(void *user, int ulx, int uly, int lrx, int lry,
                     int tile, int s, int t, int dsdx, int dtdy, int flip);

    /* Commands the decoder recognises but this backend has no handling for.
     * GoldenEye emits exactly three of these; see port/README.md. */
    void (*unsupported)(void *user, unsigned char op, unsigned w0, unsigned w1);
} gfx_backend;

#ifdef __cplusplus
}
#endif

#endif /* GEPC_GFX_BACKEND_H */
