package com.sun.m3g;

/**
 * 4x4 float matrix helpers, column-major (index = column*4 + row) because
 * that is what the Vita backend takes; M3G's Transform.get(float[]) contract
 * is row-major, and Transform converts.
 */
public final class Matrix4 {

    private Matrix4() {}

    public static void identity(float[] m) {
        for (int i = 0; i < 16; i++) {
            m[i] = 0.0f;
        }
        m[0] = m[5] = m[10] = m[15] = 1.0f;
    }

    /** out = a * b (out may alias neither). */
    public static void mul(float[] out, float[] a, float[] b) {
        float[] r = new float[16];
        for (int c = 0; c < 4; c++) {
            for (int row = 0; row < 4; row++) {
                float s = 0.0f;
                for (int k = 0; k < 4; k++) {
                    s += a[k * 4 + row] * b[c * 4 + k];
                }
                r[c * 4 + row] = s;
            }
        }
        System.arraycopy(r, 0, out, 0, 16);
    }

    /** out = a * b, no temporary: out must be a different array from a and b. */
    public static void mulNoAlias(float[] out, float[] a, float[] b) {
        for (int c = 0; c < 4; c++) {
            for (int row = 0; row < 4; row++) {
                float s = 0.0f;
                for (int k = 0; k < 4; k++) {
                    s += a[k * 4 + row] * b[c * 4 + k];
                }
                out[c * 4 + row] = s;
            }
        }
    }

    public static void copy(float[] dst, float[] src) {
        System.arraycopy(src, 0, dst, 0, 16);
    }

