/*
 * AffineTrans.java - VSCL 1.0 (see Graphics3D.java for the scope note).
 *
 * The matrix is exposed as public fields m00..m23 (3 rows x 4 columns) exactly
 * as the javadoc declares. Fixed point: 4096 = 1.0 for the 3x3 part AND for the
 * translation column, which keeps multiply() uniform.
 */
package com.vodafone.v10.graphics.j3d;

public class AffineTrans {

    public int m00;
    public int m01;
    public int m02;
    public int m03;
    public int m10;
    public int m11;
    public int m12;
    public int m13;
    public int m20;
    public int m21;
    public int m22;
    public int m23;

    public AffineTrans() {
        identity();
    }

    public AffineTrans(int[][] m) {
        set(m);
    }

    private void identity() {
        m00 = 4096; m01 = 0;    m02 = 0;    m03 = 0;
        m10 = 0;    m11 = 4096; m12 = 0;    m13 = 0;
        m20 = 0;    m21 = 0;    m22 = 4096; m23 = 0;
    }

    public void set(int[][] m) {
        if (m == null) {
            throw new NullPointerException("m");
        }
        if (m.length != 3 || m[0] == null || m[0].length != 4
                || m[1] == null || m[1].length != 4
                || m[2] == null || m[2].length != 4) {
            throw new IllegalArgumentException("need a 3x4 array");
        }
        m00 = m[0][0]; m01 = m[0][1]; m02 = m[0][2]; m03 = m[0][3];
        m10 = m[1][0]; m11 = m[1][1]; m12 = m[1][2]; m13 = m[1][3];
        m20 = m[2][0]; m21 = m[2][1]; m22 = m[2][2]; m23 = m[2][3];
    }

    public Vector3D transPoint(Vector3D src) {
        if (src == null) {
            throw new NullPointerException("src");
        }
        long x = src.x;
        long y = src.y;
        long z = src.z;
        Vector3D r = new Vector3D();
        r.x = (int) (((long) m00 * x + (long) m01 * y + (long) m02 * z
                      + ((long) m03 << 12)) >> 12);
        r.y = (int) (((long) m10 * x + (long) m11 * y + (long) m12 * z
                      + ((long) m13 << 12)) >> 12);
        r.z = (int) (((long) m20 * x + (long) m21 * y + (long) m22 * z
                      + ((long) m23 << 12)) >> 12);
        return r;
    }

    /** this = this * t */
    public void multiply(AffineTrans t) {
        if (t == null) {
            throw new NullPointerException("t");
        }
        multiply(this, t);
    }

    /** this = t1 * t2 (t1 and t2 may be this). */
    public void multiply(AffineTrans t1, AffineTrans t2) {
        if (t1 == null || t2 == null) {
            throw new NullPointerException();
        }
        /* Copy t1 first: the destination may be the same object. */
        int a00 = t1.m00, a01 = t1.m01, a02 = t1.m02, a03 = t1.m03;
        int a10 = t1.m10, a11 = t1.m11, a12 = t1.m12, a13 = t1.m13;
        int a20 = t1.m20, a21 = t1.m21, a22 = t1.m22, a23 = t1.m23;
        int b00 = t2.m00, b01 = t2.m01, b02 = t2.m02, b03 = t2.m03;
        int b10 = t2.m10, b11 = t2.m11, b12 = t2.m12, b13 = t2.m13;
        int b20 = t2.m20, b21 = t2.m21, b22 = t2.m22, b23 = t2.m23;

        m00 = mul(a00, b00, a01, b10, a02, b20);
        m01 = mul(a00, b01, a01, b11, a02, b21);
        m02 = mul(a00, b02, a01, b12, a02, b22);
        m03 = mul(a00, b03, a01, b13, a02, b23) + a03;

        m10 = mul(a10, b00, a11, b10, a12, b20);
        m11 = mul(a10, b01, a11, b11, a12, b21);
        m12 = mul(a10, b02, a11, b12, a12, b22);
        m13 = mul(a10, b03, a11, b13, a12, b23) + a13;

        m20 = mul(a20, b00, a21, b10, a22, b20);
        m21 = mul(a20, b01, a21, b11, a22, b21);
        m22 = mul(a20, b02, a21, b12, a22, b22);
        m23 = mul(a20, b03, a21, b13, a22, b23) + a23;
    }

