package com.nokia.mid.ui;

import javax.microedition.lcdui.Canvas;
import javax.microedition.lcdui.Graphics;

/** Minimal stub of the Nokia UI FullCanvas: a plain full-screen Canvas. */
public class FullCanvas extends Canvas {
    public void paint(Graphics g) {
        g.setColor(0);
        g.fillRect(0, 0, getWidth(), getHeight());
    }
}
