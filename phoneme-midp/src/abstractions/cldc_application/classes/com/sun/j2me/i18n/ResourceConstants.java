/*
 * Copyright  1990-2006 Sun Microsystems, Inc. All Rights Reserved.
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
 */

package com.sun.j2me.i18n;

/**
 * Resource constants for abstractions layer.
 * Extends the MIDP-generated ResourceConstants and adds the
 * ABSTRACTIONS_PIM_* constants that would normally be generated
 * from i18n_constants_abstractions.xml by TableGenerator (not available).
 */
public class ResourceConstants extends com.sun.midp.i18n.ResourceConstants {

    /** Abstractions: com.sun.j2me.security.PIMPermission */
    public final static int ABSTRACTIONS_PIM_CONTACTS = 394;

    /** Abstractions: com.sun.j2me.security.PIMPermission */
    public final static int ABSTRACTIONS_PIM_EVENTS = 395;

    /** Abstractions: com.sun.j2me.security.PIMPermission */
    public final static int ABSTRACTIONS_PIM_TODO = 396;
}
