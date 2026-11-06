/*
 * vita_gl3d.c - vitaGL offscreen 3D backend (see vita_gl3d.h)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <psp2/io/fcntl.h>
#include <psp2/kernel/processmgr.h>

#include <vitaGL.h>
#include <vitashark.h>

#include "vita_gl3d.h"
#include "vita_gxm.h"
#include "vita_crumb.h"

/* RAM pool for vitaGL; vglInit() would take all free user memory instead. */
#define GL3D_RAM_POOL   (24 * 1024 * 1024)
#define GL3D_CDRAM_POOL (24 * 1024 * 1024)

static int g_active = 0;
static int g_vgl_started = 0; /* vglInit ran, so vitaGL owns GXM */
static int g_w = 0, g_h = 0;
static GLuint g_fbo = 0, g_tex = 0, g_depth = 0;
static uint8_t *g_rb = NULL; /* readback buffer, bottom-up RGBA */

/* v03.42: v03.38/v03.39 tracked the scissor rectangle here so present() could
 * read back only that part. Reverted - see vita_gl3d_present(): it saved
 * nothing measurable and left the rest of the frame buffer undefined, which
 * made the present probe report garbage. */


/* v03.05: texture slots are 1-based handles, 0 = "no texture". */
#define GL3D_MAX_TEXTURES 64
static GLuint g_tex_slot[GL3D_MAX_TEXTURES];
static int g_tex_slot_used[GL3D_MAX_TEXTURES];
static int g_tex_bound = 0;
static signed char g_tex_nearest[GL3D_MAX_TEXTURES];   /* v04.29: 1 when the slot's texture was switched to GL_NEAREST */
static signed char g_tex_clamp[GL3D_MAX_TEXTURES];     /* v04.35: bit0 = wrap S clamped, bit1 = wrap T clamped */
/* v04.41: last fog state pushed to GL (mode,color,near,far,density). A scene
 * usually has one fog, so this turns "per submesh" into two GL calls total. */
static int g_fog_last[5] = { -1, -1, -1, -1, -1 };

int vita_gl3d_active(void) {
    return g_vgl_started;
}

/* Only meaningful in an UNSAFE build: the safe sandbox hides ur0:data. */
static int gl3d_shacc_visible(void) {
    static const char *paths[] = {
        "ur0:data/libshacccg.suprx",
        "ur0:data/external/libshacccg.suprx",
    };
    unsigned i;
    for (i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
        SceUID fd = sceIoOpen(paths[i], SCE_O_RDONLY, 0);
        if (fd >= 0) {
            sceIoClose(fd);
            return 1;
        }
    }
    return 0;
}

int vita_gl3d_init(int w, int h) {
    if (g_active) {
        return GL3D_OK;
    }
    if (vita_gxm_is_initialized()) {
        crumb_printf("gl3d: GXM already owned by IME path");
        return GL3D_ERR_GXM_BUSY;
    }

    /* Bail out before vglInit: once it runs, vitaGL owns GXM and the system
     * keyboard is gone, which a software fallback must not pay for. */
    if (!gl3d_shacc_visible()) {
        crumb_printf("gl3d: libshacccg.suprx not found under ur0:data");
        return GL3D_ERR_NO_SHACC;
    }

    /* Return value is "resolution was clamped", not success; success is
     * judged by the FBO check below. */
    g_vgl_started = 1;
    {
        GLboolean clamped = vglInitWithCustomSizes(0, 960, 544, GL3D_RAM_POOL,
                                GL3D_CDRAM_POOL, 0, 0, SCE_GXM_MULTISAMPLE_NONE);
        crumb_printf("gl3d: vglInit returned %d", (int)clamped);
    }
    /* vitaGL tries the default path then ur0:data/external; shark_init is a
     * no-op returning 0 once loaded, so this only reports the failure code. */
    {
        int src = shark_init("ur0:data/external/libshacccg.suprx");
        if (src < 0) {
            crumb_printf("gl3d: shacccg load failed rc=0x%08x", (unsigned)src);
            return GL3D_ERR_NO_SHACC;
        }
    }

    g_w = w;
    g_h = h;
    g_rb = (uint8_t *)malloc((size_t)w * h * 4);
    if (!g_rb) {
        return GL3D_ERR_INIT;
    }

    glGenTextures(1, &g_tex);
    glBindTexture(GL_TEXTURE_2D, g_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA,
                 GL_UNSIGNED_BYTE, NULL);
    glGenRenderbuffers(1, &g_depth);
    glBindRenderbuffer(GL_RENDERBUFFER, g_depth);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT16, w, h);
    glGenFramebuffers(1, &g_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, g_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, g_tex, 0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                              GL_RENDERBUFFER, g_depth);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        crumb_printf("gl3d: FBO incomplete");
        return GL3D_ERR_INIT;
    }

    g_active = 1;

    /* v03.32: depth testing was NEVER enabled anywhere in this file, so every
     * triangle was drawn in submission order with no z rejection at all. That
     * is wrong for a depth-buffered scene graph (the FBO has a depth
     * attachment, and M3G's CompositingMode exposes depthTest/depthWrite), and
     * it also means translucent geometry (which Renderer submits with blending
     * on and expects not to write depth) was simply painted over the opaque
     * geometry behind it.
     * Enable it here, once, with the standard func; the per-draw state
     * (blend on/off) still comes from v3d_set_state(). Depth writes stay on
     * for opaque draws - the software backend uses the same rule. */
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glClearDepthf(1.0f);

    crumb_printf("gl3d: ready %dx%d", w, h);
    return GL3D_OK;
}

