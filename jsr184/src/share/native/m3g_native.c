/*
 * JSR 184 (M3G) native layer for the PS Vita port.
 *
 * Only the drawing primitive lives here: M3G semantics (scene graph,
 * transforms, lighting, index buffers) are implemented in Java under
 * javax/microedition/m3g, and this file forwards finished triangle lists to
 * the Vita 3D backend (vita-port/src/vita_3d.c).
 *
 * Array access uses KNI_GetRawArrayRegion / KNI_SetRawArrayRegion (CLDC's
 * KNI has no raw pointer macro). Offsets and lengths are in BYTES.
 *
 * The backend entry points are weak because libobj.a is also linked into a
 * standalone libmidp link in some configurations; a missing backend then
 * degrades to a no-op instead of jumping through NULL.
 */
#include <kni.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#include <vita_3d.h>

#define M3G_WEAK __attribute__((weak))

/* v03.26: sanity ceilings for the frame buffer and per-draw vertex counts.
 * They exist so an ABI/size mistake cannot turn into a heap smash: the only
 * way these can be exceeded is a bug (a 4096x4096 surface is 64 MB, a single
 * draw of 1M indices is already far beyond any J2ME title). */
#define M3G_MAX_DIM     4096
#define M3G_MAX_PIXELS  (4096 * 4096)
#define M3G_MAX_VERTS   (1 << 20)
#define M3G_MAX_INDICES (1 << 22)

extern int v3d_init(int mode, int w, int h) M3G_WEAK;
extern int v3d_backend(void) M3G_WEAK;
extern int v3d_configured_mode(void) M3G_WEAK;
extern int v3d_configured_cam(void) M3G_WEAK;
extern void v3d_clear(uint32_t rgba, int clear_depth, int clear_color) M3G_WEAK;
extern void v3d_set_mvp(const float m[16]) M3G_WEAK;
extern void v3d_set_state(int cull, int blend, int depth_test,
                          int depth_write, int color_write,
                          float alpha_threshold, uint32_t blend_color) M3G_WEAK;
extern int v3d_texture_create(const uint32_t *argb, int w, int h) M3G_WEAK;
extern void v3d_texture_bind(int handle) M3G_WEAK;
extern void v3d_texture_filter(int nearest) M3G_WEAK;
extern void v3d_texture_wrap(int clamp_s, int clamp_t) M3G_WEAK;
/* v04.41: M3G Fog (mode, 0xRRGGBB, near*256, far*256, density*1000). */
extern void v3d_fog(int mode, int color, int near256, int far256,
                    int density1000) M3G_WEAK;
extern void v3d_texture_free(int handle) M3G_WEAK;
extern void v3d_draw_uv(const float *pos3, const float *uv2,
                        const uint8_t *rgba, int nverts) M3G_WEAK;
extern void v3d_set_scissor(int x, int y, int w, int h) M3G_WEAK;
extern unsigned v3d_present(uint32_t *dst, int dst_stride, int x, int y) M3G_WEAK;

/* v03.22: the size of the drawing surface that a Graphics target refers to.
 *
 * JSR-184 defines a viewport in the target's coordinate system, so
 * setViewport(0, 40, 240, 180) is legal on a 240x320 surface. The caller's
 * clip rectangle is NOT the target: the CAPCOM title sets clip (0,40,240,180)
 * and then asks for the same rect as the viewport, which only makes sense if
 * the target is the whole 240x320 canvas. Using the clip made us clamp the
 * viewport to 140 pixels tall and squeeze the scene into the wrong band
 * (log127/log129 "viewport clamp").
 *
 * The Displayable route (Display.getDisplay(null).getCurrent()) throws in
 * this MIDP - getDisplay needs a MIDlet instance - so the size comes straight
 * from the display layer that actually owns the virtual screen. In this port
 * the LCDUI canvas covers exactly that surface.
 */
extern int vita_display_get_width(void) M3G_WEAK;
extern int vita_display_get_height(void) M3G_WEAK;

/* v03.32: breadcrumb sink for the present-path probe. Defined in
 * vita-port/src/vita_3d.c; weak so a standalone link still works. */
extern void v3d_trace_present(unsigned long long n, int distinct, int total,
                              unsigned first, unsigned centre,
                              int alpha) M3G_WEAK;

static unsigned long long presentCount = 0;
static int probeReported = 0;

/* v03.36: reused A8B8G8R8 -> ARGB staging buffer (see present()). */
static jint *g_out = NULL;
static int g_outN = 0;

static uint32_t *g_frame = NULL;
static int g_w = 0, g_h = 0;

/* v04.28: transparent texels coloured from their opaque neighbours before upload (m3g_texfix.h); defaults.cfg m3gtex=0 off */
#include "m3g_texfix.h"
static int g_texfix_on = 1;

/* Scratch buffers, grown on demand. Keeping them here avoids a malloc per
 * draw call (a scene submits tens of meshes per frame). */
static float *g_pos = NULL;
static float *g_uv = NULL;
static unsigned char *g_col = NULL;
static int *g_idx = NULL;
static int g_capV = 0, g_capI = 0;

static int ensure_v(int nverts) {
    if (nverts <= g_capV) {
        return 1;
    }
    free(g_pos);
    free(g_uv);
    free(g_col);
    g_pos = (float *)malloc((size_t)nverts * 3 * sizeof(float));
    g_uv = (float *)malloc((size_t)nverts * 2 * sizeof(float));
    g_col = (unsigned char *)malloc((size_t)nverts * 4);
    if (!g_pos || !g_uv || !g_col) {
        free(g_pos); free(g_uv); free(g_col);
        g_pos = NULL; g_uv = NULL; g_col = NULL; g_capV = 0;
        return 0;
    }
    g_capV = nverts;
    return 1;
}

