package com.motorola.funlight;

/** Motorola FunLight region (J2ME-Loader member list, Apache-2.0). */
public interface Region {
    int getColor();

    int getControl();

    int getID();

    void releaseControl();

    int setColor(byte red, byte green, byte blue);

    int setColor(int color);

    String toString();
}
