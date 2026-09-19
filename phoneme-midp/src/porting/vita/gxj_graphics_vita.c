/*
 * gxj_graphics_vita.c - PS Vita graphics implementation for MIDP LowLevelUI
 *
 * Purpose: Provide Vita-specific implementation of gxj (gx putpixel) graphics
 *          operations. All primitives draw into the shared software
 *          framebuffer (vita_fb) exported by platform_vita.c; the buffer is
 *          blitted to the Vita display by platform_vita_swap_buffers().
 *
 * Note: This is a software-rendering fallback. A future hardware-accelerated
 *       version could use vita2d / SceGxm for fill/draw/blit.
 */

#include <kni.h>
#include <jvm.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "platform_vita.h"

/*
 * Color conversion macros
 * The Vita display pixel format is A8B8G8R8 (little-endian 0xAARRGGBB when
 * read as a 32-bit word). phoneME passes pixel values in 0x00RRGGBB form
 * (opaque RGB, alpha implied 0xFF). We compose ARGB32 here.
 */

/* Convert Java 0x00RRGGBB to 0xFFRRGGBB (opaque ARGB32). */
#define JAVA_RGB_TO_ARGB32(c)  ((uint32_t)(0xFF000000u | ((uint32_t)(c) & 0x00FFFFFFu)))

/*
 * Initialize the graphics system.
 * The shared framebuffer is allocated by platform_vita_init(); we just make
 * sure it exists and clear it to opaque white so that any missed pixel
 * (e.g. during partial redraws) is a sane default rather than garbage.
 */
void platform_vita_graphics_init(void) {
    if (platform_vita_get_fb() == NULL) {
        return;
    }
    platform_vita_clear_screen(0xFFFFFFFF);
}

/**
 * Draw a single pixel at the specified position
 * Java declaration: drawPixel(IIII)V
 * Parameters:
 *   pixel Device-dependent (gxj_pixel_type == unsigned short) pixel value
 *   x The x coordinate
 *   y The y coordinate
 */
KNIEXPORT KNI_RETURNTYPE_VOID
KNIDECL(com_sun_midp_lowlevelui_graphics_Graphics_drawPixel) {
    int pixel = KNI_GetParameterAsInt(1);
    int x     = KNI_GetParameterAsInt(2);
    int y     = KNI_GetParameterAsInt(3);

    uint32_t *fb = platform_vita_get_fb();
    int fbW = platform_vita_get_fb_width();
    int fbH = platform_vita_get_fb_height();

    if (fb == NULL)         { KNI_ReturnVoid(); }
    if (x < 0 || x >= fbW)  { KNI_ReturnVoid(); }
    if (y < 0 || y >= fbH)  { KNI_ReturnVoid(); }

    /* Expand 16-bit 565 pixel to 32-bit ARGB. */
    uint32_t r = ((pixel >> 11) & 0x1F) << 3;
    uint32_t g = ((pixel >> 5)  & 0x3F) << 2;
    uint32_t b =  (pixel        & 0x1F) << 3;
    fb[y * fbW + x] = 0xFF000000u | (r << 16) | (g << 8) | b;

    KNI_ReturnVoid();
}

/**
 * Draw a straight line between two points (Bresenham).
 * Java declaration: drawLine(IIII)V
 * Parameters:
 *   pixel Device-dependent pixel value (gxj_pixel_type)
 *   x1, y1, x2, y2 endpoints (the caller has already translated by clip)
 */
