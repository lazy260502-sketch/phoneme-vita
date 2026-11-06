/*
 * Per-vertex lighting for the JSR-184 layer (v04.20). Plain C, no KNI, so the host test can run it on arrays that
 * the real Java packing code produced and compare byte for byte with Renderer.shade's Java loop.
 *
 * Only ambient and directional lights are handled here (no per-vertex light position); Renderer falls back to the
 * Java loop when a point or spot light is present. The expressions and their order are those of the Java loop: one
 * float operation for one float operation, nothing reassociated, nothing fused (the target FPU has no FMA).
 *
 * prm: [0..2] emissive rgb, [3..5] ambient rgb, [6..8] diffuse rgb, [9..17] normal matrix (column-major 3x3),
 *      [18..21] default colour, then per light 9 floats: on(0/1), kind(0 ambient, 1 directional), r, g, b (already
 *      times intensity), x, y, z (direction), length of the direction.
 * Normals / colours are read straight from VertexArray storage: the int[] (BYTE/SHORT/FIXED, divided by 128 /
 * 32768 / 65536 by component size) or the float[] when the array has float data.
 */
#ifndef M3G_SHADE_H
#define M3G_SHADE_H

#include <math.h>

#define M3G_SHADE_PRM_BASE 22
#define M3G_SHADE_LIGHT_STRIDE 9

static float m3g_comp(const int *di, const float *df, int comps, int size, int v, int c)
{
    int idx = v * comps + c;
    if (df != 0) {
        return df[idx];
    }
    switch (size) {
    case 1: return (float)di[idx] / 128.0f;
    case 2: return (float)di[idx] / 32768.0f;
    default: return (float)di[idx] / 65536.0f;
    }
}

/* Java: (int)(v * 255.0f + 0.5f) clamped to 0..255; NaN -> 0 and saturation as in the JLS. */
static unsigned char m3g_clampb(float v)
{
    float t = v * 255.0f + 0.5f;
    int i;
    if (t != t) {
        i = 0;
    } else if (t >= 2147483648.0f) {
        i = 2147483647;
    } else if (t <= -2147483648.0f) {
        i = -2147483647 - 1;
    } else {
        i = (int)t;
    }
    if (i < 0) {
        return 0;
    }
    if (i > 255) {
        return 255;
    }
    return (unsigned char)i;
}

static void m3g_shade_apply(unsigned char *out, int n, const float *prm,
                            const int *nI, const float *nF, int nComps, int nSize,
                            const int *cI, const float *cF, int cComps, int cSize, int nl)
{
    float er = prm[0], eg = prm[1], eb = prm[2];
    float ar = prm[3], ag = prm[4], ab = prm[5];
    float dr = prm[6], dg = prm[7], db = prm[8];
    const float *nm = prm + 9;
    const float *light = prm + M3G_SHADE_PRM_BASE;
    int hasN = nComps > 0 && (nI != 0 || nF != 0);
    int hasC = cComps > 0 && (cI != 0 || cF != 0);
    int anyDirect = 0;
    int v, i;

    /* Renderer.shade: the fixed key light below is used only when no lit light has a direction. */
    for (i = 0; i < nl; i++) {
        const float *L = light + i * M3G_SHADE_LIGHT_STRIDE;
        if (L[0] != 0.0f && L[1] != 0.0f) {
            anyDirect = 1;
            break;
        }
    }

    for (v = 0; v < n; v++) {
        float r = er, gg = eg, b = eb;
        float nx = 0.0f, ny = 0.0f, nz = 1.0f;
        float cr, cg, cb, ca = 1.0f;

        if (hasN) {
            float lx = m3g_comp(nI, nF, nComps, nSize, v, 0);
            float ly = m3g_comp(nI, nF, nComps, nSize, v, 1);
            float lz = m3g_comp(nI, nF, nComps, nSize, v, 2);
            float len;
            nx = nm[0] * lx + nm[3] * ly + nm[6] * lz;
            ny = nm[1] * lx + nm[4] * ly + nm[7] * lz;
            nz = nm[2] * lx + nm[5] * ly + nm[8] * lz;
            len = sqrtf(nx * nx + ny * ny + nz * nz);
            if (len > 0.0f) {
                nx /= len;
                ny /= len;
                nz /= len;
            }
        }
        r += ar * 0.2f;
        gg += ag * 0.2f;
        b += ab * 0.2f;

        if (!anyDirect) {
            float ndl0 = nx * 0.3216f + ny * 0.6432f + nz * 0.7504f;
            float k = (ndl0 > 0.0f) ? (0.25f + 0.75f * ndl0) : 0.25f;
            r += dr * k;
            gg += dg * k;
            b += db * k;
        }

        for (i = 0; i < nl; i++) {
            const float *L = light + i * M3G_SHADE_LIGHT_STRIDE;
            float lr, lg, lb, ll, ndl;
            if (L[0] == 0.0f) {
                continue;
            }
            lr = L[2];
            lg = L[3];
            lb = L[4];
            if (L[1] == 0.0f) {
                r += ar * lr;
                gg += ag * lg;
                b += ab * lb;
                continue;
            }
            ll = L[8];
            if (ll <= 0.0f) {
                continue;
            }
            ndl = (nx * L[5] + ny * L[6] + nz * L[7]) / ll;
            if (ndl <= 0.0f) {
                continue;
            }
            r += dr * lr * ndl;
            gg += dg * lg * ndl;
            b += db * lb * ndl;
        }

        if (hasC) {
            cr = m3g_comp(cI, cF, cComps, cSize, v, 0);
            cg = m3g_comp(cI, cF, cComps, cSize, v, 1);
            cb = m3g_comp(cI, cF, cComps, cSize, v, 2);
            if (cComps > 3) {
                ca = m3g_comp(cI, cF, cComps, cSize, v, 3);
            }
        } else {
            cr = prm[18];
            cg = prm[19];
            cb = prm[20];
            ca = prm[21];
        }
        out[v * 4 + 0] = m3g_clampb(r * cr);
        out[v * 4 + 1] = m3g_clampb(gg * cg);
        out[v * 4 + 2] = m3g_clampb(b * cb);
        out[v * 4 + 3] = m3g_clampb(ca);
    }
}

#endif
