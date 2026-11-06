/*
 * vita_gl3d.h - vitaGL offscreen 3D backend (hardware path of vita_3d.c)
 *
 * 3D renders into an FBO and is read back into the 2D software framebuffer,
 * so the existing sceDisplaySetFrameBuf path is untouched. vitaGL is never
 * asked to swap, so it never owns the display.
 *
 * GXM can be initialised only once per process: vitaGL (sceGxmVshInitialize)
 * and the IME path in vita_gxm.c (sceGxmInitialize) are mutually exclusive.
 * vita_gl3d_init() refuses once the IME has initialised GXM, and
 * vita_gxm_ensure_init() must refuse once vitaGL owns it (vita_gl3d_active()).
 */
#ifndef VITA_GL3D_H
#define VITA_GL3D_H

#include <stdint.h>

#define GL3D_OK            0
#define GL3D_ERR_NO_SHACC (-1)  /* libshacccg.suprx missing or failed to load */
#define GL3D_ERR_GXM_BUSY (-2)  /* GXM already initialised by the IME path */
#define GL3D_ERR_INIT     (-3)  /* vitaGL init / FBO setup failed */

/* Idempotent. Creates the vitaGL context and a w x h offscreen target. */
int vita_gl3d_init(int w, int h);

/* Non-zero once vitaGL owns GXM. */
int vita_gl3d_active(void);

/* Bind the offscreen target and set the viewport; call before drawing. */
void vita_gl3d_begin(void);

/* Read the target back into dst (A8B8G8R8, dst_stride in pixels), flipped to
 * top-down, placed at (x, y). Returns the readback time in microseconds. */
unsigned vita_gl3d_present(uint32_t *dst, int dst_stride, int x, int y);

/* Backend contract shared with vita_sw3d.h (see vita_3d.c). rgba is
 * A8B8G8R8; m is a column-major clip-space MVP. */
void vita_gl3d_clear(uint32_t rgba, int clear_depth, int clear_color);
void vita_gl3d_set_mvp(const float m[16]);
void vita_gl3d_draw(const float *pos3, const uint8_t *rgba, int nverts);
void vita_gl3d_draw_uv(const float *pos3, const float *uv2,
                       const uint8_t *rgba, int nverts);

/* v03.05: argb is 0xAARRGGBB (Java int[] layout); handles are >= 1. */
int  vita_gl3d_texture_create(const uint32_t *argb, int w, int h);
void vita_gl3d_texture_bind(int handle);
void vita_gl3d_texture_filter(int nearest);
/* v04.35: WRAP_S / WRAP_T of the bound texture (1 = clamp to edge). */
void vita_gl3d_texture_wrap(int clamp_s, int clamp_t);
/* v04.41: M3G Fog (near*256, far*256, density*1000; mode 0/80/81). */
void vita_gl3d_fog(int mode, int color, int near256, int far256,
                   int density1000);
void vita_gl3d_texture_free(int handle);
void vita_gl3d_set_state(int cull, int blend, int depth_test, int depth_write,
                         int color_write, float alpha_threshold,
                         uint32_t blend_color);
void vita_gl3d_set_scissor(int x, int y, int w, int h);

#endif /* VITA_GL3D_H */