void vita_gl3d_begin(void) {
    if (!g_active) {
        return;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, g_fbo);
    glViewport(0, 0, g_w, g_h);
}

int vita_gl3d_texture_create(const uint32_t *argb, int w, int h) {
    uint8_t *rgba;
    size_t n, i;
    int slot;
    if (g_active == 0 || !argb || w <= 0 || h <= 0) {
        return 0;
    }
    for (slot = 0; slot < GL3D_MAX_TEXTURES; slot++) {
        if (!g_tex_slot_used[slot]) {
            break;
        }
    }
    if (slot == GL3D_MAX_TEXTURES) {
        return 0;
    }
    n = (size_t)w * h;
    rgba = (uint8_t *)malloc(n * 4);
    if (!rgba) {
        return 0;
    }
    /* Java int[] is 0xAARRGGBB; GL wants R,G,B,A bytes. */
    for (i = 0; i < n; i++) {
        uint32_t v = argb[i];
        rgba[i * 4 + 0] = (uint8_t)((v >> 16) & 0xFF); /* AARRGGBB -> R */
        rgba[i * 4 + 1] = (uint8_t)((v >> 8) & 0xFF);
        rgba[i * 4 + 2] = (uint8_t)(v & 0xFF);
        rgba[i * 4 + 3] = (uint8_t)((v >> 24) & 0xFF);
    }
    glGenTextures(1, &g_tex_slot[slot]);
    glBindTexture(GL_TEXTURE_2D, g_tex_slot[slot]);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA,
                 GL_UNSIGNED_BYTE, rgba);
    free(rgba);
    g_tex_slot_used[slot] = 1;
    g_tex_nearest[slot] = 0;
    g_tex_clamp[slot] = 0;
    return slot + 1;
}

void vita_gl3d_texture_bind(int handle) {
    if (handle <= 0 || handle > GL3D_MAX_TEXTURES || !g_tex_slot_used[handle - 1]) {
        g_tex_bound = 0;
        glDisable(GL_TEXTURE_2D);
        return;
    }
    g_tex_bound = handle;
    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, g_tex_slot[handle - 1]);
}

/* v04.29: set MIN/MAG filter of the texture bound last. Cached per slot so a scene does not re-issue it for every mesh;
 * a texture is created GL_LINEAR, so a slot starts (and starts again when it is reused) at 0. */
void vita_gl3d_texture_filter(int nearest) {
    signed char *cur = g_tex_nearest;
    int slot, want = nearest ? 1 : 0;
    if (!g_active || g_tex_bound <= 0 || g_tex_bound > GL3D_MAX_TEXTURES) {
        return;
    }
    slot = g_tex_bound - 1;
    if (cur[slot] == want) {
        return;
    }
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, want ? GL_NEAREST : GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, want ? GL_NEAREST : GL_LINEAR);
    cur[slot] = (signed char)want;
}

