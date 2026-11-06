/* Host test for jsr184/src/share/native/m3g_skin.h: compares the float kernel
 * with a double-precision reference on random data, including bad table rows.
 * Build: gcc -O2 -I../../../jsr184/src/share/native skin_test.c -o /tmp/skin_test */
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include "m3g_skin.h"

static float rnd(void) { return (float)rand() / RAND_MAX * 2.0f - 1.0f; }

int main(void)
{
    enum { N = 1500, NM = 30, NE = 60 };
    static float pos[N * 3], ref[N * 3], acc[N * 3], ws[N], mats[NM * 16];
    static double dacc[N * 3], dws[N];
    int ent[NE * 4];
    int i, e, v, bad = 0;
    double maxd = 0;

    srand(7);
    for (i = 0; i < N * 3; i++) pos[i] = ref[i] = rnd() * 50.0f;
    for (i = 0; i < NM * 16; i++) mats[i] = rnd();
    for (e = 0; e < NE; e++) {
        ent[4 * e] = (e % 17 == 5) ? -1 : (e % NM);
        ent[4 * e + 1] = (e % 13 == 3) ? -10 : rand() % N;
        ent[4 * e + 2] = rand() % 400;
        ent[4 * e + 3] = (e % 19 == 7) ? 0 : 1 + rand() % 100;
    }
    ent[4 * 8] = NM + 5;            /* matrix index out of range */
    ent[4 * 9 + 1] = N - 3;         /* range runs past n */
    ent[4 * 9 + 2] = 100;

    m3g_skin_apply(pos, N, mats, NM, ent, NE, acc, ws);

    for (e = 0; e < NE; e++) {
        int k = ent[4 * e], v0 = ent[4 * e + 1], v1, wt = ent[4 * e + 3];
        const float *m;
        if (k < 0 || k >= NM || wt <= 0) continue;
        if (v0 < 0) v0 = 0;
        v1 = ent[4 * e + 1] + ent[4 * e + 2];
        if (v1 > N) v1 = N;
        m = mats + k * 16;
        for (v = v0; v < v1; v++) {
            double x = ref[v * 3], y = ref[v * 3 + 1], z = ref[v * 3 + 2];
            dacc[v * 3] += wt * (m[0] * x + m[4] * y + m[8] * z + m[12]);
            dacc[v * 3 + 1] += wt * (m[1] * x + m[5] * y + m[9] * z + m[13]);
            dacc[v * 3 + 2] += wt * (m[2] * x + m[6] * y + m[10] * z + m[14]);
            dws[v] += wt;
        }
    }
    for (v = 0; v < N; v++) {
        for (i = 0; i < 3; i++) {
            double want = dws[v] > 0 ? dacc[v * 3 + i] / dws[v] : ref[v * 3 + i];
            double d = fabs(want - pos[v * 3 + i]);
            double rel = d / (fabs(want) + 1.0);
            if (rel > maxd) maxd = rel;
            if (rel > 1e-4) bad++;
        }
    }
    printf("max rel diff %.3g, bad %d\n", maxd, bad);
    return bad ? 1 : 0;
}
