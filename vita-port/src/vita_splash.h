/*
 * vita_splash.h - native "loading" screen shown while a round boots (v02.99).
 *
 * P1-2 of COMPATIBILITY_ROADMAP.md: the window from round_begin to the
 * first real Java frame is 7-11 s on real hardware, and until now the only
 * thing on the panel was a WHITE frame - lfjport_ui_init() deliberately
 * memset the virtual screen to 0xFFFF "to show something immediately".
 * log120's own measurements say where that time goes (preload 5.9-7.3 s,
 * of which io_read 4.8-6.4 s across ~4100 sceIoRead calls), so the fix is
 * presentation, not another loading-speed micro-optimisation: show what is
 * happening instead of white.
 *
 * Design constraints learned the hard way in this port:
 *   - ONE submit path. The splash is drawn as an overlay inside
 *     flip_to_display(), the same way the virtual keypad and the
 *     screenshot banner are. A second sceDisplaySetFrameBuf path is what
 *     broke the IME in log109 (v02.63) and is not repeated here.
 *   - The overlay is painted into the PHYSICAL framebuffer, so it does not
 *     touch vita_screen_pixels. That matters: frame_has_content() samples
 *     the virtual screen to decide when the white screen ended, and the
 *     splash must not be able to fake that measurement.
 *   - Repaints are throttled and driven from the preload loop (the VM
 *     thread that is doing the work), so no new thread and no lock.
 *   - The font bank is already loaded by the menu (it draws CJK game
 *     names), so using it here costs nothing. If it is missing the panel
 *     and the progress bar still draw, so the user never sees white.
 */

#ifndef VITA_SPLASH_H
#define VITA_SPLASH_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Activate the splash for one round. `title` is the human name shown big
 * (normally the game directory name). Safe to call before the display
 * layer has a framebuffer - drawing starts once it has one. */
void vita_splash_begin(const char *title);

/* Update the small line under the title ("读取游戏…", "初始化虚拟机…").
 * Cheap: stores the string; the next tick repaints. */
void vita_splash_stage(const char *stage);

/* Advance the running counter shown next to the bar. Called once per
 * class entry by the preload loop, so it must stay a store + compare. */
void vita_splash_entries(int entries);

/* Throttled repaint request. Called from the preload loop; returns
 * immediately unless SPLASH_MIN_MS have passed since the last paint. */
void vita_splash_tick(void);

/* Turn the splash off (first real content frame, round end, or the hard
 * timeout). Idempotent. */
void vita_splash_end(void);

int vita_splash_active(void);

/* Report a frame that the MIDlet (not us) produced. Returns 1 when this
 * is the frame that ended the splash, so the caller knows it has to wipe
 * the pixels the overlay left in the letterbox bars. Frames are ignored
 * until the class preload has FINISHED: the display layer's own first
 * flip and any MIDP warm-up refresh during the sweep must not end it. */
int vita_splash_on_foreign_flip(void);

/* The class preload sweep is over (called from JVM.cpp). From here on a
 * frame we did not request is the MIDlet drawing. */
void vita_splash_preload_done(void);

/* Non-zero once the preload sweep has visited at least one class. */
int vita_splash_preload_started(void);

/* TRUE while the splash owns the screen, i.e. other overlay tickers
 * (virtual keypad flash, screenshot banner) must stay out of the way
 * instead of triggering their own repaints. */
int vita_splash_owns_screen(void);

/* Overlay painter, called by vita_display.c from flip_to_display().
 * Any of the arguments may be used freely - it fills the whole frame. */
void vita_splash_draw(uint32_t *fb, int w, int h);

#ifdef __cplusplus
}
#endif

#endif /* VITA_SPLASH_H */
