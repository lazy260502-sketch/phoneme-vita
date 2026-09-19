/*
 * lcdui_display_vita.c - PS Vita display implementation for MIDP LCUI
 *
 * Purpose: Provide Vita-specific implementation of LCD UI display operations
 *          Directly accesses the Vita framebuffer without depending on
 *          the lcdlf framework. Integrates with phoneme-cldc's existing
 *          Vita port.
 */

#include <kni.h>
#include <sni.h>
#include <jvm.h>

#include "platform_vita.h"

/*
 * Vita screen dimensions
 * The PS Vita has a 960x544 pixel display
 */
#define VITA_SCREEN_WIDTH  960
#define VITA_SCREEN_HEIGHT 544

/**
 * Get display width in pixels
 * Java declaration: getDisplayWidth0()I
 */
KNIEXPORT KNI_RETURNTYPE_INT
KNIDECL(com_sun_midp_lcdui_DisplayDevice_getDisplayWidth) {
    KNI_ReturnInt(VITA_SCREEN_WIDTH);
}

/**
 * Get display height in pixels
 * Java declaration: getDisplayHeight0()I
 */
KNIEXPORT KNI_RETURNTYPE_INT
KNIDECL(com_sun_midp_lcdui_DisplayDevice_getDisplayHeight) {
    KNI_ReturnInt(VITA_SCREEN_HEIGHT);
}

/**
 * Refresh the display (swap buffers).
 * Blits the shared software framebuffer (vita_fb) to the Vita display.
 * Java declaration: refresh0(IIIII)V
 * Parameters:
 *   hardwareId, displayId, x1, y1, x2, y2
 */
KNIEXPORT KNI_RETURNTYPE_VOID
KNIDECL(com_sun_midp_lcdui_DisplayDevice_refresh0) {
    int y2 = KNI_GetParameterAsInt(6);
    int x2 = KNI_GetParameterAsInt(5);
    int y1 = KNI_GetParameterAsInt(4);
    int x1 = KNI_GetParameterAsInt(3);
    jint displayId = KNI_GetParameterAsInt(2);
    jint hardwareId = KNI_GetParameterAsInt(1);

    (void)displayId; (void)hardwareId;
    (void)x1; (void)y1; (void)x2; (void)y2;

    /* Push the shared framebuffer to the Vita display. */
    platform_vita_swap_buffers();

    KNI_ReturnVoid();
}

/**
 * Set the display to full screen mode
 * Java declaration: setFullScreen0(IZ)V
 */
KNIEXPORT KNI_RETURNTYPE_VOID
KNIDECL(com_sun_midp_lcdui_DisplayDevice_setFullScreen0) {
    jboolean mode = KNI_GetParameterAsBoolean(3);
    jint displayId = KNI_GetParameterAsInt(2);
    jint hardwareId = KNI_GetParameterAsInt(1);
    (void)mode; (void)displayId; (void)hardwareId;
    KNI_ReturnVoid();
}

/**
 * Called when the display gains foreground
 * Java declaration: gainedForeground0(I)V
 */
KNIEXPORT KNI_RETURNTYPE_VOID
KNIDECL(com_sun_midp_lcdui_DisplayDevice_gainedForeground0) {
    jint displayId = KNI_GetParameterAsInt(2);
    jint hardwareId = KNI_GetParameterAsInt(1);
    (void)displayId; (void)hardwareId;
    KNI_ReturnVoid();
}

/**
 * Get the absolute X coordinate
 * Java declaration: getAbsX(I)I
 */
KNIEXPORT KNI_RETURNTYPE_INT
KNIDECL(com_sun_midp_lcdui_DisplayDevice_getAbsX) {
    int logicalX = KNI_GetParameterAsInt(1);
    KNI_ReturnInt(logicalX);
}

/**
 * Get the absolute Y coordinate
 * Java declaration: getAbsY(I)I
 */
KNIEXPORT KNI_RETURNTYPE_INT
KNIDECL(com_sun_midp_lcdui_DisplayDevice_getAbsY) {
    int logicalY = KNI_GetParameterAsInt(1);
    KNI_ReturnInt(logicalY);
}

/**
 * Get screen height (for lcdlf compatibility)
 * Java declaration: getScreenHeight0(I)I
 */
KNIEXPORT KNI_RETURNTYPE_INT
KNIDECL(com_sun_midp_lcdui_DisplayDevice_getScreenHeight0) {
    jint hardwareId = KNI_GetParameterAsInt(1);
    (void)hardwareId;
    KNI_ReturnInt(VITA_SCREEN_HEIGHT);
}

/**
 * Get screen width (for lcdlf compatibility)
 * Java declaration: getScreenWidth0(I)I
 */
KNIEXPORT KNI_RETURNTYPE_INT
KNIDECL(com_sun_midp_lcdui_DisplayDevice_getScreenWidth0) {
    jint hardwareId = KNI_GetParameterAsInt(1);
    (void)hardwareId;
    KNI_ReturnInt(VITA_SCREEN_WIDTH);
}