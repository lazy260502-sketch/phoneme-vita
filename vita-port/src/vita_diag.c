/*
 * vita_diag.c - on-screen diagnostics overlay (v03.70, P1-8).
 * See vita_diag.h for the rationale and the config keys.
 *
 * Everything here is read-only: it samples counters that other modules
 * already maintain. The only state it owns is "visible / detail", so the
 * overlay can never change behaviour, only what is displayed.
 */

#include <stdio.h>
#include <string.h>

#include <psp2/io/fcntl.h>

#include "vita_diag.h"
#include "vita_perf.h"
#include "vita_membw.h"
#include "vita_crumb.h"

/* from vita_font.c: the same 1bpp CJK bank the menu uses */
extern int vita_menu_draw_utf8(uint32_t *fb, int w, int h,
                               int x, int y, const char *utf8,
                               uint32_t color);
extern int vita_menu_font_gh(void);
extern int vita_menu_font_gw(void);

/* colors 0xAABBGGRR */
#define C_PANEL  0xFF101418u
#define C_TEXT   0xFFE8E8E8u
#define C_LABEL  0xFF70C0F0u
#define C_WARN   0xFF5050E0u

static int g_visible = 1;
static int g_detail = 0;
/* v03.78: which corner. 0 = top-right (default), 1 = top-left,
 * 2 = bottom-right, 3 = bottom-left. Default changed from top-left because
 * the Vita's own frame-rate overlay lives there and the virtual keypad owns
 * the left bar. */
static int g_pos = 0;
static int g_inited = 0;

/* ---- config ------------------------------------------------------- */

static int cfg_int(const char *buf, const char *key, int def) {
    const char *p = strstr(buf, key);
    int v = 0, any = 0;
    if (p == NULL) {
        return def;
    }
    p += strlen(key);
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    while (*p >= '0' && *p <= '9') {
        v = v * 10 + (*p - '0');
        any = 1;
        p++;
    }
    return any ? v : def;
}

void vita_diag_init(void) {
    char buf[256];
    SceUID fd;
    int n;

    if (g_inited) {
        return;
    }
    g_inited = 1;

    fd = sceIoOpen("ux0:/data/J2ME00001/diag.cfg", SCE_O_RDONLY, 0);
    if (fd < 0) {
        return;   /* defaults: visible, one line */
    }
    n = (int)sceIoRead(fd, buf, sizeof(buf) - 1);
    sceIoClose(fd);
    if (n <= 0) {
        return;
    }
    buf[n] = '\0';

    g_visible = cfg_int(buf, "on=", 1) != 0;
    g_detail = cfg_int(buf, "detail=", 0) != 0;
    g_pos = cfg_int(buf, "pos=", 0);
    if (g_pos < 0 || g_pos > 3) {
        g_pos = 0;
    }
    crumb_printf("diag: visible=%d detail=%d pos=%d", g_visible, g_detail,
                 g_pos);
}

void vita_diag_toggle(void) {
    g_visible = !g_visible;
    /* A toggle is worth a breadcrumb: it changes what a later screenshot
     * shows, and "why is the number gone?" should be answerable from the log. */
    crumb_printf("diag: toggled visible=%d", g_visible);
}

int vita_diag_visible(void) {
    return g_visible;
}

/* v03.79: settings-page API. The 设置 tab used to be able to switch everything
 * EXCEPT this overlay, which left diag.cfg as the only way in - i.e. pulling
 * the card out to change a font colour. Setters apply on the next frame. */
void vita_diag_set_visible(int on) {
    g_visible = on ? 1 : 0;
}

void vita_diag_set_detail(int on) {
    g_detail = on ? 1 : 0;
}

void vita_diag_set_pos(int pos) {
    if (pos < 0 || pos > 3) {
        pos = 0;
    }
    g_pos = pos;
}

int vita_diag_get_detail(void) {
    return g_detail;
}

int vita_diag_get_pos(void) {
    return g_pos;
}

