/* Host test for jsr184/src/share/native/m3g_texfix.h.
 * Build: PATH=/usr/bin:/bin gcc -O1 -g -fsanitize=address,undefined -Wall -Wextra -I../../../jsr184/src/share/native \
 *        -I../../../vita-port/src texfix_test.c -o /tmp/texfix_test -lm
 * Run:   /tmp/texfix_test [real.png ...]   (optional PNGs are decoded with stb_image and measured) */
#include <stdio.h>
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#include "stb_image.h"
#include "m3g_texfix.h"

static int fails = 0;
#define CHECK(c, msg) do { if (!(c)) { printf("FAIL: %s\n", msg); fails++; } else printf("ok:   %s\n", msg); } while (0)

static unsigned lum(uint32_t v) { return (((v >> 16) & 0xFF) * 77 + ((v >> 8) & 0xFF) * 151 + (v & 0xFF) * 28) >> 8; }

int main(int argc, char **argv)
{
    enum { W = 12, H = 12 };
    uint32_t a[W * H], b[W * H];
    int x, y, i, n;

    for (i = 0; i < W * H; i++) a[i] = 0x00000000u;
    for (y = 4; y < 8; y++) for (x = 4; x < 8; x++) a[y * W + x] = 0xFFC83264u;     /* opaque (200,50,100) */
    memcpy(b, a, sizeof(a));
    n = m3g_bleed_argb(a, W, H, 3);
    CHECK(n > 0, "a texture with a hole gets bled");
    {
        int alphaOk = 1, opaqueOk = 1, ring1 = 1, ring3 = 1, far = 1;
        for (i = 0; i < W * H; i++) {
            if ((a[i] >> 24) != (b[i] >> 24)) alphaOk = 0;
            if ((b[i] >> 24) != 0 && a[i] != b[i]) opaqueOk = 0;
        }
        /* texel (3,3) is diagonal to the corner (4,4): ring 1; (1,5) is 3 columns left of column 4: ring 3 */
        if ((a[3 * W + 3] & 0xFFFFFF) != 0xC83264u) ring1 = 0;
        if ((a[5 * W + 1] & 0xFFFFFF) != 0xC83264u) ring3 = 0;
        if (a[0] != 0u || a[11 * W + 11] != 0u) far = 0;                  /* 4+ texels away: still black */
        CHECK(alphaOk, "alpha is never changed");
        CHECK(opaqueOk, "opaque texels are never changed");
        CHECK(ring1, "ring 1 takes the opaque colour");
        CHECK(ring3, "ring 3 takes it too (3 passes)");
        CHECK(far, "texels beyond the passes stay black");
    }

    for (i = 0; i < W * H; i++) a[i] = 0xFF112233u;
    memcpy(b, a, sizeof(a));
    CHECK(m3g_bleed_argb(a, W, H, 3) == 0 && memcmp(a, b, sizeof(a)) == 0, "fully opaque texture: untouched");

    for (i = 0; i < W * H; i++) a[i] = 0u;
    memcpy(b, a, sizeof(a));
    CHECK(m3g_bleed_argb(a, W, H, 3) == 0 && memcmp(a, b, sizeof(a)) == 0, "fully transparent texture: untouched");

    for (i = 0; i < W * H; i++) a[i] = 0x00FFFFFFu;                   /* transparent white, the artist's colour */
    a[5 * W + 5] = 0xFF0000FFu;
    memcpy(b, a, sizeof(a));
    m3g_bleed_argb(a, W, H, 3);
    CHECK(memcmp(a, b, sizeof(a)) == 0, "transparent texels with a real (non-black) colour are kept");

    for (i = 0; i < W * H; i++) a[i] = 0u;
    a[5 * W + 5] = 0x80FF0000u;                                       /* half transparent red */
    memcpy(b, a, sizeof(a));
    m3g_bleed_argb(a, W, H, 1);
    CHECK((a[5 * W + 5]) == b[5 * W + 5] && (a[5 * W + 6] & 0xFFFFFF) == 0xFF0000u, "partly transparent texel lends its colour, keeps its own");

    {   /* degenerate sizes must not read or write outside the buffer (run under ASan) */
        uint32_t one[1] = { 0u }, row[5] = { 0xFFFFFFFFu, 0u, 0u, 0u, 0u }, col[5];
        CHECK(m3g_bleed_argb(one, 1, 1, 3) == 0, "1x1");
        m3g_bleed_argb(row, 5, 1, 3);
        CHECK((row[3] & 0xFFFFFF) == 0xFFFFFF && row[4] == 0u, "5x1: 3 texels reached");
        for (i = 0; i < 5; i++) col[i] = (i == 0) ? 0xFFFFFFFFu : 0u;
        m3g_bleed_argb(col, 1, 5, 3);
        CHECK((col[3] & 0xFFFFFF) == 0xFFFFFF && col[4] == 0u, "1x5: 3 texels reached");
        CHECK(m3g_bleed_argb(NULL, 4, 4, 3) == 0 && m3g_bleed_argb(one, 0, 1, 3) == 0 && m3g_bleed_argb(one, 1, 1, 0) == 0, "bad arguments");
    }

    for (i = 1; i < argc; i++) {   /* real art: how much of the dark outline is left after linear filtering */
        int w, h, ch;
        unsigned char *img = stbi_load(argv[i], &w, &h, &ch, 4);
        uint32_t *p, *q;
        long pairs = 0, clearTexels = 0;
        double before = 0, after = 0;
        int changed, k;
        if (!img) { printf("skip %s\n", argv[i]); continue; }
        p = (uint32_t *)malloc((size_t)w * h * 4);
        q = (uint32_t *)malloc((size_t)w * h * 4);
        for (k = 0; k < w * h; k++) {
            unsigned r = img[k * 4], g = img[k * 4 + 1], bl = img[k * 4 + 2], al = img[k * 4 + 3];
            p[k] = (al << 24) | (r << 16) | (g << 8) | bl;
            if (al == 0) clearTexels++;
        }
        memcpy(q, p, (size_t)w * h * 4);
        changed = m3g_bleed_argb(q, w, h, 3);
        for (y = 0; y < h; y++) {
            for (x = 0; x + 1 < w; x++) {
                uint32_t u = p[y * w + x], v = p[y * w + x + 1], u2 = q[y * w + x], v2 = q[y * w + x + 1];
                uint32_t op = ((u >> 24) != 0) ? u : v, cl = ((u >> 24) != 0) ? v : u;
                uint32_t op2 = ((u >> 24) != 0) ? u2 : v2, cl2 = ((u >> 24) != 0) ? v2 : u2;
                if (((u >> 24) != 0) == ((v >> 24) != 0) || lum(op) == 0) continue;
                /* the middle of a bilinear sample between an opaque texel and a transparent one, as a share of the opaque brightness */
                before += ((double)(lum(op) + lum(cl)) / 2.0) / (double)lum(op);
                after += ((double)(lum(op2) + lum(cl2)) / 2.0) / (double)lum(op2);
                pairs++;
            }
        }
        printf("%s: %dx%d alpha0=%ld changed=%d, edge pairs=%ld, brightness at the edge midpoint: before %.0f%% after %.0f%%\n",
               argv[i], w, h, clearTexels, changed, pairs, pairs ? 100.0 * before / pairs : 0.0, pairs ? 100.0 * after / pairs : 0.0);
        for (k = 0; k < w * h; k++) if ((p[k] >> 24) != (q[k] >> 24)) { printf("FAIL: alpha changed in %s\n", argv[i]); fails++; break; }
        free(p); free(q); stbi_image_free(img);
    }
    printf("%s (%d failures)\n", fails ? "FAILED" : "PASSED", fails);
    return fails ? 1 : 0;
}
