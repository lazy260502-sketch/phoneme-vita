/*
 * Graphics3DImpl.java - the port's Graphics3D (see Graphics3D.java for the
 * scope note).
 *
 * The specification does not say how a MIDlet gets a Graphics3D, so this class
 * offers the three shapes real implementations have used:
 *     Graphics3DImpl.getInstance()
 *     Graphics3DImpl.getInstance(Graphics g)     // binds the target too
 *     new Graphics3DImpl()  +  bindTarget(Graphics)
 * drawFigure() then draws a wireframe placeholder: the model format (Aplix M3D)
 * is not published, so the geometry cannot be read. The log line appears once
 * per process, not per frame.
 */
package com.vodafone.v10.graphics.j3d;

import javax.microedition.lcdui.Graphics;

public class Graphics3DImpl implements Graphics3D {

    private static Graphics3DImpl instance;
    private static boolean logged;

    private Graphics target;

    public Graphics3DImpl() {
    }

    public static synchronized Graphics3DImpl getInstance() {
        if (instance == null) {
            instance = new Graphics3DImpl();
        }
        return instance;
    }

    public static synchronized Graphics3DImpl getInstance(Graphics g) {
        Graphics3DImpl i = getInstance();
        i.bindTarget(g);
        return i;
    }

    /** Which Graphics drawFigure() renders into. Not in the specification. */
    public void bindTarget(Graphics g) {
        target = g;
    }

    public void drawFigure(Figure figure, int x, int y, FigureLayout layout,
                           Effect3D effect) {
        if (figure == null || layout == null || effect == null) {
            throw new NullPointerException();
        }
        if (!logged) {
            logged = true;
            System.out.println("[V10J3D] drawFigure: VSCL model format (Aplix"
                             + " M3D) is not supported; drawing a placeholder");
        }
        Graphics g = target;
        if (g == null) {
            return;
        }
        /* Placeholder: a small wireframe diamond at the layout centre, scaled
         * by layout.getScaleX/Y() (512 = 1:1 per the specification). */
        int s = (layout.getScaleX() > 0) ? layout.getScaleX() : 512;
        int r = (s * 16) / 512;
        if (r < 2) {
            r = 2;
        }
        int cx = x + layout.getCenterX();
        int cy = y + layout.getCenterY();
        g.drawLine(cx, cy - r, cx + r, cy);
        g.drawLine(cx + r, cy, cx, cy + r);
        g.drawLine(cx, cy + r, cx - r, cy);
        g.drawLine(cx - r, cy, cx, cy - r);
    }
}