/* v04.35: wrap mode of the bound texture. Cached per slot like the filter,
 * because M3G can flip REPEAT/CLAMP per Appearance and re-issuing the two
 * glTexParameteri calls for every mesh would be pure overhead. */
void vita_gl3d_texture_wrap(int clamp_s, int clamp_t) {
    signed char *cur = g_tex_clamp;
    int slot, want = (clamp_s ? 1 : 0) | (clamp_t ? 2 : 0);

    if (!g_active || g_tex_bound <= 0 || g_tex_bound > GL3D_MAX_TEXTURES) {
        return;
    }
    slot = g_tex_bound - 1;
    if (cur[slot] == want) {
        return;
    }
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S,
                    (want & 1) ? GL_CLAMP_TO_EDGE : GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T,
                    (want & 2) ? GL_CLAMP_TO_EDGE : GL_REPEAT);
    cur[slot] = (signed char)want;
}

/* v04.41: M3G Fog. GLES1 has real fog, so this is the accurate path; the
 * software rasteriser implements the same two curves (see vita_sw3d.c). */
void vita_gl3d_fog(int mode, int color, int near256, int far256,
                   int density1000) {
    float c[4];

    if (!g_active) {
        return;
    }
    if (g_fog_last[0] == mode && g_fog_last[1] == color
            && g_fog_last[2] == near256 && g_fog_last[3] == far256
            && g_fog_last[4] == density1000) {
        return;             /* unchanged since the last draw */
    }
    g_fog_last[0] = mode;
    g_fog_last[1] = color;
    g_fog_last[2] = near256;
    g_fog_last[3] = far256;
    g_fog_last[4] = density1000;

    if (mode == 0) {
        glDisable(GL_FOG);
        return;
    }
    c[0] = (float) ((color >> 16) & 0xFF) / 255.0f;
    c[1] = (float) ((color >> 8) & 0xFF) / 255.0f;
    c[2] = (float) (color & 0xFF) / 255.0f;
    c[3] = 1.0f;
    glEnable(GL_FOG);
    glFogfv(GL_FOG_COLOR, c);
    if (mode == 81) {           /* Fog.LINEAR */
        glFogi(GL_FOG_MODE, GL_LINEAR);
        glFogf(GL_FOG_START, (float) near256 / 256.0f);
        glFogf(GL_FOG_END, (float) far256 / 256.0f);
    } else {                    /* Fog.EXPONENTIAL */
        glFogi(GL_FOG_MODE, GL_EXP);
        glFogf(GL_FOG_DENSITY, (float) density1000 / 1000.0f);
    }
    /* No glHint(GL_FOG_HINT, ...): vitaGL's GLES headers do not define it. */
}

void vita_gl3d_texture_free(int handle) {
    if (handle <= 0 || handle > GL3D_MAX_TEXTURES || !g_tex_slot_used[handle - 1]) {
        return;
    }
    glDeleteTextures(1, &g_tex_slot[handle - 1]);
    g_tex_slot_used[handle - 1] = 0;
    if (g_tex_bound == handle) {
        vita_gl3d_texture_bind(0);
    }
}

/* v04.02: the full M3G CompositingMode. `blend` is the raw JSR-184 value
 * (see vita_3d.h); `blend_color` is unused on this path because GLES has no
 * fragment shader here to mix it in, but the modes that do matter map
 * exactly:
 *
 *   REPLACE      no blend                       (was: no blend)
 *   ALPHA        SRC_ALPHA / 1-SRC_ALPHA        (was: same for ALL modes)
 *   ALPHA_ADD    SRC_ALPHA / ONE                (was: alpha over - wrong)
 *   MODULATE     DST_COLOR / ZERO               (was: alpha over - wrong)
 *   MODULATE_X2  DST_COLOR / SRC_COLOR          (was: alpha over - wrong)
 *
 * v04.42: alphaThreshold IS enforced here now. v04.02 claimed "GLES1 has no
 * alpha test" - that was WRONG: GLES1.0 has glAlphaFunc + GL_ALPHA_TEST, and
 * vitaGL implements both (GL_ALPHA_TEST, GL_GREATER and glAlphaFunc are in
 * vitaGL.h and libvitaGL.a). Without it, leaf/fence/foliage textures rendered
 * as solid quads on the hardware backend, which is the default one. */
