package com.nokia.mid.ui;

/**
 * Nokia UI API DeviceControl (vibra, lights, inactivity timer). Member list
 * and argument checks follow J2ME-Loader's class of the same name (Apache-2.0
 * / MicroEmulator Nokia API, Copyright (C) 2003 Markus Heberling, used under
 * Apache-2.0). This device has no vibrator or controllable backlight, so the
 * calls are accepted and ignored - a game that merely asks for a rumble or a
 * light flash must not fail.
 */
public class DeviceControl {
    private DeviceControl() {}

    public static int getUserInactivityTime() {
        return 0;
    }

    public static void resetUserInactivityTime() {
    }

    public static void startVibra(int frequency, long duration) {
        if (frequency < 0 || frequency > 100) {
            throw new IllegalArgumentException();
        }
    }

    public static void stopVibra() {
    }

    public static void setLights(int num, int level) {
    }

    public static void flashLights(long duration) {
    }
}
