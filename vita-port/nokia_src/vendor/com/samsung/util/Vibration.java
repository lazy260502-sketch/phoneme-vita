package com.samsung.util;

/**
 * Samsung API Vibration. Member list follows J2ME-Loader (Apache-2.0). The
 * device has no vibrator: calls are accepted and ignored, and
 * isSupported() says so, so a game that checks first can skip the effect.
 */
public class Vibration {
    private Vibration() {}

    public static void start(int duration, int strength) {
        if (duration < 0 || strength < 0 || strength > 5) {
            throw new IllegalArgumentException();
        }
    }

    public static void stop() {
    }

    public static boolean isSupported() {
        return false;
    }
}
