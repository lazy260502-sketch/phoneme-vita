/*
 * Light.java - VSCL 1.0 (see Graphics3D.java for the scope note).
 */
package com.vodafone.v10.graphics.j3d;

public class Light {

    public int dirX;
    public int dirY;
    public int dirZ;

    public Light() {
    }

    public void setDirection(Vector3D dir) {
        if (dir == null) {
            throw new NullPointerException("dir");
        }
        dirX = dir.x;
        dirY = dir.y;
        dirZ = dir.z;
    }

    public void setDirection(int x, int y, int z) {
        dirX = x;
        dirY = y;
        dirZ = z;
    }
}
