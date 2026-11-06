package com.nokia.mid.ui;

/**
 * Nokia UI API SoftNotification (idle-screen notifications). The Vita
 * launcher has no notification area, so a notification is kept in memory:
 * every call succeeds and nothing is shown - a game that posts a reminder
 * must not fail. Member list follows J2ME-Loader (Apache-2.0).
 */
public abstract class SoftNotification {
    public static SoftNotification newInstance(int notificationId) {
        return new Impl(notificationId);
    }

    public static SoftNotification newInstance() {
        return new Impl(0);
    }

    public abstract int getId();

    public abstract void post() throws SoftNotificationException;

    public abstract void remove() throws SoftNotificationException;

    public abstract void setListener(SoftNotificationListener listener);

    public abstract void setText(String text, String groupText)
        throws SoftNotificationException;

    public abstract void setSoftkeyLabels(String softkey1Label,
                                          String softkey2Label)
        throws SoftNotificationException;

    public abstract void setImage(byte[] image)
        throws SoftNotificationException;

    private static final class Impl extends SoftNotification {
        private final int id;
        private SoftNotificationListener listener;

        Impl(int id) {
            this.id = id;
        }

        public int getId() {
            return id;
        }

        public void post() {
        }

        public void remove() {
        }

        public void setListener(SoftNotificationListener l) {
            listener = l;
        }

        public void setText(String text, String groupText) {
        }

        public void setSoftkeyLabels(String a, String b) {
        }

        public void setImage(byte[] image) {
        }
    }
}
