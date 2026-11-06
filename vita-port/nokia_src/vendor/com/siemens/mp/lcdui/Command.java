package com.siemens.mp.lcdui;

/**
 * Siemens API lcdui.Command: adds a soft-key icon id, which is accepted and
 * ignored (J2ME-Loader list, Apache-2.0).
 */
public class Command extends javax.microedition.lcdui.Command {
    public Command(String shortLabel, String longLabel, int commandType,
                   int priority, char iconId) {
        super(shortLabel, longLabel, commandType, priority);
    }

    public Command(String label, int commandType, int priority,
                   char iconId) {
        super(label, commandType, priority);
    }
}
