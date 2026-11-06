/*
 * vita_sw3d.h - software 3D rasteriser (no GXM, no shader compiler)
 *
 * Same contract as the vitaGL backend (vita_gl3d.h) so vita_3d.c can pick
 * either one: GL clip-space convention (+y up, z in [-1,1]), column-major
 * 4x4 MVP, triangle lists with per-vertex colour.
 *
 * Supported: MVP transform, near clipping, perspective-correct vertex colour,
 * depth test, both-sided triangles. NOT supported yet: textures, lighting,
 * blending, fog, stencil, culling.
 */
#ifndef VITA_SW3D_H
#define VITA_SW3D_H

#include <stdint.h>

/* Idempotent. Returns 0 on success, negative on allocation failure. */
int vita_sw3d_init(int w, int h);

/* rgba is A8B8G8R8 (0xAABBGGRR). Depth is reset only when clear_depth != 0;
 * the colour buffer is written only when clear_color != 0 (M3G
 * Background.setColorClearEnable(false)). */
void vita_sw3d_clear(uint32_t rgba, int clear_depth, int clear_color);

void vita_sw3d_set_mvp(const float m[16]);

/* nverts must be a multiple of 3; pos3 = xyz floats, rgba = 4 bytes/vertex. */
void vita_sw3d_draw(const float *pos3, const uint8_t *rgba, int nverts);

/* Copy the colour buffer (already top-down) into dst at (x, y). Always 0 us:
 * there is no GPU readback. */
unsigned vita_sw3d_present(uint32_t *dst, int dst_stride, int x, int y);


/* --- v03.05: textures / culling / blending --------------------------- */
#define SW3D_MAX_TEXTURES 64

/* pixels holds w*h ARGB ints (0xAARRGGBB, the Java int[] layout). Returns a
 * handle >= 1, or 0 when the table is full / the copy fails. */
int  vita_sw3d_texture_create(const uint32_t *argb, int w, int h);
void vita_sw3d_texture_bind(int handle);  /* 0 = modulate with vertex colour only */
/* v04.35: WRAP_S / WRAP_T of the bound texture (1 = clamp to edge). */
void vita_sw3d_texture_wrap(int clamp_s, int clamp_t);
/* v04.41: M3G Fog (near*256, far*256, density*1000; mode 0/80/81). */
void vita_sw3d_fog(int mode, int color, int near256, int far256,
                   int density1000);
void vita_sw3d_texture_free(int handle);

/* cull: 0 = both sides, 1 = drop back faces.
 *
 * v04.02: was (cull, blend) with blend a bool. `blend` is now the raw M3G
 * CompositingMode value (64 ALPHA, 65 ALPHA_ADD, 66 MODULATE,
 * 67 MODULATE_X2, 68 REPLACE - see vita_3d.h). The three flags and
 * alpha_threshold are the matching CompositingMode settings; unlike the GL
 * path this rasteriser enforces alpha_threshold per pixel, which is the only
 * way JSR-184's alpha test (foliage, fences, chain-link) can work here. */
void vita_sw3d_set_state(int cull, int blend, int depth_test, int depth_write,
                         int color_write, float alpha_threshold,
                         uint32_t blend_color);
void vita_sw3d_set_scissor(int x, int y, int w, int h);

/* Same as vita_sw3d_draw but with one (u,v) pair per vertex. */
void vita_sw3d_draw_uv(const float *pos3, const float *uv2,
                       const uint8_t *rgba, int nverts);

/* v03.49 diagnostics: where did triangles/pixels go. Counters accumulate between vita_sw3d_take_stats() calls
 * (which resets them); the tex_* fields are cumulative. */
typedef struct {
    unsigned draws, tri_in, tri_clip, tri_area0, tri_cull, tri_bbox, tri_rast;
    unsigned px_out, px_zr, px_zt, px_wr, px_bright, px_tex, px_notex;
    unsigned in_r, in_g, in_b;   /* max vertex colour handed in */
    unsigned out_r, out_g, out_b; /* max colour actually written */
    unsigned tex_ok, tex_fail, tex_dark, clears; /* tex_* cumulative, clears since last take */
} vita_sw3d_stats_t;
void vita_sw3d_take_stats(vita_sw3d_stats_t *out);

/* v03.54: per-draw record for one armed frame (which draw covers how many pixels, and where). */
typedef struct {
    unsigned nverts, tex, cull, blend, tri_in, tri_rast, px_wr, px_zt;
    unsigned px_out, px_zr, tri_small; /* v03.56: tri_small = rasterised triangles with |cross| < 1 (area < 0.5 px) */
    float amax;                        /* largest |cross| (twice the area, px^2) among rasterised triangles */
    float first[6];                    /* first rasterised triangle, screen px: x0,y0,x1,y1,x2,y2 */
    int pminx, pminy, pmaxx, pmaxy; /* projected vertices (screen px), before scissor */
    int wminx, wminy, wmaxx, wmaxy; /* pixels actually written */
} vita_sw3d_drec_t;
void vita_sw3d_trace_arm(int on);
int  vita_sw3d_trace_take(vita_sw3d_drec_t *out, int max);
#endif /* VITA_SW3D_H */