static int ensure_i(int nidx) {
    if (nidx <= g_capI) {
        return 1;
    }
    free(g_idx);
    g_idx = (int *)malloc((size_t)nidx * sizeof(int));
    if (!g_idx) {
        g_capI = 0;
        return 0;
    }
    g_capI = nidx;
    return 1;
}

/* v03.31: expanded (de-indexed) copies of the attribute arrays: entry k of
 * g_xpos/g_xuv/g_xcol is attribute entry g_idx[k] of g_pos/g_uv/g_col, so the
 * backends - which only take a plain triangle list - see the real triangles.
 * Capacity is in expanded vertices (= number of indices). */
static float *g_xpos = NULL;
static float *g_xuv = NULL;
static unsigned char *g_xcol = NULL;
static int g_capX = 0;

static int ensure_x(int n) {
    if (n <= g_capX) {
        return 1;
    }
    free(g_xpos);
    free(g_xuv);
    free(g_xcol);
    g_xpos = (float *)malloc((size_t)n * 3 * sizeof(float));
    g_xuv = (float *)malloc((size_t)n * 2 * sizeof(float));
    g_xcol = (unsigned char *)malloc((size_t)n * 4);
    if (!g_xpos || !g_xuv || !g_xcol) {
        free(g_xpos); free(g_xuv); free(g_xcol);
        g_xpos = NULL; g_xuv = NULL; g_xcol = NULL; g_capX = 0;
        return 0;
    }
    g_capX = n;
    return 1;
}

/* Fills g_x* from g_* using g_idx[0..nidx). Returns 0 when the buffers cannot
 * be had. An index outside [0, nverts) is a bug in the caller (Java builds the
 * list from a validated IndexBuffer); it is clamped to vertex 0 instead of
 * being trusted, because the alternative is reading outside g_pos. */
static int expand_by_index(int nverts, int nidx, int hasUv) {
    int k;
    if (!ensure_x(nidx)) {
        return 0;
    }
    for (k = 0; k < nidx; k++) {
        int v = g_idx[k];
        if (v < 0 || v >= nverts) {
            v = 0;
        }
        g_xpos[(size_t)k * 3 + 0] = g_pos[(size_t)v * 3 + 0];
        g_xpos[(size_t)k * 3 + 1] = g_pos[(size_t)v * 3 + 1];
        g_xpos[(size_t)k * 3 + 2] = g_pos[(size_t)v * 3 + 2];
        g_xcol[(size_t)k * 4 + 0] = g_col[(size_t)v * 4 + 0];
        g_xcol[(size_t)k * 4 + 1] = g_col[(size_t)v * 4 + 1];
        g_xcol[(size_t)k * 4 + 2] = g_col[(size_t)v * 4 + 2];
        g_xcol[(size_t)k * 4 + 3] = g_col[(size_t)v * 4 + 3];
        if (hasUv) {
            g_xuv[(size_t)k * 2 + 0] = g_uv[(size_t)v * 2 + 0];
            g_xuv[(size_t)k * 2 + 1] = g_uv[(size_t)v * 2 + 1];
        }
    }
    return 1;
}

/* v03.25: P A R A M E T E R S.
 *
 * `KNIDECLARGS` expands to NOTHING in CLDC's kni.h (build/vita_arm/dist/
 * include/kni.h:64 `#define KNIDECLARGS`), so the old declarations
 *     Java_com_sun_m3g_NativeBridge_init(KNIDECLARGS jint width, jint height)
 * compiled to a two-argument C function that the VM never calls that way - it
 * invokes every native with the arguments in the KNI frame and expects
 * KNI_GetParameterAsInt()/AsObject()/AsBoolean(). `width` and `height` were
 * therefore whatever happened to be in r0/r1: garbage.
 *
 * Consequence on device (log131):
 *     [m3g] backend init rc=-1 active=0 target=240x320
 *     crumb.log: 3d: hardware init failed rc=-1, falling back to software
 *                3d: software init failed rc=-1
 * init() allocated g_frame for a nonsense size, and v3d_init() got the same
 * nonsense width/height, so vita_sw3d_init()'s malloc failed and returned -1.
 * No backend, so no frame was ever drawn - while the models had in fact loaded
 * fine ("load '/hunter.m3g' ok objects=154").
 * Every parameterised entry point below now reads its arguments through the
 * KNI accessors. Object parameters must be declared as handles first. */

KNIEXPORT KNI_RETURNTYPE_INT
Java_com_sun_m3g_NativeBridge_init(void)
{
    jint width = KNI_GetParameterAsInt(1);
    jint height = KNI_GetParameterAsInt(2);

    if (v3d_init == NULL || v3d_configured_mode == NULL) {
        KNI_ReturnInt(-1);
    }
    /* v03.26: reject nonsense dimensions.
     * log131 (v03.24, before the parameter fix) fed garbage into this call and
     * the resulting malloc sized the frame buffer from junk. Guard the range so
     * a future ABI mistake degrades to "no backend" instead of smashing the
     * heap: the product is checked in 64-bit to rule out the `w * h * 4`
     * overflow that would otherwise pass a small size to malloc while the code
     * goes on to treat the buffer as w*h pixels. */
    if (width <= 0 || height <= 0 ||
        width > M3G_MAX_DIM || height > M3G_MAX_DIM ||
        (int64_t)width * height > M3G_MAX_PIXELS) {
        KNI_ReturnInt(-1);
    }
    if (g_frame != NULL && (width != g_w || height != g_h)) {
        free(g_frame);
        g_frame = NULL;
    }
    g_w = (int)width;
    g_h = (int)height;
    if (g_frame == NULL) {
        g_frame = (uint32_t *)malloc((size_t)g_w * g_h * sizeof(uint32_t));
        if (g_frame == NULL) {
            KNI_ReturnInt(-1);
        }
    }
    KNI_ReturnInt(v3d_init(v3d_configured_mode(), g_w, g_h));
}

