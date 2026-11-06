package com.nokia.mid.ui;

/**
 * Nokia UI API Clipboard. Loader backs it with the Android ClipboardManager;
 * there is no system clipboard bridge on the Vita yet, so this keeps the text
 * in a process-wide buffer: copy-then-paste inside one MIDlet round works,
 * nothing is shared with other applications or survives a round restart.
 * Member list follows J2ME-Loader's class of the same name (Apache-2.0,
 * Copyright 2021 Arman Jussupgaliyev).
 */
public abstract class Clipboard {
    private static String text = "";

    public static synchronized void copyToClipboard(String s) {
        text = (s == null) ? "" : s;
    }

    public static synchronized String copyFromClipboard() {
        return text;
    }
}