KNIEXPORT KNI_RETURNTYPE_VOID
KNIDECL(com_sun_midp_lowlevelui_graphics_Graphics_drawLine) {
    int pixel = KNI_GetParameterAsInt(1);
    int x1    = KNI_GetParameterAsInt(2);
    int y1    = KNI_GetParameterAsInt(3);
    int x2    = KNI_GetParameterAsInt(4);
    int y2    = KNI_GetParameterAsInt(5);

    uint32_t *fb = platform_vita_get_fb();
    int fbW = platform_vita_get_fb_width();
    int fbH = platform_vita_get_fb_height();

    if (fb == NULL) { KNI_ReturnVoid(); }

    /* Expand 16-bit 565 to 32-bit ARGB once. */
    uint32_t r = ((pixel >> 11) & 0x1F) << 3;
    uint32_t g = ((pixel >> 5)  & 0x3F) << 2;
    uint32_t b =  (pixel        & 0x1F) << 3;
    uint32_t argb = 0xFF000000u | (r << 16) | (g << 8) | b;

    /* Bresenham */
    int dx = abs(x2 - x1), sx = (x1 < x2) ? 1 : -1;
    int dy = -abs(y2 - y1), sy = (y1 < y2) ? 1 : -1;
    int err = dx + dy;
    int e2;

    for (;;) {
        if (x1 >= 0 && x1 < fbW && y1 >= 0 && y1 < fbH) {
            fb[y1 * fbW + x1] = argb;
        }
        if (x1 == x2 && y1 == y2) break;
        e2 = 2 * err;
        if (e2 >= dy) { err += dy; x1 += sx; }
        if (e2 <= dx) { err += dx; y1 += sy; }
    }

    KNI_ReturnVoid();
}

/**
 * Fill a rectangle with the specified pixel value.
 * Java declaration: fillRect(IIII)V
 * Parameters:
 *   pixel Device-dependent pixel value
 *   x, y, w, h rectangle (caller has already clipped)
 */
KNIEXPORT KNI_RETURNTYPE_VOID
KNIDECL(com_sun_midp_lowlevelui_graphics_Graphics_fillRect) {
    int pixel = KNI_GetParameterAsInt(1);
    int x     = KNI_GetParameterAsInt(2);
    int y     = KNI_GetParameterAsInt(3);
    int w     = KNI_GetParameterAsInt(4);
    int h     = KNI_GetParameterAsInt(5);

    uint32_t *fb = platform_vita_get_fb();
    int fbW = platform_vita_get_fb_width();
    int fbH = platform_vita_get_fb_height();

    if (fb == NULL) { KNI_ReturnVoid(); }

    /* Clip to framebuffer. */
    if (x < 0)      { w += x; x = 0; }
    if (y < 0)      { h += y; y = 0; }
    if (x + w > fbW)  w = fbW - x;
    if (y + h > fbH)  h = fbH - y;
    if (w <= 0 || h <= 0) { KNI_ReturnVoid(); }

    uint32_t r = ((pixel >> 11) & 0x1F) << 3;
    uint32_t g = ((pixel >> 5)  & 0x3F) << 2;
    uint32_t b =  (pixel        & 0x1F) << 3;
    uint32_t argb = 0xFF000000u | (r << 16) | (g << 8) | b;

    for (int row = 0; row < h; row++) {
        uint32_t *p = &fb[(y + row) * fbW + x];
        for (int col = 0; col < w; col++) {
            p[col] = argb;
        }
    }

    KNI_ReturnVoid();
}

/**
 * Fill a triangle (the canonical MIDP primitive used by fillTriangle
 * and the AA-edge loop of fillPolygon).
 * Java declaration: fillTriangle(IIIIIII)V
 * Parameters:
 *   pixel Device-dependent pixel value
 *   x1, y1, x2, y2, x3, y3 vertices
 */