KNIEXPORT KNI_RETURNTYPE_INT
Java_com_sun_m3g_NativeBridge_camMode(void)
{
    int mode;
    if (v3d_configured_cam == NULL) {
        KNI_ReturnInt(0);
    }
    mode = v3d_configured_cam();
    g_texfix_on = ((mode >> 18) & 1) == 0;      /* defaults.cfg m3gtex=0 */
    KNI_ReturnInt(mode);
}

KNIEXPORT KNI_RETURNTYPE_INT
Java_com_sun_m3g_NativeBridge_backend(KNIDECLARGS)
{
    if (v3d_backend == NULL) {
        KNI_ReturnInt(0);
    }
    KNI_ReturnInt(v3d_backend());
}

KNIEXPORT KNI_RETURNTYPE_INT
Java_com_sun_m3g_NativeBridge_textureCreate(void)
{
    uint32_t *buf;
    int n, handle = 0;
    jint width, height;

    /* KNI_StartHandles opens a brace scope and KNI_EndHandles closes it, so
     * early exits must not call EndHandles in the middle (the handle names
     * would go out of scope). The portable shape is do/while(0) + one exit. */
    KNI_StartHandles(1);
    KNI_DeclareHandle(argb);
    KNI_GetParameterAsObject(1, argb);
    width = KNI_GetParameterAsInt(2);
    height = KNI_GetParameterAsInt(3);

    do {
        if (v3d_texture_create == NULL || KNI_IsNullHandle(argb)
            || width <= 0 || height <= 0) {
            break;
        }
        n = (int)width * (int)height;
        if (n > (int)KNI_GetArrayLength(argb)) {
            break;
        }
        buf = (uint32_t *)malloc((size_t)n * sizeof(uint32_t));
        if (buf == NULL) {
            break;
        }
        KNI_GetRawArrayRegion(argb, 0, n * 4, (jbyte *)buf);
        if (g_texfix_on) {
            m3g_bleed_argb(buf, (int)width, (int)height, 3);
        }
        handle = v3d_texture_create(buf, (int)width, (int)height);
        free(buf);
    } while (0);

    KNI_EndHandles();
    KNI_ReturnInt(handle);
}

KNIEXPORT KNI_RETURNTYPE_VOID
Java_com_sun_m3g_NativeBridge_textureFree(void)
{
    jint handle = KNI_GetParameterAsInt(1);
    if (v3d_texture_free != NULL) {
        v3d_texture_free((int)handle);
    }
    KNI_ReturnVoid();
}

/* v04.41: M3G Fog. All five values arrive as ints (see NativeBridge.setFog);
 * near/far are *256 and density is *1000, so the backends can stay in fixed
 * point and the KNI argument ABI stays integer-only. */
KNIEXPORT KNI_RETURNTYPE_VOID
Java_com_sun_m3g_NativeBridge_setFog(void)
{
    jint mode = KNI_GetParameterAsInt(1);
    jint color = KNI_GetParameterAsInt(2);
    jint near256 = KNI_GetParameterAsInt(3);
    jint far256 = KNI_GetParameterAsInt(4);
    jint density1000 = KNI_GetParameterAsInt(5);
    if (v3d_fog != NULL) {
        v3d_fog((int)mode, (int)color, (int)near256, (int)far256,
                (int)density1000);
    }
    KNI_ReturnVoid();
}

KNIEXPORT KNI_RETURNTYPE_VOID
Java_com_sun_m3g_NativeBridge_clear(void)
{
    jint argb = KNI_GetParameterAsInt(1);
    jboolean depth = KNI_GetParameterAsBoolean(2);
    jboolean color = KNI_GetParameterAsBoolean(3);

    if (v3d_clear != NULL) {
        unsigned a = ((unsigned)argb >> 24) & 0xFF;
        unsigned r = ((unsigned)argb >> 16) & 0xFF;
        unsigned g = ((unsigned)argb >> 8) & 0xFF;
        unsigned b = (unsigned)argb & 0xFF;
        v3d_clear((a << 24) | (b << 16) | (g << 8) | r,
                  depth != 0, color != 0);
    }
    KNI_ReturnVoid();
}

KNIEXPORT KNI_RETURNTYPE_VOID
Java_com_sun_m3g_NativeBridge_setScissor(void)
{
    jint x = KNI_GetParameterAsInt(1);
    jint y = KNI_GetParameterAsInt(2);
    jint w = KNI_GetParameterAsInt(3);
    jint h = KNI_GetParameterAsInt(4);

    if (v3d_set_scissor != NULL) {
        v3d_set_scissor((int)x, (int)y, (int)w, (int)h);
    }
    KNI_ReturnVoid();
}

/* v03.22: the drawing surface size, i.e. the M3G target for a Graphics.
 * Returns 0 when the display layer has not been initialised (or when the weak
 * symbol is absent in a standalone link); the Java side then keeps its
 * clip-based fallback. */
KNIEXPORT KNI_RETURNTYPE_INT
Java_com_sun_m3g_NativeBridge_screenWidth(void)
{
    if (vita_display_get_width != NULL) {
        KNI_ReturnInt((jint)vita_display_get_width());
    }
    KNI_ReturnInt(0);
}

