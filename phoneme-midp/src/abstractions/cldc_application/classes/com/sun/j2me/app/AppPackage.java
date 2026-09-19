/*
 * Copyright  1990-2006 Sun Microsystems, Inc. All Rights Reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
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
 */

package com.sun.j2me.app;

import com.sun.j2me.security.Permission;
import com.sun.j2me.security.AccessController;
import com.sun.midp.midlet.MIDletStateHandler;
import com.sun.midp.midletsuite.MIDletSuiteImpl;
import com.sun.midp.midlet.MIDletSuite;
import java.io.InputStream;

/**
 * Abstraction for application package.
 * NOTE: getId(), checkForPermission(), getCA() depend on midp internal classes.
 * getId() returns -1 if MIDletSuite is unavailable (Vita standalone mode).
 */
public class AppPackage {

    /** Static instance. Only one package can be run in an isolate */
    private static AppPackage instance = new AppPackage();

    /** Guard from 'new' operator */
    private AppPackage() {
    }

    public static AppPackage getInstance() {
        return instance;
    }

    /**
     * Returns the ID of the currently running MIDlet suite.
     * Returns UNUSED_APP_ID (-1) if no suite is available.
     */
    public int getId() {
        try {
            MIDletStateHandler msh = MIDletStateHandler.getMidletStateHandler();
            MIDletSuite ms = msh.getMIDletSuite();
            if (ms == null) {
                return UNUSED_APP_ID;
            }
            return ms.getID();
        } catch (Throwable t) {
            return UNUSED_APP_ID;
        }
    }

    /** Unused ID */
    public static final int UNUSED_APP_ID = -1;

    /**
     * Returns permission status for the specified permission.
     *
     * @param p permission to check
     * @return 1 if allowed; 0 if denied; -1 if status is unknown
     */
    public int checkPermission(Permission p) {
        return -1;
    }

    /**
     * Checks for specified permission status. Throws an exception
     * if permission is not allowed.
     *
     * @param p a permission to check
     * @exception SecurityException if permission is not allowed
     * @exception InterruptedException if another thread interrupts a calling
     *  thread while asking user
     */
    public void checkForPermission(Permission p) throws InterruptedException {
        AccessController.checkPermission(p.getName(), p.getResource(), p.getExtraValue());
    }

    /**
     * Throws an exception if a status for the permission is not allowed.
     *
     * @param p a permission to check
     * @exception SecurityException if a status for the permission is not allowed
     */
    public void checkIfPermissionAllowed(Permission p) {
        if (checkPermission(p) != 1) {
            throw new SecurityException();
        }
    }

    /**
     * Gets the name of CA that authorized this suite.
     *
     * @return name of a CA or null if the suite was not signed
     */
    public String getCA() {
        try {
            MIDletSuite ms =
                MIDletStateHandler.getMidletStateHandler().getMIDletSuite();
            if (ms instanceof com.sun.midp.midletsuite.MIDletSuiteImpl) {
                return ((MIDletSuiteImpl)ms).getInstallInfo().getCA();
            }
        } catch (Throwable t) {
        }
        return null;
    }

    /**
     * Finds a resource with a given name.
     *
     * @param name  name of the desired resource
     * @return      a <code>java.io.InputStream</code> object.
     * @throws NullPointerException if <code>name</code> is <code>null</code>.
     */
    public InputStream getResourceAsStream(String name) {
        return getClass().getResourceAsStream(name);
    }
}
