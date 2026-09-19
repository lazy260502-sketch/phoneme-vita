/*
 * platform_vita.h - PS Vita platform abstraction layer for MIDP
 *
 * Purpose: Provide Vita-specific types and functions for MIDP native code
 *          This header bridges the gap between phoneME's platform abstraction
 *          and the PS Vita SDK.
 */

#ifndef PLATFORM_VITA_H
#define PLATFORM_VITA_H

#include <psp2/types.h>
#include <psp2/ctrl.h>
#include <psp2/display.h>
#include <psp2/gxm.h>
#include <psp2/kernel/threadmgr.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Screen dimensions */
#define VITA_SCREEN_WIDTH   960
#define VITA_SCREEN_HEIGHT  544

/*
 * Shared framebuffer (Vita ARGB32 byte order: 0xAARRGGBB on display)
 * Exposed so that gxj_graphics_vita.c and lcdui_display_vita.c write into
 * the same buffer that platform_vita_swap_buffers() blits to the screen.
 * NULL until platform_vita_init() has run.
 */
extern uint32_t *vita_fb;

/* Accessor used by the native bridge. */
uint32_t *platform_vita_get_fb(void);
int platform_vita_get_fb_width(void);
int platform_vita_get_fb_height(void);

/* J2ME key codes (from Canvas.java) */
#define KEY_NUM0        48
#define KEY_NUM1        49
#define KEY_NUM2        50
#define KEY_NUM3        51
#define KEY_NUM4        52
#define KEY_NUM5        53
#define KEY_NUM6        54
#define KEY_NUM7        55
#define KEY_NUM8        56
#define KEY_NUM9        57
#define KEY_STAR        42
#define KEY_POUND       35
#define KEY_UP           1
#define KEY_DOWN         2
#define KEY_LEFT         3
#define KEY_RIGHT        4
#define KEY_FIRE         5
#define KEY_SOFT1       -1
#define KEY_SOFT2       -2
#define GAME_A          -3
#define GAME_B          -4
#define GAME_C          -5
#define GAME_D          -6

/* Platform initialization */
int platform_vita_init(void);
void platform_vita_shutdown(void);

/* Display functions */
void platform_vita_display_init(void);
void platform_vita_swap_buffers(void);

/* Input functions */
int platform_vita_input_init(void);
void platform_vita_poll_input(void);
int platform_vita_get_key_state(int keyCode);

/* Graphics functions */
void platform_vita_draw_pixel(int x, int y, uint32_t color);
void platform_vita_draw_rect(int x, int y, int w, int h, uint32_t color);
void platform_vita_clear_screen(uint32_t color);

/*
 * Software 5x7 bitmap font (Latin-1 only). Used by the native bridge
 * to render text until a real font system is wired in.
 */
void platform_vita_draw_char(uint32_t *fb, int fbW, int fbH,
                             uint32_t argb, int x, int y, unsigned short ch);

#ifdef __cplusplus
}
#endif

#endif /* PLATFORM_VITA_H */