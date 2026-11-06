/*
 * vita_3d.h - 3D backend selection (hardware vitaGL / software rasteriser)
 *
 * Mode comes from defaults.cfg "3d=auto|hw|sw" (set in 设置 > 3D 模式):
 *   auto - try the hardware path first, fall back to software
 *   hw   - hardware only; init fails if vitaGL / libshacccg is unavailable
 *   sw   - software only; never touches GXM, so the system keyboard stays
 *
 * Hardware 3D takes over GXM for the whole process, which makes the system
 * IME (vita_gxm.c) unavailable for the rest of the run.
 */
#ifndef VITA_3D_H
#define VITA_3D_H

#include <stdint.h>

#define V3D_MODE_AUTO 0
#define V3D_MODE_HW   1
#define V3D_MODE_SW   2

#define V3D_BACKEND_NONE 0
#define V3D_BACKEND_HW   1
#define V3D_BACKEND_SW   2

/* Mode stored in defaults.cfg; V3D_MODE_AUTO when absent. */
int v3d_configured_mode(void);

/* m3gcam= in defaults.cfg: 0 = inv (default, view = inverse of the setCamera transform), 1 = direct (the transform
 * is already the view matrix), 2 = invt (inverse of its transpose). Anything else / absent = 0.
 * m3gnative=0 (v03.84) sets bit 16: skinning stays in Java. Packed into the same int so the ROM's native table does not
 * change. m3gskin= (v03.55) is gone: render(Node, Transform) now ignores the node's own transform (v03.85). */
int v3d_configured_cam(void);

/* Idempotent. Returns the backend in use (V3D_BACKEND_*) or a negative
 * error; with V3D_MODE_HW a hardware failure is NOT retried in software. */
int v3d_init(int mode, int w, int h);

int v3d_backend(void);
const char *v3d_backend_name(int backend);

/* Backend-neutral drawing contract: rgba is A8B8G8R8, m is a column-major
 * clip-space MVP, triangles are lists with per-vertex colour.
 * clear_color == 0 leaves the colour buffer untouched (M3G
 * Background.setColorClearEnable(false)) while the depth buffer is still
 * cleared when clear_depth != 0. */
void v3d_clear(uint32_t rgba, int clear_depth, int clear_color);
void v3d_set_mvp(const float m[16]);
void v3d_draw(const float *pos3, const uint8_t *rgba, int nverts);

int  v3d_texture_create(const uint32_t *argb, int w, int h);
void v3d_texture_bind(int handle);
/* v04.29: image filter of the texture bound by v3d_texture_bind: 1 = FILTER_NEAREST, 0 = linear. Hardware backend only;
 * the software rasteriser keeps its own sampling. */
void v3d_texture_filter(int nearest);
/* v04.35: WRAP_S / WRAP_T of the texture bound by v3d_texture_bind:
 * 1 = clamp to edge, 0 = repeat. M3G's default is REPEAT; CLAMP matters for
 * atlases whose UVs run a hair past 1.0, where a REPEAT sampler pulls in the
 * opposite edge (the coloured line seen on room_overlay in v04.31).
 * Both backends implement it - GL via GL_CLAMP_TO_EDGE, the software
 * rasteriser by not taking the fractional part of the coordinate. */
void v3d_texture_wrap(int clamp_s, int clamp_t);
/* v04.41: M3G Fog. `mode` is the raw javax.microedition.m3g value (0 = none,
 * 80 = EXPONENTIAL, 81 = LINEAR); near/far are distance*256 and density is
 * density*1000 (integers, so the KNI layer never touches the float ABI).
 * Both backends implement it: GLES1 has real GL_FOG, the software rasteriser
 * mixes by eye-space depth. */
void v3d_fog(int mode, int color, int near256, int far256, int density1000);
void v3d_texture_free(int handle);

/* v04.02: M3G CompositingMode, the whole thing.
 *
 * `blend` is the RAW javax.microedition.m3g value, deliberately not remapped
 * to a local enum: the numbers below are the ones in the M3G 1.1 API, so a
 * log line and the Javadoc can be compared without a translation table.
 *   64 ALPHA        src*a + dst*(1-a)
 *   65 ALPHA_ADD    src*a + dst
 *   66 MODULATE     src*dst
 *   67 MODULATE_X2  2*src*dst
 *   68 REPLACE      src              (no blending at all)
 * Anything else is treated as REPLACE.
 *
 * Before v04.02 the signature was (cull, blend) with blend a bool, so all
 * four non-REPLACE modes produced the same alpha-over result. A title that
 * uses MODULATE for a shadow got a translucent rectangle instead of a
 * multiply; one that sets depthWrite=false for a decal still wrote depth and
 * hid whatever it was drawn on; alphaThreshold (JSR-184's alpha test, used
 * for foliage and fences) was ignored entirely.
 *
 * `alpha_threshold` is 0..1, compared against the pixel alpha. The software
 * rasteriser implements it per pixel; the hardware path records it but cannot
 * apply it without a fragment shader discard, so it is a no-op there - a
 * caller must not assume it took effect. */
#define V3D_BLEND_ALPHA       64
#define V3D_BLEND_ALPHA_ADD   65
#define V3D_BLEND_MODULATE    66
#define V3D_BLEND_MODULATE_X2 67
#define V3D_BLEND_REPLACE     68

void v3d_set_state(int cull, int blend, int depth_test, int depth_write,
                   int color_write, float alpha_threshold,
                   uint32_t blend_color);

/* Restricts clears and draws to a pixel rectangle (M3G viewport). */
void v3d_set_scissor(int x, int y, int w, int h);
void v3d_draw_uv(const float *pos3, const float *uv2,
                 const uint8_t *rgba, int nverts);

/* Copy the finished frame (top-down) into dst at (x, y); returns the GPU
 * readback time in microseconds (0 for software). */
unsigned v3d_present(uint32_t *dst, int dst_stride, int x, int y);

/* Spin a test triangle for `frames` frames on the configured backend and log
 * timing, coverage and centre pixel to crumb.log. Opt-in via
 * ux0:/data/J2ME00001/gl3d_test. Returns the backend used or a negative error. */
int v3d_selftest(int frames);

/* v03.32: present-path probe sink. Called from the JSR-184 NativeBridge after
 * a readback, with the number of pixels that differ from the top-left pixel,
 * the total pixel count, the raw A8B8G8R8 top-left and centre values, and how
 * many pixels came back with alpha != 0. Writes one line to crumb.log so
 * "geometry never reached the framebuffer" can be told apart from
 * "readback/blit is broken". */
void v3d_trace_present(unsigned long long n, int distinct, int total,
                       unsigned first, unsigned centre, int alpha);

#endif /* VITA_3D_H */