KNIEXPORT KNI_RETURNTYPE_VOID
KNIDECL(com_sun_midp_lowlevelui_graphics_Graphics_fillTriangle) {
    int pixel = KNI_GetParameterAsInt(1);
    int x1    = KNI_GetParameterAsInt(2);
    int y1    = KNI_GetParameterAsInt(3);
    int x2    = KNI_GetParameterAsInt(4);
    int y2    = KNI_GetParameterAsInt(5);
    int x3    = KNI_GetParameterAsInt(6);
    int y3    = KNI_GetParameterAsInt(7);

    uint32_t *fb = platform_vita_get_fb();
    int fbW = platform_vita_get_fb_width();
    int fbH = platform_vita_get_fb_height();

    if (fb == NULL) { KNI_ReturnVoid(); }

    /* Sort vertices by y ascending: v0 <= v1 <= v2 */
    int vx[3] = {x1, x2, x3};
    int vy[3] = {y1, y2, y3};
    for (int i = 0; i < 2; i++) {
        for (int j = i + 1; j < 3; j++) {
            if (vy[j] < vy[i]) {
                int t;
                t = vy[i]; vy[i] = vy[j]; vy[j] = t;
                t = vx[i]; vx[i] = vx[j]; vx[j] = t;
            }
        }
    }

    uint32_t r = ((pixel >> 11) & 0x1F) << 3;
    uint32_t g = ((pixel >> 5)  & 0x3F) << 2;
    uint32_t b =  (pixel        & 0x1F) << 3;
    uint32_t argb = 0xFF000000u | (r << 16) | (g << 8) | b;

    int total_h = vy[2] - vy[0];
    if (total_h == 0) { KNI_ReturnVoid(); }

    for (int y = vy[0]; y <= vy[2]; y++) {
        int second_half = (y > vy[1]) || (vy[1] == vy[0]);
        int segment_h = second_half ? (vy[2] - vy[1]) : (vy[1] - vy[0]);
        if (segment_h == 0) continue;

        float alpha = (float)(y - vy[0]) / (float)total_h;
        float beta  = (float)(y - vy[second_half ? 1 : 0]) / (float)segment_h;
        int xa = vx[0] + (int)((vx[2] - vx[0]) * alpha);
        int xb = vx[second_half ? 1 : 0] + (int)((vx[2] - vx[second_half ? 1 : 0]) * beta);

        if (xa > xb) { int t = xa; xa = xb; xb = t; }
        if (xa >= fbW || xb < 0) continue;
        if (xa < 0) xa = 0;
        if (xb >= fbW) xb = fbW - 1;
        if (y < 0 || y >= fbH) continue;

        uint32_t *p = &fb[y * fbW + xa];
        for (int x = xa; x <= xb; x++) {
            p[x - xa] = argb;
        }
    }

    KNI_ReturnVoid();
}

/**
 * Fill a rounded rectangle. Approximated as a regular fillRect plus
 * four corner clips. Good enough for typical MIDP UI; the user will see
 * flat corners instead of rounded ones, which is acceptable for the
 * first hardware-correct port.
 */
KNIEXPORT KNI_RETURNTYPE_VOID
KNIDECL(com_sun_midp_lowlevelui_graphics_Graphics_fillRoundRect) {
    int pixel = KNI_GetParameterAsInt(1);
    int x     = KNI_GetParameterAsInt(2);
    int y     = KNI_GetParameterAsInt(3);
    int w     = KNI_GetParameterAsInt(4);
    int h     = KNI_GetParameterAsInt(5);
    /* arcWidth, arcHeight ignored in software fallback */

    uint32_t *fb = platform_vita_get_fb();
    int fbW = platform_vita_get_fb_width();
    int fbH = platform_vita_get_fb_height();

    if (fb == NULL) { KNI_ReturnVoid(); }

    if (x < 0)      { w += x; x = 0; }
    if (y < 0)      { h += y; y = 0; }
    if (x + w > fbW)  w = fbW - x;
    if (y + h > fbH)  h = fbH - y;
    if (w <= 0 || h <= 0) { KNI_ReturnVoid(); }

    uint32_t r = ((pixel >> 11) & 0x1F) << 3;
    uint32_t g = ((pixel >> 5)  & 0x3F) << 2;
    uint32_t b =  (pixel        & 0x1F) << 3;
    uint32_t argb = 0xFF000000u | (r << 16) | (g << 8) | b;

    for (int row = 0; row < h; row++) {
        uint32_t *p = &fb[(y + row) * fbW + x];
        for (int col = 0; col < w; col++) {
            p[col] = argb;
        }
    }

    KNI_ReturnVoid();
}

