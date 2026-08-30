/*
 * screen_text.h - simple bitmap font rendering to Vita framebuffer
 * No dependencies on Gxm or vita2d. Uses SceDisplay directly.
 */

#ifndef SCREEN_TEXT_H
#define SCREEN_TEXT_H

#include <stdint.h>
#include <psp2/display.h>

#define FB_WIDTH  960
#define FB_HEIGHT 544
#define FB_STRIDE (FB_WIDTH * 4)  /* RGBA8888 */

extern uint32_t *g_fb;   /* framebuffer pointer */
extern int g_fb_initialized;

/* Init framebuffer to clear color, returns 0 on success */
int screen_init(uint32_t clear_color);

/* Wait vsync and swap */
void screen_flip(void);

/* Draw a single character at (x,y) with color. 8x8 bitmap font. */
void screen_putc(int x, int y, char c, uint32_t color);

/* Draw a null-terminated string at (x,y) with color. */
void screen_puts(int x, int y, const char *s, uint32_t color);

/* Draw a 32-bit int as decimal at (x,y) */
void screen_puti(int x, int y, int v, uint32_t color);

/* Draw filled rectangle (x,y,w,h,color) */
void screen_fill_rect(int x, int y, int w, int h, uint32_t color);

#endif
