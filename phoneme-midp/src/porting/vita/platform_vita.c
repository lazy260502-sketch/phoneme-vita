/*
 * platform_vita.c - PS Vita platform implementation for MIDP
 *
 * Purpose: Provide Vita-specific implementation of the platform abstraction layer
 *          Handles display, input, and graphics for MIDP on PS Vita
 */

#include <psp2/types.h>
#include <psp2/ctrl.h>
#include <psp2/display.h>
#include <psp2/gxm.h>
#include <psp2/kernel/threadmgr.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "platform_vita.h"

/* Shared framebuffer - the single source of truth for the on-screen pixels.
 * All MIDP drawing routines (gxj_graphics_vita.c, lcdui_display_vita.c) write
 * here, and platform_vita_swap_buffers() blits this to the Vita display. */
uint32_t *vita_fb = NULL;
static SceCtrlData padData;
static int keyState = 0;

/* Helpers exposed to the rest of the native bridge. */
uint32_t *platform_vita_get_fb(void) {
    return vita_fb;
}

int platform_vita_get_fb_width(void) {
    return VITA_SCREEN_WIDTH;
}

int platform_vita_get_fb_height(void) {
    return VITA_SCREEN_HEIGHT;
}

/*
 * Initialize the Vita platform
 * Sets up display, input, and graphics
 */
int platform_vita_init(void) {
    int ret = 0;

    /* Initialize display - using Vita SDK display API */
    SceDisplayFrameBuf frameBuf;
    memset(&frameBuf, 0, sizeof(frameBuf));
    frameBuf.base = NULL;
    frameBuf.size = sizeof(SceDisplayFrameBuf);
    frameBuf.pitch = VITA_SCREEN_WIDTH;
    frameBuf.pixelformat = SCE_DISPLAY_PIXELFORMAT_A8B8G8R8;
    frameBuf.width = VITA_SCREEN_WIDTH;
    frameBuf.height = VITA_SCREEN_HEIGHT;
    
    ret = sceDisplaySetFrameBuf(&frameBuf, SCE_DISPLAY_SETBUF_IMMEDIATE);
    if (ret < 0) {
        return -1;
    }

    /* Initialize input - Vita SDK uses sceCtrlPeekBufferPositive */
    sceCtrlPeekBufferPositive(0, &padData, 1);

    /* Allocate the shared software framebuffer (ARGB32, 4 bytes/pixel) */
    vita_fb = (uint32_t*)malloc(VITA_SCREEN_WIDTH * VITA_SCREEN_HEIGHT * sizeof(uint32_t));
    if (vita_fb == NULL) {
        return -1;
    }

    /* Clear screen to opaque white */
    platform_vita_clear_screen(0xFFFFFFFF);

    return 0;
}

/*
 * Shutdown the Vita platform
 * Clean up resources
 */
void platform_vita_shutdown(void) {
    if (vita_fb != NULL) {
        free(vita_fb);
        vita_fb = NULL;
    }
}

/*
 * Initialize display subsystem
 */
void platform_vita_display_init(void) {
    /* Display is already initialized in platform_vita_init */
}

/*
 * Swap front and back buffers.
 * Blits the shared software framebuffer (vita_fb) to the Vita display
 * using sceDisplaySetFrameBuf. This is what makes pixels actually appear
 * on screen.
 */
void platform_vita_swap_buffers(void) {
    if (vita_fb == NULL) {
        return;
    }

    SceDisplayFrameBuf frameBuf;
    memset(&frameBuf, 0, sizeof(frameBuf));
    frameBuf.base = vita_fb;
    frameBuf.size = sizeof(SceDisplayFrameBuf);
    frameBuf.pitch = VITA_SCREEN_WIDTH;
    frameBuf.pixelformat = SCE_DISPLAY_PIXELFORMAT_A8B8G8R8;
    frameBuf.width = VITA_SCREEN_WIDTH;
    frameBuf.height = VITA_SCREEN_HEIGHT;

    sceDisplaySetFrameBuf(&frameBuf, SCE_DISPLAY_SETBUF_IMMEDIATE);
}