/**
 * Draw an arc / ellipse outline.
 * Java declaration: drawArc(IIIIII)V
 * Parameters:
 *   pixel Device-dependent pixel value
 *   x, y, w, h bounding box
 *   startAngle, arcAngle in degrees (startAngle is normalized internally)
 */
KNIEXPORT KNI_RETURNTYPE_VOID
KNIDECL(com_sun_midp_lowlevelui_graphics_Graphics_drawArc) {
    int pixel      = KNI_GetParameterAsInt(1);
    int x          = KNI_GetParameterAsInt(2);
    int y          = KNI_GetParameterAsInt(3);
    int w          = KNI_GetParameterAsInt(4);
    int h          = KNI_GetParameterAsInt(5);
    int startAngle = KNI_GetParameterAsInt(6);
    int arcAngle   = KNI_GetParameterAsInt(7);

    uint32_t *fb = platform_vita_get_fb();
    int fbW = platform_vita_get_fb_width();
    int fbH = platform_vita_get_fb_height();

    uint32_t rr = ((pixel >> 11) & 0x1F) << 3;
    uint32_t gg = ((pixel >> 5)  & 0x3F) << 2;
    uint32_t bb =  (pixel        & 0x1F) << 3;
    uint32_t argb = 0xFF000000u | (rr << 16) | (gg << 8) | bb;

    /* Midpoint ellipse algorithm: only plots when the angle falls inside
     * the requested [start, start+arc) range. */
    long long a = w / 2;
    long long b = h / 2;
    if (a <= 0) a = 1;
    if (b <= 0) b = 1;
    long long a2 = a * a;
    long long b2 = b * b;
    long long cx = x + a;
    long long cy = y + b;

    int endAngle = startAngle + arcAngle;
    int sNorm = ((startAngle % 360) + 360) % 360;
    int eNorm;
    int arcPositive = (arcAngle >= 0);
    if (arcAngle >= 360 || arcAngle <= -360) {
        sNorm = 0;
        eNorm = 0; /* sentinel: always in range */
    } else if (arcPositive) {
        eNorm = (sNorm + arcAngle) % 360;
    } else {
        eNorm = ((endAngle % 360) + 360) % 360;
    }

    long long x0 = a;
    long long y0 = 0;
    long long err = 0;

    while (x0 >= 0) {
        long long px_arr[4] = { cx + x0, cx - x0, cx + x0, cx - x0 };
        long long py_arr[4] = { cy + y0, cy + y0, cy - y0, cy - y0 };
        for (int k = 0; k < 4; k++) {
            long long dx = px_arr[k] - cx;
            long long dy = py_arr[k] - cy;
            int inRange = 0;
            if (eNorm == 0 && sNorm == 0 && (arcAngle >= 360 || arcAngle <= -360)) {
                inRange = 1;
            } else {
                if (dx == 0 && dy == 0) {
                    inRange = 1;
                } else {
                    double ang = atan2((double)dy, (double)dx) * 180.0 / 3.14159265358979;
                    int ai = (int)ang;
                    if (ai < 0) ai += 360;
                    if (arcPositive) {
                        if (eNorm > sNorm) inRange = (ai >= sNorm && ai < eNorm);
                        else inRange = (ai >= sNorm || ai < eNorm);
                    } else {
                        if (eNorm > sNorm) inRange = (ai >= sNorm && ai < eNorm);
                        else inRange = (ai >= sNorm || ai < eNorm);
                    }
                }
            }
            if (inRange) {
                if (px_arr[k] >= 0 && px_arr[k] < fbW && py_arr[k] >= 0 && py_arr[k] < fbH) {
                    fb[py_arr[k] * fbW + px_arr[k]] = argb;
                }
            }
        }
        y0++;
        err += 1 + 2 * y0;
        if (2 * (err + x0) + 1 > 0) {
            x0--;
            err += 1 - 2 * x0;
        }
    }

    KNI_ReturnVoid();
}

/**
 * Fill an arc / pie slice.
 * Same shape as drawArc but rasterises the filled wedge.
 */
