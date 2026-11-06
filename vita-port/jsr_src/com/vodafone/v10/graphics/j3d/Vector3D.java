/*
 * Vector3D.java - VSCL 1.0 (see Graphics3D.java for the scope note).
 *
 * Fixed point with 4096 = 1.0, like the rest of the package.
 */
package com.vodafone.v10.graphics.j3d;

public class Vector3D {

    public int x;
    public int y;
    public int z;

    public Vector3D() {
    }

    public Vector3D(int x, int y, int z) {
        this.x = x;
        this.y = y;
        this.z = z;
    }

    public void set(int x, int y, int z) {
        this.x = x;
        this.y = y;
        this.z = z;
    }

    public void set(Vector3D v) {
        if (v == null) {
            throw new NullPointerException();
        }
        set(v.x, v.y, v.z);
    }

    public void add(Vector3D v) {
        if (v == null) {
            throw new NullPointerException();
        }
        x += v.x;
        y += v.y;
        z += v.z;
    }

    public void sub(Vector3D v) {
        if (v == null) {
            throw new NullPointerException();
        }
        x -= v.x;
        y -= v.y;
        z -= v.z;
    }

    /** this = this x v (cross product). */
    public void cross(Vector3D v) {
        if (v == null) {
            throw new NullPointerException();
        }
        int nx = (int) (((long) y * v.z - (long) z * v.y) >> 12);
        int ny = (int) (((long) z * v.x - (long) x * v.z) >> 12);
        int nz = (int) (((long) x * v.y - (long) y * v.x) >> 12);
        x = nx;
        y = ny;
        z = nz;
    }

    public int dot(Vector3D v) {
        if (v == null) {
            throw new NullPointerException();
        }
        return (int) (((long) x * v.x + (long) y * v.y + (long) z * v.z) >> 12);
    }

    public int length() {
        long l = (long) x * x + (long) y * y + (long) z * z;
        if (l <= 0) {
            return 0;
        }
        /* sqrt on a 4096-scaled square: the result is already 4096-scaled. */
        return (int) Math.sqrt((double) l);
    }

    public void normalize() {
        int len = length();
        if (len == 0) {
            return;
        }
        x = (int) (((long) x << 12) / len);
        y = (int) (((long) y << 12) / len);
        z = (int) (((long) z << 12) / len);
    }

    /** Scalar multiply by k/4096. */
    public void mul(int k) {
        x = (int) (((long) x * k) >> 12);
        y = (int) (((long) y * k) >> 12);
        z = (int) (((long) z * k) >> 12);
    }
}
