package com.nokia.mid.ui;

import javax.microedition.lcdui.Canvas;

/**
 * Minimal stub of the Nokia UI FullCanvas: a Canvas that is full-screen from
 * construction on (as on a real Nokia). Without setFullScreenMode(true) the
 * Chameleon window keeps its 18 px soft-button bar (blue, SOFTBTN_COLOR_BG)
 * and its SoftButtonLayer swallows SOFT1/SOFT2 before the game sees them
 * (log117: bottom blue strip, no response on soft-key-only prompts).
 */
public abstract class FullCanvas extends Canvas {
    /* Nokia key constants (compile-time constants, inlined into games). */
    public static final int KEY_SOFTKEY1 = -6;
    public static final int KEY_SOFTKEY2 = -7;
    public static final int KEY_SOFTKEY3 = -5;
    public static final int KEY_UP_ARROW = -1;
    public static final int KEY_DOWN_ARROW = -2;
    public static final int KEY_LEFT_ARROW = -3;
    public static final int KEY_RIGHT_ARROW = -4;
    public static final int KEY_SEND = -10;
    public static final int KEY_END = -11;

    protected FullCanvas() {
        super();
        setFullScreenMode(true);
    }
}
