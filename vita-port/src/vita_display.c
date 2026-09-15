/*
 * vita_display.c - PS Vita LCDUI port layer (lfjport) for phoneME MIDP
 *
 * Replaces phoneme-midp's lfjport_fb_export.o (whose lfjport_refresh was a
 * TODO stub). The CMake build deletes that member from libobj_no_main.a and
 * links this file instead, so every symbol exported by the old object must
 * be defined here:
 *   gxj_system_screen_buffer + 21 lfjport_* functions (see lfjport_export.h).
 *
 * Rendering model: MIDP paints (both chameleon high-level UI and Canvas
 * graphics) into gxj_system_screen_buffer (RGB565). lfjport_refresh() blits
 * that virtual phone screen (240x320) scaled, centered, onto the Vita's
 * 960x544 framebuffer and flips it with sceDisplaySetFrameBuf.
 */

#include <kni.h>
#include <psp2/display.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/sysmem.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <gxj_putpixel.h>
#include <lfjport_export.h>

#include "vita_fbmem.h"

/* ------------------------------------------------------------------ */
/* Virtual phone screen (what MIDP games see) vs Vita physical screen.
 *
 * Most J2ME games are portrait 240x320; that is the default. Landscape
 * games (320x240) select the other orientation via
 * vita_display_set_orientation(), called from the launcher after reading
 * launch.cfg, before the VM and hence the Java UI layer start. On the
 * 960x544 panel a portrait screen is shown as a centered 407x543 column. */
#define VITA_MAX_W 320
#define VITA_MAX_H 320

#ifndef VITA_DEFAULT_LANDSCAPE
#define VITA_DEFAULT_LANDSCAPE 0
#endif

#define VITA_PHYS_W 960
#define VITA_PHYS_H 544

static int virt_w = VITA_DEFAULT_LANDSCAPE ? 320 : 240;
static int virt_h = VITA_DEFAULT_LANDSCAPE ? 240 : 320;

/* Static buffers for the system screen buffer, sized for the largest
 * orientation (either way it is 320x240 = 76800 pixels). */
static gxj_pixel_type vita_screen_pixels[VITA_MAX_W * VITA_MAX_H];
static gxj_alpha_type vita_screen_alpha[VITA_MAX_W * VITA_MAX_H];

gxj_screen_buffer gxj_system_screen_buffer = {
    VITA_DEFAULT_LANDSCAPE ? 320 : 240,
    VITA_DEFAULT_LANDSCAPE ? 240 : 320,
    vita_screen_pixels,
    vita_screen_alpha
};

/* Switch the virtual screen orientation. Must be called before the VM
 * starts (the Java UI layer reads the size once at init). */
void vita_display_set_orientation(int landscape) {
    if (landscape) {
        virt_w = 320;
        virt_h = 240;
    } else {
        virt_w = 240;
        virt_h = 320;
    }
    gxj_system_screen_buffer.width = virt_w;
    gxj_system_screen_buffer.height = virt_h;
}

/* Physical framebuffer (A8B8G8R8, 4 bytes/pixel), allocated in ui_init. */
static uint32_t *vita_fb = NULL;
/* v01.67 real-hw black-screen fix: CDRAM (uncached) block behind vita_fb.
 * A cached memalign buffer is invisible to the display controller on
 * real hardware - see vita_fbmem.h. */
static VitaFbMem vita_fb_blk = { -1, NULL };
static int display_ready = 0;

/* Computed scaling: virtual screen scaled to fit physical, centered. */
static int dst_w, dst_h, dst_x, dst_y;
/* Fixed point 16.16 step per destination pixel. */
static int step_x, step_y;

static void compute_scaling(void) {
    /* Fit the virtual screen inside 960x544 keeping aspect ratio:
     * scale = min(PHYS_W/VIRT_W, PHYS_H/VIRT_H). */
    int sx = (VITA_PHYS_W << 12) / virt_w;
    int sy = (VITA_PHYS_H << 12) / virt_h;
    int scale = (sx < sy) ? sx : sy;
    dst_w = (virt_w * scale) >> 12;
    dst_h = (virt_h * scale) >> 12;
    if (dst_w > VITA_PHYS_W) dst_w = VITA_PHYS_W;
    if (dst_h > VITA_PHYS_H) dst_h = VITA_PHYS_H;
    dst_x = (VITA_PHYS_W - dst_w) / 2;
    dst_y = (VITA_PHYS_H - dst_h) / 2;
    step_x = (virt_w << 16) / dst_w;
    step_y = (virt_h << 16) / dst_h;
}

