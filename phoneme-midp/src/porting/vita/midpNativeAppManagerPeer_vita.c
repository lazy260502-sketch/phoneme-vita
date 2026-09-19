/*
 * midpNativeAppManagerPeer_vita.c - PS Vita Native AMS implementation
 *
 * Purpose: Provide Vita-specific implementation of the Native AMS peer
 *          Handles MIDlet lifecycle management on the Vita platform
 *
 * This is a minimal implementation that provides the basic lifecycle
 * management needed for MIDlets to start, pause, and stop on Vita.
 */

#include <kni.h>
#include <jvm.h>
#include <stdio.h>

#include "platform_vita.h"

/**
 * Initialize the native application manager peer
 * Called once during VM startup
 *
 * Java declaration: init()V
 */
KNIEXPORT KNI_RETURNTYPE_VOID
KNIDECL(com_sun_midp_main_NativeAppManagerPeer_init) {
    if (platform_vita_init() != 0) {
        KNI_ThrowNew("java/lang/RuntimeException", "Failed to initialize Vita platform");
    }
    KNI_ReturnVoid();
}

/**
 * Shutdown the native application manager peer
 * Called once during VM shutdown
 *
 * Java declaration: shutdown()V
 */
KNIEXPORT KNI_RETURNTYPE_VOID
KNIDECL(com_sun_midp_main_NativeAppManagerPeer_shutdown) {
    platform_vita_shutdown();
    KNI_ReturnVoid();
}

/**
 * Process pending events
 * Called regularly from the main loop to process input and other events
 *
 * Java declaration: processEvents()V
 */
KNIEXPORT KNI_RETURNTYPE_VOID
KNIDECL(com_sun_midp_main_NativeAppManagerPeer_processEvents) {
    platform_vita_poll_input();
    KNI_ReturnVoid();
}

/**
 * Check if the application manager is in the foreground
 *
 * Java declaration: isForeground()Z
 */
KNIEXPORT KNI_RETURNTYPE_BOOLEAN
KNIDECL(com_sun_midp_main_NativeAppManagerPeer_isForeground) {
    /* On Vita, our MIDlet is always in the foreground */
    KNI_ReturnBoolean(KNI_TRUE);
}

/**
 * Request that the application manager bring the foreground to this MIDlet
 *
 * Java declaration: requestForeground()V
 */
KNIEXPORT KNI_RETURNTYPE_VOID
KNIDECL(com_sun_midp_main_NativeAppManagerPeer_requestForeground) {
    /* On Vita, we're always in the foreground */
    KNI_ReturnVoid();
}