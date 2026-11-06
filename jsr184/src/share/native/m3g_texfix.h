/*
 * Alpha bleeding for texture upload (v04.28). Plain C, no KNI, host-testable.
 *
 * The art in these games stores fully transparent texels as RGB 0 (jil_all.png: 578 of 578, effects.png: 1387 of 1387).
 * GL_LINEAR averages a texel with its neighbours, so at the edge of the opaque part the colour is pulled towards black
 * and a dark outline appears. Only the RGB of transparent texels is changed here; alpha never is. A texel is touched
 * only when it is fully transparent AND its RGB is exactly 0 (an artist's real colour in a transparent texel is kept).
 * Each pass lets such a texel take the average RGB of its 8 neighbours that already carry a colour (alpha > 0, or
 * filled by an earlier pass), so the colour travels `passes` texels out of the opaque area.
 * Pixels are 0xAARRGGBB as in the Java int[]. Textures with no fully transparent texel, or nothing but transparent
 * ones, are left byte for byte as they were.
 */
#ifndef M3G_TEXFIX_H
#define M3G_TEXFIX_H

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* v04.31: room_overlay_01.png stores transparent texels as 0xFF00FF, not 0; both mean "no colour" */
#define M3G_NOCOLOUR(v) (((v) >> 24) == 0 && (((v) & 0x00FFFFFFu) == 0 || ((v) & 0x00FFFFFFu) == 0x00FF00FFu))

/* Returns the number of texels whose RGB was changed. */
static int m3g_bleed_argb(uint32_t *p, int w, int h, int passes)
{
    uint8_t *has;          /* 1 = this texel carries a usable colour */
    uint32_t *next;
    size_t n, i;
    int pass, x, y, changed = 0, anyClear = 0, anyColour = 0;

    if (p == NULL || w <= 0 || h <= 0 || passes <= 0) {
        return 0;
    }
    n = (size_t)w * (size_t)h;
    for (i = 0; i < n; i++) {
        if ((p[i] >> 24) == 0) {
            anyClear = 1;
        } else {
            anyColour = 1;
        }
    }
    if (!anyClear || !anyColour) {
        return 0;
    }
    has = (uint8_t *)malloc(n);
    next = (uint32_t *)malloc(n * sizeof(uint32_t));
    if (has == NULL || next == NULL) {
        free(has);
        free(next);
        return 0;
    }
    for (i = 0; i < n; i++) {
        has[i] = !M3G_NOCOLOUR(p[i]);   /* a transparent texel with a real colour counts */
    }
    for (pass = 0; pass < passes; pass++) {
        int any = 0;
        memcpy(next, p, n * sizeof(uint32_t));
        for (y = 0; y < h; y++) {
            for (x = 0; x < w; x++) {
                size_t k = (size_t)y * (size_t)w + (size_t)x;
                unsigned sr = 0, sg = 0, sb = 0, c = 0;
                int dy, dx;
                if (!M3G_NOCOLOUR(p[k]) || has[k]) {
                    continue;
                }
                for (dy = -1; dy <= 1; dy++) {
                    for (dx = -1; dx <= 1; dx++) {
                        int nx = x + dx, ny = y + dy;
                        size_t q;
                        if ((dx == 0 && dy == 0) || nx < 0 || ny < 0 || nx >= w || ny >= h) {
                            continue;
                        }
                        q = (size_t)ny * (size_t)w + (size_t)nx;
                        if (has[q]) {
                            sr += (p[q] >> 16) & 0xFF;
                            sg += (p[q] >> 8) & 0xFF;
                            sb += p[q] & 0xFF;
                            c++;
                        }
                    }
                }
                if (c != 0) {
                    next[k] = (p[k] & 0xFF000000u) | (((sr + c / 2) / c) << 16) | (((sg + c / 2) / c) << 8) | ((sb + c / 2) / c);
                    any = 1;
                }
            }
        }
        if (!any) {
            break;
        }
        /* texels that got a colour in this pass may lend it in the next one */
        for (i = 0; i < n; i++) {
            if (next[i] != p[i]) {
                has[i] = 1;
                changed++;
            }
        }
        memcpy(p, next, n * sizeof(uint32_t));
    }
    free(has);
    free(next);
    return changed;
}

#endif
