package com.siemens.mp.lcdui;

import javax.microedition.lcdui.Item;

/**
 * Siemens API lcdui.Form: adds setKeyDispatcher, which is accepted and
 * ignored here (J2ME-Loader does the same; Apache-2.0).
 */
public class Form extends javax.microedition.lcdui.Form {
    public Form(String title) {
        super(title);
    }

    public Form(String title, Item[] elements) {
        super(title, elements);
    }

    public void setKeyDispatcher(KeyDispatcher dispatcher) {
    }
}
