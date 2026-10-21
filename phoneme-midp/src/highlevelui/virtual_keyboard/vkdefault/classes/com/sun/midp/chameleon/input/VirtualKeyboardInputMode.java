/*
 *
 * Copyright  1990-2008 Sun Microsystems, Inc. All Rights Reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License version
 * 2 only, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License version 2 for more details (a copy is
 * included at /legal/license.txt).
 *
 * You should have received a copy of the GNU General Public License
 * version 2 along with this work; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA
 * 02110-1301 USA
 *
 * Please contact Sun Microsystems, Inc., 4150 Network Circle, Santa
 * Clara, CA 95054 or visit www.sun.com if you need additional
 * information or have any questions.
 */
package com.sun.midp.chameleon.input;

/**
 * An InputMode instance which allows to use java virtual keyboard.
 */
public class VirtualKeyboardInputMode extends KeyboardInputMode{
    /**
         * This method is called to determine if this InputMode supports
         * the given text input constraints. The semantics of the constraints
         * value are defined in the javax.microedition.lcdui.TextField API.
         * If this InputMode returns false, this InputMode must not be used
         * to process key input for the selected text component.
         *
         * @param constraints text input constraints. The semantics of the
         * constraints value are defined in the TextField API.
         *
         * @return true if this InputMode supports the given text component
         *         constraints, as defined in the MIDP TextField API
         */
        public boolean supportsConstraints(int constraints) {
            return false;
        }

    // v02.52: system keyboard bridge. TextFieldLFImpl/TextBoxLFImpl call
    // these directly when a text component gains/loses focus; the native
    // implementation (lfj_native_input_mode.c, VITA) forwards to the
    // vita-port vita_ime_show()/vita_ime_abort(). On other platforms they
    // are no-ops (the native symbols are only built under WINCE/VITA, so
    // the linker resolves them only for those targets).
    public static native void showNativeKeyboard();
    public static native void hideNativeKeyboard();

    // v02.74: hand the focused TextBox's content + capacity to the native
    // side so the system keyboard opens with it (and its result replaces it).
    public static native void setNativeKeyboardText(String text, int maxSize);

}