/* Persist all three keys. Rewritten whole rather than patched in place: the
 * file is four short lines, and SCE_O_TRUNC is unreliable on Vita3K (the
 * recurring lesson since v01.16), so remove-then-write is the pattern the rest
 * of this port already uses. */
void vita_diag_save(void) {
    char buf[96];
    int n;
    SceUID fd;

    n = snprintf(buf, sizeof(buf), "on=%d\ndetail=%d\npos=%d\n",
                 g_visible ? 1 : 0, g_detail ? 1 : 0, g_pos);
    if (n <= 0) {
        return;
    }
    sceIoRemove("ux0:/data/J2ME00001/diag.cfg");
    fd = sceIoOpen("ux0:/data/J2ME00001/diag.cfg",
                   SCE_O_WRONLY | SCE_O_CREAT, 0777);
    if (fd >= 0) {
        sceIoWrite(fd, buf, n);
        sceIoClose(fd);
    }
}

/* ---- drawing ------------------------------------------------------ */

/* Backing box, alpha-blended over the game (v04.45).
 *
 * The display controller ignores the alpha channel, so "transparent" has to be
 * done here: read the pixel that is already in the back buffer and mix. The
 * earlier comment warned that this touches uncached CDRAM every frame - true,
 * but the box is only ~300x16 = 4800 pixels, i.e. ~38 KB of traffic against
 * the 884 KB the blit already moves per frame (< 5%), and the overlay is the
 * only thing the player asked to see through. */
static void fill_box(uint32_t *fb, int w, int h,
                     int x, int y, int bw, int bh, uint32_t c, int alpha) {
    int r, col;
    if (x < 0) { bw += x; x = 0; }
    if (y < 0) { bh += y; y = 0; }
    if (x + bw > w) { bw = w - x; }
    if (y + bh > h) { bh = h - y; }
    if (alpha <= 0) {
        alpha = 255;
    }
    if (alpha >= 255) {
        for (r = 0; r < bh; r++) {
            uint32_t *row = &fb[(size_t)(y + r) * (size_t)w + (size_t)x];
            for (col = 0; col < bw; col++) {
                row[col] = c;
            }
        }
        return;
    }
    {
        /* A8B8G8R8: mix each of R,G,B; keep the destination's alpha byte. */
        unsigned int ia = (unsigned int)(255 - alpha);
        unsigned int sr = (c & 0xFF) * (unsigned int)alpha;
        unsigned int sg = ((c >> 8) & 0xFF) * (unsigned int)alpha;
        unsigned int sb = ((c >> 16) & 0xFF) * (unsigned int)alpha;
        for (r = 0; r < bh; r++) {
            uint32_t *row = &fb[(size_t)(y + r) * (size_t)w + (size_t)x];
            for (col = 0; col < bw; col++) {
                uint32_t d = row[col];
                unsigned int r8 = (sr + (d & 0xFF) * ia) / 255u;
                unsigned int g8 = (sg + ((d >> 8) & 0xFF) * ia) / 255u;
                unsigned int b8 = (sb + ((d >> 16) & 0xFF) * ia) / 255u;
                row[col] = 0xFF000000u | (b8 << 16) | (g8 << 8) | r8;
            }
        }
    }
}

static void draw_line(uint32_t *fb, int w, int h,
                      int x, int y, const char *s, uint32_t color) {
    vita_menu_draw_utf8(fb, w, h, x, y, s, color);
}

