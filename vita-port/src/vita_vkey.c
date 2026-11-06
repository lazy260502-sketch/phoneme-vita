/*
 * vita_vkey.c - on-screen virtual keypad for phoneME MIDP on PS Vita
 *
 * Why: a Vita pad has 12 usable buttons and they are all consumed by
 * directions + soft keys + game keys. A J2ME game that wants the digit
 * row (or that reads 1..9 as shortcuts) simply cannot be driven, because
 * the only way to reach '7' was to remap a physical button and lose
 * whatever it did before.
 *
 * What: two button panels drawn straight into the framebuffer that the
 * MIDP screen was just blitted into (vita_display.c calls
 * vita_vkey_draw() after the blit). Touch hits are converted to ordinary
 * KEYMAP_KEY_* press/release events by vita_input.c, so the whole MIDP
 * input path is unchanged - nothing downstream knows the difference
 * between a virtual '5' and a remapped physical button.
 *
 * Placement: a virtual phone screen is almost never 16:9, so the display
 * has vertical letterbox bars on both sides (240x320 -> a 407 px wide
 * column on a 960 px panel leaves ~276 px per side). The digit panel
 * goes in the left bar and the action panel in the right one, i.e. it
 * costs ZERO game pixels. Only when the game is stretched over the full
 * panel do the panels overlap it, and then they are blended so the game
 * stays readable underneath.
 *
 * Alpha note: the scan-out framebuffer is A8B8G8R8 but the display
 * controller ignores alpha (see the note in vita_display.c), so real
 * transparency is impossible. "Blending" here means reading the pixel
 * that is already in the back buffer and mixing the panel colour into
 * it - which is exact, just done on the CPU.
 */
#include <psp2/io/fcntl.h>
#include <psp2/kernel/processmgr.h>   /* sceKernelGetProcessTimeWide */
#include <stdio.h>
#include <string.h>

/* KEYMAP_KEY_* / KEYMAP_STATE_* come from the MIDP input layer - the
 * keypad emits exactly the same codes as a physical button, which is
 * what keeps vita_input.c and everything downstream unchanged.
 * kni.h must come first (keymap_input.h relies on its types). */
#include <kni.h>
#include <keymap_input.h>

#include "vita_vkey.h"

#define SCREEN_W 960
#define SCREEN_H 544
/* v04.45: the diagnostics overlay sits in the TOP-LEFT corner (2,2), which is
 * exactly where the bar-mode panels put their top row (soft keys at y=8, the
 * action d-pad at y=60). Shifting every bar-mode panel down by 20 px keeps the
 * keypad clear of it without moving anything into the bottom edge (the digit
 * panel is re-clamped below). Overlay mode (panels inside the game rectangle)
 * is not shifted: it never reaches the top-left corner. */
#define VKEY_Y_SHIFT 20

/* Config: a single file with "on=0|1" and "mode=0|1|2".
 *   mode 0 = auto   : bars when the game is letterboxed, overlay otherwise
 *   mode 1 = always : always overlay (blended over the game)
 *   mode 2 = bars   : only draw when there is a bar wide enough
 * The launcher does not need to know the value; the menu writes it and
 * the per-game game.cfg may carry "vkey=on|off" which vita_main passes
 * here before the VM starts. */
#define VKEY_CFG "ux0:/data/J2ME00001/vkey.cfg"

static int enabled = 0;
static int mode = 0;
/* v02.89: index of the button currently held by the player (-1 = none).
 * Written by vita_input.c from the touch poll, read by draw() on the
 * flip thread, hence volatile. Purely visual: the key event itself is
 * emitted by vita_input.c as before. */
static volatile int pressed_btn = -1;

/* v02.95: a tap must be VISIBLE even when the finger was already gone by
 * the time the next frame was drawn.
 *
 * v02.89 only highlighted the button while the finger was down, and a
 * frame is only drawn when Java calls lfjport_refresh(). A menu-driven
 * game does exactly one repaint *after* it receives the key - by then the
 * finger is up and the highlight was already cleared, so the player saw
 * nothing at all (the report that started this change).
 *
 * So a press now latches a short "flash": the button stays highlighted
 * for VKEY_FLASH_MS regardless of the finger, and vita_display.c forces a
 * repaint while that window is open (vita_display_vkey_tick). Holding the
 * button keeps it highlighted for as long as it is held. */
#define VKEY_FLASH_MS 260
/* Hard stop for the case where nothing ever draws (a game stuck inside a
 * key handler). Without it the flash would keep the event pump repainting
 * for as long as the press is uncommitted. */
#define VKEY_FLASH_MAX_MS 1200
static volatile int flash_btn = -1;
static volatile unsigned long long flash_press_us = 0;
/* 0 = the flash has not been painted yet, so the 260 ms countdown has not
 * started. draw() starts it on the first frame that shows the highlight,
 * which guarantees the player actually sees it even if the game took a
 * while to repaint (see the comment above VKEY_FLASH_MS). */
static volatile unsigned long long flash_deadline_us = 0;

/* Buttons as laid out by the last draw/hit call. Geometry depends only
 * on the game rectangle, which is stable for a whole round, so hit
 * testing can reuse the list the last draw built. */
