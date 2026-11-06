package com.siemens.mp.color_game;

import javax.microedition.lcdui.Canvas;
import javax.microedition.lcdui.Graphics;
import javax.microedition.lcdui.Image;

/**
 * Siemens color_game GameCanvas: a Canvas with an off-screen buffer and
 * polled key states (the MIDP 2.0 GameCanvas contract, in Siemens' package).
 * Differences from J2ME-Loader's version, on purpose:
 *  - the off-screen Graphics is created once and reused (Loader returns a new
 *    Graphics for the same image on every call, so state set by the game
 *    through getGraphics() would be lost between calls);
 *  - the buffer size comes from getWidth()/getHeight() (Loader uses fields of
 *    its own Canvas extension that do not exist here).
 * suppressKeyEvents is accepted but NOT acted on (same as Loader): Canvas
 * dispatches keyPressed/keyRepeated itself, so a subclass that overrides them
 * still receives the events. Only the polled state is recorded.
 */
public class GameCanvas extends Canvas {
    public static final int UP_PRESSED = 1 << Canvas.UP;
    public static final int DOWN_PRESSED = 1 << Canvas.DOWN;
    public static final int LEFT_PRESSED = 1 << Canvas.LEFT;
    public static final int RIGHT_PRESSED = 1 << Canvas.RIGHT;
    public static final int FIRE_PRESSED = 1 << Canvas.FIRE;
    public static final int GAME_A_PRESSED = 1 << Canvas.GAME_A;
    public static final int GAME_B_PRESSED = 1 << Canvas.GAME_B;
    public static final int GAME_C_PRESSED = 1 << Canvas.GAME_C;
    public static final int GAME_D_PRESSED = 1 << Canvas.GAME_D;

    private final Image image;
    private final Graphics imageGraphics;
    private int keyState = 0;

    protected GameCanvas(boolean suppressKeyEvents) {
        image = Image.createImage(getWidth(), getHeight());
        imageGraphics = image.getGraphics();
        /* start from a white buffer (MIDP 2.0 GameCanvas contract) */
        imageGraphics.setColor(0xFFFFFF);
        imageGraphics.fillRect(0, 0, image.getWidth(), image.getHeight());
        imageGraphics.setColor(0);
    }

    public void flushGraphics() {
        repaint();
        serviceRepaints();
    }

    public void flushGraphics(int x, int y, int width, int height) {
        repaint(x, y, width, height);
        serviceRepaints();
    }

    protected Graphics getGraphics() {
        return imageGraphics;
    }

    /** Returns the keys pressed since the last call, then clears them. */
    public int getKeyStates() {
        int k = keyState;
        keyState = 0;
        if (!isShown()) {
            return 0;
        }
        return k;
    }

    private void record(int keyCode) {
        switch (getGameAction(keyCode)) {
            case FIRE:   keyState |= FIRE_PRESSED; break;
            case UP:     keyState |= UP_PRESSED; break;
            case DOWN:   keyState |= DOWN_PRESSED; break;
            case LEFT:   keyState |= LEFT_PRESSED; break;
            case RIGHT:  keyState |= RIGHT_PRESSED; break;
            case GAME_A: keyState |= GAME_A_PRESSED; break;
            case GAME_B: keyState |= GAME_B_PRESSED; break;
            case GAME_C: keyState |= GAME_C_PRESSED; break;
            case GAME_D: keyState |= GAME_D_PRESSED; break;
            default: break;
        }
    }

    /* The polled state is recorded here; a subclass that overrides these
     * without calling super gets no polled state. */
    protected void keyPressed(int keyCode) {
        record(keyCode);
    }

    protected void keyRepeated(int keyCode) {
        record(keyCode);
    }

    public void paint(Graphics g) {
        g.drawImage(image, 0, 0, Graphics.TOP | Graphics.LEFT);
    }
}
