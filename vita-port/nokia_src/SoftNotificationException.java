package com.nokia.mid.ui;

/** Nokia UI API: soft notification failure (Apache-2.0, see DeviceControl). */
public class SoftNotificationException extends Exception {
    private int errorCode;

    protected SoftNotificationException() {
    }

    public SoftNotificationException(String info) {
        super(info);
    }

    public SoftNotificationException(String info, int errorCode) {
        super(info);
        this.errorCode = errorCode;
    }

    public String toString() {
        if (this.errorCode == 0) {
            return super.toString();
        }
        return super.toString() + " Native error: " + this.errorCode;
    }

    public int getErrorCode() {
        return this.errorCode;
    }
}