typedef struct {
    int x, y, w, h;
    int key;            /* KEYMAP_KEY_* */
    const char *label;  /* NULL = draw a triangle for this direction */
    int tri;            /* 0 none, else 1=up 2=down 3=left 4=right */
} VKeyBtn;

#define VKEY_MAX_BTN 48
static VKeyBtn btns[VKEY_MAX_BTN];
static int btn_count = 0;
static int layout_valid = 0;
static int layout_geom[4] = { -1, -1, -1, -1 };  /* gx, gy, gw, gh */

/* v04.33: two pages. Page 0 is the digit panel with an "abc" button at the
 * bottom, page 1 is a 4x7 letter panel with a "123" button. The page button
 * is consumed inside vita_vkey_hit() (it flips `page`, invalidates the
 * layout and returns 0), so vita_input.c and everything downstream stay
 * unaware that pages exist: a letter key is emitted as the ordinary ASCII
 * key code ('a' + i), exactly what Canvas.keyPressed() expects.
 *
 * The page button's key value must never collide with KEYMAP_KEY_* (-24..0)
 * or with a printable code, hence -200. */
#define VKEY_KEY_PAGE (-200)
#define ALPHA_COLS 4
#define ALPHA_ROWS 7
static int page = 0;

/* v04.50: the panel is NOT repainted every frame.
 *
 * vita_display.c calls clear_letterbox() only when a splash or toast ends
 * (see its `if (g_splash_clear > 0 || g_toast_clear > 0)`), while
 * vita_vkey_draw() runs every frame. While the page never changes that is
 * harmless - a button repaints exactly over itself - but the moment the page
 * flips, the buttons of the page just left stay on the bars and the two pages
 * appear stacked ("切换后字母和数字重叠").
 *
 * So remember what was drawn and, on a page change, wipe the bounding box of
 * it before drawing the new page. The wipe is repeated for TWO frames because
 * the display is double buffered: the frame right after a flip draws into the
 * buffer that still holds the old page.
 *
 * C_CLEAR is opaque black on purpose: in bar mode the bars ARE black, and in
 * overlay mode the next frame's game blit covers the box again immediately. */
#define C_CLEAR 0xFF000000u
static VKeyBtn prev_btns[VKEY_MAX_BTN];
static int prev_count = 0;
static int drawn_page = -1;          /* page the remembered buttons belong to */
static int erase_box[4] = { 0, 0, 0, 0 };
static int erase_frames = 0;         /* frames left to wipe erase_box */

/* ------------------------------------------------------------------ */
/* Tiny 5x7 font: only what the labels need.                          */
/* ------------------------------------------------------------------ */
typedef struct { char ch; unsigned char rows[7]; } VKeyGlyph;

static const VKeyGlyph glyphs[] = {
    { '0', { 0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E } },
    { '1', { 0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E } },
    { '2', { 0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F } },
    { '3', { 0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E } },
    { '4', { 0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02 } },
    { '5', { 0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E } },
    { '6', { 0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E } },
    { '7', { 0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08 } },
    { '8', { 0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E } },
    { '9', { 0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C } },
    { '*', { 0x00, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x00 } },
    { '#', { 0x0A, 0x0A, 0x1F, 0x0A, 0x1F, 0x0A, 0x0A } },
    { 'A', { 0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11 } },
    { 'B', { 0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E } },
    { 'C', { 0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E } },
    { 'D', { 0x1C, 0x12, 0x11, 0x11, 0x11, 0x12, 0x1C } },
    { 'K', { 0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11 } },
    { 'O', { 0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E } },
    { 'S', { 0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E } },
    /* v02.89: labels for the two soft keys (left / right). */
    { 'L', { 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F } },
    { 'R', { 0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11 } },
    /* v04.33: the rest of the alphabet for the letter page. A-D/K/O/S/L/R
     * above are reused; these complete A-Z. */
    { 'E', { 0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F } },
    { 'F', { 0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10 } },
    { 'G', { 0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0F } },
    { 'H', { 0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11 } },
    { 'I', { 0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E } },
    { 'J', { 0x07, 0x02, 0x02, 0x02, 0x02, 0x12, 0x0C } },
    { 'M', { 0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11 } },
    { 'N', { 0x11, 0x19, 0x15, 0x13, 0x11, 0x11, 0x11 } },
    { 'P', { 0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10 } },
    { 'Q', { 0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D } },
    { 'T', { 0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04 } },
    { 'U', { 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E } },
    { 'V', { 0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04 } },
    { 'W', { 0x11, 0x11, 0x11, 0x15, 0x15, 0x15, 0x0A } },
    { 'X', { 0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11 } },
    { 'Y', { 0x11, 0x11, 0x0A, 0x04, 0x04, 0x04, 0x04 } },
    { 'Z', { 0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F } }
};
#define GLYPH_N ((int)(sizeof(glyphs) / sizeof(glyphs[0])))

