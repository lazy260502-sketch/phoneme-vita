package com.samsung.util;

/**
 * Samsung API LCDLight (backlight control). The backlight is not
 * controllable here; calls are accepted and ignored. Member list follows
 * J2ME-Loader (Apache-2.0).
 */
public class LCDLight {
    private LCDLight() {}

    public static void on(int duration) {
    }

    public static void off() {
    }

    public static boolean isSupported() {
        return false;
    }
}
