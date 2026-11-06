/*
 * Vertex skinning kernel for the JSR-184 layer (v03.84).
 *
 * Plain C with no KNI dependency, so the same code is compiled on the host
 * and compared with the Java reference (SkinnedMesh.applySkinJava). The
 * arithmetic and its order are the same as the Java loop, one float op per
 * Java float op, so results agree to the last bit when no FMA is generated.
 *
 *   ent[4*e+0] matrix index (-1 = skip this entry)
 *   ent[4*e+1] first vertex      ent[4*e+2] vertex count      ent[4*e+3] weight
 *   mats       nmat 4x4 column-major bone matrices (bone-now * bind-inverse)
 *
 * Every vertex becomes the weight-normalised sum of (matrix * vertex);
 * a vertex no entry touches is left as it was. All indices are checked: a
 * wrong table must not be able to write outside pos/acc/ws.
 */
#ifndef M3G_SKIN_H
#define M3G_SKIN_H

static void m3g_skin_apply(float *pos, int n, const float *mats, int nmat,
                           const int *ent, int nent, float *acc, float *ws)
{
    int i, e, v;

    for (i = 0; i < n * 3; i++) {
        acc[i] = 0.0f;
    }
    for (i = 0; i < n; i++) {
        ws[i] = 0.0f;
    }
    for (e = 0; e < nent; e++) {
        int k = ent[4 * e];
        long long v0 = ent[4 * e + 1];
        long long v1 = v0 + (long long)ent[4 * e + 2];
        int wt = ent[4 * e + 3];
        const float *m;
        float fw;

        if (k < 0 || k >= nmat || wt <= 0) {
            continue;
        }
        if (v0 < 0) {
            v0 = 0;
        }
        if (v1 > n) {
            v1 = n;
        }
        m = mats + (size_t)k * 16;
        fw = (float)wt;
        for (v = (int)v0; v < (int)v1; v++) {
            float x = pos[v * 3];
            float y = pos[v * 3 + 1];
            float z = pos[v * 3 + 2];
            acc[v * 3] += fw * (m[0] * x + m[4] * y + m[8] * z + m[12]);
            acc[v * 3 + 1] += fw * (m[1] * x + m[5] * y + m[9] * z + m[13]);
            acc[v * 3 + 2] += fw * (m[2] * x + m[6] * y + m[10] * z + m[14]);
            ws[v] += fw;
        }
    }
    for (v = 0; v < n; v++) {
        float s = ws[v];
        if (s > 0.0f) {
            float r = 1.0f / s;
            pos[v * 3] = acc[v * 3] * r;
            pos[v * 3 + 1] = acc[v * 3 + 1] * r;
            pos[v * 3 + 2] = acc[v * 3 + 2] * r;
        }
    }
}

#endif
