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
 */

#include <midp_properties_port.h>
#include <midpMalloc.h>
#include <midp_logging.h>

/**
 * @file
 *
 * Platform implementation of the platformRequest() method call.
 * PS Vita version.
 */

/** Property name for platform request handler */
static const char* const PLATFORM_REQUEST_KEY __attribute__((unused)) =
    "com.sun.midp.midlet.platformRequestCommand";

/**
 * Starts a new process to handle the given URL.
 *
 * @param pszUrl The 'C' string URL
 *
 * @return true if the platform request is configured
 */
int midp_platform_request_md(const char* pszUrl) {
    (void)pszUrl;
    /* PS Vita: Platform request not implemented */
    return 0;
}