KNIEXPORT KNI_RETURNTYPE_VOID
KNIDECL(com_sun_midp_lowlevelui_graphics_Graphics_fillArc) {
    int pixel      = KNI_GetParameterAsInt(1);
    int x          = KNI_GetParameterAsInt(2);
    int y          = KNI_GetParameterAsInt(3);
    int w          = KNI_GetParameterAsInt(4);
    int h          = KNI_GetParameterAsInt(5);
    int startAngle = KNI_GetParameterAsInt(6);
    int arcAngle   = KNI_GetParameterAsInt(7);

    uint32_t *fb = platform_vita_get_fb();
    int fbW = platform_vita_get_fb_width();
    int fbH = platform_vita_get_fb_height();

    if (fb == NULL || w <= 0 || h <= 0) { KNI_ReturnVoid(); }

    uint32_t rr = ((pixel >> 11) & 0x1F) << 3;
    uint32_t gg = ((pixel >> 5)  & 0x3F) << 2;
    uint32_t bb =  (pixel        & 0x1F) << 3;
    uint32_t argb = 0xFF000000u | (rr << 16) | (gg << 8) | bb;

    long long a = w / 2;
    long long b = h / 2;
    if (a <= 0) a = 1;
    if (b <= 0) b = 1;
    long long cx = x + a;
    long long cy = y + b;
    long long a2 = a * a;
    long long b2 = b * b;

    if (arcAngle >= 360 || arcAngle <= -360) {
        startAngle = 0;
        arcAngle = 360;
    }
    int endAngle = startAngle + arcAngle;

    for (long long py = 0; py < h; py++) {
        for (long long px = 0; px < w; px++) {
            long long dx = px - a;
            long long dy = py - b;
            /* inside ellipse? */
            if ((dx * dx) * b2 + (dy * dy) * a2 > a2 * b2) continue;
            /* inside angle range? */
            double ang = atan2((double)dy, (double)dx) * 180.0 / 3.14159265358979;
            int ai = (int)ang;
            if (ai < 0) ai += 360;
            int s = ((startAngle % 360) + 360) % 360;
            int e = ((endAngle   % 360) + 360) % 360;
            int inside;
            if (e > s) inside = (ai >= s && ai < e);
            else       inside = (ai >= s || ai < e);
            if (!inside) continue;
            long long fbx = x + px;
            long long fby = y + py;
            if (fbx >= 0 && fbx < fbW && fby >= 0 && fby < fbH) {
                fb[fby * fbW + fbx] = argb;
            }
        }
    }

    KNI_ReturnVoid();
}

/**
 * Draw a string of characters (Latin-1, default font only).
 * Uses a tiny built-in 5x7 bitmap font - just enough to render menu
 * text and debug overlays until a real font system is wired in.
 * Java declaration: drawString(...)
 * The signature matches gxapi_native's Graphics.drawChars path; the
 * Java side converts Java chars to ASCII bytes before calling here.
 */
KNIEXPORT KNI_RETURNTYPE_VOID
KNIDECL(com_sun_midp_lowlevelui_graphics_Graphics_drawString) {
    int pixel = KNI_GetParameterAsInt(1);
    int x     = KNI_GetParameterAsInt(2);
    int y     = KNI_GetParameterAsInt(3);
    KNI_StartHandles(1);
    KNI_DeclareHandle(strBuf);
    KNI_GetParameterAsObject(4, strBuf);
    int len    = KNI_GetParameterAsInt(5);

    uint32_t *fb = platform_vita_get_fb();
    int fbW = platform_vita_get_fb_width();
    int fbH = platform_vita_get_fb_height();

    uint32_t rr = ((pixel >> 11) & 0x1F) << 3;
    uint32_t gg = ((pixel >> 5)  & 0x3F) << 2;
    uint32_t bb =  (pixel        & 0x1F) << 3;
    uint32_t argb = 0xFF000000u | (rr << 16) | (gg << 8) | bb;

    if (fb != NULL && len > 0) {
        /* Get raw bytes from the String. */
        jbyte rawBytes[512];
        int n = len > 255 ? 255 : len;
        KNI_GetRawArrayRegion(strBuf, 0, n, rawBytes);
        for (int i = 0; i < n; i++) {
            unsigned short ch = (unsigned short)(rawBytes[i] & 0xff);
            platform_vita_draw_char(fb, fbW, fbH, argb, x + i * 6, y, ch);
        }
    }

    KNI_EndHandles();
    KNI_ReturnVoid();
}

