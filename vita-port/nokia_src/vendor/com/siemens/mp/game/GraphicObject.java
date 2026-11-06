package com.siemens.mp.game;

/** Siemens API GraphicObject: base of Sprite and TiledBackground. */
public class GraphicObject extends com.siemens.mp.misc.NativeMem {
    private boolean visible;

    protected GraphicObject() {
        super();
        this.visible = true;
    }

    public boolean getVisible() {
        return this.visible;
    }

    public void setVisible(boolean visible) {
        this.visible = visible;
    }
}
