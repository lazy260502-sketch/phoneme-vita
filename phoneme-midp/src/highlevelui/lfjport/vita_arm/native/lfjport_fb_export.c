/*
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
 * General Public License version 2 for more details.
 *
 * You should have received a copy of the GNU General Public License
 * version 2 along with this work; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA
 * 02110-1301 USA.
 */

/*
 * LFJPORT Java Look and Feel Porting for PS Vita.
 * Stub implementations for the LCDUI porting layer.
 */

#include <kni.h>
#include <midp_logging.h>
#include <lfjport_export.h>
#include <gxj_putpixel.h>
#include <stddef.h>
#include <stdint.h>

/* PS Vita screen dimensions */
#define VITA_SCREEN_WIDTH  960
#define VITA_SCREEN_HEIGHT 544

/*
 * Vita display blit hook. platform_vita.o lives in the app-level CMake
 * archive (not inside libmidp.so), so resolve it at app link time via
 * a weak undefined symbol; a no-op fallback keeps MIDP linking standalone.
 */
extern void platform_vita_swap_buffers(void) __attribute__((weak));
extern uint32_t *vita_fb __attribute__((weak));

/* Static pixel buffer for the system screen buffer */
static gxj_pixel_type vita_screen_pixels[VITA_SCREEN_WIDTH * VITA_SCREEN_HEIGHT];

/* Static alpha buffer for the system screen buffer */
static gxj_alpha_type vita_screen_alpha[VITA_SCREEN_WIDTH * VITA_SCREEN_HEIGHT];

/**
 * Each port must define one system screen buffer.
 * This is the global screen buffer used by the graphics subsystem.
 */
gxj_screen_buffer gxj_system_screen_buffer = {
    VITA_SCREEN_WIDTH,
    VITA_SCREEN_HEIGHT,
    vita_screen_pixels,
    vita_screen_alpha
};

/**
 * Refresh the given area: convert RGB565 system screen buffer to the
 * Vita display format (A8B8G8R8, i.e. little-endian ABGR in the uint32)
 * and push it via sceDisplaySetFrameBuf.
 */
void lfjport_refresh(int hardwareId, int x, int y, int w, int h) {
    (void)hardwareId;
    (void)x; (void)y; (void)w; (void)h; /* full-frame blit is fine for now */

    if (vita_fb == NULL || platform_vita_swap_buffers == NULL) {
        return;
    }

    {
        const gxj_pixel_type *src = vita_screen_pixels;
        uint32_t *dst = (uint32_t *)vita_fb;
        int i;
        int total = VITA_SCREEN_WIDTH * VITA_SCREEN_HEIGHT;
        for (i = 0; i < total; i++) {
            gxj_pixel_type p = src[i];
            /* RGB565 -> RGB24 */
            int r = (p >> 11) & 0x1F;
            int g = (p >> 5)  & 0x3F;
            int b =  p        & 0x1F;
            r = (r << 3) | (r >> 2);
            g = (g << 2) | (g >> 4);
            b = (b << 3) | (b >> 2);
            /* 0xAARRGGBB (SCE_DISPLAY_PIXELFORMAT_A8B8G8R8 on LE) */
            dst[i] = 0xFF000000u | ((uint32_t)r << 16) |
                     ((uint32_t)g << 8) | (uint32_t)b;
        }
    }

    platform_vita_swap_buffers();
}

/**
 * Change screen orientation flag.
 */
jboolean lfjport_reverse_orientation(int hardwareId) {
    (void)hardwareId;
    return KNI_FALSE;
}

/**
 * Handle clamshell event.
 */
void lfjport_handle_clamshell_event() {
    /* PS Vita: no clamshell */
}

/**
 * Get screen orientation flag.
 */
jboolean lfjport_get_reverse_orientation(int hardwareId) {
    (void)hardwareId;
    return KNI_FALSE;
}

/**
 * Return screen width.
 */
int lfjport_get_screen_width(int hardwareId) {
    (void)hardwareId;
    return VITA_SCREEN_WIDTH;
}

/**
 * Return screen height.
 */
int lfjport_get_screen_height(int hardwareId) {
    (void)hardwareId;
    return VITA_SCREEN_HEIGHT;
}

/**
 * Set the screen mode either to fullscreen or normal.
 */
void lfjport_set_fullscreen_mode(int hardwareId, jboolean mode) {
    (void)hardwareId;
    (void)mode;
}

/**
 * Resets native resources when foreground is gained by a new display.
 */
void lfjport_gained_foreground(int hardwareId) {
    (void)hardwareId;
}

/**
 * Initializes the window system.
 */
int lfjport_ui_init() {
    return 0;
}

/**
 * Finalize the window system.
 */
void lfjport_ui_finalize() {
}

/**
 * Flushes the offscreen buffer directly to the device screen.
 */
jboolean lfjport_direct_flush(int hardwareId, const java_graphics *g,
                              const java_imagedata *offscreen_buffer, int h) {
    (void)hardwareId;
    (void)g;
    (void)offscreen_buffer;
    (void)h;
    return KNI_FALSE;
}

/**
 * Check if native softbutton is supported on platform.
 */
jboolean lfjport_is_native_softbutton_layer_supported() {
    return KNI_FALSE;
}

/**
 * Request platform to draw a label in the soft button layer.
 */
void lfjport_set_softbutton_label_on_native_layer(unsigned short *label,
                                                  int len, int index) {
    (void)label;
    (void)len;
    (void)index;
}

/**
 * Get currently enabled hardware display id.
 */
int lfjport_get_current_hardwareId() {
    return 0;
}

/**
 * Get display device name by id.
 */
char *lfjport_get_display_name(int hardwareId) {
    (void)hardwareId;
    return "PS Vita Screen";
}

/**
 * Check if the display device is primary.
 */
jboolean lfjport_is_display_primary(int hardwareId) {
    (void)hardwareId;
    return KNI_TRUE;
}

/**
 * Check if the display device is build-in.
 */
jboolean lfjport_is_display_buildin(int hardwareId) {
    (void)hardwareId;
    return KNI_TRUE;
}

/**
 * Check if the display device supports pointer events.
 */
jboolean lfjport_is_display_pen_supported(int hardwareId) {
    (void)hardwareId;
    return KNI_TRUE;
}

/**
 * Check if the display device supports pointer motion events.
 */
jboolean lfjport_is_display_pen_motion_supported(int hardwareId) {
    (void)hardwareId;
    return KNI_TRUE;
}

/**
 * Get display device capabilities.
 */
int lfjport_get_display_capabilities(int hardwareId) {
    (void)hardwareId;
    /* VITA: report all standard capabilities (matches fbapp reference impl),
     * otherwise Display.setCurrent() rejects Form titles with IAE */
    return 255; /* INPUT_EVENTS|COMMANDS|FORMS|TICKER|TITLE|ALERTS|LISTS|TEXTBOXES */
}

/**
 * Get the list of display device ids.
 */
jint *lfjport_get_display_device_ids(jint *n) {
    static jint vita_display_device_ids[] = {0};
    if (n != NULL) {
        *n = 1;
    }
    return vita_display_device_ids;
}

/**
 * Notify the display device state has been changed.
 */
void lfjport_display_device_state_changed(int hardwareId, int state) {
    (void)hardwareId;
    (void)state;
}