/*
 * Initialize input subsystem
 */
int platform_vita_input_init(void) {
    sceCtrlPeekBufferPositive(0, &padData, 1);
    return 0;
}

/*
 * Poll for input events
 */
void platform_vita_poll_input(void) {
    sceCtrlPeekBufferPositive(0, &padData, 1);
    
    /* Update key state based on Vita buttons */
    keyState = 0;
    
    if (padData.buttons & SCE_CTRL_UP) keyState |= KEY_UP;
    if (padData.buttons & SCE_CTRL_DOWN) keyState |= KEY_DOWN;
    if (padData.buttons & SCE_CTRL_LEFT) keyState |= KEY_LEFT;
    if (padData.buttons & SCE_CTRL_RIGHT) keyState |= KEY_RIGHT;
    if (padData.buttons & SCE_CTRL_CROSS) keyState |= KEY_FIRE;
    if (padData.buttons & SCE_CTRL_CIRCLE) keyState |= KEY_SOFT1;
    if (padData.buttons & SCE_CTRL_SQUARE) keyState |= KEY_SOFT2;
    if (padData.buttons & SCE_CTRL_TRIANGLE) keyState |= GAME_A;
    if (padData.buttons & SCE_CTRL_LTRIGGER) keyState |= GAME_B;
    if (padData.buttons & SCE_CTRL_RTRIGGER) keyState |= GAME_C;
    if (padData.buttons & SCE_CTRL_START) keyState |= KEY_STAR;
    if (padData.buttons & SCE_CTRL_SELECT) keyState |= KEY_POUND;
}

/*
 * Get the current state of a key
 * Returns 1 if pressed, 0 if not
 */
int platform_vita_get_key_state(int keyCode) {
    return (keyState & keyCode) ? 1 : 0;
}

/*
 * Draw a single pixel
 */
void platform_vita_draw_pixel(int x, int y, uint32_t color) {
    if (x < 0 || x >= VITA_SCREEN_WIDTH || y < 0 || y >= VITA_SCREEN_HEIGHT) {
        return;
    }
    if (vita_fb != NULL) {
        vita_fb[y * VITA_SCREEN_WIDTH + x] = color;
    }
}

/*
 * Draw a rectangle outline
 */
void platform_vita_draw_rect(int x, int y, int w, int h, uint32_t color) {
    int i;
    /* Draw top and bottom lines */
    for (i = 0; i < w; i++) {
        platform_vita_draw_pixel(x + i, y, color);
        platform_vita_draw_pixel(x + i, y + h - 1, color);
    }
    /* Draw left and right lines */
    for (i = 1; i < h - 1; i++) {
        platform_vita_draw_pixel(x, y + i, color);
        platform_vita_draw_pixel(x + w - 1, y + i, color);
    }
}

/*
 * Clear the screen with a solid color
 */
void platform_vita_clear_screen(uint32_t color) {
    int i;
    if (vita_fb != NULL) {
        for (i = 0; i < VITA_SCREEN_WIDTH * VITA_SCREEN_HEIGHT; i++) {
            vita_fb[i] = color;
        }
    }
}

/*
 * 5x7 bitmap font (Latin-1, 96 printable ASCII glyphs + a few extras).
 * Each glyph is encoded as 7 uint8 rows; each byte has 5 LSBs used.
 * glyph_table[0] = ' ' (32), glyph_table[1] = '!' (33), ... up to '~' (126).
 * Characters >= 128 fall back to '?' (printable glyph 63 in the table).
 */
