package com.mot.iden.multimedia;

/**
 * Motorola iDEN Vibrator. Member list and argument checks follow
 * J2ME-Loader (Apache-2.0); no vibrator here, so valid calls are ignored.
 */
public class Vibrator extends Thread {
    public static final int MAX_VIBRATE_TIME = 500;
    public static final int MIN_PAUSE_TIME = 50;

    public static void vibratorOn() {
    }

    public static void vibratorOff() {
    }

    public static void vibrateFor(int timeInMs)
        throws IllegalArgumentException {
        if (timeInMs <= 0 || timeInMs > MAX_VIBRATE_TIME) {
            throw new IllegalArgumentException();
        }
    }

    public static void vibratePeriodicaly(int timeOnInMs, int timeOffInMs)
        throws IllegalArgumentException {
        if (timeOnInMs <= 0 || timeOnInMs > MAX_VIBRATE_TIME
                || timeOffInMs < MIN_PAUSE_TIME) {
            throw new IllegalArgumentException();
        }
    }

    public static void vibratePeriodicaly(int timeInMs)
        throws IllegalArgumentException {
        if (timeInMs < MIN_PAUSE_TIME || timeInMs > MAX_VIBRATE_TIME) {
            throw new IllegalArgumentException();
        }
    }

    public void run() {
    }
}
