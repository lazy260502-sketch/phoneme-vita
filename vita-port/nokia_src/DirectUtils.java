package com.nokia.mid.ui;

import javax.microedition.lcdui.Displayable;
import javax.microedition.lcdui.Font;
import javax.microedition.lcdui.Graphics;
import javax.microedition.lcdui.Image;

/**
 * Nokia UI API DirectUtils: converts lcdui classes to Nokia UI classes and
 * creates images with a defined ARGB fill. Member list follows J2ME-Loader's
 * class of the same name (Apache-2.0; derived from MicroEmulator's Nokia API,
 * Copyright (C) 2003 Markus Heberling, dual LGPL-2.1 / Apache-2.0, used here
 * under Apache-2.0), re-implemented on this tree's CLDC/MIDP classes.
 */
public final class DirectUtils {
    /* S40v6 getFont(int) identifiers */
    private static final int SMALL_FONT = 1;
    private static final int MEDIUM_FONT = 2;
    private static final int LARGE_FONT = 3;
    private static final int MEDIUM_BOLD_FONT = 4;

    private DirectUtils() {}

    /**
     * Wraps a Graphics. The result refers to the SAME graphics context, so
     * state changed through either object is visible through the other.
     * Games call this for every sprite they draw, so the last wrapper is
     * reused while the Graphics stays the same (no per-sprite garbage).
     */
    public static DirectGraphics getDirectGraphics(Graphics g) {
        if (g == null) {
            throw new NullPointerException();
        }
        synchronized (DirectUtils.class) {
            DirectGraphicsImpl d = last;
            if (d == null || d.g != g) {
                d = new DirectGraphicsImpl(g);
                Image t = lastImageFor(g);
                d.target = t;
                last = d;
            }
            return d;
        }
    }

    private static DirectGraphicsImpl last;
    /* Graphics obtained through DirectUtils.getGraphics()/createImage():
     * remembered so getPixels() can read the backing Image. */
    private static Graphics knownG;
    private static Image knownImg;

    private static Image lastImageFor(Graphics g) {
        return (g == knownG) ? knownImg : null;
    }

    private static void remember(Graphics g, Image img) {
        synchronized (DirectUtils.class) {
            knownG = g;
            knownImg = img;
            last = null;
        }
    }

    /** Non-standard helper kept from the earlier stub. */
    public static Graphics getGraphics(Image img) {
        Graphics g = img.getGraphics();
        remember(g, img);
        return g;
    }

    /** A mutable image decoded from encoded data (e.g. PNG). */
    public static Image createImage(byte[] imageData, int imageOffset,
                                    int imageLength) {
        Image src = Image.createImage(imageData, imageOffset, imageLength);
        if (src.isMutable()) {
            return src;
        }
        /* Image.createImage(byte[]) is immutable on MIDP: copy it into a
         * mutable image of the same size. */
        int w = src.getWidth();
        int h = src.getHeight();
        Image dst = Image.createImage(w, h);
        Graphics g = dst.getGraphics();
        g.drawImage(src, 0, 0, Graphics.TOP | Graphics.LEFT);
        remember(g, dst);
        return dst;
    }

    /**
     * A mutable w x h image with every pixel set to the given ARGB colour.
     * Limitation: the MIDP mutable image here has no alpha channel, so the
     * alpha byte is ignored except for alpha == 0, which is filled with
     * white (not transparent).
     */
    public static Image createImage(int width, int height, int argb) {
        Image img = Image.createImage(width, height);
        Graphics g = img.getGraphics();
        g.setColor(((argb >>> 24) == 0) ? 0xFFFFFF : (argb & 0xFFFFFF));
        g.fillRect(0, 0, width, height);
        remember(g, img);
        return img;
    }

    /** Pre-existing helper of the earlier stub (not part of the Nokia API). */
    public static Image createImage(int width, int height) {
        return createImage(width, height, 0xFFFFFFFF);
    }

    /* S40v6 */
    public static Font getFont(int identifier) {
        switch (identifier) {
            case SMALL_FONT:
                return Font.getFont(Font.FACE_SYSTEM, Font.STYLE_PLAIN,
                                    Font.SIZE_SMALL);
            case MEDIUM_FONT:
                return Font.getFont(Font.FACE_SYSTEM, Font.STYLE_PLAIN,
                                    Font.SIZE_MEDIUM);
            case LARGE_FONT:
                return Font.getFont(Font.FACE_SYSTEM, Font.STYLE_PLAIN,
                                    Font.SIZE_LARGE);
            case MEDIUM_BOLD_FONT:
                return Font.getFont(Font.FACE_SYSTEM, Font.STYLE_BOLD,
                                    Font.SIZE_MEDIUM);
            default:
                return Font.getDefaultFont();
        }
    }

    /* S40 free-size font: this MIDP has no arbitrary pixel heights, so the
     * closest standard size is used. */
    public static Font getFont(int face, int style, int height) {
        int size = (height <= 10) ? Font.SIZE_SMALL
                 : (height <= 16) ? Font.SIZE_MEDIUM : Font.SIZE_LARGE;
        return Font.getFont(face, style, size);
    }

    /* S40 header bar: not supported, reported as such. */
    public static boolean setHeader(Displayable displayable,
                                    String headerText, Image headerImage,
                                    int headerTextColor, int headerBgColor,
                                    int headerDividerColor) {
        return false;
    }
}
