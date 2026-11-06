package com.nokia.mid.ui;

/** Nokia UI API: soft notification callbacks (Apache-2.0, see DeviceControl). */
public interface SoftNotificationListener {
    void notificationSelected(SoftNotification notification);

    void notificationDismissed(SoftNotification notification);
}