KNIEXPORT KNI_RETURNTYPE_INT
Java_com_sun_m3g_NativeBridge_screenHeight(void)
{
    if (vita_display_get_height != NULL) {
        KNI_ReturnInt((jint)vita_display_get_height());
    }
    KNI_ReturnInt(0);
}

KNIEXPORT KNI_RETURNTYPE_VOID
Java_com_sun_m3g_NativeBridge_setMvp(void)
{
    float buf[16];

    KNI_StartHandles(1);
    KNI_DeclareHandle(m);
    KNI_GetParameterAsObject(1, m);
    do {
        if (v3d_set_mvp == NULL || KNI_IsNullHandle(m)
            || KNI_GetArrayLength(m) < 16) {
            break;
        }
        KNI_GetRawArrayRegion(m, 0, 64, (jbyte *)buf);
        v3d_set_mvp(buf);
    } while (0);
    KNI_EndHandles();
    KNI_ReturnVoid();
}

KNIEXPORT KNI_RETURNTYPE_VOID
Java_com_sun_m3g_NativeBridge_setState(void)
{
    jboolean cull = KNI_GetParameterAsBoolean(1);
    jint blendMode = KNI_GetParameterAsInt(2);
    jboolean depthTest = KNI_GetParameterAsBoolean(3);
    jboolean depthWrite = KNI_GetParameterAsBoolean(4);
    jboolean colorWrite = KNI_GetParameterAsBoolean(5);
    jfloat alphaThreshold = KNI_GetParameterAsFloat(6);
    jint blendColor = KNI_GetParameterAsInt(7);

    if (v3d_set_state != NULL) {
        /* v04.02: pass the M3G CompositingMode through unchanged. The backend
         * decides how much of it it can honour; keeping the translation in one
         * place (the backends) is what stops "blend on/off" from silently
         * standing in for five different blend equations. */
        v3d_set_state(cull != 0, (int)blendMode, depthTest != 0,
                      depthWrite != 0, colorWrite != 0,
                      (float)alphaThreshold, (uint32_t)blendColor);
    }
    KNI_ReturnVoid();
}

KNIEXPORT KNI_RETURNTYPE_VOID
Java_com_sun_m3g_NativeBridge_draw(void)
{
    int nverts, nidx = 0, n;
    int expanded = 0;
    jint indexCount, texHandle;
    int texNearest, texWrap;
    int hasUv;

    KNI_StartHandles(4);
    KNI_DeclareHandle(pos3);
    KNI_DeclareHandle(uv2);
    KNI_DeclareHandle(rgba);
    KNI_DeclareHandle(indices);
    KNI_GetParameterAsObject(1, pos3);
    KNI_GetParameterAsObject(2, uv2);
    KNI_GetParameterAsObject(3, rgba);
    KNI_GetParameterAsObject(4, indices);
    indexCount = KNI_GetParameterAsInt(5);
    texHandle = KNI_GetParameterAsInt(6);
    texNearest = (int)((texHandle >> 16) & 1);      /* v04.29: Renderer packs FILTER_NEAREST above the handle */
    texWrap = (int)((texHandle >> 17) & 3);         /* v04.35: bit1 = wrap S clamp, bit2 = wrap T clamp */
    texHandle &= 0xFFFF;

    do {
        if (v3d_draw_uv == NULL || KNI_IsNullHandle(pos3)
            || KNI_IsNullHandle(rgba)) {
            break;
        }
        nverts = (int)KNI_GetArrayLength(pos3) / 3;
        if (nverts <= 0 || nverts > M3G_MAX_VERTS || !ensure_v(nverts)) {
            break;
        }
        KNI_GetRawArrayRegion(pos3, 0, nverts * 12, (jbyte *)g_pos);
        KNI_GetRawArrayRegion(rgba, 0, nverts * 4, (jbyte *)g_col);

        hasUv = !KNI_IsNullHandle(uv2) && KNI_GetArrayLength(uv2) >= nverts * 2;
        if (hasUv) {
            KNI_GetRawArrayRegion(uv2, 0, nverts * 8, (jbyte *)g_uv);
        }

        n = nverts;
        if (!KNI_IsNullHandle(indices) && indexCount > 0) {
            nidx = (int)indexCount;
            if (nidx > (int)KNI_GetArrayLength(indices)) {
                nidx = (int)KNI_GetArrayLength(indices);
            }
            if (nidx > M3G_MAX_INDICES) {
                nidx = M3G_MAX_INDICES;
            }
            nidx -= nidx % 3;          /* whole triangles only */
            if (nidx > 0 && ensure_i(nidx)) {
                KNI_GetRawArrayRegion(indices, 0, nidx * 4, (jbyte *)g_idx);
                /* v03.31: EXPAND the indices. This used to read the indices
                 * into g_idx and then never look at them: the backends take a
                 * plain triangle LIST (vertex i, i+1, i+2 form a triangle), so
                 * v3d_draw_uv(g_pos, ..., n) with n = indexCount drew the
                 * first `n` vertices of g_pos as consecutive triangles - which
                 * is not the model - and, because n is larger than nverts
                 * whenever vertices are shared (hunter: 1523 vertices, 2052
                 * indices; zombie 675/1668; jill_han 1073/3030), read PAST the
                 * end of g_pos/g_col/g_uv. With the hardware backend that is a
                 * GPU-visible out-of-range vertex read. Build the expanded
                 * arrays here so neither backend has to change. */
                if (expand_by_index(nverts, nidx, hasUv)) {
                    n = nidx;
                    expanded = 1;
                } else {
                    n = 0;
                }
            } else {
                n = 0;
            }
        }
        if (n <= 0) {
            break;
        }
        if (v3d_texture_bind != NULL) {
            v3d_texture_bind((int)texHandle);
            if (v3d_texture_filter != NULL) {
                v3d_texture_filter(g_texfix_on ? texNearest : 0);   /* defaults.cfg m3gtex=0: always linear, as before */
            }
            if (v3d_texture_wrap != NULL) {
                v3d_texture_wrap(texWrap & 1, (texWrap >> 1) & 1);
            }
        }
        if (expanded) {
            v3d_draw_uv(g_xpos, hasUv ? g_xuv : NULL, g_xcol, n);
        } else {
            v3d_draw_uv(g_pos, hasUv ? g_uv : NULL, g_col, n);
        }
    } while (0);

    KNI_EndHandles();
    KNI_ReturnVoid();
}