    /** (a*c1 + b*c2 + c*c3) >> 12, kept in 64 bits so nothing overflows. */
    private static int mul(int a, int c1, int b, int c2, int c, int c3) {
        return (int) (((long) a * c1 + (long) b * c2 + (long) c * c3) >> 12);
    }

    /** Fixed-point (4096) product. */
    private static int fmul(int a, int b) {
        return (int) (((long) a * b) >> 12);
    }

    public void rotationX(int a) {
        int c = Util3D.cos(a);
        int s = Util3D.sin(a);
        AffineTrans r = new AffineTrans();
        r.m11 = c;
        r.m12 = -s;
        r.m21 = s;
        r.m22 = c;
        multiply(this, r);
    }

    public void rotationY(int a) {
        int c = Util3D.cos(a);
        int s = Util3D.sin(a);
        AffineTrans r = new AffineTrans();
        r.m00 = c;
        r.m02 = s;
        r.m20 = -s;
        r.m22 = c;
        multiply(this, r);
    }

    public void rotationZ(int a) {
        int c = Util3D.cos(a);
        int s = Util3D.sin(a);
        AffineTrans r = new AffineTrans();
        r.m00 = c;
        r.m01 = -s;
        r.m10 = s;
        r.m11 = c;
        multiply(this, r);
    }

    /** Rotation of `a` (4096 per revolution) about the axis `vec`. */
    public void rotationV(Vector3D vec, int a) {
        if (vec == null) {
            throw new NullPointerException("vec");
        }
        Vector3D n = new Vector3D(vec.x, vec.y, vec.z);
        n.normalize();
        int c = Util3D.cos(a);
        int s = Util3D.sin(a);
        int t = 4096 - c;
        AffineTrans r = new AffineTrans();
        /* Rodrigues: R = c*I + (1-c)*n*n^T + s*[n]x */
        r.m00 = c + fmul(fmul(n.x, n.x), t);
        r.m01 = fmul(fmul(n.x, n.y), t) - fmul(s, n.z);
        r.m02 = fmul(fmul(n.x, n.z), t) + fmul(s, n.y);
        r.m10 = fmul(fmul(n.y, n.x), t) + fmul(s, n.z);
        r.m11 = c + fmul(fmul(n.y, n.y), t);
        r.m12 = fmul(fmul(n.y, n.z), t) - fmul(s, n.x);
        r.m20 = fmul(fmul(n.z, n.x), t) - fmul(s, n.y);
        r.m21 = fmul(fmul(n.z, n.y), t) + fmul(s, n.x);
        r.m22 = c + fmul(fmul(n.z, n.z), t);
        multiply(this, r);
    }

    /** Builds the view matrix from eye position, look direction and up. */
    public void setViewTrans(Vector3D position, Vector3D look, Vector3D up) {
        if (position == null || look == null || up == null) {
            throw new NullPointerException();
        }
        Vector3D z = new Vector3D(look.x, look.y, look.z);
        if (z.x == 0 && z.y == 0 && z.z == 0) {
            throw new IllegalArgumentException("look is the zero vector");
        }
        z.normalize();
        Vector3D x = new Vector3D(z.x, z.y, z.z);
        x.cross(up);
        if (x.x == 0 && x.y == 0 && x.z == 0) {
            throw new IllegalArgumentException("look and up are parallel");
        }
        x.normalize();
        Vector3D y = new Vector3D(z.x, z.y, z.z);
        y.cross(x);
        y.normalize();

        m00 = x.x; m01 = x.y; m02 = x.z;
        m10 = y.x; m11 = y.y; m12 = y.z;
        m20 = z.x; m21 = z.y; m22 = z.z;
        m03 = -(mul(m00, position.x, m01, position.y, m02, position.z));
        m13 = -(mul(m10, position.x, m11, position.y, m12, position.z));
        m23 = -(mul(m20, position.x, m21, position.y, m22, position.z));
    }
}
