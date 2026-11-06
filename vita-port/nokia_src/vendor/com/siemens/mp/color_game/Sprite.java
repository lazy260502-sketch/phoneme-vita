package com.siemens.mp.color_game;

import javax.microedition.lcdui.Graphics;
import javax.microedition.lcdui.Image;

/**
 * Siemens color_game Sprite: wraps javax.microedition.lcdui.game.Sprite so
 * frame handling, collision (rectangle and pixel level) and drawing are the
 * standard, already-tested implementation. Only the position/visibility is
 * kept here (it belongs to this package's own Layer) and copied onto the
 * wrapped sprite before each operation.
 *
 * The Siemens class has no transform and no reference pixel, so only the
 * members J2ME-Loader's version exposes are provided
 * (setCollisionRectangle instead of defineCollisionRectangle).
 */
public class Sprite extends Layer {
    private javax.microedition.lcdui.game.Sprite s;

    public Sprite(Image image) {
        super(image.getWidth(), image.getHeight());
        s = new javax.microedition.lcdui.game.Sprite(image);
    }

    public Sprite(Image image, int frameWidth, int frameHeight) {
        super(frameWidth, frameHeight);
        /* the standard class throws IllegalArgumentException for frame sizes
         * that do not divide the image - the same contract */
        s = new javax.microedition.lcdui.game.Sprite(image, frameWidth,
                                                    frameHeight);
    }

    public Sprite(Sprite other) {
        super(other.width, other.height);
        s = new javax.microedition.lcdui.game.Sprite(other.s);
        x = other.x;
        y = other.y;
        visible = other.visible;
    }

    javax.microedition.lcdui.game.Sprite impl() {
        s.setPosition(x, y);
        s.setVisible(visible);
        return s;
    }

    public void setFrame(int sequenceIndex) {
        s.setFrame(sequenceIndex);
    }

    public final int getFrame() {
        return s.getFrame();
    }

    public int getRawFrameCount() {
        return s.getRawFrameCount();
    }

    public int getFrameSequenceLength() {
        return s.getFrameSequenceLength();
    }

    public void nextFrame() {
        s.nextFrame();
    }

    public void prevFrame() {
        s.prevFrame();
    }

    public final void paint(Graphics g) {
        if (g == null) {
            throw new NullPointerException();
        }
        if (visible) {
            impl().paint(g);
        }
    }

    public void setFrameSequence(int[] sequence) {
        s.setFrameSequence(sequence);
    }

    public void setImage(Image img, int frameWidth, int frameHeight) {
        s.setImage(img, frameWidth, frameHeight);
        width = s.getWidth();
        height = s.getHeight();
    }

    public final boolean collidesWith(Sprite other, boolean pixelLevel) {
        return impl().collidesWith(other.impl(), pixelLevel);
    }

    public final boolean collidesWith(TiledLayer t, boolean pixelLevel) {
        return impl().collidesWith(t.impl(), pixelLevel);
    }

    public final boolean collidesWith(Image image, int imageX, int imageY,
                                      boolean pixelLevel) {
        return impl().collidesWith(image, imageX, imageY, pixelLevel);
    }

    public void setCollisionRectangle(int rx, int ry, int rw, int rh) {
        s.defineCollisionRectangle(rx, ry, rw, rh);
    }
}