/**
 * Render an array of RGB pixels directly into the framebuffer.
 * Java declaration: drawRGB(IIIIIII[I)V
 * Parameters:
 *   rgbData  int[] of 0xAARRGGBB (Java ARGB int) pixels
 *   offset   start index in rgbData
 *   scanlen  number of array entries per source row
 *   x, y     destination top-left in the framebuffer
 *   w, h     region size
 */
KNIEXPORT KNI_RETURNTYPE_VOID
KNIDECL(com_sun_midp_lowlevelui_graphics_Graphics_drawRGB) {
    int offset  = KNI_GetParameterAsInt(1);
    int scanlen = KNI_GetParameterAsInt(2);
    int x       = KNI_GetParameterAsInt(3);
    int y       = KNI_GetParameterAsInt(4);
    int w       = KNI_GetParameterAsInt(5);
    int h       = KNI_GetParameterAsInt(6);

    KNI_StartHandles(1);
    KNI_DeclareHandle(rgbArr);
    KNI_GetParameterAsObject(7, rgbArr);

    uint32_t *fb = platform_vita_get_fb();
    int fbW = platform_vita_get_fb_width();
    int fbH = platform_vita_get_fb_height();

    jbyte rawBytes[4096];
    int nbytes = w * h * 4;
    if (nbytes > 4096) nbytes = 4096;
    KNI_GetRawArrayRegion(rgbArr, offset, nbytes, rawBytes);

    if (fb != NULL) {
        int x2 = x + w;
        int y2 = y + h;
        int cx1 = (x < 0) ? 0 : x;
        int cy1 = (y < 0) ? 0 : y;
        int cx2 = (x2 > fbW) ? fbW : x2;
        int cy2 = (y2 > fbH) ? fbH : y2;

        for (int row = cy1; row < cy2; row++) {
            int srcRow = (row - y) * scanlen + (cx1 - x);
            uint32_t *dst = &fb[row * fbW + cx1];
            for (int col = cx1; col < cx2; col++) {
                int idx = (srcRow + (col - cx1)) * 4;
                if (idx + 3 >= nbytes) break;
                /* Java ARGB 0xAARRGGBB -> Vita native ARGB */
                uint32_t a = (uint8_t)rawBytes[idx];
                uint32_t r = (uint8_t)rawBytes[idx + 1];
                uint32_t g = (uint8_t)rawBytes[idx + 2];
                uint32_t b = (uint8_t)rawBytes[idx + 3];
                dst[col - cx1] = (a << 24) | (r << 16) | (g << 8) | b;
            }
        }
    }

    KNI_EndHandles();
    KNI_ReturnVoid();
}

/**
 * Get the width of an image (off-screen ImageData).
 * Java declaration: imageWidth([BII)I
 */
KNIEXPORT KNI_RETURNTYPE_INT
KNIDECL(com_sun_midp_lowlevelui_graphics_Graphics_imageWidth) {
    KNI_ReturnInt(0);
}

/**
 * Get the height of an image.
 * Java declaration: imageHeight([BII)I
 */
KNIEXPORT KNI_RETURNTYPE_INT
KNIDECL(com_sun_midp_lowlevelui_graphics_Graphics_imageHeight) {
    KNI_ReturnInt(0);
}

/**
 * Set the current color (no-op in software renderer - the Java side
 * converts RGB -> 16-bit pixel on every primitive call).
 */
KNIEXPORT KNI_RETURNTYPE_VOID
KNIDECL(com_sun_midp_lowlevelui_graphics_Graphics_setColor) {
    KNI_ReturnVoid();
}