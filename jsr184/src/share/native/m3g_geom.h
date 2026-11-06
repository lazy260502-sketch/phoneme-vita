/*
 * Vertex attribute decode and skeleton matrices for the JSR-184 layer (v04.24). Plain C, no KNI, so the host test can run
 * it on the arrays the real Java code produced and compare byte for byte with the Java loops they replace.
 *
 * decode: out[v*stride + c] = raw(v*comps + c) * scale + bias[c]   (Renderer: positions stride 3, texcoords stride 2)
 *         raw is VertexArray.componentRaw: the float[] when the array has float data, else the int[] converted to float.
 * bones:  world[k] = parent[k] < 0 ? local[k] : world[parent[k]] * local[k]        (SkinnedMesh.computeWorldInto)
 *         bm[k]    = world[k] * bindInv[k] for every k with used[k] != 0            (SkinnedMesh.applySkin)
 *         state[k] = 1 when every element of bm[k] is finite and within +-1e20, else 2; 0 when not used.
 * Matrices are 4x4 column-major, the product is Matrix4.mulNoAlias term for term (s += a[k*4+row] * b[c*4+k], k = 0..3),
 * nothing reassociated and nothing fused (the target FPU has no FMA).
 */
#ifndef M3G_GEOM_H
#define M3G_GEOM_H

static void m3g_decode_apply(float *out, int n, int stride, const int *di, const float *df, int comps,
                             float scale, float b0, float b1, float b2)
{
    float bias[3];
    int v, c;
    bias[0] = b0;
    bias[1] = b1;
    bias[2] = b2;
    for (v = 0; v < n; v++) {
        for (c = 0; c < stride; c++) {
            int idx = v * comps + c;
            float raw = (df != 0) ? df[idx] : (float)di[idx];
            out[v * stride + c] = raw * scale + bias[c];
        }
    }
}

static void m3g_mul44(float *out, const float *a, const float *b)
{
    int c, row, k;
    for (c = 0; c < 4; c++) {
        for (row = 0; row < 4; row++) {
            float s = 0.0f;
            for (k = 0; k < 4; k++) {
                s += a[k * 4 + row] * b[c * 4 + k];
            }
            out[c * 4 + row] = s;
        }
    }
}

static void m3g_bones_apply(float *world, float *bm, unsigned char *state, const float *local, const int *parent,
                            const float *bindInv, const unsigned char *used, int nk)
{
    int k, q;
    for (k = 0; k < nk; k++) {
        if (parent[k] < 0) {
            for (q = 0; q < 16; q++) {
                world[k * 16 + q] = local[k * 16 + q];
            }
        } else {
            m3g_mul44(world + k * 16, world + parent[k] * 16, local + k * 16);
        }
    }
    for (k = 0; k < nk; k++) {
        state[k] = 0;
        if (!used[k]) {
            continue;
        }
        m3g_mul44(bm + k * 16, world + k * 16, bindInv + k * 16);
        state[k] = 1;
        for (q = 0; q < 16; q++) {
            float x = bm[k * 16 + q];
            if (x != x || x > 1e20f || x < -1e20f) {
                state[k] = 2;
            }
        }
    }
}

#endif
