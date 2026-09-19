/*
 *   
 *
 * Copyright  1990-2007 Sun Microsystems, Inc. All Rights Reserved.
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
 * Please contact Sun Microsystems, Inc., 4150 Network Circle, Santa
 * Clara, CA 95054 or visit www.sun.com if you need additional
 * information or have any questions.
 */

#include <jvm.h>
#include <midp_logging.h>
#include <midp_mastermode_port.h>
#include <timer_queue.h>

/**
 * @file
 *
 * Export master mode functions for PS Vita platform.
 */

/**
 * Stub implementation of checkForSystemSignal for PS Vita.
 * The Vita does not use Unix signals; events are handled via the
 * master mode event queue.
 */
void checkForSystemSignal(MidpReentryData* pNewSignal,
                          MidpEvent* pNewMidpEvent,
                          jlong timeout) {
    (void)pNewSignal;
    (void)pNewMidpEvent;
    (void)timeout;
    /* PS Vita: no signal-based event handling */
}