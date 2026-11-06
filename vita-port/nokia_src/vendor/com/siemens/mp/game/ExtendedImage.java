package com.siemens.mp.game;

import javax.microedition.lcdui.Canvas;
import javax.microedition.lcdui.Display;
import javax.microedition.lcdui.Displayable;
import javax.microedition.lcdui.Graphics;
import javax.microedition.lcdui.Image;

/**
 * Siemens API game.ExtendedImage: an lcdui Image whose width is a multiple of
 * 8, with byte-per-pixel access. Behaviour follows J2ME-Loader's class
 * (Apache-2.0 / MicroEmulator Siemens API, Copyright (C) 2003 Markus
 * Heberling, used under Apache-2.0).
 *
 * Not implemented (same as Loader): getPixel, getPixelBytes and setPixel are
 * no-ops / return 0 - an lcdui Image here has no per-pixel read/write.
 * blitToScreen needs a Canvas.flushBuffer() extension that this MIDP lacks, so
 * it draws through the current Canvas' repaint instead of writing directly.
 */
public class ExtendedImage extends com.siemens.mp.misc.NativeMem {
    private Image image;

    public ExtendedImage(Image image) {
        if (image == null || image.getWidth() % 8 != 0) {
            throw new IllegalArgumentException(
                "ExtendedImage: width is not divisible by 8");
        }
        this.image = image;
    }

    /** Not supported here: the screen cannot be written outside paint(). */
    public void blitToScreen(int x, int y) {
        /* Without Canvas.flushBuffer there is no way to draw outside the
         * paint callback; a game that depends on it will show nothing for
         * this call. Logged once so it is visible in vm_output.log. */
        if (!warned) {
            warned = true;
            System.out.println("[VENDOR] siemens ExtendedImage.blitToScreen "
                               + "is not supported");
        }
    }

    private static boolean warned;

    public void clear(byte color) {
        int c = (color == 0) ? 0x00FFFFFF : 0x00000000;
        Graphics g = image.getGraphics();
        g.setColor(c);
        g.fillRect(0, 0, image.getWidth(), image.getHeight());
    }

    public Image getImage() {
        return image;
    }

    public int getPixel(int x, int y) {
        return 0;
    }

    public void getPixelBytes(byte[] pixels, int x, int y, int width,
                              int height) {
    }

    public void setPixel(int x, int y, byte color) {
    }

    public void setPixels(byte[] pixels, int x, int y, int width, int height) {
        Image img = com.siemens.mp.ui.Image.createImageFromBitmap(
            pixels, width, height);
        if (img != null) {
            image.getGraphics().drawImage(img, x, y,
                                          Graphics.TOP | Graphics.LEFT);
        }
    }
}
