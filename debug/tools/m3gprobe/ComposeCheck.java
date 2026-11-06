package com.sun.m3g;

import java.util.Random;

/**
 * Compares Matrix4.compose (closed form, v04.10) with the two-general-multiply form it replaced, element by element,
 * on random and degenerate TRS values. -0 and +0 count as equal. Prints the number of differing elements (want 0).
 */
public class ComposeCheck {
    static void oldCompose(float[] dst, float[] t, float[] r, float[] s) {
        float[] rot = new float[16];
        float ax = r[1], ay = r[2], az = r[3];
        float len = (float) Math.sqrt(ax * ax + ay * ay + az * az);
        if (r[0] == 0.0f || len == 0.0f) {
            Matrix4.identity(rot);
        } else {
            ax /= len; ay /= len; az /= len;
            double rad = r[0] * (Math.PI / 180.0);
            float c = (float) Math.cos(rad);
            float sn = (float) Math.sin(rad);
            float k = 1.0f - c;
            rot[0] = c + ax * ax * k; rot[1] = ay * ax * k + az * sn; rot[2] = az * ax * k - ay * sn; rot[3] = 0;
            rot[4] = ax * ay * k - az * sn; rot[5] = c + ay * ay * k; rot[6] = az * ay * k + ax * sn; rot[7] = 0;
            rot[8] = ax * az * k + ay * sn; rot[9] = ay * az * k - ax * sn; rot[10] = c + az * az * k; rot[11] = 0;
            rot[12] = 0; rot[13] = 0; rot[14] = 0; rot[15] = 1;
        }
        float[] sc = { s[0], 0, 0, 0, 0, s[1], 0, 0, 0, 0, s[2], 0, 0, 0, 0, 1 };
        float[] tr = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, t[0], t[1], t[2], 1 };
        float[] m = new float[16];
        Matrix4.mul(m, rot, sc);
        Matrix4.mul(dst, tr, m);
    }

    public static void main(String[] a) {
        Random rnd = new Random(11);
        int bad = 0, cases = 0;
        for (int i = 0; i < 200000; i++) {
            float[] t = { rf(rnd), rf(rnd), rf(rnd) };
            float[] s = { rf(rnd), rf(rnd), rf(rnd) };
            float[] r = { rf(rnd) * 360f, rf(rnd), rf(rnd), rf(rnd) };
            if (i % 7 == 0) r[0] = 0.0f;
            if (i % 11 == 0) { r[1] = 0; r[2] = 0; r[3] = 0; }
            if (i % 13 == 0) s[1] = 0.0f;
            if (i % 17 == 0) s[2] = -s[2];
            float[] o = new float[16], n = new float[16];
            oldCompose(o, t, r, s);
            Matrix4.compose(n, t, r, s);
            cases++;
            for (int k = 0; k < 16; k++) {
                if (o[k] != n[k]) bad++;
            }
        }
        System.out.println("compose cases=" + cases + " differing elements=" + bad);
    }

    static float rf(Random r) {
        return (r.nextFloat() - 0.5f) * 200f;
    }
}
