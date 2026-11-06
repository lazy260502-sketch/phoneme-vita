/* Replays the cases GeomCheck dumped (what Renderer.decodeAttr / SkinnedMesh.applySkin really passed to the native
 * decode() / bones(), plus what their Java loops produced) through jsr184/src/share/native/m3g_geom.h and compares
 * every result bit. Build: PATH=/usr/bin:/bin gcc -O2 -Wall -I../../../jsr184/src/share/native geom_test.c -o /tmp/geom_test -lm
 * Run: /tmp/geom_test <dir with d_*.bin and b_*.bin> */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "m3g_geom.h"

static unsigned char *slurp(const char *path, long *size)
{
    FILE *f = fopen(path, "rb");
    unsigned char *b;
    if (f == NULL) return NULL;
    fseek(f, 0, SEEK_END);
    *size = ftell(f);
    fseek(f, 0, SEEK_SET);
    b = (unsigned char *)malloc((size_t)*size);
    if (fread(b, 1, (size_t)*size, f) != (size_t)*size) { fclose(f); free(b); return NULL; }
    fclose(f);
    return b;
}

int main(int argc, char **argv)
{
    int idx, okD = 0, badD = 0, okB = 0, badB = 0;
    char path[512];
    long size;
    unsigned char *buf;

    if (argc < 2) return 2;
    for (idx = 0;; idx++) {
        int hdr[8], n, stride, comps, isF, cnt, i, diff = 0;
        const unsigned char *p;
        float sb[4], *res;
        const int *di = NULL;
        const float *df = NULL, *expected;
        snprintf(path, sizeof(path), "%s/d_%d.bin", argv[1], idx);
        buf = slurp(path, &size);
        if (buf == NULL) break;
        memcpy(hdr, buf, sizeof(hdr));
        n = hdr[0]; stride = hdr[1]; comps = hdr[2]; isF = hdr[3];
        p = buf + sizeof(hdr);
        memcpy(sb, p, 16);
        p += 16;
        cnt = n * comps;
        if (isF) df = (const float *)p; else di = (const int *)p;
        p += (size_t)cnt * 4;
        expected = (const float *)p;
        res = (float *)malloc((size_t)n * (size_t)stride * 4);
        m3g_decode_apply(res, n, stride, di, df, comps, sb[0], sb[1], sb[2], sb[3]);
        for (i = 0; i < n * stride; i++) {
            if (memcmp(&res[i], &expected[i], 4) != 0 && !(res[i] != res[i] && expected[i] != expected[i])) diff++;
        }
        if (diff) { badD++; printf("DIFF d_%d: %d of %d values (n=%d stride=%d comps=%d float=%d)\n", idx, diff, n * stride, n, stride, comps, isF); }
        else okD++;
        free(res);
        free(buf);
    }
    for (idx = 0;; idx++) {
        int nk, k, q, diff = 0, pad;
        const unsigned char *p;
        const float *local, *inv, *expBm;
        const int *parent;
        const unsigned char *used, *expState;
        float *world, *bm;
        unsigned char *state;
        snprintf(path, sizeof(path), "%s/b_%d.bin", argv[1], idx);
        buf = slurp(path, &size);
        if (buf == NULL) break;
        memcpy(&nk, buf, 4);
        pad = (nk + 3) & ~3;
        p = buf + 4;
        local = (const float *)p; p += (size_t)nk * 64;
        parent = (const int *)p; p += (size_t)nk * 4;
        inv = (const float *)p; p += (size_t)nk * 64;
        used = p; p += pad;
        expState = p; p += pad;
        expBm = (const float *)p;
        world = (float *)calloc((size_t)nk * 16, 4);
        bm = (float *)calloc((size_t)nk * 16, 4);
        state = (unsigned char *)calloc((size_t)nk, 1);
        m3g_bones_apply(world, bm, state, local, parent, inv, used, nk);
        for (k = 0; k < nk; k++) {
            if (state[k] != expState[k]) { diff++; continue; }
            if (state[k] == 1) {
                for (q = 0; q < 16; q++) {
                    if (memcmp(&bm[k * 16 + q], &expBm[k * 16 + q], 4) != 0) diff++;
                }
            }
        }
        if (diff) { badB++; printf("DIFF b_%d: %d differences (nk=%d)\n", idx, diff, nk); }
        else okB++;
        free(world); free(bm); free(state); free(buf);
    }
    printf("decode: %d identical, %d differ; bones: %d identical, %d differ\n", okD, badD, okB, badB);
    return (badD || badB || okD == 0 || okB == 0) ? 1 : 0;
}