/* v03.84: CPU skinning in native code. Java passes the bind-pose positions, the
 * bone matrices and the entry table; the kernel (m3g_skin.h) deforms pos in
 * place. Scratch buffers are kept between calls (one per frame per skinned mesh). */
#include "m3g_skin.h"

static float *g_skpos = NULL, *g_skacc = NULL, *g_skws = NULL, *g_skmat = NULL;
static int *g_skent = NULL;
static int g_skCapN = 0, g_skCapM = 0, g_skCapE = 0;

static int sk_reserve(int n, int nmat, int nent) {
    if (n > g_skCapN) {
        free(g_skpos); free(g_skacc); free(g_skws);
        g_skpos = (float *)malloc((size_t)n * 3 * sizeof(float));
        g_skacc = (float *)malloc((size_t)n * 3 * sizeof(float));
        g_skws = (float *)malloc((size_t)n * sizeof(float));
        if (!g_skpos || !g_skacc || !g_skws) {
            free(g_skpos); free(g_skacc); free(g_skws);
            g_skpos = g_skacc = g_skws = NULL;
            g_skCapN = 0;
            return 0;
        }
        g_skCapN = n;
    }
    if (nmat > g_skCapM) {
        free(g_skmat);
        g_skmat = (float *)malloc((size_t)nmat * 16 * sizeof(float));
        if (!g_skmat) {
            g_skCapM = 0;
            return 0;
        }
        g_skCapM = nmat;
    }
    if (nent > g_skCapE) {
        free(g_skent);
        g_skent = (int *)malloc((size_t)nent * 4 * sizeof(int));
        if (!g_skent) {
            g_skCapE = 0;
            return 0;
        }
        g_skCapE = nent;
    }
    return 1;
}

KNIEXPORT KNI_RETURNTYPE_BOOLEAN
Java_com_sun_m3g_NativeBridge_skin(void)
{
    jboolean ok = KNI_FALSE;
    jint n, nent;
    int nmat;

    KNI_StartHandles(3);
    KNI_DeclareHandle(pos);
    KNI_DeclareHandle(mats);
    KNI_DeclareHandle(ent);
    KNI_GetParameterAsObject(1, pos);
    n = KNI_GetParameterAsInt(2);
    KNI_GetParameterAsObject(3, mats);
    KNI_GetParameterAsObject(4, ent);
    nent = KNI_GetParameterAsInt(5);

    do {
        if (KNI_IsNullHandle(pos) || KNI_IsNullHandle(mats)
            || KNI_IsNullHandle(ent)) {
            break;
        }
        if (n <= 0 || n > M3G_MAX_VERTS
            || (int)KNI_GetArrayLength(pos) < (int)n * 3) {
            break;
        }
        nmat = (int)KNI_GetArrayLength(mats) / 16;
        if (nent < 0) {
            nent = 0;
        }
        if (nent > (int)KNI_GetArrayLength(ent) / 4) {
            nent = (int)KNI_GetArrayLength(ent) / 4;
        }
        if (nmat <= 0 || nmat > 4096 || nent > (1 << 16)
            || !sk_reserve((int)n, nmat, (int)nent)) {
            break;
        }
        KNI_GetRawArrayRegion(pos, 0, (int)n * 12, (jbyte *)g_skpos);
        KNI_GetRawArrayRegion(mats, 0, nmat * 64, (jbyte *)g_skmat);
        if (nent > 0) {
            KNI_GetRawArrayRegion(ent, 0, (int)nent * 16, (jbyte *)g_skent);
        }
        m3g_skin_apply(g_skpos, (int)n, g_skmat, nmat, g_skent, (int)nent,
                       g_skacc, g_skws);
        KNI_SetRawArrayRegion(pos, 0, (int)n * 12, (const jbyte *)g_skpos);
        ok = KNI_TRUE;
    } while (0);

    KNI_EndHandles();
    KNI_ReturnBoolean(ok);
}

/* v04.20: per-vertex lighting (ambient + directional lights) in native code; see m3g_shade.h. Every argument is
 * checked against the array lengths; a refusal returns false and Java runs its own loop instead. */
#include "m3g_shade.h"

#define M3G_SHADE_MAX_LIGHTS 64

static void *g_shN = NULL, *g_shC = NULL;
static unsigned char *g_shOut = NULL;
static size_t g_shCapN = 0, g_shCapC = 0, g_shCapO = 0;
static float g_shPrm[M3G_SHADE_PRM_BASE + M3G_SHADE_LIGHT_STRIDE * M3G_SHADE_MAX_LIGHTS];

static int sh_reserve(void **p, size_t *cap, size_t need) {
    if (need <= *cap) {
        return 1;
    }
    free(*p);
    *p = malloc(need);
    if (*p == NULL) {
        *cap = 0;
        return 0;
    }
    *cap = need;
    return 1;
}

