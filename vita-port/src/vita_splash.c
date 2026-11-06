/*
 * vita_splash.c - native "loading" screen for the boot window (v02.99).
 * See vita_splash.h for the design constraints.
 */

#include <stdio.h>
#include <string.h>

#include <psp2/kernel/processmgr.h>

#include "vita_splash.h"

/* Repaint throttle. One full-screen paint here is a 960x544 fill plus the
 * text, i.e. well under 1 ms, but the point is not the pixel cost: the
 * preload loop is the thing we are trying to make feel alive, and 4 fps is
 * plenty for a progress bar. 250 ms also keeps the added work under ~1% of
 * an 8 s load even if a future paint gets expensive. */
#define SPLASH_MIN_MS   250u

/* Hard stop. vita_splash_end() normally fires on the first frame that is
 * not pure white; this only exists so an app that never paints anything
 * cannot keep a stale "载入中" on screen forever. */
#define SPLASH_HARD_MS  30000u

/* v04.51: how long the splash must stay up before a foreign flip may end it.
 * The 30 s hard stop above is the wrong shape for a 3D title: a Graphics3D
 * game never writes vita_screen_pixels, so the "first non-white frame" test
 * cannot fire, and the splash used to repaint a full screen every frame
 * (5.96 ms measured in log191) for half a minute while the game was already
 * running. This grace window is what stops the very first warm-up flips from
 * tearing the splash away before anybody sees it; everything after that is
 * the game's time, not ours. */
#define SPLASH_MIN_VISIBLE_MS 400u

#define SPLASH_TITLE_MAX 96
#define SPLASH_STAGE_MAX 48

/* colors 0xAABBGGRR (SCE_DISPLAY_PIXELFORMAT_A8B8G8R8) */
#define C_BG    0xFF101418u
#define C_FG    0xFFF0F0F0u
#define C_DIM   0xFFA0998Cu
#define C_BAR   0xFF40B0E0u
#define C_TRACK 0xFF2C3440u

/* from vita_font.c: same bank the menu uses for CJK game names */
extern int vita_menu_draw_utf8(uint32_t *fb, int w, int h,
                               int x, int y, const char *utf8,
                               uint32_t color);
/* from vita_display.c: repaint through the ONE submit path */
extern void vita_display_splash_repaint(void);

static int spl_active = 0;
static int spl_ended_by_timeout = 0;
static int spl_preload_done = 0;
static char spl_title[SPLASH_TITLE_MAX] = "";
static char spl_stage[SPLASH_STAGE_MAX] = "";
static int spl_entries = 0;
static unsigned long long spl_t0_us = 0;
static unsigned long long spl_last_us = 0;
static int spl_paints = 0;

static void copy_trunc(char *dst, size_t cap, const char *src) {
    size_t n;
    if (src == NULL) {
        src = "";
    }
    n = strlen(src);
    if (n >= cap) {
        n = cap - 1;
    }
    memcpy(dst, src, n);
    dst[n] = '\0';
}

void vita_splash_begin(const char *title) {
    copy_trunc(spl_title, sizeof(spl_title),
               (title != NULL && title[0] != '\0') ? title : "J2ME");
    copy_trunc(spl_stage, sizeof(spl_stage), "读取游戏…");
    spl_entries = 0;
    spl_t0_us = sceKernelGetProcessTimeWide();
    spl_last_us = 0;          /* first tick/tick-caller paints at once */
    spl_paints = 0;
    spl_ended_by_timeout = 0;
    spl_preload_done = 0;
    spl_active = 1;
}

void vita_splash_stage(const char *stage) {
    if (!spl_active) {
        return;
    }
    copy_trunc(spl_stage, sizeof(spl_stage), stage);
    /* Do not wait out the throttle for a stage change: the next tick
     * should show it immediately. */
    spl_last_us = 0;
}