static const VKeyGlyph *glyph_find(char c) {
    int i;
    if (c >= 'a' && c <= 'z') {
        c = (char)(c - 'a' + 'A');
    }
    for (i = 0; i < GLYPH_N; i++) {
        if (glyphs[i].ch == c) {
            return &glyphs[i];
        }
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Pixel helpers                                                      */
/* ------------------------------------------------------------------ */
static inline unsigned int get_px(const unsigned int *fb, int x, int y) {
    return fb[y * SCREEN_W + x];
}

static inline void put_px(unsigned int *fb, int x, int y, unsigned int c) {
    if (x >= 0 && x < SCREEN_W && y >= 0 && y < SCREEN_H) {
        fb[y * SCREEN_W + x] = c;
    }
}

/* Mix `c` into the existing pixel with weight num/256. */
static inline void blend_px(unsigned int *fb, int x, int y,
                            unsigned int c, int num) {
    unsigned int d, out;
    int r, g, b;
    if (x < 0 || x >= SCREEN_W || y < 0 || y >= SCREEN_H) {
        return;
    }
    d = fb[y * SCREEN_W + x];
    r = (int)(d & 0xFF) * (256 - num) + (int)(c & 0xFF) * num;
    g = (int)((d >> 8) & 0xFF) * (256 - num) + (int)((c >> 8) & 0xFF) * num;
    b = (int)((d >> 16) & 0xFF) * (256 - num) + (int)((c >> 16) & 0xFF) * num;
    out = 0xFF000000u |
          (((unsigned int)(b >> 8) & 0xFF) << 16) |
          (((unsigned int)(g >> 8) & 0xFF) << 8) |
          ((unsigned int)(r >> 8) & 0xFF);
    fb[y * SCREEN_W + x] = out;
}

/* A filled rectangle. `opaque` = 1 writes the colour, otherwise it is
 * blended into whatever the game just drew. */
static void rect(unsigned int *fb, int x, int y, int w, int h,
                 unsigned int color, int opaque, int alpha) {
    int r, c;
    for (r = 0; r < h; r++) {
        for (c = 0; c < w; c++) {
            if (opaque) {
                put_px(fb, x + c, y + r, color);
            } else {
                blend_px(fb, x + c, y + r, color, alpha);
            }
        }
    }
}

static void frame_rect(unsigned int *fb, int x, int y, int w, int h,
                       unsigned int color) {
    int c;
    for (c = 0; c < w; c++) {
        put_px(fb, x + c, y, color);
        put_px(fb, x + c, y + h - 1, color);
    }
    for (c = 0; c < h; c++) {
        put_px(fb, x, y + c, color);
        put_px(fb, x + w - 1, y + c, color);
    }
}

static void draw_glyph(unsigned int *fb, int x, int y, char ch, int scale,
                       unsigned int color) {
    const VKeyGlyph *g = glyph_find(ch);
    int r, c, sx, sy;
    if (g == NULL) {
        return;
    }
    for (r = 0; r < 7; r++) {
        unsigned char row = g->rows[r];
        for (c = 0; c < 5; c++) {
            if (row & (1 << (4 - c))) {
                for (sy = 0; sy < scale; sy++) {
                    for (sx = 0; sx < scale; sx++) {
                        put_px(fb, x + c * scale + sx, y + r * scale + sy,
                               color);
                    }
                }
            }
        }
    }
}

/* Centered one-line label. */
static void draw_label(unsigned int *fb, int cx, int cy, const char *s,
                       int scale, unsigned int color) {
    int len = (int)strlen(s);
    int w = len * 6 * scale - scale;
    int h = 7 * scale;
    int x = cx - w / 2;
    int y = cy - h / 2;
    int i;
    for (i = 0; i < len; i++) {
        draw_glyph(fb, x + i * 6 * scale, y, s[i], scale, color);
    }
}

/* Direction triangle. dir: 1=up 2=down 3=left 4=right */
static void draw_triangle(unsigned int *fb, int cx, int cy, int size,
                          int dir, unsigned int color) {
    int i;
    for (i = 0; i < size; i++) {
        int half = (size - i) / 2;
        int j;
        for (j = -half; j <= half; j++) {
            if (dir == 1) {
                put_px(fb, cx + j, cy - size / 2 + i, color);
            } else if (dir == 2) {
                put_px(fb, cx + j, cy + size / 2 - i, color);
            } else if (dir == 3) {
                put_px(fb, cx - size / 2 + i, cy + j, color);
            } else {
                put_px(fb, cx + size / 2 - i, cy + j, color);
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* Layout                                                             */
/* ------------------------------------------------------------------ */
/* Colours: the bars are already black, so the opaque variants look the
 * same there and cost nothing. Over the game the same colours are
 * blended to keep it visible. */
#define C_PANEL 0xFF201820u   /* ABGR: dark, slightly warm */
#define C_BTN   0xFF3A3540u
#define C_EDGE  0xFF8A8490u
#define C_TEXT  0xFFFFFFFFu
/* v02.89: held-down colours. Warm amber reads clearly over both the
 * black bars and a colourful game frame, and the dark text on it is the
 * same "inverted" look a physical keypad overlay uses. */
#define C_DOWN  0xFF30A0F0u   /* ABGR: R=0xF0 G=0xA0 B=0x30 */
#define C_DTEXT 0xFF101018u
#define C_DEDGE 0xFFFFFFFFu

static void push_btn(int x, int y, int w, int h, int key, const char *label,
                     int tri) {
    if (btn_count >= VKEY_MAX_BTN) {
        return;
    }
    btns[btn_count].x = x;
    btns[btn_count].y = y;
    btns[btn_count].w = w;
    btns[btn_count].h = h;
    btns[btn_count].key = key;
    btns[btn_count].label = label;
    btns[btn_count].tri = tri;
    btn_count++;
}

/* Digit panel: 3 columns x 4 rows. Returns the panel width needed. */
#define DIGIT_COLS 3
#define DIGIT_ROWS 4
static void layout_digits(int x0, int y0, int bw, int bh, int gap) {
    static const char *labels[DIGIT_ROWS][DIGIT_COLS] = {
        { "1", "2", "3" },
        { "4", "5", "6" },
        { "7", "8", "9" },
        { "*", "0", "#" }
    };
    static const int keys[DIGIT_ROWS][DIGIT_COLS] = {
        { KEYMAP_KEY_1, KEYMAP_KEY_2, KEYMAP_KEY_3 },
        { KEYMAP_KEY_4, KEYMAP_KEY_5, KEYMAP_KEY_6 },
        { KEYMAP_KEY_7, KEYMAP_KEY_8, KEYMAP_KEY_9 },
        { KEYMAP_KEY_ASTERISK, KEYMAP_KEY_0, KEYMAP_KEY_POUND }
    };
    int r, c;
    for (r = 0; r < DIGIT_ROWS; r++) {
        for (c = 0; c < DIGIT_COLS; c++) {
            push_btn(x0 + c * (bw + gap), y0 + r * (bh + gap), bw, bh,
                     keys[r][c], labels[r][c], 0);
        }
    }
}

/* v04.33: letter page. 4 columns x 7 rows = 26 letters + a "123" page
 * button. `key` is the ASCII code ('a'..'z') so Canvas.keyPressed() sees an
 * ordinary letter; the label is the upper-case glyph (draw_label() ->
 * glyph_find() folds lower case to upper). Space/Backspace stay on the digit
 * page, which is where a game that names a save file needs them anyway. */
static const char *alpha_labels[26] = {
    "A", "B", "C", "D", "E", "F", "G", "H", "I", "J", "K", "L", "M",
    "N", "O", "P", "Q", "R", "S", "T", "U", "V", "W", "X", "Y", "Z"
};

static void layout_alpha(int x0, int y0, int bw, int bh, int gap) {
    int i;

    for (i = 0; i < 26; i++) {
        int r = i / ALPHA_COLS;
        int c = i % ALPHA_COLS;
        push_btn(x0 + c * (bw + gap), y0 + r * (bh + gap), bw, bh,
                 'a' + i, alpha_labels[i], 0);
    }
    /* v04.48: the page button goes in the FREE CELLS of the last row, not at
     * x0. With 26 letters on 4 columns the last row (r = 6) holds only Y (c=0)
     * and Z (c=1); c=2 and c=3 are empty, so "123" spans exactly those two.
     *
     * Before this fix the button was pushed at x0 with a width of 2 cells, i.e.
     * it covered Y and Z completely. Two symptoms on the device, both from that
     * one wrong x:
     *   - visually the "123" key drew on top of the Y and Z keys;
     *   - every tap there hit an earlier entry in btns[] (Y or Z are pushed
     *     first), so the page button was unreachable and tapping "123" sent
     *     'y' instead - i.e. "clicking the digit key does not switch back".
     * y is right (row 6) and ALPHA_ROWS = 7 already counts this row. */
    push_btn(x0 + 2 * (bw + gap), y0 + 6 * (bh + gap), 2 * bw + gap, bh,
             VKEY_KEY_PAGE, "123", 0);
}

static int page_cols(void) {
    return page ? ALPHA_COLS : DIGIT_COLS;
}

/* Digit page needs one extra row for its "abc" button. */
static int page_rows(void) {
    return page ? ALPHA_ROWS : DIGIT_ROWS + 1;
}

/* Lay out the current page at (x0,y0); returns the rows it used. */
static int layout_page(int x0, int y0, int bw, int bh, int gap) {
    if (page == 0) {
        layout_digits(x0, y0, bw, bh, gap);
        push_btn(x0, y0 + DIGIT_ROWS * (bh + gap),
                 DIGIT_COLS * bw + (DIGIT_COLS - 1) * gap, bh,
                 VKEY_KEY_PAGE, "abc", 0);
        return DIGIT_ROWS + 1;
    }
    layout_alpha(x0, y0, bw, bh, gap);
    return ALPHA_ROWS;
}

/* Action panel: d-pad cross, A/B/C/D block, two soft keys. */
static void layout_actions(int x0, int y0, int unit, int gap) {
    int cxs = x0 + unit + gap + unit / 2;  /* centre of the 3x3 d-pad */
    int cys = y0 + unit + gap + unit / 2;
    int k;
    int bx, by;

    /* d-pad: up / down / left / right around a dead centre */
    push_btn(cxs - unit / 2, cys - unit / 2 - (unit + gap), unit, unit,
             KEYMAP_KEY_UP, NULL, 1);
    push_btn(cxs - unit / 2, cys - unit / 2 + (unit + gap), unit, unit,
             KEYMAP_KEY_DOWN, NULL, 2);
    push_btn(cxs - unit / 2 - (unit + gap), cys - unit / 2, unit, unit,
             KEYMAP_KEY_LEFT, NULL, 3);
    push_btn(cxs - unit / 2 + (unit + gap), cys - unit / 2, unit, unit,
             KEYMAP_KEY_RIGHT, NULL, 4);
    /* v04.21: FIRE in the centre cell. BioMain (and any title that reads the fire key) has no other way to confirm or
     * attack from the touch panel: its digit '5' is ignored by the game. The cell is one gap away from all four arms. */
    push_btn(cxs - unit / 2, cys - unit / 2, unit, unit, KEYMAP_KEY_SELECT, "OK", 0);

    /* A/B/C/D in a 2x2 block under the d-pad */
    by = y0 + 3 * unit + 2 * gap + gap;
    for (k = 0; k < 4; k++) {
        static const int keys[4] = {
            KEYMAP_KEY_GAMEA, KEYMAP_KEY_GAMEB,
            KEYMAP_KEY_GAMEC, KEYMAP_KEY_GAMED
        };
        static const char *labels[4] = { "A", "B", "C", "D" };
        bx = x0 + (k % 2) * (unit + gap);
        push_btn(bx, by + (k / 2) * (unit + gap), unit, unit,
                 keys[k], labels[k], 0);
    }
    /* v02.89: the two soft keys no longer live here - they are placed in
     * the top corners of the game by layout_soft_keys(), which is where a
     * phone puts them and where they never collide with either panel. */
}

/* v02.89: left/right soft keys in the top corners.
 *
 * Before this, the soft keys were two small "S1"/"S2" cells at the bottom
 * of the action block: easy to miss and only present when that block was
 * drawn at all. They are now a dedicated pair of wide, labelled buttons in
 * the top-left and top-right corner of the game rectangle (the bars when
 * the game is letterboxed), i.e. exactly where a feature phone has its
 * 左软键 / 右软键. Those corners are free in every layout: the digit panel
 * sits bottom-left (or centred in the left bar) and the action block
 * bottom-right (or from y=60 in the right bar). */
static void layout_soft_keys(int gx, int gy, int gw, int gh, int bar_mode) {
    int sw, sh, ly, lx, rx;

    if (bar_mode) {
        int left_w = gx;
        int right_w = SCREEN_W - (gx + gw);
        sw = 96;
        sh = 44;
        if (sw > left_w - 16) sw = left_w - 16;
        if (sw > right_w - 16) sw = right_w - 16;
        if (sw < 44) return;          /* no room in the bar */
        lx = (left_w - sw) / 2;
        rx = gx + gw + (right_w - sw) / 2;
        ly = 8 + VKEY_Y_SHIFT;
    } else {
        sw = 96;
        sh = 40;
        if (sw > gw / 3) sw = gw / 3;
        if (sw < 44) return;
        lx = gx + 8;
        rx = gx + gw - sw - 8;
        ly = gy + 8;
    }
    if (lx < 2) lx = 2;
    if (rx + sw > SCREEN_W - 2) rx = SCREEN_W - 2 - sw;
    push_btn(lx, ly, sw, sh, KEYMAP_KEY_SOFT1, "L", 0);
    push_btn(rx, ly, sw, sh, KEYMAP_KEY_SOFT2, "R", 0);
}

/* Build the button list for this game rectangle. Returns 1 when
 * anything is drawn at all.
 *
 * Width of the action block is 3*unit + 2*gap, not 2*unit + gap: the
 * d-pad is a 3x3 cross whose left and right arms stick out one unit
 * beyond the A/B/C/D block underneath. Getting this wrong pushed the
 * right arm off-screen at 960-34*3 (found by the layout test). */
static int build_layout(int gx, int gy, int gw, int gh) {
    int left_w = gx;
    int right_w = SCREEN_W - (gx + gw);
    int use_bars = (mode != 1);

    if (layout_valid && layout_geom[0] == gx && layout_geom[1] == gy &&
        layout_geom[2] == gw && layout_geom[3] == gh) {
        return btn_count > 0;
    }
    btn_count = 0;
    layout_valid = 1;
    layout_geom[0] = gx;
    layout_geom[1] = gy;
    layout_geom[2] = gw;
    layout_geom[3] = gh;
    /* A rebuild renumbers the buttons, so a stored press index would point
     * at a different key. The finger is treated as lifted. */
    pressed_btn = -1;
    flash_btn = -1;
    flash_press_us = 0;
    flash_deadline_us = 0;

    /* ---- keypad panel: digits (page 0) or letters (page 1) ---- */
    if (use_bars && left_w >= 200) {
        int gap = 8;
        int cols = page_cols();
        int rows = page_rows();
        int bw = (left_w - 2 * 12 - (cols - 1) * gap) / cols;
        int bh = bw;
        int max_bh = (SCREEN_H - 32 - (rows - 1) * gap) / rows;
        int total_w, total_h, x0, y0;

        /* The letter page has three more rows than the digit page, so on a
         * short bar its cells must shrink or the panel runs off the panel. */
        if (bh > max_bh) {
            bh = max_bh;
        }
        total_w = cols * bw + (cols - 1) * gap;
        total_h = rows * bh + (rows - 1) * gap;
        x0 = (left_w - total_w) / 2;
        y0 = (SCREEN_H - total_h) / 2 + VKEY_Y_SHIFT;
        /* Never let the shift push the panel off the bottom edge; the digit
         * page is 5 rows tall on a 544 px panel, so this only bites when the
         * cell size is clamped for a tall page. */
        if (y0 + total_h > SCREEN_H - 4) {
            y0 = SCREEN_H - total_h - 4;
        }
        if (bw > 30) {
            layout_page(x0, y0, bw, bh, gap);
        }
    } else if (mode != 2) {
        /* Overlay: bottom-left corner of the GAME rectangle, so it is
         * always visible and always where the user expects it. */
        int gap = 6;
        int cols = page_cols();
        int rows = page_rows();
        int bw = 46, bh = 46;
        int total_w, total_h, x0, y0;

        /* Fit inside the game rectangle; the letter page needs smaller
         * cells because it has 28 of them instead of 13. */
        while (bw > 24 && cols * bw + (cols - 1) * gap > gw - 20) {
            bw--;
        }
        while (bh > 24 && rows * bh + (rows - 1) * gap > gh - 20) {
            bh--;
        }
        total_w = cols * bw + (cols - 1) * gap;
        total_h = rows * bh + (rows - 1) * gap;
        x0 = gx + 10;
        y0 = gy + gh - total_h - 10;
        if (x0 + total_w > SCREEN_W - 4) {
            x0 = SCREEN_W - total_w - 4;
        }
        if (y0 < 4) {
            y0 = 4;
        }
        layout_page(x0, y0, bw, bh, gap);
    }

    /* ---- action panel: d-pad + A/B/C/D (v02.89: no soft keys) ---- */
    if (use_bars && right_w >= 200) {
        int gap = 8;
        int unit = 56;
        int block_w = 3 * unit + 2 * gap;
        int x0 = gx + gw + (right_w - block_w) / 2;
        int y0 = 60 + VKEY_Y_SHIFT;
        if (x0 + block_w > SCREEN_W) {
            x0 = SCREEN_W - block_w;
        }
        layout_actions(x0, y0, unit, gap);
    } else if (mode != 2) {
        int gap = 5, unit = 34;
        int block_w = 3 * unit + 2 * gap;
        int block_h = 3 * unit + 2 * gap        /* d-pad     */
                      + gap                      /* spacing   */
                      + 2 * (unit + gap) - gap;  /* A B C D   */
        int x0 = gx + gw - block_w - 10;
        int y0 = gy + gh - block_h - 10;
        if (x0 < gx + 4) {
            x0 = gx + 4;
        }
        if (y0 < 4) {
            y0 = 4;
        }
        layout_actions(x0, y0, unit, gap);
    }

    /* ---- v02.89: left/right soft keys in the top corners ---- */
    layout_soft_keys(gx, gy, gw, gh, use_bars);

    return btn_count > 0;
}

/* ------------------------------------------------------------------ */
/* Public API                                                         */
/* ------------------------------------------------------------------ */
int vita_vkey_enabled(void) {
    return enabled;
}

void vita_vkey_set_enabled(int on) {
    enabled = (on != 0);
    if (!enabled) {
        /* Nothing can be held while the keypad is hidden - and a stale
         * index would highlight the wrong key when it comes back. */
        pressed_btn = -1;
        flash_btn = -1;
        flash_press_us = 0;
        flash_deadline_us = 0;
    }
}

void vita_vkey_set_mode(int m) {
    if (m < 0 || m > 2) {
        m = 0;
    }
    mode = m;
    layout_valid = 0;
}

int vita_vkey_mode(void) {
    return mode;
}

/* Load "<dir>/vkey.cfg" (or the passed path). Returns 1 when the file
 * asked for the keypad, 0 otherwise; *mode_out may be NULL. */
int vita_vkey_load(const char *path) {
    char buf[128];
    SceUID fd;
    int n;
    int on = enabled;

    if (path == NULL) {
        path = VKEY_CFG;
    }
    fd = sceIoOpen(path, SCE_O_RDONLY, 0);
    if (fd < 0) {
        return enabled;
    }
    memset(buf, 0, sizeof(buf));
    n = sceIoRead(fd, buf, sizeof(buf) - 1);
    sceIoClose(fd);
    if (n <= 0) {
        return enabled;
    }
    buf[n] = '\0';

    {
        char *p, *nl;
        for (p = buf; p != NULL; p = (nl != NULL) ? nl + 1 : NULL) {
            nl = strpbrk(p, "\r\n");
            if (nl != NULL) {
                *nl = '\0';
            }
            if (strncmp(p, "on=", 3) == 0) {
                on = (p[3] == '1' || p[3] == 'y' || p[3] == 'Y');
                enabled = on;
            } else if (strncmp(p, "mode=", 5) == 0 &&
                       p[5] >= '0' && p[5] <= '2') {
                mode = p[5] - '0';
                layout_valid = 0;
            }
        }
    }
    return on;
}

void vita_vkey_save(const char *path) {
    char buf[48];
    int n;
    SceUID fd;

    if (path == NULL) {
        path = VKEY_CFG;
    }
    n = snprintf(buf, sizeof(buf), "on=%d\nmode=%d\n", enabled ? 1 : 0, mode);
    sceIoRemove(path);   /* Vita3K ignores O_TRUNC */
    fd = sceIoOpen(path, SCE_O_WRONLY | SCE_O_CREAT, 0777);
    if (fd >= 0) {
        sceIoWrite(fd, buf, (SceSize)n);
        sceIoClose(fd);
    }
}

/* Draw the keypad over an already-blitted frame.
 * gx/gy/gw/gh is the game rectangle in physical (960x544) pixels.
 *
 * v02.89: the button the player is holding is drawn "pressed" - amber
 * fill, white edge, dark glyph poking 1 px lower - so tapping a key gives
 * the same visual answer a physical key does. Without it a touch on the
 * panel produced no feedback at all until the game reacted.
 * v02.95: and the state is latched for VKEY_FLASH_MS after the tap, so the
 * highlight survives the finger lifting before the next repaint. */
void vita_vkey_draw(unsigned int *fb, int gx, int gy, int gw, int gh) {
    int i;
    int overlay;
    int flash_alive = vita_vkey_flash_active();

    if (!enabled || fb == NULL) {
        return;
    }
    if (!build_layout(gx, gy, gw, gh)) {
        return;
    }

    /* v04.50: if the page changed since the last frame, wipe what the old page
     * left on the panel BEFORE drawing the new one (see the C_CLEAR block for
     * why this is needed at all). Two frames so both swap buffers are cleaned. */
    if (drawn_page >= 0 && drawn_page != page && prev_count > 0) {
        int x0 = prev_btns[0].x;
        int y0 = prev_btns[0].y;
        int x1 = prev_btns[0].x + prev_btns[0].w;
        int y1 = prev_btns[0].y + prev_btns[0].h;
        for (i = 1; i < prev_count; i++) {
            if (prev_btns[i].x < x0) {
                x0 = prev_btns[i].x;
            }
            if (prev_btns[i].y < y0) {
                y0 = prev_btns[i].y;
            }
            if (prev_btns[i].x + prev_btns[i].w > x1) {
                x1 = prev_btns[i].x + prev_btns[i].w;
            }
            if (prev_btns[i].y + prev_btns[i].h > y1) {
                y1 = prev_btns[i].y + prev_btns[i].h;
            }
        }
        erase_box[0] = x0;
        erase_box[1] = y0;
        erase_box[2] = x1 - x0;
        erase_box[3] = y1 - y0;
        erase_frames = 2;
    }
    if (erase_frames > 0) {
        rect(fb, erase_box[0], erase_box[1], erase_box[2], erase_box[3],
             C_CLEAR, 1, 0);
        erase_frames--;
    }

    /* A panel that sits in a bar needs no blending (the bar is black);
     * one that overlaps the game does. Decide per button by checking
     * whether its centre is inside the game rectangle. */
    for (i = 0; i < btn_count; i++) {
        VKeyBtn *b = &btns[i];
        int cx = b->x + b->w / 2;
        int cy = b->y + b->h / 2;
        int down = (i == pressed_btn) ||
                   (flash_alive && i == flash_btn);
        unsigned int face = down ? C_DOWN : C_BTN;
        unsigned int edge = down ? C_DEDGE : C_EDGE;
        unsigned int text = down ? C_DTEXT : C_TEXT;
        int dy = down ? 1 : 0;      /* pressed content sits 1 px lower */

        overlay = (cx >= gx && cx < gx + gw && cy >= gy && cy < gy + gh);

        if (overlay) {
            /* A held key must stay obvious over a busy frame, so raise the
             * alpha instead of relying on colour contrast alone. */
            rect(fb, b->x, b->y, b->w, b->h, face, 0,
                 down ? 225 : 150);
            {
                int c;
                for (c = 0; c < b->w; c++) {
                    blend_px(fb, b->x + c, b->y, edge, 190);
                    blend_px(fb, b->x + c, b->y + b->h - 1, edge, 190);
                }
                for (c = 0; c < b->h; c++) {
                    blend_px(fb, b->x, b->y + c, edge, 190);
                    blend_px(fb, b->x + b->w - 1, b->y + c, edge, 190);
                }
            }
        } else {
            rect(fb, b->x, b->y, b->w, b->h, face, 1, 0);
            frame_rect(fb, b->x, b->y, b->w, b->h, edge);
            if (down) {
                /* second, inset outline: reads as "sunken" even on a
                 * small 34 px button */
                frame_rect(fb, b->x + 2, b->y + 2, b->w - 4, b->h - 4,
                           0x60FFFFFFu);
            }
        }

        if (b->tri != 0) {
            int size = b->w / 2;
            if (size < 10) size = 10;
            if (down) size -= size / 6;
            draw_triangle(fb, b->x + b->w / 2, b->y + b->h / 2 + dy, size,
                          b->tri, text);
        } else if (b->label != NULL) {
            int scale = b->h / 16;
            if (scale < 2) scale = 2;
            /* Keep the glyph size unchanged when pressed: growing it by
             * one step would spill outside the button and paint over the
             * neighbour. Colour + inset outline + 1 px offset is plenty. */
            draw_label(fb, b->x + b->w / 2, b->y + b->h / 2 + dy, b->label,
                       scale, text);
        }

        /* v02.95: this frame shows the highlight, so start its countdown
         * now. Doing it here (not at press time) means a game that repaints
         * late still gets the full visible flash. */
        if (down && i == flash_btn && flash_deadline_us == 0) {
            flash_deadline_us = sceKernelGetProcessTimeWide() +
                                (unsigned long long)VKEY_FLASH_MS * 1000ULL;
        }
    }

    /* v04.50: remember what this frame put on the panel, so a page change can
     * wipe exactly that area (see the C_CLEAR block at the top). */
    for (i = 0; i < btn_count; i++) {
        prev_btns[i] = btns[i];
    }
    prev_count = btn_count;
    drawn_page = page;
}

/* v02.89/v02.95: tell the keypad which button is held so draw() can
 * highlight it, and latch a short flash so the highlight is still visible
 * on the FIRST frame drawn after the tap (a menu game repaints only after
 * it received the key - see the VKEY_FLASH_MS comment above).
 *
 * btn >= 0 : finger went down on that button  -> hold + flash
 * btn <  0 : finger lifted                    -> stop holding, let the
 *                                               flash run out on its own
 *
 * Called from vita_input.c's touch poll. */
void vita_vkey_set_pressed(int btn) {
    if (btn >= 0) {
        flash_btn = btn;
        flash_press_us = sceKernelGetProcessTimeWide();
        flash_deadline_us = 0;      /* starts when the frame is drawn */
    }
    pressed_btn = btn;
}

int vita_vkey_pressed(void) {
    return pressed_btn;
}

/* v02.95: 1 while a tap is still flashing, i.e. while the display layer
 * must keep repainting so the player sees the feedback. Also true while a
 * button is held (a held key stays lit). */
int vita_vkey_flash_active(void) {
    unsigned long long now;

    if (pressed_btn >= 0) {
        return 1;
    }
    if (flash_btn < 0) {
        return 0;
    }
    now = sceKernelGetProcessTimeWide();
    if (flash_deadline_us == 0) {
        /* Not painted yet: keep asking for a repaint, but give up if
         * nothing has drawn it within the hard cap. */
        if (now - flash_press_us >
            (unsigned long long)VKEY_FLASH_MAX_MS * 1000ULL) {
            flash_btn = -1;
            return 0;
        }
        return 1;
    }
    if (now >= flash_deadline_us) {
        flash_btn = -1;
        flash_deadline_us = 0;
        return 0;
    }
    return 1;
}

/* Hit test in physical screen coordinates. Returns 1 and fills *key
 * (plus *btn for press/release tracking) when a button was hit. */
int vita_vkey_hit(int px, int py, int *key, int *btn) {
    int i;
    if (!enabled || key == NULL || !layout_valid) {
        return 0;
    }
    for (i = 0; i < btn_count; i++) {
        const VKeyBtn *b = &btns[i];
        if (px >= b->x && px < b->x + b->w &&
            py >= b->y && py < b->y + b->h) {
            /* v04.33: the page button is consumed here - no key event is
             * produced, the layout is rebuilt for the other page and the
             * caller sees "not a button" (which also releases anything that
             * was held, see vkey_touch's !hit branch). */
            if (b->key == VKEY_KEY_PAGE) {
                page = page ? 0 : 1;
                layout_valid = 0;
                pressed_btn = -1;
                flash_btn = -1;
                flash_press_us = 0;
                flash_deadline_us = 0;
                build_layout(layout_geom[0], layout_geom[1],
                             layout_geom[2], layout_geom[3]);
                return 0;
            }
            *key = b->key;
            if (btn != NULL) {
                *btn = i;
            }
            return 1;
        }
    }
    return 0;
}

/* v04.33: which page the keypad shows (0 = digits, 1 = letters). Read-only
 * for the diagnostics overlay; set_page exists so a future vkey.cfg key can
 * remember the last page. */
int vita_vkey_page(void) {
    return page;
}

void vita_vkey_set_page(int p) {
    page = p ? 1 : 0;
    layout_valid = 0;
}

/* The launcher gives the game rectangle to the keypad as soon as the
 * display knows it, so a touch that arrives before the first flip still
 * hits the right button. */
void vita_vkey_set_geometry(int gx, int gy, int gw, int gh) {
    build_layout(gx, gy, gw, gh);
}
