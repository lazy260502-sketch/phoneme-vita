package com.siemens.mp.color_game;

import javax.microedition.lcdui.Graphics;

/**
 * Siemens color_game Layer. The Siemens color_game package (Siemens C65/SL65
 * era) is an early copy of the MIDP 2.0 game API with its own, unrelated
 * class hierarchy: a game that uses it expects com.siemens.mp.color_game.Layer
 * etc., and cannot be handed javax.microedition.lcdui.game.Layer (whose
 * constructor is package-private and cannot be subclassed from here).
 *
 * The classes in this package are written against the public MIDP 2.0 game
 * API behaviour (and cross-checked against J2ME-Loader's com.siemens.mp
 * color_game, Apache-2.0); each one wraps the matching javax.microedition.lcdui.
 * game class instead of duplicating its collision/tiling code.
 */
public abstract class Layer {
    int x;
    int y;
    int width;
    int height;
    boolean visible = true;

    Layer(int width, int height) {
        setWidth(width);
        setHeight(height);
    }

    public void setPosition(int x, int y) {
        this.x = x;
        this.y = y;
    }

    public void move(int dx, int dy) {
        x += dx;
        y += dy;
    }

    public final int getX() {
        return x;
    }

    public final int getY() {
        return y;
    }

    public final int getWidth() {
        return width;
    }

    public final int getHeight() {
        return height;
    }

    public void setVisible(boolean visible) {
        this.visible = visible;
    }

    public final boolean isVisible() {
        return visible;
    }

    public abstract void paint(Graphics g);

    public void setWidth(int width) {
        if (width < 0) {
            throw new IllegalArgumentException();
        }
        this.width = width;
    }

    public void setHeight(int height) {
        if (height < 0) {
            throw new IllegalArgumentException();
        }
        this.height = height;
    }
}