void vita_splash_entries(int entries) {
    if (!spl_active) {
        return;
    }
    if (entries > spl_entries) {
        spl_entries = entries;
    }
}

void vita_splash_end(void) {
    spl_active = 0;
}

int vita_splash_active(void) {
    return spl_active;
}

int vita_splash_preload_started(void) {
    return spl_entries > 0;
}

int vita_splash_owns_screen(void) {
    return spl_active;
}

void vita_splash_preload_done(void) {
    spl_preload_done = 1;
}

int vita_splash_on_foreign_flip(void) {
    if (!spl_active) {
        return 0;
    }
    /* v04.51: END THE SPLASH ON THE FIRST FOREIGN FLIP, not on a flag that
     * nothing sets.
     *
     * The old gate was `if (!spl_preload_done) return 0;` and
     * vita_splash_preload_done() has ZERO CALLERS in this tree (verified with
     * grep over vita-port/src). So this function could never succeed, and the
     * splash survived until SPLASH_HARD_MS (30 s) - repainting a full-screen
     * overlay every frame at 5.96 ms while the game underneath was already
     * drawing (log191, 极品飞车).
     *
     * A "foreign" flip means the MIDlet asked for a frame: we did not request
     * it (our own repaints go through vita_display_splash_repaint(), which
     * sets g_splash_own_flip). That is exactly the moment the splash has
     * served its purpose, and it does not depend on being able to inspect the
     * Java-side pixel buffer - which for a Graphics3D title is never written,
     * so frame_has_content() could not end the splash either.
     *
     * The grace window keeps the splash visible for a moment after boot even
     * if MIDP warms up by flipping blank frames. SPLASH_MIN_VISIBLE_MS is
     * deliberately small: a visible loading screen that overstays by 30 s is
     * far worse than one that disappears 0.4 s early. */
    {
        unsigned long long now = sceKernelGetProcessTimeWide();
        if (spl_t0_us != 0 &&
            now - spl_t0_us < (unsigned long long)SPLASH_MIN_VISIBLE_MS * 1000ull) {
            return 0;      /* too early: let people see it once */
        }
    }
    spl_active = 0;
    return 1;
}

void vita_splash_tick(void) {
    unsigned long long now;

    if (!spl_active) {
        return;
    }
    now = sceKernelGetProcessTimeWide();
    if (spl_t0_us != 0 &&
        now - spl_t0_us > (unsigned long long)SPLASH_HARD_MS * 1000ull) {
        /* Never took over - the round is either running with its own
         * content or wedged. Either way a stale splash is worse. */
        if (!spl_ended_by_timeout) {
            spl_ended_by_timeout = 1;
            spl_active = 0;
            /* One last repaint, so the bars the overlay covered get
             * wiped by the normal blit path instead of keeping the old
             * progress bar on screen forever. */
            vita_display_splash_repaint();
        }
        return;
    }
    if (spl_last_us != 0 &&
        now - spl_last_us < (unsigned long long)SPLASH_MIN_MS * 1000ull) {
        return;
    }
    spl_last_us = now;
    spl_paints++;
    vita_display_splash_repaint();
}

/* ---- drawing ------------------------------------------------------ */

static void fill_rect(uint32_t *fb, int w, int h,
                      int x, int y, int rw, int rh, uint32_t color) {
    int r, c;
    if (x < 0) { rw += x; x = 0; }
    if (y < 0) { rh += y; y = 0; }
    if (x + rw > w) { rw = w - x; }
    if (y + rh > h) { rh = h - y; }
    for (r = 0; r < rh; r++) {
        uint32_t *row = &fb[(y + r) * w + x];
        for (c = 0; c < rw; c++) {
            row[c] = color;
        }
    }
}