/* Copies a VertexArray's storage (int[] or float[]) into *buf; returns 0 when it is too short for n*comps values. */
static int sh_fetch(KNIDECLARGS jobject iH, jobject fH, int n, int comps, void **buf, size_t *cap,
                    const int **ip, const float **fp) {
    size_t bytes = (size_t)n * (size_t)comps * 4;
    *ip = NULL;
    *fp = NULL;
    if (comps == 0) {
        return 1;
    }
    if (!KNI_IsNullHandle(fH)) {
        if ((size_t)KNI_GetArrayLength(fH) * 4 < bytes || !sh_reserve(buf, cap, bytes)) {
            return 0;
        }
        KNI_GetRawArrayRegion(fH, 0, (jint)bytes, (jbyte *)*buf);
        *fp = (const float *)*buf;
        return 1;
    }
    if (KNI_IsNullHandle(iH)) {
        return 0;
    }
    if ((size_t)KNI_GetArrayLength(iH) * 4 < bytes || !sh_reserve(buf, cap, bytes)) {
        return 0;
    }
    KNI_GetRawArrayRegion(iH, 0, (jint)bytes, (jbyte *)*buf);
    *ip = (const int *)*buf;
    return 1;
}

KNIEXPORT KNI_RETURNTYPE_BOOLEAN
Java_com_sun_m3g_NativeBridge_shade(void)
{
    jboolean ok = KNI_FALSE;
    jint n, nComps, nSize, cComps, cSize, nl;

    KNI_StartHandles(6);
    KNI_DeclareHandle(outH);
    KNI_DeclareHandle(prmH);
    KNI_DeclareHandle(nIH);
    KNI_DeclareHandle(nFH);
    KNI_DeclareHandle(cIH);
    KNI_DeclareHandle(cFH);
    KNI_GetParameterAsObject(1, outH);
    n = KNI_GetParameterAsInt(2);
    KNI_GetParameterAsObject(3, prmH);
    KNI_GetParameterAsObject(4, nIH);
    KNI_GetParameterAsObject(5, nFH);
    nComps = KNI_GetParameterAsInt(6);
    nSize = KNI_GetParameterAsInt(7);
    KNI_GetParameterAsObject(8, cIH);
    KNI_GetParameterAsObject(9, cFH);
    cComps = KNI_GetParameterAsInt(10);
    cSize = KNI_GetParameterAsInt(11);
    nl = KNI_GetParameterAsInt(12);

    do {
        const int *nI, *cI;
        const float *nF, *cF;
        if (KNI_IsNullHandle(outH) || KNI_IsNullHandle(prmH)) {
            break;
        }
        if (n <= 0 || n > M3G_MAX_VERTS || nl < 0 || nl > M3G_SHADE_MAX_LIGHTS) {
            break;
        }
        if ((nComps != 0 && (nComps < 3 || nComps > 4)) || (cComps != 0 && (cComps < 3 || cComps > 4))) {
            break;
        }
        if ((int)KNI_GetArrayLength(outH) < (int)n * 4
            || (int)KNI_GetArrayLength(prmH) < M3G_SHADE_PRM_BASE + M3G_SHADE_LIGHT_STRIDE * (int)nl) {
            break;
        }
        if (!sh_reserve((void **)&g_shOut, &g_shCapO, (size_t)n * 4)) {
            break;
        }
        KNI_GetRawArrayRegion(prmH, 0, (jint)((M3G_SHADE_PRM_BASE + M3G_SHADE_LIGHT_STRIDE * (int)nl) * 4),
                              (jbyte *)g_shPrm);
        if (!sh_fetch(KNIPASSARGS nIH, nFH, (int)n, (int)nComps, &g_shN, &g_shCapN, &nI, &nF)
            || !sh_fetch(KNIPASSARGS cIH, cFH, (int)n, (int)cComps, &g_shC, &g_shCapC, &cI, &cF)) {
            break;
        }
        m3g_shade_apply(g_shOut, (int)n, g_shPrm, nI, nF, (int)nComps, (int)nSize, cI, cF, (int)cComps,
                        (int)cSize, (int)nl);
        KNI_SetRawArrayRegion(outH, 0, (jint)n * 4, (const jbyte *)g_shOut);
        ok = KNI_TRUE;
    } while (0);

    KNI_EndHandles();
    KNI_ReturnBoolean(ok);
}

/* v04.24: vertex attribute decode and skeleton matrices in native code; see m3g_geom.h. Same contract as skin/shade:
 * every argument is checked against the array lengths and a refusal returns false (Java then runs its own loop). */
#include "m3g_geom.h"

static void *g_gdSrc = NULL;
static float *g_gdOut = NULL;
static size_t g_gdCapS = 0, g_gdCapO = 0;

