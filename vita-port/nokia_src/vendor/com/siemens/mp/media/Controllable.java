package com.siemens.mp.media;

/** Siemens API media.Controllable (J2ME-Loader list, Apache-2.0). */
public interface Controllable {
    Control[] getControls();

    Control getControl(String s);
}
