package com.motorola.funlight;

/**
 * Motorola FunLight (keypad LEDs). No LEDs here: control requests are
 * reported as QUEUED and ignored. Constants and signatures follow
 * J2ME-Loader (Apache-2.0).
 */
public class FunLight {
    public static int BLACK = 0x00000000;
    public static int BLANK = 0;
    public static int BLUE = 0x000000FF;
    public static int CYAN = 0x0000FFFF;
    public static int GREEN = 0x0000FF00;
    public static int IGNORED = 2;
    public static int MAGENTA = 0x00FF00FF;
    public static int OFF = 0x00000000;
    public static int ON = 0x00FFFFFF;
    public static int QUEUED = 1;
    public static int RED = 0x00FF0000;
    public static int SUCCESS = 0;
    public static int WHITE = 0x00FFFFFF;
    public static int YELLOW = 0x00FFFF00;

    public static int getControl() {
        return QUEUED;
    }

    public static Region getRegion(int id) {
        return new BlankRegion(id);
    }

    public static Region[] getRegions() {
        return null;
    }

    public static int[] getRegionsIDs() {
        return null;
    }

    public static void releaseControl() {
    }

    public static int setColor(byte red, byte green, byte blue) {
        return QUEUED;
    }

    public static int setColor(int color) {
        return QUEUED;
    }

    static class BlankRegion implements Region {
        private final int id;

        BlankRegion(int id) {
            this.id = id;
        }

        public int getColor() {
            return BLACK;
        }

        public int getControl() {
            return QUEUED;
        }

        public int getID() {
            return id;
        }

        public void releaseControl() {
        }

        public int setColor(byte red, byte green, byte blue) {
            return QUEUED;
        }

        public int setColor(int color) {
            return QUEUED;
        }

        public String toString() {
            return "BlankRegion[" + id + "]";
        }
    }
}
