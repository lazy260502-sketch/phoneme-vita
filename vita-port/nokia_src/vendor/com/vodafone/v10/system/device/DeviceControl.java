package com.vodafone.v10.system.device;

/**
 * Vodafone v10 system.device.DeviceControl. Constants, signatures and
 * exceptions follow J2ME-Loader's class (Apache-2.0). Differences:
 *  - getDeviceState(KEY_STATE) returns 0 and logs once: Loader reads the
 *    virtual keyboard bit mask, which has no Java-side equivalent here.
 *  - setDeviceActive(VIBRATION, ...) is accepted and ignored (no vibrator).
 */
public class DeviceControl {
    public static final int BATTERY = 1;
    public static final int FIELD_INTENSITY = 2;
    public static final int KEY_STATE = 3;
    public static final int VIBRATION = 4;
    public static final int BACK_LIGHT = 5;
    public static final int EIGHT_DIRECTIONS = 6;

    private static DeviceControl instance;
    private static boolean keyStateWarned;

    public static synchronized DeviceControl getDefaultDeviceControl() {
        if (instance == null) {
            instance = new DeviceControl();
        }
        return instance;
    }

    public static void setMailListener(MailListener listener) {
    }

    public static void setRingStateListener(RingStateListener listener) {
    }

    public static void setScheduledAlarmListener(
            ScheduledAlarmListener listener) {
    }

    public static void setTelephonyListener(TelephonyListener listener) {
    }

    public void blink(int lighting, int extinction, int repeat) {
    }

    public int getDeviceState(int deviceNo) {
        switch (deviceNo) {
            case BATTERY:
            case FIELD_INTENSITY:
                return 100;
            case KEY_STATE:
                if (!keyStateWarned) {
                    keyStateWarned = true;
                    System.out.println("[VENDOR] vodafone DeviceControl."
                                       + "getDeviceState(KEY_STATE) is not "
                                       + "supported, returns 0");
                }
                return 0;
            default:
                throw new IllegalStateException();
        }
    }

    public boolean getKeyRepeatState(int key) {
        return false;
    }

    public boolean isDeviceActive(int deviceNo) {
        return false;
    }

    public boolean setDeviceActive(int deviceNo, boolean active) {
        switch (deviceNo) {
            case BACK_LIGHT:
            case EIGHT_DIRECTIONS:
            case VIBRATION:
                break;
            default:
                throw new IllegalStateException();
        }
        return true;
    }

    public boolean setKeyRepeatState(int key, boolean state) {
        return false;
    }
}