void vita_splash_draw(uint32_t *fb, int w, int h) {
    char line[128];
    int bar_w, bar_h, bar_x, bar_y, filled, head;
    int y;
    unsigned long long elapsed_ms;
    int permille;

    if (!spl_active || fb == NULL || w <= 0 || h <= 0) {
        return;
    }

    /* Opaque background over the whole panel - the white virtual screen
     * (and whatever the previous round left) must not peek through. */
    fill_rect(fb, w, h, 0, 0, w, h, C_BG);

    /* Progress model: honest, monotonic, and never claims to be done.
     * The real denominator (how many classes the preload will visit) is
     * not known before the sweep finishes, so the bar creeps by time
     * towards 90% while the counter supplies the actual evidence of life.
     * The first content frame removes the splash entirely, so the bar
     * never has to reach 100%. */
    elapsed_ms = (spl_t0_us != 0)
                     ? (sceKernelGetProcessTimeWide() - spl_t0_us) / 1000ull
                     : 0ull;
    permille = (int)(elapsed_ms * 900ull / 12000ull);
    if (permille > 900) {
        permille = 900;
    }
    if (permille < 0) {
        permille = 0;
    }

    /* Title, vertically a bit above centre. */
    y = h / 2 - 60;
    {
        int tw = 0;
        /* vita_menu_draw_utf8 has no measuring mode; draw once and read
         * the returned pen. Centring needs the width first, so estimate
         * from the byte length the same way vita_shot.c does: CJK is
         * full-width, ASCII half-width. */
        const unsigned char *p = (const unsigned char *)spl_title;
        int adv = 16;   /* the bank's default cell advance */
        while (*p) {
            if (*p < 0x80) {
                tw += adv / 2;
                p++;
            } else if ((*p & 0xE0) == 0xC0) {
                tw += adv;
                p += 2;
            } else if ((*p & 0xF0) == 0xE0) {
                tw += adv;
                p += 3;
            } else {
                tw += adv;
                p += 4;
            }
        }
        if (tw > w - 40) {
            tw = w - 40;
        }
        vita_menu_draw_utf8(fb, w, h, (w - tw) / 2, y, spl_title, C_FG);
    }

    /* Stage line under the title. */
    y += 34;
    {
        int sw = 0;
        const unsigned char *p = (const unsigned char *)spl_stage;
        int adv = 16;
        while (*p) {
            if (*p < 0x80) {
                sw += adv / 2;
                p++;
            } else if ((*p & 0xE0) == 0xC0) {
                sw += adv;
                p += 2;
            } else if ((*p & 0xF0) == 0xE0) {
                sw += adv;
                p += 3;
            } else {
                sw += adv;
                p += 4;
            }
        }
        if (sw > w - 40) {
            sw = w - 40;
        }
        vita_menu_draw_utf8(fb, w, h, (w - sw) / 2, y, spl_stage, C_DIM);
    }

    /* Progress bar: track + fill + a brighter head block. */
    bar_w = w / 2;
    if (bar_w > 480) {
        bar_w = 480;
    }
    if (bar_w < 120) {
        bar_w = w - 40;
    }
    bar_h = 10;
    bar_x = (w - bar_w) / 2;
    bar_y = h / 2 + 26;
    fill_rect(fb, w, h, bar_x, bar_y, bar_w, bar_h, C_TRACK);
    filled = bar_w * permille / 1000;
    if (filled > 0) {
        fill_rect(fb, w, h, bar_x, bar_y, filled, bar_h, C_BAR);
    }
    head = 6;
    if (head > bar_w - filled) {
        head = bar_w - filled;
    }
    if (head > 0 && filled < bar_w) {
        fill_rect(fb, w, h, bar_x + filled, bar_y, head, bar_h, C_BAR);
    }

    /* Counter line. Only once there is something to count - "0 个类"
     * reads like a stall. */
    if (spl_entries > 0) {
        snprintf(line, sizeof(line), "已载入 %d 个类", spl_entries);
        vita_menu_draw_utf8(fb, w, h, bar_x, bar_y + bar_h + 12, line,
                            C_DIM);
    }
}