static const unsigned char glyph_table[][5] = {
    /* 32 ' ' */ {0,0,0,0,0},
    /* 33 '!' */ {0,0,0x5F,0,0},
    /* 34 '"' */ {0,0x07,0,0x07,0},
    /* 35 '#' */ {0x14,0x7F,0x14,0x7F,0x14},
    /* 36 '$' */ {0x24,0x2A,0x7F,0x2A,0x12},
    /* 37 '%' */ {0x23,0x13,0x08,0x64,0x62},
    /* 38 '&' */ {0x36,0x49,0x55,0x22,0x50},
    /* 39 '\''*/ {0,0,0x05,0x03,0},
    /* 40 '(' */ {0,0x1C,0x22,0x41,0},
    /* 41 ')' */ {0,0x41,0x22,0x1C,0},
    /* 42 '*' */ {0x08,0x2A,0x1C,0x2A,0x08},
    /* 43 '+' */ {0x08,0x08,0x3E,0x08,0x08},
    /* 44 ',' */ {0,0x50,0x30,0,0},
    /* 45 '-' */ {0x08,0x08,0x08,0x08,0x08},
    /* 46 '.' */ {0,0x60,0x60,0,0},
    /* 47 '/' */ {0x20,0x10,0x08,0x04,0x02},
    /* 48 '0' */ {0x3E,0x51,0x49,0x45,0x3E},
    /* 49 '1' */ {0,0x42,0x7F,0x40,0},
    /* 50 '2' */ {0x42,0x61,0x51,0x49,0x46},
    /* 51 '3' */ {0x21,0x41,0x45,0x4B,0x31},
    /* 52 '4' */ {0x18,0x14,0x12,0x7F,0x10},
    /* 53 '5' */ {0x27,0x45,0x45,0x45,0x39},
    /* 54 '6' */ {0x3C,0x4A,0x49,0x49,0x30},
    /* 55 '7' */ {0x01,0x71,0x09,0x05,0x03},
    /* 56 '8' */ {0x36,0x49,0x49,0x49,0x36},
    /* 57 '9' */ {0x06,0x49,0x49,0x29,0x1E},
    /* 58 ':' */ {0,0x36,0x36,0,0},
    /* 59 ';' */ {0,0x56,0x36,0,0},
    /* 60 '<' */ {0,0x08,0x14,0x22,0x41},
    /* 61 '=' */ {0x14,0x14,0x14,0x14,0x14},
    /* 62 '>' */ {0x41,0x22,0x14,0x08,0},
    /* 63 '?' */ {0x02,0x01,0x51,0x09,0x06},
    /* 64 '@' */ {0x32,0x49,0x79,0x41,0x3E},
    /* 65 'A' */ {0x7E,0x11,0x11,0x11,0x7E},
    /* 66 'B' */ {0x7F,0x49,0x49,0x49,0x36},
    /* 67 'C' */ {0x3E,0x41,0x41,0x41,0x22},
    /* 68 'D' */ {0x7F,0x41,0x41,0x22,0x1C},
    /* 69 'E' */ {0x7F,0x49,0x49,0x49,0x41},
    /* 70 'F' */ {0x7F,0x09,0x09,0x01,0x01},
    /* 71 'G' */ {0x3E,0x41,0x41,0x51,0x32},
    /* 72 'H' */ {0x7F,0x08,0x08,0x08,0x7F},
    /* 73 'I' */ {0,0x41,0x7F,0x41,0},
    /* 74 'J' */ {0x20,0x40,0x41,0x3F,0x01},
    /* 75 'K' */ {0x7F,0x08,0x14,0x22,0x41},
    /* 76 'L' */ {0x7F,0x40,0x40,0x40,0x40},
    /* 77 'M' */ {0x7F,0x02,0x04,0x02,0x7F},
    /* 78 'N' */ {0x7F,0x04,0x08,0x10,0x7F},
    /* 79 'O' */ {0x3E,0x41,0x41,0x41,0x3E},
    /* 80 'P' */ {0x7F,0x09,0x09,0x09,0x06},
    /* 81 'Q' */ {0x3E,0x41,0x51,0x21,0x5E},
    /* 82 'R' */ {0x7F,0x09,0x19,0x29,0x46},
    /* 83 'S' */ {0x46,0x49,0x49,0x49,0x31},
    /* 84 'T' */ {0x01,0x01,0x7F,0x01,0x01},
    /* 85 'U' */ {0x3F,0x40,0x40,0x40,0x3F},
    /* 86 'V' */ {0x1F,0x20,0x40,0x20,0x1F},
    /* 87 'W' */ {0x7F,0x20,0x18,0x20,0x7F},
    /* 88 'X' */ {0x63,0x14,0x08,0x14,0x63},
    /* 89 'Y' */ {0x03,0x04,0x78,0x04,0x03},
    /* 90 'Z' */ {0x61,0x51,0x49,0x45,0x43},
    /* 91 '[' */ {0,0x7F,0x41,0x41,0},
    /* 92 '\\'*/ {0x02,0x04,0x08,0x10,0x20},
    /* 93 ']' */ {0,0x41,0x41,0x7F,0},
    /* 94 '^' */ {0x04,0x02,0x01,0x02,0x04},
    /* 95 '_' */ {0x40,0x40,0x40,0x40,0x40},
    /* 96 '`' */ {0,0x01,0x02,0x04,0},
    /* 97 'a' */ {0x20,0x54,0x54,0x54,0x78},
    /* 98 'b' */ {0x7F,0x48,0x44,0x44,0x38},
    /* 99 'c' */ {0x38,0x44,0x44,0x44,0x20},
    /* 100 'd'*/ {0x38,0x44,0x44,0x48,0x7F},
    /* 101 'e'*/ {0x38,0x54,0x54,0x54,0x18},
    /* 102 'f'*/ {0x08,0x7E,0x09,0x01,0x02},
    /* 103 'g'*/ {0x08,0x14,0x54,0x54,0x3C},
    /* 104 'h'*/ {0x7F,0x08,0x04,0x04,0x78},
    /* 105 'i'*/ {0,0x44,0x7D,0x40,0},
    /* 106 'j'*/ {0x20,0x40,0x44,0x3D,0},
    /* 107 'k'*/ {0x00,0x7F,0x10,0x28,0x44},
    /* 108 'l'*/ {0x00,0x41,0x7F,0x40,0},
    /* 109 'm'*/ {0x7C,0x04,0x18,0x04,0x78},
    /* 110 'n'*/ {0x7C,0x08,0x04,0x04,0x78},
    /* 111 'o'*/ {0x38,0x44,0x44,0x44,0x38},
    /* 112 'p'*/ {0x7C,0x14,0x14,0x14,0x08},
    /* 113 'q'*/ {0x08,0x14,0x14,0x18,0x7C},
    /* 114 'r'*/ {0x7C,0x08,0x04,0x04,0x08},
    /* 115 's'*/ {0x48,0x54,0x54,0x54,0x20},
    /* 116 't'*/ {0x04,0x3F,0x44,0x40,0x20},
    /* 117 'u'*/ {0x3C,0x40,0x40,0x20,0x7C},
    /* 118 'v'*/ {0x1C,0x20,0x40,0x20,0x1C},
    /* 119 'w'*/ {0x3C,0x40,0x30,0x40,0x3C},
    /* 120 'x'*/ {0x44,0x28,0x10,0x28,0x44},
    /* 121 'y'*/ {0x0C,0x50,0x50,0x50,0x3C},
    /* 122 'z'*/ {0x44,0x64,0x54,0x4C,0x44},
    /* 123 '{'*/ {0,0x08,0x36,0x41,0},
    /* 124 '|'*/ {0,0,0x7F,0,0},
    /* 125 '}'*/ {0,0x41,0x36,0x08,0},
    /* 126 '~'*/ {0x10,0x08,0x08,0x10,0x10}
};

/*
 * Render a single 5x7 character at (x,y) in the supplied ARGB color.
 * Caller is responsible for passing a sensible framebuffer.
 */
void platform_vita_draw_char(uint32_t *fb, int fbW, int fbH,
                             uint32_t argb, int x, int y, unsigned short ch) {
    if (fb == NULL) return;
    if (ch < 32 || ch > 126) ch = '?';
    const unsigned char *g = glyph_table[ch - 32];
    for (int row = 0; row < 7; row++) {
        int py = y + row;
        if (py < 0 || py >= fbH) continue;
        for (int col = 0; col < 5; col++) {
            if ((g[col] >> row) & 1) {
                int px = x + col;
                if (px < 0 || px >= fbW) continue;
                fb[py * fbW + px] = argb;
            }
        }
    }
}