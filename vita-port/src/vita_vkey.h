/*
 * vita_vkey.h - on-screen virtual keypad (see vita_vkey.c).
 *
 * The buttons are laid out in the letterbox bars next to the game (or
 * blended over it when the game fills the panel). A touch that lands on
 * a button is turned into a normal KEYMAP_KEY_* event by vita_input.c,
 * so nothing in MIDP has to know the keypad exists.
 */
#ifndef VITA_VKEY_H
#define VITA_VKEY_H

int vita_vkey_enabled(void);
void vita_vkey_set_enabled(int on);

/* 0 = auto (bars when letterboxed, overlay otherwise), 1 = always
 * overlay, 2 = bars only. */
void vita_vkey_set_mode(int m);
int vita_vkey_mode(void);

/* Read/write ux0:/data/J2ME00001/vkey.cfg ("on=0|1", "mode=0|1|2").
 * vita_vkey_load returns the resulting enable flag. */
int vita_vkey_load(const char *path);
void vita_vkey_save(const char *path);

/* Draw over an already-blitted frame. gx/gy/gw/gh = game rectangle. */
void vita_vkey_draw(unsigned int *fb, int gx, int gy, int gw, int gh);

/* Tell the keypad where the game is before the first frame. */
void vita_vkey_set_geometry(int gx, int gy, int gw, int gh);

/* Physical coordinate -> KEYMAP key. Returns 0 when outside any button.
 * `btn` (may be NULL) receives the button index for press/release
 * tracking. */
int vita_vkey_hit(int px, int py, int *key, int *btn);

/* v02.89: which button is currently held (-1 = none). Purely visual -
 * the key event itself is still emitted by vita_input.c; this only makes
 * draw() highlight the key so the player gets feedback. */
void vita_vkey_set_pressed(int btn);
int  vita_vkey_pressed(void);

/* v02.95: 1 while a tap should stay highlighted - either a button is held
 * or the short flash that follows a tap has not expired yet. The display
 * layer repaints while this is true (vita_display_vkey_tick), which is what
 * makes the feedback visible in a game that only repaints on input. */
int  vita_vkey_flash_active(void);

/* v04.33: keypad pages. 0 = digits + "abc", 1 = letters A-Z + "123".
 * A letter key is emitted as its ASCII code ('a'+i), so MIDP needs no new
 * key constants. */
int  vita_vkey_page(void);
void vita_vkey_set_page(int p);

#endif /* VITA_VKEY_H */