KNIEXPORT KNI_RETURNTYPE_BOOLEAN
Java_com_sun_m3g_NativeBridge_decode(void)
{
    jboolean ok = KNI_FALSE;
    jint n, stride, comps;

    KNI_StartHandles(4);
    KNI_DeclareHandle(outH);
    KNI_DeclareHandle(iH);
    KNI_DeclareHandle(fH);
    KNI_DeclareHandle(sbH);
    KNI_GetParameterAsObject(1, outH);
    n = KNI_GetParameterAsInt(2);
    stride = KNI_GetParameterAsInt(3);
    KNI_GetParameterAsObject(4, iH);
    KNI_GetParameterAsObject(5, fH);
    comps = KNI_GetParameterAsInt(6);
    KNI_GetParameterAsObject(7, sbH);       /* float[4]: scale, bias0, bias1, bias2 - no float arguments, see v03.48 */

    do {
        const int *di = NULL;
        const float *df = NULL;
        float sb[4];
        size_t bytes;
        if (KNI_IsNullHandle(outH) || KNI_IsNullHandle(sbH) || KNI_GetArrayLength(sbH) < 4) {
            break;
        }
        KNI_GetRawArrayRegion(sbH, 0, 16, (jbyte *)sb);
        if (n <= 0 || n > M3G_MAX_VERTS || (stride != 2 && stride != 3) || comps < stride || comps > 4) {
            break;
        }
        if ((size_t)KNI_GetArrayLength(outH) < (size_t)n * (size_t)stride) {
            break;
        }
        bytes = (size_t)n * (size_t)comps * 4;
        if (!sh_reserve(&g_gdSrc, &g_gdCapS, bytes) || !sh_reserve((void **)&g_gdOut, &g_gdCapO, (size_t)n * (size_t)stride * 4)) {
            break;
        }
        if (!KNI_IsNullHandle(fH)) {
            if ((size_t)KNI_GetArrayLength(fH) * 4 < bytes) {
                break;
            }
            KNI_GetRawArrayRegion(fH, 0, (jint)bytes, (jbyte *)g_gdSrc);
            df = (const float *)g_gdSrc;
        } else if (!KNI_IsNullHandle(iH)) {
            if ((size_t)KNI_GetArrayLength(iH) * 4 < bytes) {
                break;
            }
            KNI_GetRawArrayRegion(iH, 0, (jint)bytes, (jbyte *)g_gdSrc);
            di = (const int *)g_gdSrc;
        } else {
            break;
        }
        m3g_decode_apply(g_gdOut, (int)n, (int)stride, di, df, (int)comps, sb[0], sb[1], sb[2], sb[3]);
        KNI_SetRawArrayRegion(outH, 0, (jint)((size_t)n * (size_t)stride * 4), (const jbyte *)g_gdOut);
        ok = KNI_TRUE;
    } while (0);

    KNI_EndHandles();
    KNI_ReturnBoolean(ok);
}

static float *g_bnWorld = NULL, *g_bnBm = NULL, *g_bnLocal = NULL, *g_bnInv = NULL;
static unsigned char *g_bnState = NULL, *g_bnUsed = NULL;
static int *g_bnParent = NULL;
static size_t g_bnCapF = 0, g_bnCapW = 0, g_bnCapB = 0, g_bnCapI = 0, g_bnCapS = 0, g_bnCapU = 0, g_bnCapP = 0;

KNIEXPORT KNI_RETURNTYPE_BOOLEAN
Java_com_sun_m3g_NativeBridge_bones(void)
{
    jboolean ok = KNI_FALSE;
    jint nk;

    KNI_StartHandles(6);
    KNI_DeclareHandle(localH);
    KNI_DeclareHandle(parentH);
    KNI_DeclareHandle(invH);
    KNI_DeclareHandle(usedH);
    KNI_DeclareHandle(bmH);
    KNI_DeclareHandle(stateH);
    KNI_GetParameterAsObject(1, localH);
    KNI_GetParameterAsObject(2, parentH);
    KNI_GetParameterAsObject(3, invH);
    KNI_GetParameterAsObject(4, usedH);
    nk = KNI_GetParameterAsInt(5);
    KNI_GetParameterAsObject(6, bmH);
    KNI_GetParameterAsObject(7, stateH);

    do {
        size_t fb, k;
        if (KNI_IsNullHandle(localH) || KNI_IsNullHandle(parentH) || KNI_IsNullHandle(invH)
            || KNI_IsNullHandle(usedH) || KNI_IsNullHandle(bmH) || KNI_IsNullHandle(stateH)) {
            break;
        }
        if (nk <= 0 || nk > 4096) {
            break;
        }
        fb = (size_t)nk * 16 * 4;
        if ((size_t)KNI_GetArrayLength(localH) * 4 < fb || (size_t)KNI_GetArrayLength(invH) * 4 < fb
            || (size_t)KNI_GetArrayLength(bmH) * 4 < fb || (size_t)KNI_GetArrayLength(parentH) < (size_t)nk
            || (size_t)KNI_GetArrayLength(usedH) < (size_t)nk || (size_t)KNI_GetArrayLength(stateH) < (size_t)nk) {
            break;
        }
        if (!sh_reserve((void **)&g_bnLocal, &g_bnCapF, fb) || !sh_reserve((void **)&g_bnWorld, &g_bnCapW, fb)
            || !sh_reserve((void **)&g_bnBm, &g_bnCapB, fb) || !sh_reserve((void **)&g_bnInv, &g_bnCapI, fb)
            || !sh_reserve((void **)&g_bnState, &g_bnCapS, (size_t)nk) || !sh_reserve((void **)&g_bnUsed, &g_bnCapU, (size_t)nk)
            || !sh_reserve((void **)&g_bnParent, &g_bnCapP, (size_t)nk * 4)) {
            break;
        }
        KNI_GetRawArrayRegion(localH, 0, (jint)fb, (jbyte *)g_bnLocal);
        KNI_GetRawArrayRegion(invH, 0, (jint)fb, (jbyte *)g_bnInv);
        KNI_GetRawArrayRegion(parentH, 0, (jint)((size_t)nk * 4), (jbyte *)g_bnParent);
        KNI_GetRawArrayRegion(usedH, 0, nk, (jbyte *)g_bnUsed);
        /* a parent must come before its child (the chain is built that way); anything else is refused */
        for (k = 0; k < (size_t)nk; k++) {
            if (g_bnParent[k] >= (int)k) {
                break;
            }
        }
        if (k != (size_t)nk) {
            break;
        }
        m3g_bones_apply(g_bnWorld, g_bnBm, g_bnState, g_bnLocal, g_bnParent, g_bnInv, g_bnUsed, (int)nk);
        KNI_SetRawArrayRegion(bmH, 0, (jint)fb, (const jbyte *)g_bnBm);
        KNI_SetRawArrayRegion(stateH, 0, nk, (const jbyte *)g_bnState);
        ok = KNI_TRUE;
    } while (0);

    KNI_EndHandles();
    KNI_ReturnBoolean(ok);
}