/* Convert RGB565 to 0xAABBGGRR (SCE_DISPLAY_PIXELFORMAT_A8B8G8R8). */
static inline uint32_t rgb565_to_abgr(gxj_pixel_type p) {
    uint32_t r5 = (p >> 11) & 0x1F;
    uint32_t g6 = (p >> 5) & 0x3F;
    uint32_t b5 = p & 0x1F;
    uint32_t r = (r5 << 3) | (r5 >> 2);
    uint32_t g = (g6 << 2) | (g6 >> 4);
    uint32_t b = (b5 << 3) | (b5 >> 2);
    return 0xFF000000u | (b << 16) | (g << 8) | r;
}

/*
 * Map a physical (960x544) touch coordinate back to the virtual J2ME
 * screen (240x320 portrait / 320x240 landscape), i.e. invert the
 * centered/letterboxed scaling done by compute_scaling().
 * Returns 0 when the touch is inside the virtual screen, -1 when it
 * lands in the letterbox (out of range; caller should ignore it).
 */
int vita_display_map_touch(int px, int py, int *vx, int *vy) {
    if (vx == NULL || vy == NULL) return -1;
    if (px < dst_x || px >= dst_x + dst_w ||
        py < dst_y || py >= dst_y + dst_h) {
        return -1;
    }
    *vx = (px - dst_x) * virt_w / dst_w;
    *vy = (py - dst_y) * virt_h / dst_h;
    if (*vx < 0) *vx = 0;
    if (*vx >= virt_w) *vx = virt_w - 1;
    if (*vy < 0) *vy = 0;
    if (*vy >= virt_h) *vy = virt_h - 1;
    return 0;
}

static void flip_to_display(void) {
    SceDisplayFrameBuf fb;
    int x, y;

    if (vita_fb == NULL) {
        return;
    }

    /* Blit scaled virtual screen into the physical framebuffer.
     * Nearest neighbour via fixed point steps. */
    for (y = 0; y < dst_h; y++) {
        const gxj_pixel_type *src_row =
            &vita_screen_pixels[((y * step_y) >> 16) * virt_w];
        uint32_t *dst_row = &vita_fb[(dst_y + y) * VITA_PHYS_W + dst_x];
        int sx = 0;
        for (x = 0; x < dst_w; x++) {
            dst_row[x] = rgb565_to_abgr(src_row[sx >> 16]);
            sx += step_x;
        }
    }

    memset(&fb, 0, sizeof(fb));
    fb.size = sizeof(SceDisplayFrameBuf);
    fb.base = vita_fb;
    fb.pitch = VITA_PHYS_W;
    fb.pixelformat = SCE_DISPLAY_PIXELFORMAT_A8B8G8R8;
    fb.width = VITA_PHYS_W;
    fb.height = VITA_PHYS_H;
    /* v01.69: NEXTFRAME - IMMEDIATE returns INVALID_UPDATETIMING
     * (0x80290006) on real fw 3.65 (see vita_menu.c menu_flip). */
    sceDisplaySetFrameBuf(&fb, SCE_DISPLAY_SETBUF_NEXTFRAME);
}

/* ------------------------------------------------------------------ */
/* lfjport implementation                                             */
/* ------------------------------------------------------------------ */

void lfjport_refresh(int hardwareId, int x, int y, int w, int h) {
    (void)hardwareId; (void)x; (void)y; (void)w; (void)h;
    /* The scaled blit covers the whole screen anyway; full flip. */
    flip_to_display();
}

jboolean lfjport_reverse_orientation(int hardwareId) {
    (void)hardwareId;
    return KNI_FALSE;
}

void lfjport_handle_clamshell_event(void) {
    /* PS Vita: no clamshell */
}

jboolean lfjport_get_reverse_orientation(int hardwareId) {
    (void)hardwareId;
    return KNI_FALSE;
}