    /**
     * General 4x4 inverse (cofactor expansion). Returns false for a singular
     * matrix, which M3G treats as "no projection" (nothing is drawn).
     */
    public static boolean invert(float[] out, float[] m) {
        float[] inv = new float[16];
        inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14]
               - m[9] * m[6] * m[15] + m[9] * m[7] * m[14]
               + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
        inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14]
               + m[8] * m[6] * m[15] - m[8] * m[7] * m[14]
               - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
        inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13]
               - m[8] * m[5] * m[15] + m[8] * m[7] * m[13]
               + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
        inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13]
                + m[8] * m[5] * m[14] - m[8] * m[6] * m[13]
                - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
        inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14]
               + m[9] * m[2] * m[15] - m[9] * m[3] * m[14]
               - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
        inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14]
               - m[8] * m[2] * m[15] + m[8] * m[3] * m[14]
               + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
        inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13]
               + m[8] * m[1] * m[15] - m[8] * m[3] * m[13]
               - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
        inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13]
                - m[8] * m[1] * m[14] + m[8] * m[2] * m[13]
                + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
        inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14]
               - m[5] * m[2] * m[15] + m[5] * m[3] * m[14]
               + m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
        inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14]
               + m[4] * m[2] * m[15] - m[4] * m[3] * m[14]
               - m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
        inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13]
                - m[4] * m[1] * m[15] + m[4] * m[3] * m[13]
                + m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
        inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13]
                + m[4] * m[1] * m[14] - m[4] * m[2] * m[13]
                - m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
        inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10]
               + m[5] * m[2] * m[11] - m[5] * m[3] * m[10]
               - m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
        inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10]
               - m[4] * m[2] * m[11] + m[4] * m[3] * m[10]
               + m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
        inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9]
                + m[4] * m[1] * m[11] - m[4] * m[3] * m[9]
                - m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
        inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9]
                - m[4] * m[1] * m[10] + m[4] * m[2] * m[9]
                + m[8] * m[1] * m[6] - m[8] * m[2] * m[5];

        float det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8]
                  + m[3] * inv[12];
        /* v04.49: `det == 0.0f` is the wrong test for "not invertible".
         *
         * Two separate holes, both measured on host
         * (debug/tools/m3gprobe/SingularChk.java, InvChk.java):
         *
         *   (a) NaN input. If ANY element is NaN, det comes out NaN, and
         *       `NaN == 0.0f` is FALSE - so this returned true and handed the
         *       caller a matrix whose 16 elements were all NaN. A title reaches
         *       this by normalising a zero-length vector:
         *           invLen = 1/sqrt(0) = +Inf; 0 * Inf = NaN
         *       which is exactly 极品飞车's camera basis (log189).
         *
         *   (b) Inf input. If an element is infinite, det can be +Inf, and
         *       `Inf > 0` is TRUE, so the pivot test passes - but the inverse
         *       then contains `0 * Inf = NaN`. The determinant being infinite
         *       does not mean the matrix has a finite inverse.
         *
         * Both are "not invertible" in the only sense that matters to a caller:
         * NO USABLE INVERSE EXISTS. So the check is on the RESULT, not just the
         * determinant: compute the candidate inverse and reject it if any
         * element is not finite. `!(x == x)` is how NaN is detected without
         * Float.isNaN (which is available, but this keeps the loop branchless
         * and obviously total).
         *
         * JSR-184: "Throws ArithmeticException - if this transformation is not
         * invertible"; returning false here is what lets Transform.invert()
         * decide what to do (it now keeps the matrix unchanged instead of
         * killing the title - see that method for why).
         */
        if (!(Math.abs(det) > 0.0f)) {
            return false;
        }
        float scale = 1.0f / det;
        for (int i = 0; i < 16; i++) {
            float v = inv[i] * scale;
            if (!(v == v) || v == Float.POSITIVE_INFINITY
                    || v == Float.NEGATIVE_INFINITY) {
                return false;
            }
            out[i] = v;
        }
        return true;
    }

    /**
     * dst = T * R * S, the JSR-184 Transformable composition order.
     * <p>
     * v03.30: r is {angle in DEGREES, axisX, axisY, axisZ} - JSR-184's
     * setOrientation(angle, ax, ay, az), and exactly what the .m3g files
     * store. This used to read r[0..2] as three Euler angles in radians
     * (Rz*Ry*Rx), so a node with orientation (90, 0, 1, 0) - hunter.m3g has
     * several - turned 90 RADIANS about X, 0 about Y and 1 about Z, and
     * even the DEFAULT {0, 0, 0, 1}... was read as 1 radian about Y. Every
     * node that was not at identity came out rotated wrongly, so the models
     * were not just invisible but would have been mis-posed when visible.
     * An axis of zero length means "no rotation".
     */
    public static void compose(float[] dst, float[] t, float[] r, float[] s) {
        float[] rot = new float[16];
        float t0 = t[0], t1 = t[1], t2 = t[2];
        float s0 = s[0], s1 = s[1], s2 = s[2];
        float ax = r[1], ay = r[2], az = r[3];
        float len = (float)Math.sqrt(ax * ax + ay * ay + az * az);
        if (r[0] == 0.0f || len == 0.0f) {
            identity(rot);
        } else {
            ax /= len;
            ay /= len;
            az /= len;
            double rad = r[0] * (Math.PI / 180.0);
            float c = (float)Math.cos(rad);
            float sn = (float)Math.sin(rad);
            float k = 1.0f - c;
            /* Column-major rotation about a unit axis (Rodrigues). */
            rot[0]  = c + ax * ax * k;
            rot[1]  = ay * ax * k + az * sn;
            rot[2]  = az * ax * k - ay * sn;
            rot[3]  = 0.0f;
            rot[4]  = ax * ay * k - az * sn;
            rot[5]  = c + ay * ay * k;
            rot[6]  = az * ay * k + ax * sn;
            rot[7]  = 0.0f;
            rot[8]  = ax * az * k + ay * sn;
            rot[9]  = ay * az * k - ax * sn;
            rot[10] = c + az * az * k;
            rot[11] = 0.0f;
            rot[12] = 0.0f;
            rot[13] = 0.0f;
            rot[14] = 0.0f;
            rot[15] = 1.0f;
        }
        /* T * (R * S) with T a pure translation and S diagonal: the product is R's columns scaled, plus t. Equal to
         * the two general multiplies it replaces (every other term is a product with an exact 0 or 1). */
        dst[0] = rot[0] * s0;
        dst[1] = rot[1] * s0;
        dst[2] = rot[2] * s0;
        dst[3] = 0.0f;
        dst[4] = rot[4] * s1;
        dst[5] = rot[5] * s1;
        dst[6] = rot[6] * s1;
        dst[7] = 0.0f;
        dst[8] = rot[8] * s2;
        dst[9] = rot[9] * s2;
        dst[10] = rot[10] * s2;
        dst[11] = 0.0f;
        dst[12] = t0;
        dst[13] = t1;
        dst[14] = t2;
        dst[15] = 1.0f;
    }

    /** CLDC 1.1 Math has no atan2. Octant reduction plus two half-angle steps; error well below 1e-7 rad. */
    public static double atan2(double y, double x) {
        if (x == 0.0 && y == 0.0) {
            return 0.0;
        }
        double ax = (x < 0.0) ? -x : x;
        double ay = (y < 0.0) ? -y : y;
        boolean swap = ay > ax;
        double t = swap ? ax / ay : ay / ax;
        t = t / (1.0 + Math.sqrt(1.0 + t * t));
        t = t / (1.0 + Math.sqrt(1.0 + t * t));
        double t2 = t * t;
        double s = t * (1.0 - t2 / 3.0 + t2 * t2 / 5.0 - t2 * t2 * t2 / 7.0
                        + t2 * t2 * t2 * t2 / 9.0 - t2 * t2 * t2 * t2 * t2 / 11.0);
        double a = 4.0 * s;
        if (swap) {
            a = Math.PI / 2.0 - a;
        }
        if (x < 0.0) {
            a = Math.PI - a;
        }
        return (y < 0.0) ? -a : a;
    }

    public static void transformPoint(float[] out4, float[] m, float x, float y,
                               float z) {
        out4[0] = m[0] * x + m[4] * y + m[8] * z + m[12];
        out4[1] = m[1] * x + m[5] * y + m[9] * z + m[13];
        out4[2] = m[2] * x + m[6] * y + m[10] * z + m[14];
        out4[3] = m[3] * x + m[7] * y + m[11] * z + m[15];
    }

    /** 3x3 normal matrix = inverse transpose of the upper-left 3x3. */
    public static void normalMatrix(float[] out9, float[] m) {
        float a = m[0], b = m[4], c = m[8];
        float d = m[1], e = m[5], f = m[9];
        float g = m[2], h = m[6], i = m[10];
        float A = e * i - f * h, B = -(d * i - f * g), C = d * h - e * g;
        float det = a * A + b * B + c * C;
        if (det == 0.0f) {
            out9[0] = 1; out9[1] = 0; out9[2] = 0;
            out9[3] = 0; out9[4] = 1; out9[5] = 0;
            out9[6] = 0; out9[7] = 0; out9[8] = 1;
            return;
        }
        det = 1.0f / det;
        /* cofactor matrix / det == inverse transpose for the 3x3 part */
        out9[0] = A * det;
        out9[1] = B * det;
        out9[2] = C * det;
        out9[3] = -(b * i - c * h) * det;
        out9[4] = (a * i - c * g) * det;
        out9[5] = -(a * h - b * g) * det;
        out9[6] = (b * f - c * e) * det;
        out9[7] = -(a * f - c * d) * det;
        out9[8] = (a * e - b * d) * det;
    }
}
