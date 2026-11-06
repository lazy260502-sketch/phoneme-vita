package com.mot.iden.multimedia;

/**
 * Motorola iDEN Lighting: constants and signatures follow J2ME-Loader
 * (Apache-2.0). No controllable lights here; calls are accepted and ignored.
 */
public class Lighting {
    public static final int LIGHT_CALL_INDICATOR = 4;
    public static final int LIGHT_DISPLAY = 1;
    public static final int LIGHT_KEYPAD = 2;
    public static final int LIGHT_STATE_BLUE = 4;
    public static final int LIGHT_STATE_CYAN = 5;
    public static final int LIGHT_STATE_GREEN = 1;
    public static final int LIGHT_STATE_MAGENTA = 6;
    public static final int LIGHT_STATE_OFF = 0;
    public static final int LIGHT_STATE_ON = 255;
    public static final int LIGHT_STATE_RED = 2;
    public static final int LIGHT_STATE_WHITE = 7;
    public static final int LIGHT_STATE_YELLOW = 3;
    public static final int LIGHT_STATUS = 3;
    public static final int STATUS_AMBER = 3;
    public static final int STATUS_GREEN = 1;
    public static final int STATUS_OFF = 0;
    public static final int STATUS_RED = 2;

    public static void setLighting(int light, int state)
        throws IllegalStateException {
    }

    public static void backlightOn() {
    }

    public static void backlightOff() {
    }

    public static void keypadLightOn() {
    }

    public static void keypadLightOff() {
    }

    public static void setStatusLight(int color) {
    }

    public static int getPhotoSensorLevel() {
        return -100;
    }

    public static void javaOverRideLighting(boolean state) {
    }
}