int lfjport_get_screen_width(int hardwareId) {
    (void)hardwareId;
    return virt_w;
}

int lfjport_get_screen_height(int hardwareId) {
    (void)hardwareId;
    return virt_h;
}

void lfjport_set_fullscreen_mode(int hardwareId, jboolean mode) {
    /* Always fullscreen on Vita */
    (void)hardwareId; (void)mode;
}

void lfjport_gained_foreground(int hardwareId) {
    (void)hardwareId;
    /* Push current buffer so a foreground switch repaints immediately. */
    flip_to_display();
}

int lfjport_ui_init(void) {
    if (display_ready) {
        return 0;
    }
    compute_scaling();

    vita_fb = NULL;
    if (vita_fbmem_alloc(&vita_fb_blk,
            VITA_PHYS_W * VITA_PHYS_H * sizeof(uint32_t)) == 0) {
        vita_fb = (uint32_t *)vita_fb_blk.base;
    }
    if (vita_fb == NULL) {
        return -1;
    }
    /* Black letterbox background. */
    memset(vita_fb, 0, VITA_PHYS_W * VITA_PHYS_H * sizeof(uint32_t));

    display_ready = 1;

    /* Show something immediately (white virtual screen). */
    memset(vita_screen_pixels, 0xFF, sizeof(vita_screen_pixels));
    flip_to_display();
    return 0;
}

void lfjport_ui_finalize(void) {
    display_ready = 0;
    if (vita_fb != NULL) {
        vita_fb = NULL;
        vita_fbmem_release(&vita_fb_blk);
    }
}

jboolean lfjport_direct_flush(int hardwareId, const java_graphics *g,
                              const java_imagedata *offscreen_buffer, int h) {
    /* Let the generic path copy into gxj_system_screen_buffer and call
     * lfjport_refresh; we handle the actual flip there. */
    (void)hardwareId; (void)g; (void)offscreen_buffer; (void)h;
    return KNI_FALSE;
}

jboolean lfjport_is_native_softbutton_layer_supported(void) {
    /* Soft button labels are drawn by the Java chameleon layer. */
    return KNI_FALSE;
}

void lfjport_set_softbutton_label_on_native_layer(unsigned short *label,
                                                  int len, int index) {
    (void)label; (void)len; (void)index;
}

int lfjport_get_current_hardwareId(void) {
    return 0;
}

char *lfjport_get_display_name(int hardwareId) {
    (void)hardwareId;
    return "PS Vita Screen";
}

jboolean lfjport_is_display_primary(int hardwareId) {
    (void)hardwareId;
    return KNI_TRUE;
}

jboolean lfjport_is_display_buildin(int hardwareId) {
    (void)hardwareId;
    return KNI_TRUE;
}

jboolean lfjport_is_display_pen_supported(int hardwareId) {
    /* Vita front touch panel -> MIDP_PEN_EVENT (vita_input.c). */
    (void)hardwareId;
    return KNI_TRUE;
}

jboolean lfjport_is_display_pen_motion_supported(int hardwareId) {
    (void)hardwareId;
    return KNI_TRUE;
}

int lfjport_get_display_capabilities(int hardwareId) {
    /* Return 0 here and Display.setCurrent() throws
     * "This display does not support title" — a Form can never be shown.
     * Bits (DisplayDevice.java): 1=input, 2=commands, 4=forms, 8=ticker,
     * 16=title, 32=alerts, 64=lists, 128=textboxes, 256=tabbedpanes,
     * 512=fileselectors. We support them all. */
    (void)hardwareId;
    return 0x3FF;
}

jint *lfjport_get_display_device_ids(jint *n) {
    /* Contract (fbapp_export.c upstream): return a non-NULL array of ids.
     * Returning NULL with *n=1 made KNI_SetIntArrayElement dereference NULL
     * in DisplayDeviceContainer.getDisplayDevicesIds0 and crash the VM. */
    static jint display_device_ids[] = { 0 };

    if (n != NULL) {
        *n = 1;
    }
    return display_device_ids;
}

void lfjport_display_device_state_changed(int hardwareId, int state) {
    (void)hardwareId; (void)state;
}
