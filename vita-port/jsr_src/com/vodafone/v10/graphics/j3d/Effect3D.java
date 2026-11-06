/*
 * Effect3D.java - VSCL 1.0 (see Graphics3D.java for the scope note).
 *
 * Plain state holder: the port's placeholder renderer ignores it, but a game
 * that sets up lights before drawing keeps working.
 */
package com.vodafone.v10.graphics.j3d;

public class Effect3D {

    public boolean isTransparent;
    public int shading;
    public Light light;
    public Light light2;

    public Effect3D() {
    }

    public void setLight(Light light1, Light light2) {
        this.light = light1;
        this.light2 = light2;
    }
}
