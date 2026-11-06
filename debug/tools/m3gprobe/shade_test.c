/* Replays the cases ShadeCheck dumped (the arguments Renderer.shade really packed, plus the output of its Java loop)
 * through the C kernel in jsr184/src/share/native/m3g_shade.h and compares every output byte.
 * Build: PATH=/usr/bin:/bin gcc -O2 -Wall -I../../../jsr184/src/share/native shade_test.c -o /tmp/shade_test -lm
 * Run:   /tmp/shade_test <dir with case_*.bin> */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "m3g_shade.h"

int main(int argc, char **argv)
{
    int idx, ok = 0, bad = 0, badBytes = 0;
    char path[512];

    if (argc < 2) {
        fprintf(stderr, "usage: %s dir\n", argv[0]);
        return 2;
    }
    for (idx = 0;; idx++) {
        FILE *f;
        long size;
        unsigned char *buf, *p, *res;
        int hdr[8], n, nC, nS, cC, cS, nl, prmLen, flags, nCount, cCount, i, diff = 0;
        const float *prm;
        const int *nI = NULL, *cI = NULL;
        const float *nF = NULL, *cF = NULL;
        const unsigned char *expected;

        snprintf(path, sizeof(path), "%s/case_%d.bin", argv[1], idx);
        f = fopen(path, "rb");
        if (f == NULL) {
            break;
        }
        fseek(f, 0, SEEK_END);
        size = ftell(f);
        fseek(f, 0, SEEK_SET);
        buf = (unsigned char *)malloc((size_t)size);
        if (fread(buf, 1, (size_t)size, f) != (size_t)size) {
            fprintf(stderr, "short read %s\n", path);
            return 2;
        }
        fclose(f);

        memcpy(hdr, buf, sizeof(hdr));
        n = hdr[0]; nC = hdr[1]; nS = hdr[2]; cC = hdr[3]; cS = hdr[4]; nl = hdr[5]; prmLen = hdr[6]; flags = hdr[7];
        p = buf + sizeof(hdr);
        prm = (const float *)p;
        p += (size_t)prmLen * 4;
        nCount = (flags & 3) ? n * nC : 0;
        cCount = (flags & 12) ? n * cC : 0;
        if (flags & 1) nI = (const int *)p;
        if (flags & 2) nF = (const float *)p;
        p += (size_t)nCount * 4;
        if (flags & 4) cI = (const int *)p;
        if (flags & 8) cF = (const float *)p;
        p += (size_t)cCount * 4;
        expected = p;
        if ((long)(p - buf) + n * 4 != size) {
            fprintf(stderr, "bad size in %s\n", path);
            return 2;
        }

        res = (unsigned char *)malloc((size_t)n * 4);
        m3g_shade_apply(res, n, prm, nI, nF, nC, nS, cI, cF, cC, cS, nl);
        for (i = 0; i < n * 4; i++) {
            if (res[i] != expected[i]) diff++;
        }
        if (diff) {
            bad++;
            badBytes += diff;
            if (bad <= 5) printf("DIFF case_%d: %d of %d bytes differ (nl=%d nC=%d cC=%d)\n", idx, diff, n * 4, nl, nC, cC);
        } else {
            ok++;
        }
        free(res);
        free(buf);
    }
    printf("cases %d identical, %d differ (%d bytes)\n", ok, bad, badBytes);
    return (bad || ok == 0) ? 1 : 0;
}
