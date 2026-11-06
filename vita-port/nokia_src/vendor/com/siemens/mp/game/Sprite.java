package com.siemens.mp.game;

import javax.microedition.lcdui.Graphics;
import javax.microedition.lcdui.Image;

/**
 * Siemens API game.Sprite (monochrome-era sprite: N frames stacked vertically
 * in one image, optional mask, rectangle collision). Behaviour follows
 * J2ME-Loader's class (Apache-2.0 / MicroEmulator Siemens API, Copyright (C)
 * 2003 Markus Heberling, used under Apache-2.0). Loader's version builds each
 * frame with a non-standard Image.createImage(w, h, argb); here a frame is
 * cut out of the (already alpha-carrying) source with createRGBImage.
 */
public class Sprite extends GraphicObject {
    private Image[] pixels;
    private int x;
    private int y;
    private int frame;
    private int collx, colly, collw, collh;

    public Sprite(byte[] pixels, int pixelOffset, int width, int height,
                  byte[] mask, int maskOffset, int numFrames) {
        this(com.siemens.mp.ui.Image.createImageFromBitmap(
                 pixels, mask, width, height * numFrames),
             null, numFrames);
    }

    public Sprite(ExtendedImage pixels, ExtendedImage mask, int numFrames) {
        this(pixels.getImage(), mask == null ? null : mask.getImage(),
             numFrames);
    }

    public Sprite(Image pixels, Image mask, int numFrames) {
        if (numFrames <= 0) {
            throw new IllegalArgumentException("numFrames " + numFrames);
        }
        if (mask != null) {
            pixels = com.siemens.mp.ui.Image.createTransparentImageFromMask(
                pixels, mask);
        }
        int w = pixels.getWidth();
        int fh = pixels.getHeight() / numFrames;
        this.pixels = new Image[numFrames];
        int[] buf = new int[w * fh];
        for (int i = 0; i < numFrames; i++) {
            pixels.getRGB(buf, 0, w, 0, i * fh, w, fh);
            this.pixels[i] = Image.createRGBImage(buf, w, fh, true);
            /* createRGBImage copies the data, so buf can be reused */
        }
        collx = 0;
        colly = 0;
        collw = w;
        collh = fh;
    }

    public int getFrame() {
        return frame;
    }

    public int getXPosition() {
        return x;
    }

    public int getYPosition() {
        return y;
    }

    public boolean isCollidingWith(Sprite other) {
        int left = x + collx;
        int right = x + collx + collw;
        int top = y + colly;
        int bottom = y + colly + collh;
        int oLeft = other.x + other.collx;
        int oRight = other.x + other.collx + other.collw;
        int oTop = other.y + other.colly;
        int oBottom = other.y + other.colly + other.collh;
        return left < oRight && oLeft < right && top < oBottom && oTop < bottom;
    }

    public boolean isCollidingWithPos(int xpos, int ypos) {
        int left = x + collx;
        int right = x + collx + collw;
        int top = y + colly;
        int bottom = y + colly + collh;
        return xpos >= left && xpos < right && ypos >= top && ypos < bottom;
    }

    public void setCollisionRectangle(int rx, int ry, int width, int height) {
        collx = rx;
        colly = ry;
        collw = width;
        collh = height;
    }

    public void setFrame(int framenumber) {
        if (framenumber < 0 || framenumber >= pixels.length) {
            throw new IndexOutOfBoundsException();
        }
        frame = framenumber;
    }

    public void setPosition(int x, int y) {
        this.x = x;
        this.y = y;
    }

    protected void paint(Graphics g) {
        g.drawImage(pixels[frame], x, y, Graphics.TOP | Graphics.LEFT);
    }

    /* GraphicObjectManager lives in the same package but is not a subclass */
    void paintTo(Graphics g) {
        paint(g);
    }
}
