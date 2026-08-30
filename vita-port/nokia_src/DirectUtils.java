package com.nokia.mid.ui;

import javax.microedition.lcdui.Image;
import javax.microedition.lcdui.Graphics;

/** Minimal stub of Nokia DirectUtils. */
public final class DirectUtils {
    private DirectUtils() {}

    public static Image createImage(int width, int height) {
        return Image.createImage(width, height);
    }

    public static Image createImage(byte[] imageData, int imageOffset,
                                    int imageLength) {
        try {
            return Image.createImage(imageData, imageOffset, imageLength);
        } catch (Exception e) {
            return null;
        }
    }

    public static Graphics getGraphics(Image img) {
        return img.getGraphics();
    }
}
