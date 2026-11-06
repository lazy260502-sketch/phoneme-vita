/*
 * Util3D.java - VSCL 1.0 (see Graphics3D.java for the scope note).
 *
 * Angle unit: 4096 per revolution (the javadoc says so for sin/cos), so one
 * quarter turn is 1024.
 */
package com.vodafone.v10.graphics.j3d;

public class Util3D {

    private Util3D() {
    }

    public static final int sqrt(int x) {
        if (x < 0) {
            throw new IllegalArgumentException("negative");
        }
        return (int) Math.sqrt((double) x);
    }

    public static final int sin(int a) {
        double rad = ((double) a) * 2.0 * Math.PI / 4096.0;
        return (int) (Math.sin(rad) * 4096.0);
    }

    public static final int cos(int a) {
        double rad = ((double) a) * 2.0 * Math.PI / 4096.0;
        return (int) (Math.cos(rad) * 4096.0);
    }
}