void vita_gl3d_set_state(int cull, int blend, int depth_test, int depth_write,
                         int color_write, float alpha_threshold,
                         uint32_t blend_color) {
    (void)blend_color;
    if (!g_active) {
        return;
    }
    if (cull) {
        glEnable(GL_CULL_FACE);
        glCullFace(GL_BACK);
        glFrontFace(GL_CCW);
    } else {
        glDisable(GL_CULL_FACE);
    }

    switch (blend) {
    case 64: /* ALPHA */
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        break;
    case 65: /* ALPHA_ADD */
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE);
        break;
    case 66: /* MODULATE: dst = src*dst */
        glEnable(GL_BLEND);
        glBlendFunc(GL_DST_COLOR, GL_ZERO);
        break;
    case 67: /* MODULATE_X2: dst = 2*src*dst */
        glEnable(GL_BLEND);
        glBlendFunc(GL_DST_COLOR, GL_SRC_COLOR);
        break;
    default: /* REPLACE and anything unknown */
        glDisable(GL_BLEND);
        break;
    }

    if (depth_test) {
        glEnable(GL_DEPTH_TEST);
    } else {
        glDisable(GL_DEPTH_TEST);
    }
    glDepthMask(depth_write ? GL_TRUE : GL_FALSE);
    glColorMask(color_write ? GL_TRUE : GL_FALSE,
                color_write ? GL_TRUE : GL_FALSE,
                color_write ? GL_TRUE : GL_FALSE,
                color_write ? GL_TRUE : GL_FALSE);

    /* v04.42: real alpha test (see the note at the top of this function).
     * `alpha_threshold` is the M3G CompositingMode value, 0..1, where 0 means
     * "disabled". GL_GREATER matches the software rasteriser, which discards
     * when alpha < threshold (the specification's rule). */
    if (alpha_threshold > 0.0f) {
        glEnable(GL_ALPHA_TEST);
        glAlphaFunc(GL_GREATER, alpha_threshold);
    } else {
        glDisable(GL_ALPHA_TEST);
    }
}

void vita_gl3d_set_scissor(int x, int y, int w, int h) {
    if (!g_active) {
        return;
    }
    if (x == 0 && y == 0 && w == g_w && h == g_h) {
        glDisable(GL_SCISSOR_TEST);
        return;
    }
    glEnable(GL_SCISSOR_TEST);
    /* GL's origin is bottom-left; the caller uses top-left. */
    glScissor(x, g_h - (y + h), w, h);
}

void vita_gl3d_draw_uv(const float *pos3, const float *uv2,
                       const uint8_t *rgba, int nverts) {
    int tex = g_tex_bound && uv2 != NULL;
    if (!g_active) {
        return;
    }
    /* v04.10: a mesh can carry a texture WITHOUT texture coordinates.
     * Renderer.drawMesh() passes `(texHandle != 0) ? uv : null`, so the
     * combination "texture bound, uv2 == NULL" is reachable, and it used to
     * do glTexCoordPointer(2, GL_FLOAT, 0, NULL) - i.e. tell the GPU that
     * every texcoord lives at address 0. Whatever happens to be mapped there
     * is used as (s,t) for every vertex: the mesh comes out as one texel
     * blown up over the whole surface, or the fetch faults. M3G allows a
     * textured Appearance on a VertexBuffer with no TEXCOORD array, so this
     * must degrade instead of reading address 0.
     * Degrading means "draw it untextured": the vertex/material colour is
     * still meaningful, one stretched texel is not. */
    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_COLOR_ARRAY);
    glVertexPointer(3, GL_FLOAT, 0, pos3);
    glColorPointer(4, GL_UNSIGNED_BYTE, 0, rgba);
    if (g_tex_bound) {
        if (tex) {
            glEnable(GL_TEXTURE_2D);
            glEnableClientState(GL_TEXTURE_COORD_ARRAY);
            glTexCoordPointer(2, GL_FLOAT, 0, uv2);
        } else {
            glDisable(GL_TEXTURE_2D);
        }
    }
    glDrawArrays(GL_TRIANGLES, 0, nverts);
    if (g_tex_bound) {
        glDisableClientState(GL_TEXTURE_COORD_ARRAY);
        glEnable(GL_TEXTURE_2D);
    }
    glDisableClientState(GL_COLOR_ARRAY);
    glDisableClientState(GL_VERTEX_ARRAY);
}

