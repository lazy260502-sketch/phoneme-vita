/*
 * FigureLayout.java - VSCL 1.0 (see Graphics3D.java for the scope note).
 *
 * The javadoc gives the defaults: scale 512,512 (1:1) and the centre of the
 * real screen. This port cannot read the screen size from here, so the centre
 * defaults to 0,0 and drawFigure() adds its own x/y - a game that sets the
 * centre explicitly (the usual case) is unaffected.
 */
package com.vodafone.v10.graphics.j3d;

public class FigureLayout {

    private AffineTrans trans;
    private int scaleX = 512;
    private int scaleY = 512;
    private int centerX;
    private int centerY;

    public FigureLayout() {
    }

    public FigureLayout(AffineTrans trans, int x_scale, int y_scale,
                        int cx, int cy) {
        this.trans = trans;
        this.scaleX = x_scale;
        this.scaleY = y_scale;
        this.centerX = cx;
        this.centerY = cy;
    }

    public AffineTrans getAffineTrans() {
        return trans;
    }

    public void setAffineTrans(AffineTrans t) {
        trans = t;
    }

    public int getScaleX() {
        return scaleX;
    }

    public int getScaleY() {
        return scaleY;
    }

    public void setScale(int x_scale, int y_scale) {
        scaleX = x_scale;
        scaleY = y_scale;
    }

    public int getCenterX() {
        return centerX;
    }

    public int getCenterY() {
        return centerY;
    }

    public void setCenter(int cx, int cy) {
        centerX = cx;
        centerY = cy;
    }
}
