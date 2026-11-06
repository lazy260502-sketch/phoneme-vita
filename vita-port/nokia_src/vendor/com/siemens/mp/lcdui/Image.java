package com.siemens.mp.lcdui;

import java.io.IOException;

/**
 * Siemens API lcdui.Image (extends ui.Image). Loader's setPixelColor() writes
 * into an Android Bitmap; an lcdui Image here has no per-pixel write, so it
 * is implemented by drawing a 1x1 rectangle through the image's Graphics,
 * which works for mutable images and throws IllegalArgumentException (per the
 * Siemens contract for an unsuitable image) otherwise.
 */
public class Image extends com.siemens.mp.ui.Image {
    public static javax.microedition.lcdui.Image createImageFromFile(
            String resname, boolean scaleToFullScreen) throws IOException {
        return javax.microedition.lcdui.Image.createImage(resname);
    }

    public static void setPixelColor(javax.microedition.lcdui.Image image,
                                     int x, int y, int color)
        throws IllegalArgumentException {
        if (image == null || !image.isMutable()) {
            throw new IllegalArgumentException("image is not mutable");
        }
        if (x < 0 || y < 0 || x >= image.getWidth() || y >= image.getHeight()) {
            throw new IllegalArgumentException("pixel outside image");
        }
        javax.microedition.lcdui.Graphics g = image.getGraphics();
        g.setColor(color & 0xFFFFFF);
        g.fillRect(x, y, 1, 1);
    }
}