static unsigned long long ptAccV = 0, ptAccRet = 0, ptAccConv = 0, ptAccCopy = 0;
static int ptAccN = 0;

static unsigned long long pt_now_us(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (unsigned long long)tv.tv_sec * 1000000ULL + (unsigned long long)tv.tv_usec;
}

KNIEXPORT KNI_RETURNTYPE_VOID
Java_com_sun_m3g_NativeBridge_present(void)
{
    int n, i;
    unsigned long long pt0 = 0, pt1 = 0, pt2 = 0, pt3 = 0;
    unsigned ptRet = 0;

    KNI_StartHandles(1);
    KNI_DeclareHandle(argb);
    KNI_GetParameterAsObject(1, argb);
    do {
        if (v3d_present == NULL || KNI_IsNullHandle(argb)
            || g_frame == NULL) {
            break;
        }
        /* v03.26: write exactly the frame and require the destination to hold
         * it. The old code did `n = g_w*g_h; if (n > len) n = len;`, so a
         * destination smaller than the frame produced a PARTIAL blit and
         * whatever length the destination reported was trusted. Requiring the
         * full size and writing exactly g_w*g_h*4 makes a mismatch a no-op
         * instead of a partial write (see the heap-corruption note in draw()). */
        n = g_w * g_h;
        if (n <= 0 || (int)KNI_GetArrayLength(argb) < n) {
            break;
        }
        pt0 = pt_now_us();
        ptRet = v3d_present(g_frame, g_w, 0, 0);
        pt1 = pt_now_us();

        /* v03.36: keep the staging buffer between frames.
         *
         * This used to malloc(n*4) + free() every single frame. At 240x320 that
         * is a 307 KB allocation per frame; with the 3D path now running at
         * ~10 fps the allocator churn is pure waste and it grows the heap
         * (user: "生化危机越来越卡"). One buffer, reallocated only when the
         * surface size changes. */
        if (g_out == NULL || g_outN < n) {
            free(g_out);
            g_out = (jint *)malloc((size_t)n * sizeof(jint));
            g_outN = (g_out != NULL) ? n : 0;
        }
        if (g_out == NULL) {
            break;
        }
        /* A8B8G8R8 -> 0xAARRGGBB for Graphics.drawRGB(). */
        for (i = 0; i < n; i++) {
            uint32_t v = g_frame[i];
            unsigned a = (v >> 24) & 0xFF;
            unsigned b = (v >> 16) & 0xFF;
            unsigned g = (v >> 8) & 0xFF;
            unsigned r = v & 0xFF;
            g_out[i] = (jint)((a << 24) | (r << 16) | (g << 8) | b);
        }
        pt2 = pt_now_us();
        KNI_SetRawArrayRegion(argb, 0, n * 4, (const jbyte *)g_out);
        pt3 = pt_now_us();
        /* v04.28: where the ~20 ms of present() go. v3d = v3d_present as a whole (GPU wait + glReadPixels + row copy;
         * ret = what the backend itself says it spent), conv = the ABGR->ARGB loop above, copy = KNI_SetRawArrayRegion. */
        ptAccV += pt1 - pt0;
        ptAccRet += ptRet;
        ptAccConv += pt2 - pt1;
        ptAccCopy += pt3 - pt2;
        if (++ptAccN >= 20) {
            fprintf(stderr, "[m3g] present split us/frame: v3d=%llu (backend says %llu) conv=%llu copy=%llu n=%d\n",
                    ptAccV / 20, ptAccRet / 20, ptAccConv / 20, ptAccCopy / 20, n);
            ptAccV = ptAccRet = ptAccConv = ptAccCopy = 0;
            ptAccN = 0;
        }

        /* v03.32/03.36: sample the readback, but ONLY while we still have
         * reports to emit.
         *
         * v03.32 counted every one of the n pixels on EVERY present, which is
         * 76800 iterations per frame added to the frame time - a self-inflicted
         * slowdown on top of a 10 fps path. Now the scan only happens while
         * probeReported < 8, and then on a stride so even that is cheap.
         * Values are printed raw (A8B8G8R8 as stored). */
        presentCount++;
        if (probeReported < 8 && (presentCount <= 3 || (presentCount % 30) == 0)) {
            uint32_t first = g_frame[0];
            int distinct = 0, alpha = 0, seen = 0;
            int stride = 1;
            while ((n / stride) > 20000) {
                stride <<= 1;
            }
            for (i = 0; i < n; i += stride) {
                if (g_frame[i] != first) {
                    distinct++;
                }
                if ((g_frame[i] >> 24) != 0) {
                    alpha++;
                }
                seen++;
            }
            if (stride != 1) {
                distinct *= stride;
                alpha *= stride;
                if (distinct > n) distinct = n;
                if (alpha > n) alpha = n;
            }
            probeReported++;
            if (v3d_trace_present != NULL) {
                v3d_trace_present(presentCount, distinct, n,
                                  (unsigned)first,
                                  (unsigned)g_frame[n / 2], alpha);
            }
        }
    } while (0);

    KNI_EndHandles();
    KNI_ReturnVoid();
}