unsigned vita_gl3d_present(uint32_t *dst, int dst_stride, int x, int y) {
    unsigned long long t0, t1;
    if (!g_active || !dst) {
        return 0;
    }
    t0 = sceKernelGetProcessTimeWide();
    /* v03.42: REVERTED the v03.39 "read back only the scissor rectangle".
     *
     * It was measurably worthless. perf.log before and after:
     *     v03.38 (full readback)   flip=25.6  prepare=17.1
     *     v03.39 (viewport only)   flip=25.3  prepare=17.1
     * flip is dominated by sceDisplayWaitVblankStart (16.7 ms), and prepare did
     * not move at all, so nothing was won.
     *
     * What it DID cost is worse: the caller (m3g_native.c present()) then
     * converts all g_w*g_h pixels and hands the whole array to Java, so the
     * rows outside the scissor rectangle were never written by the readback
     * and stayed uninitialised malloc memory. log140's probe consequently
     * reported pure garbage:
     *     first=0x81504690   distinct=76780/76800   alpha!=0=56488
     * 56488 > the 33600-pixel viewport, i.e. impossible for real content, and
     * first looks like a heap pointer. I nearly read that as progress.
     * Reading the full surface again keeps the frame fully defined for the cost
     * of one extra glReadPixels, which the numbers say was free anyway. */
    glReadPixels(0, 0, g_w, g_h, GL_RGBA, GL_UNSIGNED_BYTE, g_rb);
    t1 = sceKernelGetProcessTimeWide();
    /* GL rows are bottom-up; the 2D framebuffer is top-down. */
    {
        int row;
        for (row = 0; row < g_h; row++) {
            memcpy(dst + (size_t)(y + row) * dst_stride + x,
                   g_rb + (size_t)(g_h - 1 - row) * g_w * 4, (size_t)g_w * 4);
        }
    }
    return (unsigned)(t1 - t0);
}

void vita_gl3d_clear(uint32_t rgba, int clear_depth, int clear_color) {
    GLbitfield bits = 0;
    if (!g_active) {
        return;
    }
    vita_gl3d_begin();
    if (clear_color) {
        glClearColor((float)(rgba & 0xFF) / 255.0f,
                     (float)((rgba >> 8) & 0xFF) / 255.0f,
                     (float)((rgba >> 16) & 0xFF) / 255.0f,
                     (float)((rgba >> 24) & 0xFF) / 255.0f);
        bits |= GL_COLOR_BUFFER_BIT;
    }
    if (clear_depth) {
        bits |= GL_DEPTH_BUFFER_BIT;
    }
    if (bits != 0) {
        glClear(bits);
    }
}

/* The whole MVP goes into MODELVIEW with an identity PROJECTION, so clip
 * space is exactly MVP * v, matching the software backend. */
void vita_gl3d_set_mvp(const float m[16]) {
    if (!g_active) {
        return;
    }
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glMatrixMode(GL_MODELVIEW);
    glLoadMatrixf(m);
}

void vita_gl3d_draw(const float *pos3, const uint8_t *rgba, int nverts) {
    if (!g_active) {
        return;
    }
    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_COLOR_ARRAY);
    glVertexPointer(3, GL_FLOAT, 0, pos3);
    glColorPointer(4, GL_UNSIGNED_BYTE, 0, rgba);
    glDrawArrays(GL_TRIANGLES, 0, nverts);
    glDisableClientState(GL_COLOR_ARRAY);
    glDisableClientState(GL_VERTEX_ARRAY);
}