void vita_diag_draw(uint32_t *fb, int w, int h) {
    unsigned int period = 0, prep = 0, total = 0, fps10 = 0, frames = 0;
    unsigned int frame32 = 0, conv_u = 0, conv_c = 0;
    int lh, gw, rows, box_h, box_w, y;
    char l1[96], l2[96];

    if (!g_visible || fb == NULL || w <= 0 || h <= 0) {
        return;
    }

    vita_perf_get(&period, &prep, &total, &fps10, &frames);
    if (g_detail) {
        /* Careful with the argument order: positions 3/4 are the memcpy
         * variants, 5/6 are the rgb565->abgr conversion (the one the blit
         * actually performs). We want the conversion numbers. */
        vita_membw_get(NULL, NULL, NULL, NULL, &conv_c, &conv_u,
                       &frame32, NULL);
    }

    lh = vita_menu_font_gh();
    if (lh <= 0) {
        lh = 16;
    }
    gw = vita_menu_font_gw();
    if (gw <= 0) {
        gw = 8;
    }
    /* Line 1 is always present (it is the one that answers "what fps"), the
     * clock rides on it because every performance discussion in this port has
     * had to re-establish that the firmware refuses 444 MHz.
     * v03.78 two changes to this line:
     *   - "cpu" is printed in MILLISECONDS. It used to print the raw
     *     microsecond value, so it read "cpu 10450" next to "102ms" - two
     *     different units side by side.
     *   - single spaces, and the period is rounded to whole ms. Every character
     *     here costs 8 px of box width, and the box has to fit between the
     *     right edge of the game column (x=683 in portrait) and the panel edge
     *     (960). At 300+ px it overlapped the game; at this length the box
     *     starts at ~702 and covers only the right letterbox bar, i.e. no game
     *     pixel and no key. */
    snprintf(l1, sizeof(l1), "%u.%u fps %ums cpu %u.%u arm %d",
             fps10 / 10u, fps10 % 10u,
             period / 1000u, prep / 1000u, (prep % 1000u) / 100u,
             vita_perf_clock_arm());

    if (g_detail) {
        /* Same rule: the fewest characters that still say what the number is. */
        snprintf(l2, sizeof(l2), "blit %u conv %u/%u f%u",
                 frame32, conv_u, conv_c, frames);
        rows = 2;
    } else {
        l2[0] = '\0';
        rows = 1;
    }

    /* v03.78: width follows the text instead of a fixed 300 px.
     * A fixed width was both too wide and in the wrong place: the Vita's own
     * frame-rate overlay sits in the TOP-LEFT corner, exactly where this box
     * used to be, and the virtual keypad occupies the left letterbox bar below
     * it. So:
     *   - position: top-RIGHT by default, which is empty in portrait (the game
     *     column ends at x=683, the action pad starts at y=60) and is the one
     *     corner the Vita overlay never uses;
     *   - width: measured from the actual strings, so the box is as small as
     *     it can be and cannot reach the game area it does not need to.
     * "pos=" in diag.cfg overrides the corner for anyone whose overlay plugin
     * lives somewhere else. */
    {
        int n1 = (int)strlen(l1);
        int n2 = (int)strlen(l2);
        int n = (n1 > n2) ? n1 : n2;
        box_w = n * gw + 8;         /* 2 px inset each side + 4 px text pad */
    }
    if (box_w > w - 4) {
        box_w = w - 4;
    }
    box_h = rows * (lh + 2) + 8;

    /* pos: 0 = top-right (default), 1 = top-left, 2 = bottom-right,
     *      3 = bottom-left. */
    {
        int bx, by;
        switch (g_pos) {
        case 1:  bx = 2;                   by = 2;         break;
        case 2:  bx = w - box_w - 2;       by = h - box_h - 2; break;
        case 3:  bx = 2;                   by = h - box_h - 2; break;
        default: bx = w - box_w - 2;       by = 2;         break;
        }
        if (bx < 0) { bx = 0; }
        if (by < 0) { by = 0; }
        /* v04.45: 170/255 (~67%) keeps the numbers readable over a bright game
         * while still showing what is underneath. */
        fill_box(fb, w, h, bx, by, box_w, box_h, C_PANEL, 170);
        y = by + 4;
        draw_line(fb, w, h, bx + 4, y, l1, (prep > 12000u) ? C_WARN : C_TEXT);
        if (rows == 2) {
            y += lh + 2;
            draw_line(fb, w, h, bx + 4, y, l2, C_LABEL);
        }
    }
}
