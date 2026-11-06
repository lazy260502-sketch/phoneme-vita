package com.samsung.util;

/**
 * Samsung API SMS. Same policy as J2ME-Loader: isSupported() reports true and
 * send() succeeds without sending anything. Some games refuse to run (or lock
 * content) unless SMS "works"; the user decided to let them through. The
 * request is written to the VM log so it is visible what a game tried to send.
 */
public class SMS {
    private SMS() {}

    public static boolean isSupported() {
        return true;
    }

    public static void send(SM sm) {
        if (sm == null) {
            throw new NullPointerException();
        }
        System.out.println("[VENDOR] com.samsung.util.SMS.send ignored: dest="
                           + sm.getDestAddress());
    }
}
