package com.siemens.mp.game;

import java.util.Vector;

import javax.microedition.lcdui.Graphics;
import javax.microedition.lcdui.Image;

/**
 * Siemens API game.GraphicObjectManager: an ordered list of sprites and tiled
 * backgrounds painted into an Image. Behaviour follows J2ME-Loader's class
 * (Apache-2.0 / MicroEmulator Siemens API, Copyright (C) 2003 Markus
 * Heberling, used under Apache-2.0). Loader paints through a non-standard
 * Image.getSingleGraphics(); here the Image's Graphics is taken once per
 * paint() call.
 */
public class GraphicObjectManager extends com.siemens.mp.misc.NativeMem {
    private Vector v = new Vector();

    public void addObject(GraphicObject gobject) {
        v.addElement(gobject);
    }

    public static byte[] createTextureBits(int width, int height,
                                           byte[] texture) {
        return null;
    }

    public void deleteObject(GraphicObject gobject) {
        v.removeElement(gobject);
    }

    public void deleteObject(int position) {
        v.removeElementAt(position);
    }

    public GraphicObject getObjectAt(int index) {
        return (GraphicObject) v.elementAt(index);
    }

    public int getObjectPosition(GraphicObject gobject) {
        return v.indexOf(gobject);
    }

    public void insertObject(GraphicObject gobject, int position) {
        v.insertElementAt(gobject, position);
    }

    public void paint(ExtendedImage eimage, int x, int y) {
        paint(eimage.getImage(), x, y);
    }

    /**
     * Paints all visible objects into image. x and y are the drawing offset.
     * NOTE: J2ME-Loader ignores x and y here (paints at 0,0). The offset is
     * applied because that is what the parameters are for, but this differs
     * from Loader and is UNVERIFIED against a real Siemens device: if a game
     * passes non-zero x/y and its sprites land in the wrong place, drop the
     * two translate() calls below.
     */
    public void paint(Image image, int x, int y) {
        Graphics g = image.getGraphics();
        g.translate(x, y);
        for (int i = 0; i < v.size(); i++) {
            GraphicObject go = (GraphicObject) v.elementAt(i);
            if (!go.getVisible()) {
                continue;
            }
            if (go instanceof Sprite) {
                ((Sprite) go).paintTo(g);
            } else if (go instanceof TiledBackground) {
                ((TiledBackground) go).paintTo(g);
            }
        }
        g.translate(-x, -y);
    }
}
