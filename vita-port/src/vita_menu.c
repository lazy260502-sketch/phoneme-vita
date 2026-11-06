/*
 * vita_menu.c - native game menu (list + install + launch + uninstall)
 *
 * Runs BEFORE the JVM starts. Draws directly into its own framebuffer
 * with a built-in 5x7 ASCII font - no GPU, no extra libraries, nothing
 * left allocated when it returns. The display handover to the JVM is a
 * plain sceDisplaySetFrameBuf from lfjport_ui_init, exactly the path the
 * known-good baseline uses.
 *
 * Directory layout:
 *   ux0:/data/J2ME00001/games/<name>/game.jar    installed game
 *   ux0:/data/J2ME00001/games/<name>/game.cfg    key=value (v02.83):
 *                                                class=.. ('-' = auto),
 *                                                orient=portrait|landscape,
 *                                                width=/height= (0 = follow
 *                                                the orientation),
 *                                                scale=fit|stretch|integer
 *                                                The legacy two-line form
 *                                                (class, orientation) is
 *                                                still read.
 *   ux0:/data/J2ME00001/games/<name>/cache.bin   scan cache (jar fp +
 *                                                name/class/icon metadata)
 *   ux0:/data/J2ME00001/inbox/*.jar              drop jars here, START
 *                                                installs them
 *
 * Keys: UP/DOWN select, CROSS open dialog (launch/orientation/uninstall/
 * back), CIRCLE close, START install inbox, SELECT rescan. Empty list +
 * CROSS quits the menu (falls back to launch.cfg / bundled Hello).
 */

#include <stdarg.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <psp2/ctrl.h>
#include <psp2/display.h>
#include <psp2/io/dirent.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/processmgr.h> /* v02.x: sceKernelGetProcessTimeWide (VitaSDK 15+) */
#include <psp2/rtc.h>
#include <psp2/touch.h>
#include <psp2/appmgr.h>

#include <zlib.h>

#include "vita_menu.h"
#include "vita_icon.h"
#include "vita_version.h"
#include "vita_fbmem.h"
#include "vita_crumb.h"
#include "vita_vkey.h"
#include "vita_diag.h"  /* v03.79: 设置页驱动诊断浮层（不再改 diag.cfg） */

/* from vita_font.c: menu-side UTF-8 rendering over the shared CJK bank */
int vita_menu_font_gw(void);
int vita_menu_font_gh(void);
int vita_menu_draw_utf8(uint32_t *fb, int w, int h,
                        int x, int y, const char *utf8, uint32_t color);

#define GAMES_DIR "ux0:/data/J2ME00001/games"
#define INBOX_DIR "ux0:/data/J2ME00001/inbox"
#define CFG_NAME  "game.cfg"
#define JAR_NAME  "game.jar"

#define MAX_GAMES 64
#define FB_W 960
#define FB_H 544

/* colors 0xAABBGGRR (matches SCE_DISPLAY_PIXELFORMAT_A8B8G8R8) */
#define C_BG    0xFF201810u
#define C_PANEL 0xFF2C3440u
#define C_PANEL2 0xFF3A4250u   /* current tab, focus elsewhere */
#define C_FG    0xFFF0F0F0u
#define C_SEL   0xFF3050A0u
#define C_TITLE 0xFF40B0E0u
#define C_HINT  0xFF908878u
#define C_WARN  0xFF5050E0u

static uint32_t *menu_fb = NULL;
/* v01.67 real-hw black-screen fix: the framebuffer must be UNCACHED or
 * the display controller cannot see the CPU writes (see vita_fbmem.h).
 * Owns the CDRAM block behind menu_fb for the whole process.
 * v01.70: DOUBLE BUFFERED - one CDRAM block holds two frames; drawing
 * goes to the back buffer and menu_flip() submits it with NEXTFRAME
 * and swaps. Single-buffered rendering painted into the frame the
 * display was scanning out -> constant flicker on real hw (and the
 * occasional Vita3K tear - same race, just rarely lost). */
static VitaFbMem menu_fb_blk = { -1, NULL };
static uint32_t *menu_fb_front = NULL;   /* being scanned out   */
static uint32_t *menu_fb_back = NULL;    /* being drawn into    */
/* v01.68 diagnostics: flip counter + last sceDisplaySetFrameBuf rc. */
static unsigned int menu_flip_count = 0;
static int menu_last_flip_rc = 0x7FFFFFFF;
/* heartbeat timestamp ( microseconds, sceKernelGetProcessTimeWide) */
static unsigned long long menu_hb_us = 0;

/* ------------------------------------------------------------------ */
/* 5x7 ASCII font (public-domain glyph table)                          */
/* ------------------------------------------------------------------ */

static const unsigned char glyph_table[][5] = {
    {0,0,0,0,0},{0,0,0x5F,0,0},{0,0x07,0,0x07,0},{0x14,0x7F,0x14,0x7F,0x14},
    {0x24,0x2A,0x7F,0x2A,0x12},{0x23,0x13,0x08,0x64,0x62},{0x36,0x49,0x55,0x22,0x50},
    {0,0x05,0x03,0,0},{0,0x1C,0x22,0x41,0},{0,0x41,0x22,0x1C,0},{0x08,0x2A,0x1C,0x2A,0x08},
    {0x08,0x08,0x3E,0x08,0x08},{0,0x50,0x30,0,0},{0x08,0x08,0x08,0x08,0x08},
    {0,0x60,0x60,0,0},{0x20,0x10,0x08,0x04,0x02},{0x3E,0x51,0x49,0x45,0x3E},
    {0,0x42,0x7F,0x40,0},{0x42,0x61,0x51,0x49,0x46},{0x21,0x41,0x45,0x4B,0x31},
    {0x18,0x14,0x12,0x7F,0x10},{0x27,0x45,0x45,0x45,0x39},{0x3C,0x4A,0x49,0x49,0x30},
    {0x01,0x71,0x09,0x05,0x03},{0x36,0x49,0x49,0x49,0x36},{0x06,0x49,0x49,0x29,0x1E},
    {0,0x36,0x36,0,0},{0,0x56,0x36,0,0},{0,0x08,0x14,0x22,0x41},
    {0x14,0x14,0x14,0x14,0x14},{0x41,0x22,0x14,0x08,0},{0x02,0x01,0x51,0x09,0x06},
    {0x32,0x49,0x79,0x41,0x3E},{0x7E,0x11,0x11,0x11,0x7E},{0x7F,0x49,0x49,0x49,0x36},
    {0x3E,0x41,0x41,0x41,0x22},{0x7F,0x41,0x41,0x22,0x1C},{0x7F,0x49,0x49,0x49,0x41},
    {0x7F,0x09,0x09,0x01,0x01},{0x3E,0x41,0x41,0x51,0x32},{0x7F,0x08,0x08,0x08,0x7F},
    {0,0x41,0x7F,0x41,0},{0x20,0x40,0x41,0x3F,0x01},{0x7F,0x08,0x14,0x22,0x41},
    {0x7F,0x40,0x40,0x40,0x40},{0x7F,0x02,0x04,0x02,0x7F},{0x7F,0x04,0x08,0x10,0x7F},
    {0x3E,0x41,0x41,0x41,0x3E},{0x7F,0x09,0x09,0x09,0x06},{0x3E,0x41,0x51,0x21,0x5E},
    {0x7F,0x09,0x19,0x29,0x46},{0x46,0x49,0x49,0x49,0x31},{0x01,0x01,0x7F,0x01,0x01},
    {0x3F,0x40,0x40,0x40,0x3F},{0x1F,0x20,0x40,0x20,0x1F},{0x7F,0x20,0x18,0x20,0x7F},
    {0x63,0x14,0x08,0x14,0x63},{0x03,0x04,0x78,0x04,0x03},{0x61,0x51,0x49,0x45,0x43},
    {0,0x7F,0x41,0x41,0},{0x02,0x04,0x08,0x10,0x20},{0,0x41,0x41,0x7F,0},
    {0x04,0x02,0x01,0x02,0x04},{0x40,0x40,0x40,0x40,0x40},{0,0x01,0x06,0x08,0x10},
    {0x20,0x54,0x54,0x54,0x78},{0x7F,0x48,0x44,0x44,0x38},{0x38,0x44,0x44,0x44,0x20},
    {0x38,0x44,0x44,0x48,0x7F},{0x38,0x54,0x54,0x54,0x18},{0x08,0x7E,0x09,0x01,0x02},
    {0x08,0x14,0x54,0x54,0x3C},{0x7F,0x08,0x04,0x04,0x78},{0,0x44,0x7D,0x40,0},
    {0x20,0x40,0x44,0x3D,0},{0,0x7F,0x10,0x28,0x44},{0,0x41,0x7F,0x40,0},
    {0x7C,0x04,0x18,0x04,0x78},{0x7C,0x08,0x04,0x04,0x78},{0x38,0x44,0x44,0x44,0x38},
    {0x7C,0x14,0x14,0x14,0x08},{0x08,0x14,0x14,0x18,0x7C},{0x7C,0x08,0x04,0x04,0x08},
    {0x48,0x54,0x54,0x54,0x20},{0x04,0x3F,0x44,0x40,0x20},{0x3C,0x40,0x40,0x20,0x7C},
    {0x1C,0x20,0x40,0x20,0x1C},{0x3C,0x40,0x30,0x40,0x3C},{0x44,0x28,0x10,0x28,0x44},
    {0x0C,0x50,0x50,0x50,0x3C},{0x44,0x64,0x54,0x4C,0x44},{0,0x08,0x36,0x41,0},
    {0,0,0x7F,0,0},{0,0x41,0x36,0x08,0},{0x10,0x08,0x08,0x10,0x10}
};

static void fill_rect(int x, int y, int w, int h, uint32_t color) {
    int row, col;
    if (menu_fb == NULL) return;
    for (row = 0; row < h; row++) {
        int py = y + row;
        if (py < 0 || py >= FB_H) continue;
        for (col = 0; col < w; col++) {
            int px = x + col;
            if (px >= 0 && px < FB_W) {
                menu_fb[py * FB_W + px] = color;
            }
        }
    }
}

/* 5x7 glyph scaled 3x -> 15x21 px characters. Pure-ASCII fast path;
 * strings containing UTF-8 multibyte sequences (Chinese game names)
 * route through the shared CJK bitmap bank in vita_font.c instead. */
static void draw_text(int x, int y, const char *s, int scale, uint32_t color) {
    int has_multibyte = 0;
    const unsigned char *p;
    for (p = (const unsigned char *)s; *p; p++) {
        if (*p >= 0x80) { has_multibyte = 1; break; }
    }
    if (has_multibyte) {
        /* Route through the shared CJK bitmap bank (vita_font.c). When
         * the bank failed to load nothing would be drawn at all - fall
         * back to '?' placeholders so the row is still visible. */
        int nx = vita_menu_draw_utf8(menu_fb, FB_W, FB_H, x, y, s, color);
        if (nx == x) {
            int nq = (int)strlen(s);
            int k;
            for (k = 0; k < nq; k++) {
                draw_text(x + k * 6 * scale, y, "?", scale, color);
            }
        }
        return;
    }
    while (*s) {
        unsigned idx = (unsigned char)*s;
        int row, col, sx, sy;
        if (idx < 32 || idx > 126) idx = '?' - 32;
        else idx -= 32;
        for (row = 0; row < 7; row++) {
            for (col = 0; col < 5; col++) {
                if ((glyph_table[idx][col] >> row) & 1) {
                    for (sy = 0; sy < scale; sy++) {
                        int py = y + row * scale + sy;
                        if (py < 0 || py >= FB_H) continue;
                        for (sx = 0; sx < scale; sx++) {
                            int px = x + col * scale + sx;
                            if (px >= 0 && px < FB_W) {
                                menu_fb[py * FB_W + px] = color;
                            }
                        }
                    }
                }
            }
        }
        x += 6 * scale;
        s++;
    }
}

static void draw_textf(int x, int y, int scale, uint32_t color,
                       const char *fmt, ...) {
    char buf[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    draw_text(x, y, buf, scale, color);
}

static void menu_flip(void) {
    SceDisplayFrameBuf fb;
    uint32_t *tmp;
    memset(&fb, 0, sizeof(fb));
    fb.size = sizeof(SceDisplayFrameBuf);
    fb.base = menu_fb_back;
    fb.pitch = FB_W;
    fb.pixelformat = SCE_DISPLAY_PIXELFORMAT_A8B8G8R8;
    fb.width = FB_W;
    fb.height = FB_H;
    /* v01.69 fix: NEXTFRAME, not IMMEDIATE - on real fw 3.65
     * sceDisplaySetFrameBuf(IMMEDIATE) returns
     * SCE_DISPLAY_ERROR_INVALID_UPDATETIMING (0x80290006) and the panel
     * stays black; every official SDK sample (debugScreen/camera/ime)
     * uses NEXTFRAME. Vita3K accepts IMMEDIATE, so this only shows on
     * real hardware. Confirmed by the crumb.log heartbeat:
     * flips incremented 60/s but rc=0x80290006 on every flip.
     * v01.70: submit the BACK buffer, then swap - the just-submitted
     * frame becomes the one scanned out while drawing restarts into
     * the old front (now free). */
    menu_last_flip_rc = sceDisplaySetFrameBuf(&fb, SCE_DISPLAY_SETBUF_NEXTFRAME);
    menu_flip_count++;
    tmp = menu_fb_front;
    menu_fb_front = menu_fb_back;
    menu_fb_back = tmp;
    /* menu_fb aliases the drawing target for all fill/draw helpers. */
    menu_fb = menu_fb_back;
}

/* ------------------------------------------------------------------ */
/* Input: edge-triggered, time-based auto-repeat                       */
/* ------------------------------------------------------------------ */

static unsigned int prev_btn = 0;
static unsigned long long press_us = 0;
static unsigned long long repeat_us = 0;

static unsigned int poll_buttons(void) {
    SceCtrlData pad;
    unsigned int cur, pressed;
    unsigned long long t = sceKernelGetProcessTimeWide();

    sceCtrlPeekBufferPositive(0, &pad, 1);
    cur = pad.buttons;
    pressed = cur & ~prev_btn;

    if (pressed & (SCE_CTRL_UP | SCE_CTRL_DOWN)) {
        press_us = t;
        repeat_us = t;
    } else if (cur & (SCE_CTRL_UP | SCE_CTRL_DOWN)) {
        if (t - press_us > 300000ULL && t - repeat_us > 100000ULL) {
            pressed = cur & (SCE_CTRL_UP | SCE_CTRL_DOWN);
            repeat_us = t;
        }
    }
    prev_btn = cur;
    return pressed;
}

/* ------------------------------------------------------------------
 * Touch: front panel, normalised to the 960x544 menu coordinate space.
 * Same panel-info-driven scaling as vita_input.c (real hw reports in
 * a 1920x1088 grid, Vita3K in 960x544 - read the active area instead
 * of hardcoding). Menu taps only; drag/zoom are not needed here.
 * ------------------------------------------------------------------ */
static int touch_max_x = 1919, touch_max_y = 1087;
static int touch_prev_down = 0;

static void menu_touch_init(void) {
    SceTouchSamplingState ss = SCE_TOUCH_SAMPLING_STATE_STOP;
    if (sceTouchGetSamplingState(SCE_TOUCH_PORT_FRONT, &ss) < 0 ||
        ss != SCE_TOUCH_SAMPLING_STATE_START) {
        sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT,
                                 SCE_TOUCH_SAMPLING_STATE_START);
    }
    {
        SceTouchPanelInfo panel;
        memset(&panel, 0, sizeof(panel));
        if (sceTouchGetPanelInfo(SCE_TOUCH_PORT_FRONT, &panel) == 0 &&
            panel.maxAaX > 0 && panel.maxAaY > 0) {
            touch_max_x = panel.maxAaX;
            touch_max_y = panel.maxAaY;
        }
    }
}

/* Poll for a finger release ("tap"). Returns 1 and fills *x/*y with the
 * release position in menu coordinates when the panel transitioned
 * down->up since the last poll. */
static int poll_tap(int *x, int *y) {
    SceTouchData td;
    int down;
    memset(&td, 0, sizeof(td));
    sceTouchPeek(SCE_TOUCH_PORT_FRONT, &td, 1);
    down = (td.reportNum > 0);
    if (down) {
        *x = td.report[0].x * FB_W / (touch_max_x + 1);
        *y = td.report[0].y * FB_H / (touch_max_y + 1);
    }
    if (touch_prev_down && !down) {
        touch_prev_down = down;
        return 1;
    }
    touch_prev_down = down;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Game catalogue                                                      */
/* ------------------------------------------------------------------ */

typedef struct {
    char dir[160];
    char name[128];
    char jar[176];
    char cls[128];
    char icon[128];   /* icon path inside the jar ("" = none) */
    int landscape;
    /* v02.82: per-game virtual screen (game.cfg). width/height == 0 means
     * "follow landscape"; scale_mode mirrors VITA_SCALE_* in
     * vita_display.c (0 fit, 1 stretch, 2 integer). */
    int width;
    int height;
    int scale_mode;
    /* v02.84: device profile / locale / MIDlet selection. profile[] is
     * "" for "generic" so an untouched game.cfg stays untouched;
     * midlet_idx is 1-based (0 = leave the single detected class). */
    char profile[24];
    char locale[16];
    int midlet_idx;
    /* v04.34: per-game overrides for settings that used to be global-only.
     * -1 = "follow defaults.cfg / the global setting", >= 0 = explicit.
     * Written to game.cfg as vkey= / vibrate= / perms= / jit= (the launcher
     * parses all four; jit= since v04.33). Keeping -1 as the default means an
     * untouched game.cfg stays byte-identical to before. */
    int vkey;
    int vibrate;
    int perms;
    int jit;
} GameEntry;

/* v02.84: profile names must match vita_profiles[] in vita_main.c; the
 * launcher is what actually applies them (it re-reads game.cfg). w/h is
 * the same default size, duplicated here only so the menu can show the
 * effective resolution and let "screen: auto" mean something concrete. */
static const struct {
    const char *name;
    const char *label;
    int w, h;
} profile_choices[] = {
    { "generic",      "generic",      240, 320 },
    { "nokia6230",    "Nokia 6230",   128, 128 },
    { "nokia_n95",    "Nokia N95",    240, 320 },
    { "se_k800",      "SE K800i",     240, 320 },
    { "samsung_e250", "Samsung E250", 128, 160 },
    { "siemens_c65",  "Siemens C65",  130, 130 }
};
#define PROFILE_N (int)(sizeof(profile_choices) / sizeof(profile_choices[0]))

/* defined later in this file (mini zip reader) */
static unsigned char *zip_read_entry(const char *jarpath, const char *want,
                                     long *out_size);

static int profile_index(const GameEntry *g) {
    int i;
    for (i = 0; i < PROFILE_N; i++) {
        if (strcmp(profile_choices[i].name, g->profile) == 0) {
            return i;
        }
    }
    return 0;
}

static const char *profile_label(const GameEntry *g) {
    return profile_choices[profile_index(g)].label;
}

/* Effective virtual screen when game.cfg has no explicit width/height:
 * the profile decides, and the orientation swaps the axes. */
static void effective_size(const GameEntry *g, int *w, int *h) {
    int i = profile_index(g);
    if (g->width > 0 && g->height > 0) {
        *w = g->width;
        *h = g->height;
        return;
    }
    if (g->landscape) {
        *w = profile_choices[i].h;
        *h = profile_choices[i].w;
    } else {
        *w = profile_choices[i].w;
        *h = profile_choices[i].h;
    }
}

/* ---- v02.84: MIDlet-1..N of the game currently shown in the dialog ---- */
#define MAX_MIDLETS 8
typedef struct {
    char name[64];
    char cls[128];
} MidletInfo;

static MidletInfo midlets[MAX_MIDLETS];
static int midlet_count = 0;
static char midlets_for[176];   /* jar the list was built from */

static void midlet_add(const char *value, int len);

/* Parse "Name, icon, class" (the MIDlet-N value). Tolerates missing icon
 * and extra trailing fields; the class is what comes after the LAST
 * comma, matching parse_manifest_class(). */
static void midlet_add(const char *value, int len) {
    int i, c1 = -1, c2 = -1;
    int o;
    MidletInfo *m;

    if (midlet_count >= MAX_MIDLETS || len <= 0) {
        return;
    }
    for (i = 0; i < len; i++) {
        if (value[i] == ',') {
            if (c1 < 0) {
                c1 = i;
            }
            c2 = i;
        }
    }
    if (c2 < 0) {
        return; /* no commas: not a MIDlet-N line */
    }
    m = &midlets[midlet_count];
    memset(m, 0, sizeof(*m));
    /* name: first field, trimmed */
    for (i = 0, o = 0; i < c1 && o < (int)sizeof(m->name) - 1; i++) {
        unsigned char ch = (unsigned char)value[i];
        if (ch >= 0x20) m->name[o++] = (char)ch;
    }
    while (o > 0 && m->name[o - 1] == ' ') o--;
    m->name[o] = '\0';
    /* class: last field, trimmed */
    for (i = c2 + 1, o = 0; i < len && o < (int)sizeof(m->cls) - 1; i++) {
        unsigned char ch = (unsigned char)value[i];
        if (ch == ' ' && o == 0) continue;
        if (ch >= 0x20) m->cls[o++] = (char)ch;
    }
    while (o > 0 && m->cls[o - 1] == ' ') o--;
    m->cls[o] = '\0';
    if (m->cls[0] == '\0') {
        return;
    }
    if (m->name[0] == '\0') {
        snprintf(m->name, sizeof(m->name), "%s", m->cls);
    }
    midlet_count++;
}

/* Scan MIDlet-N from a JAD (plain text) or MANIFEST.MF buffer. */
static void midlets_scan_buffer(const unsigned char *buf, long n) {
    long i = 0;
    while (i < n) {
        long ls = i, le, v;
        int len;
        le = ls;
        while (le < n && buf[le] != '\n' && buf[le] != '\r') le++;
        if (le - ls > 9 &&
            (buf[ls] == 'M' || buf[ls] == 'm') &&
            memcmp(buf + ls + 1, "IDlet-", 6) == 0 &&
            buf[ls + 7] >= '1' && buf[ls + 7] <= '0' + MAX_MIDLETS &&
            buf[ls + 8] == ':') {
            v = ls + 9;
            while (v < le && (buf[v] == ' ' || buf[v] == '\t')) v++;
            len = (int)(le - v);
            if (len > 0) {
                /* NUL-safe: copy to a temp so midlet_add can treat it as
                 * a C string region with an explicit length */
                midlet_add((const char *)buf + v, len);
            }
        }
        i = le;
        while (i < n && (buf[i] == '\n' || buf[i] == '\r')) i++;
    }
}

/* Build the MIDlet list for this jar. A JAD next to the jar wins over
 * the manifest (same precedence as the launcher, which hands the JAD to
 * runMidlet). Cached per jar so the dialog does not re-open the zip on
 * every frame. */
static void midlets_scan(const char *jarpath) {
    char jad[300];
    const char *slash;
    const char *base, *dot;
    SceUID fd;
    unsigned char *buf;
    long len = 0;

    if (strcmp(midlets_for, jarpath) == 0) {
        return;   /* already built for this jar */
    }
    snprintf(midlets_for, sizeof(midlets_for), "%s", jarpath);
    midlet_count = 0;

    /* 1) JAD */
    jad[0] = '\0';
    slash = strrchr(jarpath, '/');
    if (slash != NULL) {
        char dir[300];
        size_t dn = (size_t)(slash - jarpath);
        if (dn < sizeof(dir)) {
            memcpy(dir, jarpath, dn);
            dir[dn] = '\0';
            snprintf(jad, sizeof(jad), "%s/game.jad", dir);
            fd = sceIoOpen(jad, SCE_O_RDONLY, 0);
            if (fd < 0) {
                base = slash + 1;
                dot = strrchr(base, '.');
                if (dot != NULL && dot > base) {
                    snprintf(jad, sizeof(jad), "%s/%.*s.jad", dir,
                             (int)(dot - base), base);
                    fd = sceIoOpen(jad, SCE_O_RDONLY, 0);
                } else {
                    jad[0] = '\0';
                }
            }
            if (fd >= 0) {
                buf = (unsigned char *)malloc(16 * 1024);
                if (buf != NULL) {
                    len = sceIoRead(fd, buf, 16 * 1024 - 1);
                    sceIoClose(fd);
                    if (len > 0) {
                        midlets_scan_buffer(buf, len);
                    }
                    free(buf);
                } else {
                    sceIoClose(fd);
                }
            }
        }
    }

    /* 2) MANIFEST.MF (only when the JAD had no MIDlet-N) */
    if (midlet_count == 0) {
        buf = zip_read_entry(jarpath, "META-INF/MANIFEST.MF", &len);
        if (buf != NULL) {
            if (len > 0) {
                midlets_scan_buffer(buf, len);
            }
            free(buf);
        }
    }

    /* 3) nothing parseable: fall back to the class the scan resolved */
    if (midlet_count == 0) {
        midlet_count = 0;
    }
    fprintf(stderr, "[menu] midlets: %d in %s\n", midlet_count, jarpath);
    fflush(stderr);
}


static GameEntry games[MAX_GAMES];
static int game_count = 0;

/* ------------------------------------------------------------------
 * Mini zip reader: parse the jar directly with sceIo + zlib.
 * Rationale: the phoneME jar API matches entry names by exact length
 * using pcsl_string_utf8_length(), which the baseline pcsl implements
 * as a 3x upper bound (length*3) - so EVERY lookup fails with
 * "no MIDlet class in jar". Parsing the zip ourselves avoids that
 * whole chain and touches nothing global.
 * ------------------------------------------------------------------ */

static unsigned int rd16(const unsigned char *p) {
    return (unsigned int)p[0] | ((unsigned int)p[1] << 8);
}

static unsigned int rd32(const unsigned char *p) {
    return rd16(p) | (rd16(p + 2) << 16);
}

/* raw-deflate (zip method 8) into dst; returns bytes produced or -1 */
static long inflate_raw(unsigned char *dst, unsigned long dst_cap,
                        const unsigned char *src, unsigned long src_len) {
    z_stream zs;
    int r;
    memset(&zs, 0, sizeof(zs));
    if (inflateInit2(&zs, -15) != Z_OK) {
        return -1;
    }
    zs.next_in = (Bytef *)src;
    zs.avail_in = (uInt)src_len;
    zs.next_out = dst;
    zs.avail_out = (uInt)dst_cap;
    r = inflate(&zs, Z_FINISH);
    inflateEnd(&zs);
    return (r == Z_STREAM_END) ? (long)zs.total_out : -1;
}

/* Read one entry (case-insensitive ASCII match) into a malloc'd buffer.
 * Returns NULL when not found / malformed. */
static unsigned char *zip_read_entry(const char *jarpath, const char *want,
                                     long *out_size) {
    SceUID fd;
    unsigned int size;
    unsigned char *data = NULL;
    unsigned char *result = NULL;
    long i, eocd = -1, p;
    unsigned int cd_count, e;
    long scan;

    *out_size = 0;
    fd = sceIoOpen(jarpath, SCE_O_RDONLY, 0);
    if (fd < 0) {
        fprintf(stderr, "[zip] open FAILED %s\n", jarpath);
        fflush(stderr);
        return NULL;
    }
    size = (unsigned int)sceIoLseek(fd, 0, SCE_SEEK_END);
    sceIoLseek(fd, 0, SCE_SEEK_SET);
    data = (unsigned char *)malloc(size ? size : 1);
    if (data == NULL) {
        fprintf(stderr, "[zip] malloc(%u) failed\n", size);
        fflush(stderr);
        goto done;
    }
    if (sceIoRead(fd, data, size) != (int)size) {
        fprintf(stderr, "[zip] short read (%u)\n", size);
        fflush(stderr);
        goto done;
    }
    fprintf(stderr, "[zip] jar %s size=%u\n", jarpath, size);
    fflush(stderr);

    /* find End Of Central Directory from the tail */
    scan = (long)size - 22;
    for (i = scan; i >= 0 && i >= scan - 65536; i--) {
        if (rd32(data + i) == 0x06054b50u) {
            eocd = i;
            break;
        }
    }
    if (eocd < 0) {
        fprintf(stderr, "[zip] no EOCD\n");
        fflush(stderr);
        goto done;
    }
    cd_count = rd16(data + eocd + 10);
    p = (long)rd32(data + eocd + 16);
    fprintf(stderr, "[zip] eocd=%ld entries=%u cd_off=%ld\n",
            eocd, cd_count, p);
    fflush(stderr);

    for (e = 0; e < cd_count; e++) {
        unsigned int name_len, extra_len, comment_len, method;
        unsigned int comp_len, decomp_len, lho;
        const unsigned char *nm;
        int match = 1;
        unsigned int k;

        if (p + 46 > (long)size || rd32(data + p) != 0x02014b50u) {
            break;
        }
        name_len = rd16(data + p + 28);
        extra_len = rd16(data + p + 30);
        comment_len = rd16(data + p + 32);
        method = rd16(data + p + 10);
        comp_len = rd32(data + p + 20);
        decomp_len = rd32(data + p + 24);
        lho = rd32(data + p + 42);
        nm = data + p + 46;

        if (name_len == strlen(want)) {
            match = 1;
            for (k = 0; k < name_len; k++) {
                char a = (char)nm[k];
                char b = want[k];
                /* case-insensitive on BOTH sides (the entry-name-only
                 * fold made every lowercase want, e.g. "40.png",
                 * unmatched - the v01.29 icon failure) */
                if (a >= 'a' && a <= 'z') {
                    a = (char)(a - 32);
                }
                if (b >= 'a' && b <= 'z') {
                    b = (char)(b - 32);
                }
                if (a != b) {
                    match = 0;
                    break;
                }
            }
        } else {
            match = 0;
        }
        if (e < 4 || match) {
            fprintf(stderr, "[zip] entry[%u] len=%u '%.*s' match=%d\n",
                    e, name_len, (int)name_len, nm, match);
            fflush(stderr);
        }

        if (match) {
            fprintf(stderr, "[zip] MANIFEST matched, method=%u comp=%u decomp=%u\n",
                    method, comp_len, decomp_len);
            fflush(stderr);
            if (lho + 30 <= size && rd32(data + lho) == 0x04034b50u) {
                unsigned int ln = rd16(data + lho + 26);
                unsigned int le = rd16(data + lho + 28);
                long doff = (long)lho + 30 + ln + le;
                if (doff + (long)comp_len <= (long)size) {
                    result = (unsigned char *)malloc(decomp_len ? decomp_len : 1);
                    if (result != NULL) {
                        if (method == 0) { /* stored */
                            memcpy(result, data + doff, decomp_len);
                            *out_size = (long)decomp_len;
                        } else if (method == 8) { /* deflated */
                            long n = inflate_raw(result, decomp_len,
                                                 data + doff, comp_len);
                            if (n < 0) {
                                free(result);
                                result = NULL;
                            } else {
                                *out_size = n;
                            }
                        } else {
                            free(result);
                            result = NULL;
                        }
                    }
                }
            }
            break;
        }
        p += 46 + name_len + extra_len + comment_len;
    }

done:
    free(data);
    if (fd >= 0) {
        sceIoClose(fd);
    }
    return result;
}

/* Drop an incomplete UTF-8 sequence at the end of s[0..len).
 * Returns the new length, which is len unchanged for well-formed text.
 * Used when a name is cut to fit a fixed buffer: a Chinese character is
 * three bytes, so a plain byte limit can leave one or two of them
 * behind. The renderer would then skip the broken glyph (or, worse,
 * swallow the bytes that follow). */
static int utf8_trim_partial(char *s, int len) {
    int back = 0;
    unsigned char lead;
    int need;

    while (back < len && back < 3 &&
           ((unsigned char)s[len - back - 1] & 0xC0) == 0x80) {
        back++;              /* count trailing continuation bytes */
    }
    if (back >= len) {
        return 0;
    }
    lead = (unsigned char)s[len - back - 1];
    if ((lead & 0xE0) == 0xC0) {
        need = 2;
    } else if ((lead & 0xF0) == 0xE0) {
        need = 3;
    } else if ((lead & 0xF8) == 0xF0) {
        need = 4;
    } else {
        need = 1;            /* not a lead byte: nothing to trim */
    }
    if (need > back + 1) {
        return len - back - 1;   /* lead byte's sequence is incomplete */
    }
    return len;
}

/* Find "Name: value" in a MANIFEST buffer. Handles the manifest line
 * continuation rule (a line starting with a single space continues the
 * previous attribute). Matching is case-insensitive per the MIDP spec.
 * Returns value length or -1 when absent; value copied into out. */
static int manifest_get(const unsigned char *buf, long n,
                        const char *name, char *out, size_t outsz) {
    long i;
    size_t nlen = strlen(name);
    out[0] = '\0';
    for (i = 0; i < n; ) {
        long ls, le, vs, ve, p;
        /* only line starts */
        if (i > 0 && buf[i - 1] != '\n') {
            while (i < n && buf[i] != '\n') i++;
            i++; /* skip past '\n' */
            continue;
        }
        ls = i;
        /* line extent (before continuation join) */
        le = ls;
        while (le < n && buf[le] != '\n' && buf[le] != '\r') le++;
        if (le - ls < (long)nlen + 1 || buf[ls + nlen] != ':') {
            i = le;
            while (i < n && (buf[i] == '\n' || buf[i] == '\r')) i++;
            continue;
        }
        {
            long k;
            int match = 1;
            for (k = 0; k < (long)nlen; k++) {
                char a = buf[ls + k], b = name[k];
                if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
                if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
                if (a != b) { match = 0; break; }
            }
            if (!match) {
                i = le;
                while (i < n && (buf[i] == '\n' || buf[i] == '\r')) i++;
                continue;
            }
        }
        /* value: skip ": " then join continuations */
        vs = ls + nlen + 1;
        while (vs < le && buf[vs] == ' ') vs++;
        ve = le;
        p = ve;
        while (p + 1 < n && (buf[p] == '\r' || buf[p] == '\n') &&
               buf[p + 1] == ' ') {
            p += 2;
            while (p < n && buf[p] != '\r' && buf[p] != '\n') p++;
            ve = p;
        }
        {
            size_t o = 0;
            long q;
            for (q = vs; q < ve && o + 1 < outsz; q++) {
                /* v01.81: keep the byte UNSIGNED. arm-vita-eabi-gcc's
                 * plain char is signed, so the old `char ch = buf[q];
                 * if (ch >= 0x20)` test threw away every byte >= 0x80 -
                 * i.e. exactly the bytes of a UTF-8 Chinese name. The
                 * name then kept only its ASCII fragments, which is how
                 * a MIDlet called "泡泡龙4-豪华版" showed up as "4-". */
                unsigned char ch = buf[q];
                if (ch >= 0x20) out[o++] = (char)ch;
            }
            while (o > 0 && out[o - 1] == ' ') o--;
            /* only when the buffer limit cut the value: never leave a
             * half UTF-8 character at the end */
            if (q < ve) {
                o = (size_t)utf8_trim_partial(out, (int)o);
            }
            out[o] = '\0';
            return (int)o;
        }
    }
    return -1;
}

/* Display name: MIDlet-Name attribute; falls back to the jar base name
 * (i.e. the directory name) when absent. */
static void parse_manifest_name(const char *jarpath, const char *fallback,
                                char *out, size_t outsz) {
    long n = 0;
    unsigned char *buf = zip_read_entry(jarpath, "META-INF/MANIFEST.MF", &n);
    out[0] = '\0';
    if (buf != NULL && n > 0 &&
        manifest_get(buf, n, "MIDlet-Name", out, outsz) > 0) {
        free(buf);
        return;
    }
    free(buf);
    strncpy(out, fallback, outsz - 1);
    out[outsz - 1] = '\0';
}

/* Icon path: 2nd comma field of the "MIDlet-1: name, icon, class" line.
 * Returns 1 and fills out when an icon is declared. */
static int parse_manifest_icon(const char *jarpath, char *out, size_t outsz) {
    long n = 0;
    unsigned char *buf;
    long i;
    out[0] = '\0';
    buf = zip_read_entry(jarpath, "META-INF/MANIFEST.MF", &n);
    if (buf == NULL || n <= 0) {
        free(buf);
        return 0;
    }
    for (i = 0; i + 9 <= n; i++) {
        if (memcmp(buf + i, "MIDlet-1:", 9) == 0) {
            long p = i + 9;
            long line_end = p;
            long c1 = -1, c2 = -1;
            int len = 0;
            while (line_end < n && buf[line_end] != '\n' &&
                   buf[line_end] != '\r') {
                line_end++;
            }
            for (p = i + 9; p < line_end; p++) {
                if (buf[p] == ',') {
                    if (c1 < 0) c1 = p; else { c2 = p; break; }
                }
            }
            /* 2nd field: (c1, c2) or (c1, line_end) */
            if (c1 >= 0) {
                long s = c1 + 1, e = (c2 >= 0) ? c2 : line_end;
                while (s < e && buf[s] == ' ') s++;
                while (e > s && buf[e - 1] == ' ') e--;
                while (s < e && len + 1 < (int)outsz) {
                    /* v01.81: unsigned - see manifest_get() */
                    unsigned char ch = buf[s++];
                    if (ch >= 0x20) out[len++] = (char)ch;
                }
                out[len] = '\0';
            }
            break;
        }
    }
    free(buf);
    /* Empty icon field is legal ("name, , class") */
    return out[0] != '\0';
}

/* The class is the text after the LAST comma on the "MIDlet-1:" line;
 * non-printable characters are dropped (a stray CR makes Class.forName
 * fail with an invisible suffix). */
static void parse_manifest_class(const char *jarpath, char *out, size_t outsz) {
    long n = 0;
    unsigned char *buf;
    long i;
    out[0] = '\0';

    buf = zip_read_entry(jarpath, "META-INF/MANIFEST.MF", &n);
    if (buf == NULL || n <= 0) {
        free(buf);
        return;
    }

    for (i = 0; i + 9 <= n; i++) {
        if (buf[i] == 'M' && buf[i + 1] == 'I' && buf[i + 2] == 'D' &&
            buf[i + 3] == 'l' && buf[i + 4] == 'e' && buf[i + 5] == 't' &&
            buf[i + 6] == '-' && buf[i + 7] == '1' && buf[i + 8] == ':') {
            long p = i + 9;
            long line_end = p;
            long last_comma = -1;
            int len = 0;
            char tmp[128];
            while (line_end < n && buf[line_end] != '\n' && buf[line_end] != '\r') {
                line_end++;
            }
            for (p = i + 9; p < line_end; p++) {
                if (buf[p] == ',') {
                    last_comma = p;
                }
            }
            if (last_comma < 0) {
                break;
            }
            p = last_comma + 1;
            while (p < line_end && buf[p] == ' ') p++;
            while (p < line_end && len < (int)sizeof(tmp) - 1) {
                /* v01.81: unsigned - see manifest_get() */
                unsigned char ch = buf[p++];
                if (ch >= 0x20) {
                    tmp[len++] = (char)ch;
                }
            }
            while (len > 0 && tmp[len - 1] == ' ') {
                len--;
            }
            tmp[len] = '\0';
            strncpy(out, tmp, outsz - 1);
            out[outsz - 1] = '\0';
            break;
        }
    }
    free(buf);
}

static void save_cfg(const GameEntry *g) {
    char path[256];
    char line[400];
    int n;
    SceUID fd;

    snprintf(path, sizeof(path), "%s/" CFG_NAME, g->dir);
    n = snprintf(line, sizeof(line), "class=%s\norient=%s\n",
                 g->cls[0] ? g->cls : "-",
                 g->landscape ? "landscape" : "portrait");
    /* v02.82: only persist an explicit screen size; 0/0 means "follow
     * the orientation", which keeps the file identical to the legacy
     * behaviour for games that never needed a custom size. */
    if (g->width > 0 && g->height > 0 && n > 0 && n < (int)sizeof(line)) {
        n += snprintf(line + n, sizeof(line) - (size_t)n,
                      "width=%d\nheight=%d\n", g->width, g->height);
    }
    if (n > 0 && n < (int)sizeof(line)) {
        n += snprintf(line + n, sizeof(line) - (size_t)n, "scale=%s\n",
                      (g->scale_mode == 1) ? "stretch" :
                      (g->scale_mode == 2) ? "integer" : "fit");
    }
    /* v02.84: identity / locale / MIDlet choice. Omitted when default so
     * an untouched game keeps a minimal file. */
    if (n > 0 && n < (int)sizeof(line) && g->profile[0] != '\0' &&
        strcmp(g->profile, "generic") != 0) {
        n += snprintf(line + n, sizeof(line) - (size_t)n, "profile=%s\n",
                      g->profile);
    }
    if (n > 0 && n < (int)sizeof(line) && g->locale[0] != '\0') {
        n += snprintf(line + n, sizeof(line) - (size_t)n, "locale=%s\n",
                      g->locale);
    }
    if (n > 0 && n < (int)sizeof(line) && g->midlet_idx > 1) {
        n += snprintf(line + n, sizeof(line) - (size_t)n, "midlet=%d\n",
                      g->midlet_idx);
    }
    /* v04.34: per-game overrides. -1 means "follow the global setting", so
     * the key is omitted and the file stays minimal. */
    if (n > 0 && n < (int)sizeof(line) && g->vkey >= 0) {
        n += snprintf(line + n, sizeof(line) - (size_t)n, "vkey=%d\n",
                      g->vkey ? 1 : 0);
    }
    if (n > 0 && n < (int)sizeof(line) && g->vibrate >= 0) {
        n += snprintf(line + n, sizeof(line) - (size_t)n, "vibrate=%d\n",
                      g->vibrate ? 1 : 0);
    }
    if (n > 0 && n < (int)sizeof(line) && g->perms >= 0) {
        n += snprintf(line + n, sizeof(line) - (size_t)n, "perms=%s\n",
                      g->perms ? "strict" : "relaxed");
    }
    if (n > 0 && n < (int)sizeof(line) && g->jit >= 0) {
        n += snprintf(line + n, sizeof(line) - (size_t)n, "jit=%d\n", g->jit);
    }
    if (n <= 0) {
        return;
    }
    if (n > (int)sizeof(line)) {
        n = (int)sizeof(line);
    }
    /* remove+create: Vita3K ignores SCE_O_TRUNC, and the new format is
     * usually LONGER than the legacy two-line file (stale tail would
     * confuse the parser). */
    sceIoRemove(path);
    fd = sceIoOpen(path, SCE_O_WRONLY | SCE_O_CREAT, 0777);
    if (fd >= 0) {
        sceIoWrite(fd, line, (SceSize)n);
        sceIoClose(fd);
    }
}

/* v02.82: read game.cfg, accepting the new key=value form and the
 * pre-02.82 positional form (line 1 class, line 2 portrait|landscape).
 * Fields not present keep their defaults (landscape=0, size 0/0 =
 * follow the orientation, fit). */
static void game_cfg_load(GameEntry *g) {
    char cfgpath[256];
    char buf[512];
    SceUID fd;
    int n, positional = 0;
    char *p;

    g->landscape = 0;
    g->width = 0;
    g->height = 0;
    g->scale_mode = 0;
    g->profile[0] = '\0';
    g->locale[0] = '\0';
    g->midlet_idx = 0;
    /* v04.34: -1 = follow the global setting (nothing written to game.cfg). */
    g->vkey = -1;
    g->vibrate = -1;
    g->perms = -1;
    g->jit = -1;

    snprintf(cfgpath, sizeof(cfgpath), "%s/" CFG_NAME, g->dir);
    fd = sceIoOpen(cfgpath, SCE_O_RDONLY, 0);
    if (fd < 0) {
        return;
    }
    memset(buf, 0, sizeof(buf));
    n = sceIoRead(fd, buf, sizeof(buf) - 1);
    sceIoClose(fd);
    if (n <= 0) {
        return;
    }
    buf[n] = '\0';

    for (p = buf; *p != '\0'; ) {
        char *nl = strpbrk(p, "\r\n");
        char *end = (nl != NULL) ? nl : p + strlen(p);
        if (*end != '\0') {
            *end = '\0';
        }
        if (p[0] != '\0' && p[0] != '#') {
            char *eq = strchr(p, '=');
            if (eq != NULL) {
                char *k = p;
                char *v = eq + 1;
                char *ke = eq;
                char *ve;
                while (*k == ' ' || *k == '\t') k++;
                while (ke > k && (ke[-1] == ' ' || ke[-1] == '\t')) ke--;
                *ke = '\0';
                while (*v == ' ' || *v == '\t') v++;
                ve = v + strlen(v);
                while (ve > v && (ve[-1] == ' ' || ve[-1] == '\t')) {
                    *--ve = '\0';
                }
                if (strcmp(k, "class") == 0) {
                    if (strcmp(v, "-") != 0) {
                        strncpy(g->cls, v, sizeof(g->cls) - 1);
                        g->cls[sizeof(g->cls) - 1] = '\0';
                    }
                } else if (strcmp(k, "orient") == 0) {
                    g->landscape = (strcmp(v, "landscape") == 0);
                } else if (strcmp(k, "width") == 0) {
                    int w = atoi(v);
                    if (w > 0) g->width = w;
                } else if (strcmp(k, "height") == 0) {
                    int h = atoi(v);
                    if (h > 0) g->height = h;
                } else if (strcmp(k, "scale") == 0) {
                    g->scale_mode = (strcmp(v, "stretch") == 0) ? 1 :
                                    (strcmp(v, "integer") == 0) ? 2 : 0;
                } else if (strcmp(k, "profile") == 0) {
                    snprintf(g->profile, sizeof(g->profile), "%s", v);
                } else if (strcmp(k, "locale") == 0) {
                    snprintf(g->locale, sizeof(g->locale), "%s", v);
                } else if (strcmp(k, "midlet") == 0) {
                    int m = atoi(v);
                    if (m > 0 && m <= MAX_MIDLETS) g->midlet_idx = m;
                } else if (strcmp(k, "vkey") == 0) {
                    g->vkey = (strcmp(v, "off") == 0 || v[0] == '0') ? 0 : 1;
                } else if (strcmp(k, "vibrate") == 0) {
                    g->vibrate = (strcmp(v, "off") == 0 || v[0] == '0') ? 0 : 1;
                } else if (strcmp(k, "perms") == 0) {
                    g->perms = (strcmp(v, "strict") == 0) ? 1 : 0;
                } else if (strcmp(k, "jit") == 0) {
                    if (v[0] >= '0' && v[0] <= '2') g->jit = v[0] - '0';
                }
            } else {
                /* legacy positional lines */
                if (positional == 0) {
                    if (strcmp(p, "-") != 0) {
                        strncpy(g->cls, p, sizeof(g->cls) - 1);
                        g->cls[sizeof(g->cls) - 1] = '\0';
                    }
                } else if (positional == 1 && strncmp(p, "landscape", 9) == 0) {
                    g->landscape = 1;
                }
                positional++;
            }
        }
        if (nl == NULL) {
            break;
        }
        p = nl + 1;
    }
}

/* ------------------------------------------------------------------
 * MIDlet-class validation + fallback (FileManagerMIDl lesson).
 *
 * Many jars (esp. Chinese feature-phone software) ship a MANIFEST
 * whose MIDlet-1 class name does not match any .class actually in the
 * jar - Class.forName throws CNFE before a single pixel is drawn.
 *
 * verify_class_in_jar(): walks the zip central directory once and
 * checks that <class>.class exists as an entry (cheap: only reads
 * entry NAMES, no decompression).
 *
 * resolve_class_in_jar(): fallback picker when the MANIFEST name is
 * wrong. Strategy in order:
 *   1. exact suffix matches ending in "MIDlet" (best signal)
 *   2. any class ending in "MIDlet"/"Midlet"
 *   3. class whose simple name best matches the jar's base name
 * Tie-break: shortest fully-qualified name (top-level classes are
 * the likelier entry points).
 * ------------------------------------------------------------------ */

/* callback-based central-directory walk (names only) */
typedef void (*zip_entry_cb)(const char *name, unsigned int name_len, void *ctx);

static void zip_walk_names(const char *jarpath, zip_entry_cb cb, void *ctx) {
    SceUID fd;
    unsigned int size;
    unsigned char *data = NULL;
    long i, eocd = -1, p, scan;
    unsigned int cd_count, e;

    fd = sceIoOpen(jarpath, SCE_O_RDONLY, 0);
    if (fd < 0) {
        return;
    }
    size = (unsigned int)sceIoLseek(fd, 0, SCE_SEEK_END);
    sceIoLseek(fd, 0, SCE_SEEK_SET);
    data = (unsigned char *)malloc(size ? size : 1);
    if (data == NULL) {
        sceIoClose(fd);
        return;
    }
    if (sceIoRead(fd, data, size) != (int)size) {
        free(data);
        sceIoClose(fd);
        return;
    }

    scan = (long)size - 22;
    for (i = scan; i >= 0 && i >= scan - 65536; i--) {
        if (rd32(data + i) == 0x06054b50u) {
            eocd = i;
            break;
        }
    }
    if (eocd >= 0) {
        cd_count = rd16(data + eocd + 10);
        p = (long)rd32(data + eocd + 16);

        for (e = 0; e < cd_count; e++) {
            unsigned int name_len, extra_len, comment_len;
            const unsigned char *nm;

            if (p + 46 > (long)size || rd32(data + p) != 0x02014b50u) {
                break;
            }
            name_len = rd16(data + p + 28);
            extra_len = rd16(data + p + 30);
            comment_len = rd16(data + p + 32);
            nm = data + p + 46;
            if (p + 46 + (long)name_len > (long)size) {
                break;
            }
            cb((const char *)nm, name_len, ctx);
            p += 46 + name_len + extra_len + comment_len;
        }
    }

    free(data);
    sceIoClose(fd);
}

/* does <cls>.class exist in the jar? (cls uses '/' separators) */
struct verify_ctx {
    const char *cls;
    size_t cls_len;
    int found;
};

static void verify_cb(const char *name, unsigned int name_len, void *ctx_) {
    struct verify_ctx *ctx = (struct verify_ctx *)ctx_;
    if (!ctx->found && name_len == ctx->cls_len + 6 &&
        memcmp(name, ctx->cls, ctx->cls_len) == 0 &&
        memcmp(name + ctx->cls_len, ".class", 6) == 0) {
        ctx->found = 1;
    }
}

static int verify_class_in_jar(const char *jarpath, const char *cls) {
    struct verify_ctx ctx;
    char dotname[128];
    size_t i, n;

    if (cls == NULL || cls[0] == '\0') {
        return 0;
    }
    /* convert dotted name to slashed */
    n = strlen(cls);
    if (n >= sizeof(dotname)) {
        return 0;
    }
    for (i = 0; i < n; i++) {
        dotname[i] = (cls[i] == '.') ? '/' : cls[i];
    }
    dotname[n] = '\0';

    ctx.cls = dotname;
    ctx.cls_len = n;
    ctx.found = 0;
    zip_walk_names(jarpath, verify_cb, &ctx);
    return ctx.found;
}

/* fallback picker state */
struct resolve_ctx {
    char best[128];
    int best_score;   /* higher = better */
};

/* score a candidate: 3 = *MIDlet exact-cased, 2 = *Midlet any-case,
 * 1 = same simple name as jar base, 0 = otherwise (not stored) */
static int score_candidate(const char *name, unsigned int name_len) {
    /* work on a local NUL-terminated copy */
    char buf[160];
    const char *simple;
    int score = 0;

    if (name_len < 7 || name_len >= sizeof(buf)) {
        return -1; /* not a class entry or too long */
    }
    if (memcmp(name + name_len - 6, ".class", 6) != 0) {
        return -1;
    }
    memcpy(buf, name, name_len - 6);
    buf[name_len - 6] = '\0';

    /* skip inner classes and known non-entry names */
    if (strstr(buf, "$") != NULL) {
        return -1;
    }
    if (strncmp(buf, "META-INF/", 9) == 0) {
        return -1;
    }

    simple = strrchr(buf, '/');
    simple = (simple != NULL) ? simple + 1 : buf;

    if (strcmp(simple, "MIDlet") == 0) {
        return 3;
    }
    if (strlen(simple) >= 6 &&
        (strcmp(simple + strlen(simple) - 6, "MIDlet") == 0 ||
         strcmp(simple + strlen(simple) - 6, "Midlet") == 0)) {
        return 2;
    }
    return 1; /* any top-level class - weak candidate */
}

static void resolve_cb(const char *name, unsigned int name_len, void *ctx_) {
    struct resolve_ctx *ctx = (struct resolve_ctx *)ctx_;
    int score = score_candidate(name, name_len);
    char buf[160];
    unsigned int i;

    if (score <= 0) {
        return;
    }
    /* shorter FQN wins on ties (top-level classes are likelier) */
    if (score > ctx->best_score ||
        (score == ctx->best_score && ctx->best[0] != '\0' &&
         name_len - 6 < strlen(ctx->best))) {
        memcpy(buf, name, name_len - 6);
        buf[name_len - 6] = '\0';
        for (i = 0; buf[i]; i++) {
            if (buf[i] == '/') {
                buf[i] = '.';
            }
        }
        strncpy(ctx->best, buf, sizeof(ctx->best) - 1);
        ctx->best[sizeof(ctx->best) - 1] = '\0';
        ctx->best_score = score;
    }
}

/* If the MANIFEST class is missing from the jar, pick the best
 * available MIDlet-ish class instead. Returns 1 when out was fixed. */
static int resolve_class_in_jar(const char *jarpath, char *out, size_t outsz) {
    struct resolve_ctx ctx;
    ctx.best[0] = '\0';
    ctx.best_score = 0;
    zip_walk_names(jarpath, resolve_cb, &ctx);
    if (ctx.best_score >= 2 && ctx.best[0] != '\0') {
        strncpy(out, ctx.best, outsz - 1);
        out[outsz - 1] = '\0';
        return 1;
    }
    return 0;
}

/* Validate + fix the class name of one game entry. Called at scan and
 * install time. Writes game.cfg when the resolution changed anything. */
static void validate_game_class(GameEntry *g, int allow_resave) {
    char fixed[128];

    if (g->cls[0] == '\0') {
        return; /* nothing parsed - leave for the VM's own error path */
    }
    if (verify_class_in_jar(g->jar, g->cls)) {
        return; /* MANIFEST class is fine */
    }

    fprintf(stderr, "[menu] class '%s' not in jar '%s' - resolving\n",
            g->cls, g->jar);
    fflush(stderr);

    fixed[0] = '\0';
    if (resolve_class_in_jar(g->jar, fixed, sizeof(fixed)) &&
        strcmp(fixed, g->cls) != 0) {
        fprintf(stderr, "[menu] resolved '%s' -> '%s'\n", g->cls, fixed);
        fflush(stderr);
        strncpy(g->cls, fixed, sizeof(g->cls) - 1);
        g->cls[sizeof(g->cls) - 1] = '\0';
        if (allow_resave) {
            save_cfg(g);
        }
    }
}

static int dir_exists(const char *path) {
    char norm[256];
    size_t n;
    SceUID d;

    /* v01.80: "ux0:" names a volume, not a directory path - real
     * firmware refuses sceIoDopen() on it, only "ux0:/" works. Without
     * the trailing separator every partition probe fails on hardware
     * and the 文件 tab's root list falls back to its two hardcoded
     * names. */
    snprintf(norm, sizeof(norm), "%s", path);
    n = strlen(norm);
    if (n > 0 && norm[n - 1] == ':' && n + 1 < sizeof(norm)) {
        norm[n] = '/';
        norm[n + 1] = '\0';
    }

    d = sceIoDopen(norm);
    if (d < 0) {
        return 0;
    }
    sceIoDclose(d);
    return 1;
}

/* ------------------------------------------------------------------
 * Per-game metadata cache (<gamedir>/cache.bin).
 *
 * scan_games() opens each jar up to five times (MANIFEST x3, central
 * directory walk for class validation, icon PNG) and PNG-decodes the
 * icon - a few hundred ms per game on real hardware, all repeated on
 * every boot and every rescan.
 *
 * cache.bin stores everything the scan needs, keyed by a fingerprint
 * of the jar (size + mtime). On fingerprint hit the jar is never
 * opened; on mismatch (jar reinstalled/replaced) it is re-parsed and
 * the cache rewritten.
 *
 * Layout (little-endian, all u32 unless noted):
 *   magic  'J2CB'   version 1   jar_size   jar_mtime_lo   jar_mtime_hi
 *   name_len  name bytes        cls_len  cls bytes
 *   icondecl_len  icondecl bytes (path inside jar, "" = none)
 *   iconpng_len  icon PNG bytes (0 = none)  [icon PNG data follows]
 * ------------------------------------------------------------------ */

#define CACHE_NAME "cache.bin"
#define CACHE_MAGIC 0x4243324Au /* 'J2CB' */
/* v2 (v01.81): v1 caches were written by the broken name parser, which
 * dropped every UTF-8 byte and stored only the ASCII fragments ("4-"
 * instead of a Chinese MIDlet-Name). Bumping the version makes every
 * old cache.bin a miss, so the name is re-read from the jar once. */
#define CACHE_VERSION 2u

static int cache_valid(const GameEntry *g,
                       unsigned int jar_size, const SceDateTime *mtime) {
    SceUID fd;
    unsigned int hdr[6];
    int ok = 0;
    char path[256];
    SceRtcTick tick;

    snprintf(path, sizeof(path), "%s/" CACHE_NAME, g->dir);
    fd = sceIoOpen(path, SCE_O_RDONLY, 0);
    if (fd < 0) {
        return 0;
    }
    if (sceIoRead(fd, hdr, sizeof(hdr)) == (int)sizeof(hdr) &&
        hdr[0] == CACHE_MAGIC && hdr[1] == CACHE_VERSION &&
        hdr[2] == jar_size &&
        sceRtcGetTick(mtime, &tick) >= 0 &&
        hdr[3] == (unsigned int)(tick.tick & 0xFFFFFFFFu) &&
        hdr[4] == (unsigned int)(tick.tick >> 32)) {
        ok = 1;
    }
    sceIoClose(fd);
    return ok;
}

/* read one length-prefixed field; returns malloc'd buffer or NULL */
static unsigned char *cache_read_field(SceUID fd, unsigned int *out_len) {
    unsigned int len;
    unsigned char *buf;

    if (sceIoRead(fd, &len, sizeof(len)) != (int)sizeof(len) ||
        len > 512 * 1024) {
        return NULL;
    }
    buf = (unsigned char *)malloc(len ? len : 1);
    if (buf == NULL) {
        return NULL;
    }
    if (len > 0 && sceIoRead(fd, buf, len) != (int)len) {
        free(buf);
        return NULL;
    }
    *out_len = len;
    return buf;
}

/* load everything from a validated cache; returns 1 on success */
static int cache_load(GameEntry *g, unsigned char **icon_png,
                      unsigned int *icon_png_len) {
    SceUID fd;
    unsigned int nl, cl, il, pl;
    unsigned char *name, *cls, *icondecl, *png;
    char path[256];

    snprintf(path, sizeof(path), "%s/" CACHE_NAME, g->dir);
    fd = sceIoOpen(path, SCE_O_RDONLY, 0);
    if (fd < 0) {
        return 0;
    }
    sceIoLseek(fd, sizeof(unsigned int) * 6, SCE_SEEK_SET);
    name = cache_read_field(fd, &nl);
    cls = cache_read_field(fd, &cl);
    icondecl = cache_read_field(fd, &il);
    png = cache_read_field(fd, &pl);
    sceIoClose(fd);

    if (name == NULL || cls == NULL || icondecl == NULL || png == NULL ||
        nl == 0 || nl >= sizeof(g->name) || cl >= sizeof(g->cls) ||
        il >= sizeof(g->icon)) {
        free(name); free(cls); free(icondecl); free(png);
        return 0;
    }
    memcpy(g->name, name, nl); g->name[nl] = '\0';
    memcpy(g->cls, cls, cl); g->cls[cl] = '\0';
    memcpy(g->icon, icondecl, il); g->icon[il] = '\0';
    *icon_png = png;
    *icon_png_len = pl;
    free(name); free(cls); free(icondecl);
    return 1;
}

/* write one length-prefixed field */
static int cache_write_field(SceUID fd, const void *data, unsigned int len) {
    if (sceIoWrite(fd, &len, sizeof(len)) != (int)sizeof(len)) {
        return 0;
    }
    if (len > 0 && sceIoWrite(fd, data, len) != (int)len) {
        return 0;
    }
    return 1;
}

/* persist the scan result for next boot */
static void cache_save(const GameEntry *g, const unsigned char *icon_png,
                       unsigned int icon_png_len,
                       unsigned int jar_size, const SceDateTime *mtime) {
    char path[256];
    SceUID fd;
    unsigned int hdr[6];
    SceRtcTick tick;

    snprintf(path, sizeof(path), "%s/" CACHE_NAME, g->dir);
    fd = sceIoOpen(path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
    if (fd < 0) {
        return;
    }
    hdr[0] = CACHE_MAGIC;
    hdr[1] = CACHE_VERSION;
    hdr[2] = jar_size;
    if (sceRtcGetTick(mtime, &tick) >= 0) {
        hdr[3] = (unsigned int)(tick.tick & 0xFFFFFFFFu);
        hdr[4] = (unsigned int)(tick.tick >> 32);
    } else {
        hdr[3] = hdr[4] = 0;
    }
    hdr[5] = 0;
    sceIoWrite(fd, hdr, sizeof(hdr));
    if (cache_write_field(fd, g->name, (unsigned int)strlen(g->name)) &&
        cache_write_field(fd, g->cls, (unsigned int)strlen(g->cls)) &&
        cache_write_field(fd, g->icon, (unsigned int)strlen(g->icon)) &&
        cache_write_field(fd, icon_png, icon_png_len)) {
        /* written */
    }
    sceIoClose(fd);
}

/* ------------------------------------------------------------------ */
/* Icon rendering: decode the jar's icon PNG once per scan and cache    */
/* the 32bpp pixels; nearest-neighbour scale into a square box.         */
/* ------------------------------------------------------------------ */

#define ICON_CACHE_MAX MAX_GAMES
static uint32_t *icon_pix[ICON_CACHE_MAX];
static int icon_w[ICON_CACHE_MAX], icon_h[ICON_CACHE_MAX];

/* Release every decoded texture. Only ever called at the START of
 * scan_games(): the menu-exit path must not touch the malloc arena
 * (v01.78g quit-tab crash), and a fresh scan is the one point where no
 * texture can still be referenced by a drawn frame or by the slots
 * themselves. The v01.78f park/restore scheme (cache_hit_pix[]) that
 * tried to survive the exit is gone: it was the only code that freed a
 * pointer it did not own, and it bought nothing - cache.bin already
 * keeps the PNG *bytes*, so a re-scan decodes from RAM, not from the jar. */
static void icon_cache_clear(void) {
    int i;
    for (i = 0; i < ICON_CACHE_MAX; i++) {
        free(icon_pix[i]);
        icon_pix[i] = NULL;
        icon_w[i] = icon_h[i] = 0;
    }
}

static void icon_load(const GameEntry *g, int idx) {
    long n = 0;
    unsigned char *buf;
    /* MIDlet-1 icon paths are conventionally written with a leading '/'
     * ("/res/icon.png") but zip entry names never have one, so strip it
     * before the lookup (zip_read_entry matches entry names exactly). */
    const char *path = g->icon;
    if (idx < 0 || idx >= ICON_CACHE_MAX || g->icon[0] == '\0') {
        return;
    }
    while (*path == '/') {
        path++;
    }
    buf = zip_read_entry(g->jar, path, &n);
    if (buf == NULL) {
        fprintf(stderr, "[icon] zip entry not found: '%s' (from '%s')\n",
                path, g->icon);
        fflush(stderr);
        return; /* declared but missing: menu shows text-only row */
    }
    /* the decoder returns 0 on success, -1 on failure (the sense of this
     * test was inverted until v01.78g: real failures went unlogged while
     * every success printed a spurious FAILED line) */
    if (vita_icon_decode_png(buf, (unsigned long)n,
                             &icon_pix[idx], &icon_w[idx], &icon_h[idx]) != 0) {
        fprintf(stderr, "[icon] png decode FAILED: '%s' size=%ld\n",
                path, n);
        fflush(stderr);
    }
    free(buf);
}

/* Draw game[idx]'s icon scaled to fit box x..x+box (aspect preserved,
 * nearest-neighbour sampling handles non-integer factors), vertically
 * centered on (y + box/2). No-op when the game has no decodable icon. */
static void draw_icon_scaled(int x, int y, const GameEntry *g, int box) {
    int idx = (int)(g - games);
    const uint32_t *src;
    int sw, sh;
    int dw, dh, ox, oy;
    int r, c;

    if (menu_fb == NULL || idx < 0 || idx >= ICON_CACHE_MAX ||
        icon_pix[idx] == NULL) {
        return;
    }
    src = icon_pix[idx];
    sw = icon_w[idx];
    sh = icon_h[idx];

    /* fit into the box, keep aspect */
    if (sw > sh) {
        dw = box;
        dh = box * sh / sw;
    } else {
        dh = box;
        dw = box * sw / sh;
    }
    if (dw < 1) dw = 1;
    if (dh < 1) dh = 1;
    ox = x + (box - dw) / 2;
    oy = y + (box - dh) / 2;

    for (r = 0; r < dh; r++) {
        int py = oy + r;
        int srcy = (r * sh) / dh;
        const uint32_t *srow = src + (size_t)srcy * sw;
        if (py < 0 || py >= FB_H) continue;
        for (c = 0; c < dw; c++) {
            int px = ox + c;
            uint32_t v = srow[(c * sw) / dw];
            int a = (int)(v >> 24);
            if (px < 0 || px >= FB_W) continue;
            if (a < 8) continue;             /* transparent */
            if (a < 0xF8) {                  /* cheap alpha blend to bg */
                uint32_t bg = menu_fb[py * FB_W + px];
                int bg_r = (int)(bg & 0xFF), bg_g = (int)((bg >> 8) & 0xFF);
                int bg_b = (int)((bg >> 16) & 0xFF);
                int bg_a = (int)(bg >> 24);
                int fg_r = (int)(v & 0xFF), fg_g = (int)((v >> 8) & 0xFF);
                int fg_b = (int)((v >> 16) & 0xFF);
                int rr = (bg_r * (256 - a) + fg_r * a) >> 8;
                int gg = (bg_g * (256 - a) + fg_g * a) >> 8;
                int bb = (bg_b * (256 - a) + fg_b * a) >> 8;
                int aa = (bg_a * (256 - a) + 255 * a) >> 8;
                menu_fb[py * FB_W + px] =
                    ((uint32_t)aa << 24) | ((uint32_t)bb << 16) |
                    ((uint32_t)gg << 8) | (uint32_t)rr;
            } else {
                menu_fb[py * FB_W + px] = v | 0xFF000000u;
            }
        }
    }
}

static void scan_games(void) {
    SceUID d;
    SceIoDirent ent;
    game_count = 0;
    memset(games, 0, sizeof(games));
    /* v02.84: a rescan may follow an uninstall/reinstall of the same
     * path, so drop the cached MIDlet list for the dialog too. */
    midlets_for[0] = '\0';
    midlet_count = 0;
    /* v01.78g: drop the previous scan's textures HERE. Freeing them on the
     * menu-exit path is what crashed; releasing them at scan start is
     * equivalent (every slot is about to be rewritten) and guarantees the
     * slots never hold a stale pointer while the VM owns the heap. */
    icon_cache_clear();

    d = sceIoDopen(GAMES_DIR);
    if (d < 0) {
        return;
    }
    for (;;) {
        /* v01.70: zero the whole dirent BEFORE every sceIoDread -
         * d_private is device-private state and real firmware walks
         * it when it holds garbage (Vita3K ignores it). */
        memset(&ent, 0, sizeof(ent));
        if (sceIoDread(d, &ent) <= 0 || game_count >= MAX_GAMES) {
            break;
        }
        GameEntry *g;
        SceDateTime mtime;
        unsigned int jar_size;
        unsigned char *icon_png = NULL;
        long icon_png_len_l = 0;
        unsigned int icon_png_len_c = 0;

        if (!SCE_S_ISDIR(ent.d_stat.st_mode)) {
            continue;
        }
        if (strcmp(ent.d_name, ".") == 0 || strcmp(ent.d_name, "..") == 0) {
            continue;
        }
        g = &games[game_count];
        snprintf(g->dir, sizeof(g->dir), GAMES_DIR "/%s", ent.d_name);
        snprintf(g->jar, sizeof(g->jar), "%s/" JAR_NAME, g->dir);

        /* fingerprint: jar size + mtime ( sceIoGetstat by path ) */
        {
            SceIoStat st;
            if (sceIoGetstat(g->jar, &st) < 0) {
                continue; /* jar vanished; skip this entry */
            }
            jar_size = (unsigned int)st.st_size;
            mtime = st.st_mtime;
        }

        if (cache_valid(g, jar_size, &mtime) &&
            cache_load(g, &icon_png, &icon_png_len_c)) {
            /* cache hit: no jar open at all. game.cfg may still carry a
             * newer manual class override / orientation / screen size.
             * Do NOT clear cls here: cache_load() already restored it and
             * game_cfg_load() only overwrites it when the file has a
             * class entry. */
            game_cfg_load(g);
            /* decode icon from the cached PNG bytes (cheap, no jar IO) */
            if (icon_png_len_c > 0 && icon_png != NULL) {
                /* the slot is NULL here (icon_cache_clear() ran at scan
                 * start), so the decode can hand its buffer over directly */
                if (vita_icon_decode_png(icon_png,
                                         (unsigned long)icon_png_len_c,
                                         &icon_pix[game_count],
                                         &icon_w[game_count],
                                         &icon_h[game_count]) != 0) {
                    fprintf(stderr,
                            "[icon] cached png decode FAILED\n");
                    fflush(stderr);
                }
            }
            free(icon_png);
        } else {
            /* cache miss: full parse, then persist for next boot */
            /* display name: MIDlet-Name from MANIFEST (often Chinese),
             * the directory name is only the fallback */
            parse_manifest_name(g->jar, ent.d_name,
                                g->name, sizeof(g->name));
            parse_manifest_icon(g->jar, g->icon, sizeof(g->icon));

            /* game.cfg: class override + orientation + screen size */
            g->cls[0] = '\0';
            game_cfg_load(g);
            if (g->cls[0] == '\0') {
                parse_manifest_class(g->jar, g->cls, sizeof(g->cls));
            }
            /* fix jars whose MANIFEST class does not exist
             * (FileManagerMIDlet lesson): verify against the central
             * directory and fall back to the best MIDlet-ish class;
             * persist the fix in game.cfg */
            validate_game_class(g, 1);
            /* cache-miss path decodes straight into icon_pix[idx]; the slot
             * is NULL because icon_cache_clear() ran at scan start */
            icon_load(g, game_count);

            /* persist for next boot: name/cls/icon path + icon PNG
             * source bytes so next boot skips every jar open */
            if (g->icon[0] != '\0') {
                const char *ipath = g->icon;
                while (*ipath == '/') {
                    ipath++;
                }
                icon_png = zip_read_entry(g->jar, ipath, &icon_png_len_l);
            }
            cache_save(g, icon_png,
                       icon_png ? (unsigned int)icon_png_len_l : 0,
                       jar_size, &mtime);
            free(icon_png);
            icon_png = NULL;
        }
        game_count++;
    }
    sceIoDclose(d);
    /* (v01.78g: no icon-cache bookkeeping here - the textures decoded by
     * this scan are owned by icon_pix[0..game_count-1] until the next
     * scan_games() releases them) */
}

static void uninstall_game(GameEntry *g) {
    char path[256];
    snprintf(path, sizeof(path), "%s/" JAR_NAME, g->dir);
    sceIoRemove(path);
    snprintf(path, sizeof(path), "%s/" CFG_NAME, g->dir);
    sceIoRemove(path);
    sceIoRmdir(g->dir);
}

static void install_inbox(char *msg, size_t msg_sz) {
    SceUID d;
    SceIoDirent ent;
    int installed = 0;

    if (!dir_exists(INBOX_DIR)) {
        sceIoMkdir(INBOX_DIR, 0777);
    }
    d = sceIoDopen(INBOX_DIR);
    if (d < 0) {
        snprintf(msg, msg_sz, "no inbox dir");
        return;
    }
    for (;;) {
        /* v01.70: zero d_private before every sceIoDread (real fw) */
        memset(&ent, 0, sizeof(ent));
        if (sceIoDread(d, &ent) <= 0) {
            break;
        }
        char src[256], dstjar[256], dstdir[200];
        size_t blen;
        char base[128];
        if (SCE_S_ISDIR(ent.d_stat.st_mode)) {
            continue;
        }
        blen = strlen(ent.d_name);
        if (blen < 5 || strcmp(ent.d_name + blen - 4, ".jar") != 0) {
            continue;
        }
        snprintf(base, sizeof(base), "%s", ent.d_name);
        /* v01.78g: re-derive the length from the *copied* name. ent.d_name
         * can be longer than base[128], and base[blen-4] then wrote past
         * the end of the buffer (a stack smash, not just a wrong name). */
        blen = strlen(base);
        if (blen >= 5 && strcmp(base + blen - 4, ".jar") == 0) {
            base[blen - 4] = '\0';
        }

        snprintf(dstdir, sizeof(dstdir), GAMES_DIR "/%s", base);
        sceIoMkdir(dstdir, 0777);
        snprintf(src, sizeof(src), INBOX_DIR "/%s", ent.d_name);
        snprintf(dstjar, sizeof(dstjar), "%s/" JAR_NAME, dstdir);
        if (sceIoRename(src, dstjar) < 0) {
            continue;
        }
        installed++;
        {
            GameEntry tmp;
            memset(&tmp, 0, sizeof(tmp));
            snprintf(tmp.dir, sizeof(tmp.dir), "%s", dstdir);
            snprintf(tmp.jar, sizeof(tmp.jar), "%s/" JAR_NAME, dstdir);
            parse_manifest_class(tmp.jar, tmp.cls, sizeof(tmp.cls));
            validate_game_class(&tmp, 1);
            save_cfg(&tmp);
        }
    }
    sceIoDclose(d);
    if (installed > 0) {
        snprintf(msg, msg_sz, "installed %d game(s)", installed);
    } else {
        snprintf(msg, msg_sz, "inbox empty (copy .jar to inbox)");
    }
}

/* ------------------------------------------------------------------ */
/* Menu main loop                                                      */
/* ------------------------------------------------------------------ */

enum { DLG_LAUNCH = 0, DLG_ORIENT, DLG_SCREEN, DLG_SCALE, DLG_PROFILE,
       DLG_MIDLET, DLG_LOCALE, DLG_VKEY, DLG_VIBRATE, DLG_PERMS, DLG_JIT,
       DLG_UNINSTALL, DLG_BACK, DLG_N };

/* v04.34: per-game locale / keypad / rumble / permission / JIT rows. Each is
 * a small cycle whose first stop is "follow the global setting" (-1), so a
 * game.cfg written here never overrides more than the user asked for. */
static const char *locale_choices[] = {
    "", "zh-CN", "en-US", "ja-JP", "ko-KR", "zh-TW"
};
#define LOCALE_N (int)(sizeof(locale_choices) / sizeof(locale_choices[0]))

static int locale_index(const GameEntry *g) {
    int i;
    for (i = 0; i < LOCALE_N; i++) {
        if (strcmp(locale_choices[i], g->locale) == 0) {
            return i;
        }
    }
    return 0;
}

static const char *locale_label(const GameEntry *g) {
    return (g->locale[0] != '\0') ? g->locale : "follow global";
}

/* -1 (follow) -> 0 -> 1 -> ... -> max -> -1 */
static int cycle_follow(int v, int max) {
    if (v < 0) {
        return 0;
    }
    if (v >= max) {
        return -1;
    }
    return v + 1;
}

static const char *follow_onoff(int v) {
    return (v < 0) ? "follow global" : (v ? "on" : "off");
}

static const char *follow_label(int v, const char *a, const char *b) {
    return (v < 0) ? "follow global" : (v ? a : b);
}

static const char *jit_label_pergame(int v) {
    return (v < 0) ? "follow global" : (v == 0) ? "off (interpreter)"
                            : (v == 1) ? "on, first round only"
                                       : "on, every round";
}

/* v02.82: per-game virtual screen presets. index 0 = "follow the
 * orientation" (no width/height written to game.cfg), which is the
 * behaviour every pre-02.82 game had. */
static const struct {
    int w, h;
    const char *label;
} screen_presets[] = {
    { 0,   0,   "auto (orientation)" },
    { 128, 160, "128x160" },
    { 176, 208, "176x208" },
    { 176, 220, "176x220" },
    { 240, 320, "240x320" },
    { 320, 240, "320x240" },
    { 360, 640, "360x640" },
    { 480, 800, "480x800" }
};
#define SCREEN_PRESET_N (int)(sizeof(screen_presets) / sizeof(screen_presets[0]))

static int screen_index(const GameEntry *g) {
    int i;
    if (g->width <= 0 || g->height <= 0) {
        return 0;
    }
    for (i = 1; i < SCREEN_PRESET_N; i++) {
        if (screen_presets[i].w == g->width &&
            screen_presets[i].h == g->height) {
            return i;
        }
    }
    return -1; /* custom size from a hand-edited game.cfg */
}

static void screen_cycle(GameEntry *g) {
    int i = screen_index(g);
    i = (i + 1) % SCREEN_PRESET_N;   /* -1 (custom) wraps to 0 */
    g->width = screen_presets[i].w;
    g->height = screen_presets[i].h;
}

static const char *scale_name(int mode) {
    return (mode == 1) ? "stretch" : (mode == 2) ? "integer" : "fit";
}

/* v02.84: pick MIDlet-N. Cycling sets the class the launcher runs, so
 * a multi-MIDlet suite can start any of its entries. */
static void midlet_cycle(GameEntry *g) {
    if (midlet_count <= 0) {
        return;
    }
    /* midlet_idx is 1-based; 0 means "whatever the scan resolved". The
     * first press therefore lands on entry 1 (so explicitly choosing the
     * default is possible), later presses wrap 1..N. */
    if (g->midlet_idx < 1 || g->midlet_idx > midlet_count) {
        g->midlet_idx = 1;
    } else {
        g->midlet_idx = (g->midlet_idx % midlet_count) + 1;
    }
    snprintf(g->cls, sizeof(g->cls), "%s", midlets[g->midlet_idx - 1].cls);
}

/* ==================================================================
 * v01.78: TV-style tab UI.
 *   [已安装] games installed under games/ (list view / grid view)
 *   [未安装] jars found in inbox/ waiting for install
 *   [文件]   minimal file browser (ux0:/data/J2ME00001 sandbox)
 *   [设置]   view-only info + game view toggle, persisted in menu.cfg
 *   [退出]   quit the menu (fallback to launch.cfg / bundled tests)
 * Tabs switch with L/R shoulder buttons, by tapping the tab column,
 * or with the dpad two-focus model: LEFT from the content pulls the
 * focus into the tab column (UP/DOWN then walk the tabs, RIGHT/X goes
 * back). In the installed grid LEFT only exits on the edge column so
 * in-row navigation is never hijacked.
 * ================================================================== */
#define TAB_N        5
#define TABBAR_W     220
#define TAB_H        72
#define TAB_ROWMODE  0
#define TAB_GRIDMODE 1

/* everything the menu shows lives under this sandbox root */
#define DATA_ROOT "ux0:/data/J2ME00001"

enum { FE_DIR = 0, FE_JAR, FE_FILE, FE_N };

typedef struct {
    char name[64];       /* display name (UTF-8) */
    char full[256];      /* absolute path */
    int type;            /* FE_* */
} FileEntry;

static FileEntry fes[MAX_GAMES];
static int fe_count = 0;
/* v01.78d: browse from the Vita pseudo-root ("/" lists ux0:, ur0:,
 * uma0:, ...) so jars on any partition can be installed; the data
 * dir is still where installs land. */
static char fe_cwd[256] = "/";
static const char *fe_root = "/";

/* Candidate partitions probed when building the pseudo-root list.
 * sceIoDopen("/") is NOT reliable (some firmware/emulator builds fail
 * it outright), so the device list is built by probing these names and
 * then merging whatever sceIoDopen("/") happens to return. */
static const char *fe_devices[] = {
    "ux0:", "ur0:", "uma0:", "imc0:", "grw0:", "xmc0:", "xmc1:"
};

/* ---- settings (menu.cfg + launch.cfg line 4) ---- */
#define MENU_CFG DATA_ROOT "/menu.cfg"
#define LAUNCH_CFG DATA_ROOT "/launch.cfg"
/* v03.79: three on-screen-overlay rows live at the end, driven through
 * vita_diag.h so the overlay itself holds the state (one source of truth,
 * same as vkey). JITPROBE is a file-existence switch - the probe executes
 * freshly written code, so it must stay an explicit opt-in, but making the
 * user create a file by hand was the same mistake as diag.cfg. */
/* v04.24: "Java 堆" sits right after "JIT 编译" ON PURPOSE. log183's user
 * report was "there is nowhere to set the Java heap" - it existed, but as row
 * 14 of a 10-row window that needs scrolling, and nobody looks for a JIT
 * prerequisite three screens down from JIT. Related settings belong together. */
/* v04.26: SET_JITPROBE removed together with the jitprobe.cfg gate in
 * vita_jitmem.c - the probe now runs whenever the user asks for jit!=0, so
 * there is nothing left to toggle here. */
enum { SET_VIEW = 0, SET_PROFILE, SET_SCALE, SET_VKEY, SET_VIBRATE,
       SET_KEYMAP, SET_PERMS, SET_JIT, SET_HEAPMB, SET_3D,
       SET_DIAG, SET_DIAGPOS, SET_DIAGDETAIL,
       SET_N };

/* vita_input.c: user key remapping (keymap.cfg) */
int vita_keymap_count(void);
const char *vita_keymap_btn_name(int i);
int vita_keymap_get(int i);
const char *vita_keymap_key_name(int key);
void vita_keymap_cycle(int i, int dir);
void vita_keymap_reset(void);
void vita_keymap_save(void);
static int km_page = 0;            /* 1 = key-mapping sub page open */
static int km_sel = 0;             /* row; == count is the "reset" row */
static int km_top = 0;             /* first visible row */
#define KM_LINES 13
static int set_list_grid = 0;      /* 0 = list, 1 = grid */
static int set_jit = 0;            /* 0/1/2, mirrors launch.cfg "jit=" */
/* v02.85: virtual keypad. Kept in vkey.cfg (the launcher reads it before
 * the VM starts), NOT in menu.cfg, so there is one source of truth. */
static int set_vkey = 0;
/* v02.86: global defaults written to defaults.cfg - the launcher reads
 * this file before every round, and a per-game game.cfg overrides it.
 * Not in menu.cfg because menu.cfg is UI state the launcher never sees. */
static int set_profile = 0;        /* index into profile_choices[] */
static int set_scale = 0;          /* 0 fit, 1 stretch, 2 integer */
static int set_vibrate = 1;        /* 1 = allow Display.vibrate */
/* v02.90: MIDP permission policy. 0 = relaxed (default, no prompts),
 * 1 = strict (phoneME's own tables). Written to defaults.cfg as perms=,
 * read by the launcher and again by the policy loader itself. */
static int set_perms = 0;
/* v03.04: 3D backend. 0 = auto (hardware, then software), 1 = hardware,
 * 2 = software. Written to defaults.cfg as 3d=auto|hw|sw and read by
 * vita_3d.c itself. */
static int set_3d = 0;

static const char *mode3d_name(int m) {
    return (m == 1) ? "硬件" : (m == 2) ? "软件" : "自动";
}

#define DEFAULTS_CFG DATA_ROOT "/defaults.cfg"

static void defaults_save(void) {
    char buf[256];
    int n;
    SceUID fd;
    n = snprintf(buf, sizeof(buf),
                 "profile=%s\nscale=%s\nvkey=%d\nvibrate=%d\nperms=%s\n"
                 "3d=%s\n",
                 profile_choices[set_profile].name,
                 (set_scale == 1) ? "stretch" :
                 (set_scale == 2) ? "integer" : "fit",
                 set_vkey ? 1 : 0, set_vibrate ? 1 : 0,
                 set_perms ? "strict" : "relaxed",
                 (set_3d == 1) ? "hw" : (set_3d == 2) ? "sw" : "auto");
    if (n <= 0) {
        return;
    }
    sceIoRemove(DEFAULTS_CFG);   /* Vita3K ignores SCE_O_TRUNC */
    fd = sceIoOpen(DEFAULTS_CFG, SCE_O_WRONLY | SCE_O_CREAT, 0777);
    if (fd >= 0) {
        sceIoWrite(fd, buf, (SceSize)n);
        sceIoClose(fd);
    }
}

static void defaults_load(void) {
    char buf[256];
    SceUID fd;
    int n;
    const char *p;

    fd = sceIoOpen(DEFAULTS_CFG, SCE_O_RDONLY, 0);
    if (fd < 0) {
        return;
    }
    memset(buf, 0, sizeof(buf));
    n = sceIoRead(fd, buf, sizeof(buf) - 1);
    sceIoClose(fd);
    if (n <= 0) {
        return;
    }
    buf[n] = '\0';
    for (p = buf; *p != '\0'; ) {
        const char *nl = strpbrk(p, "\r\n");
        const char *end = (nl != NULL) ? nl : p + strlen(p);
        int len = (int)(end - p);
        char line[128];
        if (len > 0 && len < (int)sizeof(line)) {
            memcpy(line, p, (size_t)len);
            line[len] = '\0';
            if (line[0] != '#') {
                char *eq = strchr(line, '=');
                if (eq != NULL) {
                    char *k = line, *v = eq + 1, *ke = eq, *ve;
                    int i;
                    while (*k == ' ' || *k == '\t') k++;
                    while (ke > k && (ke[-1] == ' ' || ke[-1] == '\t')) ke--;
                    *ke = '\0';
                    while (*v == ' ' || *v == '\t') v++;
                    ve = v + strlen(v);
                    while (ve > v && (ve[-1] == ' ' || ve[-1] == '\t'))
                        *--ve = '\0';
                    if (strcmp(k, "profile") == 0) {
                        for (i = 0; i < PROFILE_N; i++)
                            if (strcmp(profile_choices[i].name, v) == 0)
                                set_profile = i;
                    } else if (strcmp(k, "scale") == 0) {
                        set_scale = (strcmp(v, "stretch") == 0) ? 1 :
                                    (strcmp(v, "integer") == 0) ? 2 : 0;
                    } else if (strcmp(k, "vkey") == 0) {
                        set_vkey = (v[0] == '1' || v[0] == 'y' || v[0] == 'Y');
                    } else if (strcmp(k, "vibrate") == 0) {
                        set_vibrate = !(v[0] == '0' || v[0] == 'n' ||
                                        v[0] == 'N');
                    } else if (strcmp(k, "perms") == 0) {
                        set_perms = (strcmp(v, "strict") == 0) ? 1 : 0;
                    } else if (strcmp(k, "3d") == 0) {
                        set_3d = (strcmp(v, "hw") == 0) ? 1 :
                                 (strcmp(v, "sw") == 0) ? 2 : 0;
                    }
                }
            }
        }
        if (nl == NULL) break;
        p = nl + 1;
    }
}

static const char *jit_name(int j) {
    return (j == 0) ? "关闭" : (j == 1) ? "仅首轮" : "每轮";
}

/* v03.79: corner names for the diagnostics overlay, in pos= order. */
static const char *diagpos_name(int p) {
    return (p == 1) ? "左上" : (p == 2) ? "右下" :
           (p == 3) ? "左下" : "右上";
}

/* v04.19: Java heap size override, in MiB, read by runMidlet.c before the VM
 * starts. 0 / absent = platform default (32 MiB), which does NOT fit a VM
 * domain block; <=15 lets the heap be executable and the JIT usable. */
#define HEAPMB_CFG DATA_ROOT "/heap_mb"

static int heapmb_get(void) {
    SceUID fd = sceIoOpen(HEAPMB_CFG, SCE_O_RDONLY, 0);
    char b[16];
    int n, v = 0, k;

    if (fd < 0) {
        return 0;
    }
    n = (int)sceIoRead(fd, b, sizeof(b) - 1);
    sceIoClose(fd);
    if (n <= 0) {
        return 0;
    }
    b[n] = '\0';
    for (k = 0; k < n && b[k] >= '0' && b[k] <= '9'; k++) {
        v = v * 10 + (b[k] - '0');
    }
    return (v >= 4 && v <= 64) ? v : 0;
}

static const char *heapmb_label(void) {
    static char lb[16];
    int mb = heapmb_get();

    if (mb == 0) {
        return "默认 32MB";
    }
    snprintf(lb, sizeof(lb), "%dMB", mb);
    return lb;
}

static void heapmb_set(int mb) {
    SceUID fd;
    char b[16];
    int n;

    sceIoRemove(HEAPMB_CFG);
    if (mb <= 0) {
        return;                 /* 0 = no file = platform default */
    }
    n = snprintf(b, sizeof(b), "%d\n", mb);
    fd = sceIoOpen(HEAPMB_CFG, SCE_O_WRONLY | SCE_O_CREAT, 0777);
    if (fd >= 0) {
        sceIoWrite(fd, b, n);
        sceIoClose(fd);
    }
}


static void settings_load(void) {
    SceUID fd = sceIoOpen(MENU_CFG, SCE_O_RDONLY, 0);
    /* v02.85/02.86: the keypad state lives in vkey.cfg (manual/legacy)
     * and defaults.cfg (what this page writes). Read vkey.cfg first,
     * then let defaults.cfg win when it has the key. */
    set_vkey = vita_vkey_load(NULL);
    defaults_load();
    if (fd >= 0) {
        char buf[64];
        int n = sceIoRead(fd, buf, sizeof(buf) - 1);
        sceIoClose(fd);
        if (n > 0) {
            char *p, *nl;
            buf[n] = '\0';
            for (p = buf; p != NULL; p = (nl != NULL) ? nl + 1 : NULL) {
                nl = strpbrk(p, "\r\n");
                if (nl != NULL) {
                    *nl = '\0';
                }
                if (strncmp(p, "view=", 5) == 0) {
                    set_list_grid = (p[5] == 'g');
                } else if (strncmp(p, "jit=", 4) == 0 &&
                           p[4] >= '0' && p[4] <= '2') {
                    set_jit = p[4] - '0';
                } else if (p[0] == 'g' || p[0] == 'l') {
                    /* v01.78 single-char format */
                    set_list_grid = (p[0] == 'g');
                }
            }
        }
    }
}

static void settings_save(void) {
    char buf[32];
    int len;
    SceUID fd;
    sceIoRemove(MENU_CFG); /* Vita3K ignores O_TRUNC (v01.16 lesson) */
    fd = sceIoOpen(MENU_CFG, SCE_O_WRONLY | SCE_O_CREAT, 0777);
    if (fd >= 0) {
        len = snprintf(buf, sizeof(buf), "view=%c\njit=%d\n",
                       set_list_grid ? 'g' : 'l', set_jit);
        sceIoWrite(fd, buf, len);
        sceIoClose(fd);
    }
}

/* Persist the JIT policy as launch.cfg line 4. The first 3 lines
 * (jar/class/orientation) are preserved; a missing launch.cfg gets a
 * jit-only file, which read_launch_cfg rejects (empty jar/class) so
 * the bundled defaults still win - only read_jit_policy sees it. */
static void write_jit_to_launch_cfg(int jit) {
    static char buf[512];
    char keep[3][160];
    int nkeep = 0, i, off = 0;
    SceUID fd;
    char *p, *nl;

    memset(keep, 0, sizeof(keep));
    fd = sceIoOpen(LAUNCH_CFG, SCE_O_RDONLY, 0);
    if (fd >= 0) {
        memset(buf, 0, sizeof(buf));
        (void)sceIoRead(fd, buf, sizeof(buf) - 1);
        sceIoClose(fd);
        for (p = buf; p != NULL && nkeep < 3;
             p = (nl != NULL) ? nl + 1 : NULL) {
            nl = strpbrk(p, "\r\n");
            if (nl != NULL) {
                *nl = '\0';
            }
            if (p[0] != '\0' && strncmp(p, "jit=", 4) != 0) {
                snprintf(keep[nkeep++], sizeof(keep[0]), "%s", p);
            }
        }
    }
    memset(buf, 0, sizeof(buf));
    for (i = 0; i < nkeep; i++) {
        off += snprintf(buf + off, sizeof(buf) - off, "%s\n", keep[i]);
    }
    snprintf(buf + off, sizeof(buf) - off, "jit=%d\n", jit);
    sceIoRemove(LAUNCH_CFG);
    fd = sceIoOpen(LAUNCH_CFG, SCE_O_WRONLY | SCE_O_CREAT, 0777);
    if (fd >= 0) {
        sceIoWrite(fd, buf, strlen(buf));
        sceIoClose(fd);
    }
}

static void settings_toggle(int item, char *msg, size_t msg_sz) {
    if (item == SET_VIEW) {
        set_list_grid = !set_list_grid;
        settings_save();
        snprintf(msg, msg_sz, "游戏视图: %s",
                 set_list_grid ? "网格" : "列表");
    } else if (item == SET_JIT) {
        set_jit = (set_jit + 1) % 3;
        settings_save();
        write_jit_to_launch_cfg(set_jit);
        snprintf(msg, msg_sz, "JIT: %s", jit_name(set_jit));
    } else if (item == SET_VKEY) {
        /* v02.85/02.86: toggle the on-screen keypad. Written to
         * defaults.cfg because the launcher (not the menu) applies it;
         * vkey.cfg stays readable for manual experiments. */
        set_vkey = !set_vkey;
        vita_vkey_set_enabled(set_vkey);
        defaults_save();
        snprintf(msg, msg_sz, "虚拟键盘: %s (需重启游戏)",
                 set_vkey ? "开" : "关");
    } else if (item == SET_PROFILE) {
        set_profile = (set_profile + 1) % PROFILE_N;
        defaults_save();
        snprintf(msg, msg_sz, "默认机型: %s (需重启游戏)",
                 profile_choices[set_profile].label);
    } else if (item == SET_SCALE) {
        set_scale = (set_scale + 1) % 3;
        defaults_save();
        snprintf(msg, msg_sz, "默认缩放: %s (需重启游戏)",
                 scale_name(set_scale));
    } else if (item == SET_VIBRATE) {
        set_vibrate = !set_vibrate;
        defaults_save();
        snprintf(msg, msg_sz, "振动: %s", set_vibrate ? "开" : "关");
    } else if (item == SET_PERMS) {
        /* v02.90: relaxed = every permission allowed (no dialog, the
         * default); strict = phoneME's own tables, where an unsigned jar
         * can be prompted. Requires a restart because the policy tables
         * are read once while the VM initialises. */
        set_perms = !set_perms;
        defaults_save();
        snprintf(msg, msg_sz, "权限: %s (需重启游戏)",
                 set_perms ? "严格" : "宽松");
    } else if (item == SET_3D) {
        set_3d = (set_3d + 1) % 3;
        defaults_save();
        snprintf(msg, msg_sz, "3D 模式: %s (需重启游戏)", mode3d_name(set_3d));
    } else if (item == SET_KEYMAP) {
        km_page = 1;
        km_sel = 0;
        km_top = 0;
    } else if (item == SET_DIAG) {
        /* v03.79: the overlay is the one thing a tester wants to flip mid-run,
         * and until now the only way was editing diag.cfg on a PC. State lives
         * in vita_diag.c; this writes it through and persists. */
        vita_diag_set_visible(!vita_diag_visible());
        vita_diag_save();
        snprintf(msg, msg_sz, "帧率浮层: %s",                 vita_diag_visible() ? "开" : "关");
    } else if (item == SET_DIAGPOS) {
        vita_diag_set_pos((vita_diag_get_pos() + 1) % 4);
        vita_diag_save();
        snprintf(msg, msg_sz, "浮层位置: %s", diagpos_name(vita_diag_get_pos()));
    } else if (item == SET_DIAGDETAIL) {
        vita_diag_set_detail(!vita_diag_get_detail());
        vita_diag_save();
        snprintf(msg, msg_sz, "浮层详细: %s",
                 vita_diag_get_detail() ? "开 (两行)" : "关 (一行)");
    } else if (item == SET_HEAPMB) {
        /* v04.48: the Java heap must fit ONE VM-domain block for the compiler
         * area (and therefore JIT code) to be executable. Blocks are capped at
         * 16 MiB by the API, but log186 measured that a 16 MiB request comes
         * back 0x80024b0b (MEMBLOCK_OVERFLOW) while 15 MiB succeeds, so the
         * usable maximum is a 15 MiB block = a 14 MB heap:
         *     14 MB -> 14688256 -> rounds to 15728640 (15 MiB)  works
         *     15 MB -> 15736832 -> rounds to 16777216 (16 MiB)  OVERFLOW
         * 15 MB used to be offered here; selecting it produced the log188
         * crash (JIT enabled, heap fell back to non-executable malloc).
         * 0 = leave the platform default alone (32 MB, JIT unavailable). */
        static const int steps[] = { 0, 14, 12, 10, 8, 6 };
        int cur = heapmb_get();
        int idx = 0, k;
        for (k = 0; k < (int)(sizeof(steps) / sizeof(steps[0])); k++) {
            if (steps[k] == cur) { idx = k; break; }
        }
        idx = (idx + 1) % (int)(sizeof(steps) / sizeof(steps[0]));
        heapmb_set(steps[idx]);
        if (steps[idx] == 0) {
            snprintf(msg, msg_sz, "Java 堆: 默认(32MB)");
        } else {
            snprintf(msg, msg_sz, "Java 堆: %dMB%s (需重启游戏)",
                     steps[idx],
                     (steps[idx] <= 14) ? " 可启用JIT" : "");
        }
    }
}

/* ---- file browser ---- */
static int fe_parent(char *out, size_t outsz);

/* Insert one entry keeping the invariant: ".." first, then dirs, then
 * files, each group alphabetically (lists are <= MAX_GAMES, insertion
 * sort is plenty). */
static int fe_has(const char *name) {
    int i;
    for (i = 0; i < fe_count; i++) {
        if (strcmp(fes[i].name, name) == 0) {
            return 1;
        }
    }
    return 0;
}

static void fe_insert(const char *name, const char *full, int type) {
    int i, j;
    if (fe_count >= MAX_GAMES) {
        return;
    }
    /* find the insertion slot: skip the parent, then dirs while this
     * is a dir and name sorts after, then files */
    i = (fe_count > 0 && strcmp(fes[0].name, "..") == 0) ? 1 : 0;
    if (type != FE_FILE) {
        while (i < fe_count && fes[i].type != FE_FILE &&
               strcmp(fes[i].name, name) < 0) {
            i++;
        }
    } else {
        i = fe_count;
    }
    /* shift up */
    for (j = fe_count; j > i; j--) {
        fes[j] = fes[j - 1];
    }
    snprintf(fes[i].name, sizeof(fes[i].name), "%s", name);
    snprintf(fes[i].full, sizeof(fes[i].full), "%s", full);
    fes[i].type = type;
    fe_count++;
}

/* Scan `path` and replace the file-browser listing.
 *
 * Returns 1 when the listing was replaced, 0 when the directory could
 * not be opened (the previous listing and fe_cwd are then untouched -
 * a failed open must not look like an empty folder).
 *
 * v01.80 aliasing fix: callers pass fes[i].full, i.e. a pointer INTO
 * the array this function rewrites. Using the argument directly meant
 * the ".." entry (and later sorted inserts, which shift fes[]) overwrote
 * the very string being scanned. sceIoDopen() then opened the wrong
 * directory - or a bare entry name with no slash - and every full path
 * built afterwards was garbage. Visible symptoms on hardware:
 *   - opening a folder showed an empty list (Dopen on the clobbered
 *     path failed), and
 *   - listings showed names but selecting anything did nothing: the
 *     entry's `full` did not point at the file at all.
 * So the path is copied into a private buffer before any fes[] write. */
static int fe_scan(const char *path) {
    char target[256];
    size_t tlen;
    SceUID d;
    SceIoDirent ent;

    if (path == NULL || path[0] == '\0') {
        return 0;
    }
    snprintf(target, sizeof(target), "%s", path);
    tlen = strlen(target);
    /* volume root needs the separator for sceIoDopen ("ux0:" -> "ux0:/") */
    if (tlen > 0 && target[tlen - 1] == ':' && tlen + 1 < sizeof(target)) {
        target[tlen] = '/';
        target[tlen + 1] = '\0';
    }

    if (strcmp(target, "/") == 0) {
        /* Root: build the partition list by probing (see fe_devices),
         * then merge anything sceIoDopen("/") returns that we did not
         * already add. The old code early-returned when sceIoDopen
         * failed, which skipped the fallback and left the tab empty. */
        int i;
        snprintf(fe_cwd, sizeof(fe_cwd), "%s", target);
        fe_count = 0;
        for (i = 0; i < (int)(sizeof(fe_devices) / sizeof(fe_devices[0]));
             i++) {
            if (dir_exists(fe_devices[i])) {
                fe_insert(fe_devices[i], fe_devices[i], FE_DIR);
            }
        }
        d = sceIoDopen("/");
        if (d >= 0) {
            for (;;) {
                memset(&ent, 0, sizeof(ent));
                if (sceIoDread(d, &ent) <= 0) {
                    break;
                }
                if (ent.d_name[0] == '\0' || ent.d_name[0] == '.' ||
                    fe_has(ent.d_name)) {
                    continue;
                }
                fe_insert(ent.d_name, ent.d_name, FE_DIR);
            }
            sceIoDclose(d);
        }
        if (fe_count == 0) {
            /* last resort: neither the probe nor the enumeration
             * answered (both should be impossible on a real Vita).
             * ux0: is where the data dir lives, so list it verbatim so
             * the tab is never blank and the user sees a device name
             * to work from. */
            fe_insert("ux0:", "ux0:", FE_DIR);
            fe_insert("ur0:", "ur0:", FE_DIR);
        }
        return 1;
    }

    /* open BEFORE clearing: on failure keep the current directory and
     * listing instead of presenting an empty folder */
    d = sceIoDopen(target);
    if (d < 0) {
        return 0;
    }

    snprintf(fe_cwd, sizeof(fe_cwd), "%s", target);
    fe_count = 0;
    /* virtual ".." parent (except at the sandbox root) so up/down/tap
     * all treat going up like opening any other directory */
    {
        char parent[256];
        if (fe_parent(parent, sizeof(parent))) {
            fe_insert("..", parent, FE_DIR);
        }
    }
    for (;;) {
        int type;
        char full[256];
        memset(&ent, 0, sizeof(ent));
        if (sceIoDread(d, &ent) <= 0) {
            break;
        }
        if (strcmp(ent.d_name, ".") == 0 || strcmp(ent.d_name, "..") == 0) {
            continue;
        }
        {
            /* build from `target`, never from `path`: fe_insert() below
             * may overwrite the caller's buffer */
            const char *sep = (target[strlen(target) - 1] == '/') ? "" : "/";
            snprintf(full, sizeof(full), "%s%s%s", target, sep, ent.d_name);
        }
        if (SCE_S_ISDIR(ent.d_stat.st_mode)) {
            type = FE_DIR;
        } else {
            type = (strstr(ent.d_name, ".jar") != NULL) ? FE_JAR : FE_FILE;
        }
        fe_insert(ent.d_name, full, type);
    }
    sceIoDclose(d);
    return 1;
}

static int fe_parent(char *out, size_t outsz) {
    char *slash;
    size_t n;
    int dev_root;

    if (fe_cwd[0] == '\0' || strcmp(fe_cwd, "/") == 0) {
        return 0; /* Vita pseudo-root - no parent */
    }
    snprintf(out, outsz, "%s", fe_cwd);
    slash = strrchr(out, '/');
    if (slash == NULL) {
        snprintf(out, outsz, "/"); /* "ux0:" -> "/" */
        return 1;
    }
    /* true when the separator being dropped is the last character, i.e.
     * we go up from a volume root ("ux0:/"), not from a directory
     * inside it ("ux0:/data") */
    dev_root = (slash[1] == '\0');
    if (slash == out) {
        slash[1] = '\0'; /* "/ux0:" -> "/" */
    } else {
        *slash = '\0';
    }
    /* v01.80: fe_scan() stores volume roots with their separator
     * ("ux0:/"), so going up trims them to "ux0:" - the volume itself,
     * whose parent is the pseudo-root. Without this, O/CIRCLE from a
     * volume root looped on the same directory. "ux0:/data" must still
     * go up to the volume, not to "/". */
    n = strlen(out);
    if (dev_root && n > 0 && out[n - 1] == ':') {
        snprintf(out, outsz, "/");
    }
    return 1;
}

/* Copy a jar across partitions (sceIoRename fails EXDEV there).
 * Removes the destination first - Vita3K ignores O_TRUNC (v01.16). */
static int copy_jar(const char *src, const char *dst) {
    SceUID in = sceIoOpen(src, SCE_O_RDONLY, 0);
    SceUID out;
    static char buf[8192];
    if (in < 0) {
        return -1;
    }
    sceIoRemove(dst);
    out = sceIoOpen(dst, SCE_O_WRONLY | SCE_O_CREAT, 0777);
    if (out < 0) {
        sceIoClose(in);
        return -1;
    }
    for (;;) {
        ssize_t n = sceIoRead(in, buf, sizeof(buf));
        if (n <= 0) {
            break;
        }
        if (sceIoWrite(out, buf, n) < 0) {
            sceIoClose(in);
            sceIoClose(out);
            return -1;
        }
    }
    sceIoClose(in);
    sceIoClose(out);
    return 0;
}

/* install a jar picked from the inbox tab or the file browser by
 * moving it into games/<base>/game.jar (same procedure install_inbox
 * uses) and returning 1 when it was installed */
static int install_jar(const char *jarpath, char *msg, size_t msg_sz) {
    char base[128], dstdir[200], dstjar[256];
    const char *bn;
    size_t blen;

    bn = strrchr(jarpath, '/');
    bn = (bn != NULL) ? bn + 1 : jarpath;
    blen = strlen(bn);
    if (blen < 5 || strcmp(bn + blen - 4, ".jar") != 0) {
        snprintf(msg, msg_sz, "not a .jar");
        return 0;
    }
    snprintf(base, sizeof(base), "%s", bn);
    /* v01.78g: use the length that actually landed in base[] - a path with
     * a basename longer than the buffer made base[blen-4] write past it. */
    blen = strlen(base);
    if (blen >= 5 && strcmp(base + blen - 4, ".jar") == 0) {
        base[blen - 4] = '\0';
    }
    snprintf(dstdir, sizeof(dstdir), GAMES_DIR "/%s", base);
    sceIoMkdir(dstdir, 0777);
    snprintf(dstjar, sizeof(dstjar), "%s/" JAR_NAME, dstdir);
    if (sceIoRename(jarpath, dstjar) < 0) {
        /* cross-partition (e.g. uma0: -> ux0:) rename fails EXDEV;
         * fall back to a copy. The SOURCE IS KEPT - a jar on another
         * partition is the user's archive, not an inbox drop. */
        if (copy_jar(jarpath, dstjar) < 0) {
            snprintf(msg, msg_sz, "install failed");
            return 0;
        }
    }
    {
        GameEntry tmp;
        memset(&tmp, 0, sizeof(tmp));
        snprintf(tmp.dir, sizeof(tmp.dir), "%s", dstdir);
        snprintf(tmp.jar, sizeof(tmp.jar), "%s/" JAR_NAME, dstdir);
        parse_manifest_class(tmp.jar, tmp.cls, sizeof(tmp.cls));
        validate_game_class(&tmp, 1);
        save_cfg(&tmp);
    }
    snprintf(msg, msg_sz, "installed: %s", base);
    return 1;
}

/* Act on file-browser entry `idx`: enter a directory, or install a jar.
 *
 * The entry's path is copied out FIRST - fe_scan() and install_jar()
 * both reuse/rewrite the fes[] slot it lives in, and the previous code
 * passed &fes[idx].full straight through (see the fe_scan() comment).
 *
 * Returns 1 when the listing changed (folder entered / jar installed),
 * 0 otherwise; msg always describes the result so a press never looks
 * like it did nothing. */
static int fe_activate(int idx, char *msg, size_t msg_sz) {
    char sel[256];
    char nm[64];
    int type;

    if (idx < 0 || idx >= fe_count) {
        return 0;
    }
    snprintf(sel, sizeof(sel), "%s", fes[idx].full);
    snprintf(nm, sizeof(nm), "%s", fes[idx].name);
    type = fes[idx].type;

    if (type == FE_DIR) {
        if (!fe_scan(sel)) {
            snprintf(msg, msg_sz, "无法打开: %.80s", nm);
            return 0;
        }
        return 1;
    }
    if (type != FE_JAR) {
        /* previously silent - the cursor sat on a non-jar entry and the
         * X press appeared to be ignored */
        snprintf(msg, msg_sz, "不是 .jar: %.80s", nm);
        return 0;
    }
    if (install_jar(sel, msg, msg_sz)) {
        scan_games();
        fe_scan(fe_cwd);
        return 1;
    }
    return 0;
}

/* inbox scan for the "未安装" tab: list .jar files in inbox/ */
static char inbox_names[MAX_GAMES][104];
static int inbox_count = 0;

static void inbox_scan(void) {
    SceUID d;
    SceIoDirent ent;
    inbox_count = 0;
    if (!dir_exists(INBOX_DIR)) {
        sceIoMkdir(INBOX_DIR, 0777);
    }
    d = sceIoDopen(INBOX_DIR);
    if (d < 0) {
        return;
    }
    for (;;) {
        size_t blen;
        memset(&ent, 0, sizeof(ent));
        if (sceIoDread(d, &ent) <= 0 || inbox_count >= MAX_GAMES) {
            break;
        }
        if (SCE_S_ISDIR(ent.d_stat.st_mode)) {
            continue;
        }
        blen = strlen(ent.d_name);
        if (blen < 5 || blen > 100 || strcmp(ent.d_name + blen - 4, ".jar") != 0) {
            continue;
        }
        snprintf(inbox_names[inbox_count], 104, "%s", ent.d_name);
        inbox_count++;
    }
    sceIoDclose(d);
}

/* v01.78h exit-via-longjmp state (see vita_menu.h for the rationale).
 * Defined here so the jmp_buf lives in .bss: whatever zeroes the top
 * of the menu frame cannot touch it. */
jmp_buf vita_menu_escape;
int vita_menu_result = 0;

int vita_menu_run(VitaGameSel *out) {
    /* Allocate ONCE per process and reuse across rounds: menu_fb is a
     * static, so a plain memalign here used to overwrite the pointer
     * every round, leaking the old 2MB block (6h session with several
     * menu<->game rounds = tens of MB gone). The "no free on exit"
     * policy below only ever meant "don't hand the block back while
     * the display still scans it out" - it never justified leaking.
     * v01.67: CDRAM (uncached) instead of memalign - a cached heap
     * buffer is invisible to the display controller on real hw. */
    if (menu_fb == NULL) {
        if (vita_fbmem_alloc(&menu_fb_blk, FB_W * FB_H * 4 * 2) == 0) {
            menu_fb_front = (uint32_t *)menu_fb_blk.base;
            menu_fb_back = menu_fb_front + FB_W * FB_H;
            menu_fb = menu_fb_back;
        }
    }
    if (menu_fb == NULL) {
        crumb_printf("menu: CDRAM alloc FAILED block=0x%08x",
                     (unsigned)menu_fb_blk.block);
        crumb_flush();
        vita_menu_result = 0;
        longjmp(vita_menu_escape, 1);
        return 0; /* not reached */
    }
    crumb_printf("menu: fb=%p/%p block=0x%08x", menu_fb_front, menu_fb_back,
                 (unsigned)menu_fb_blk.block);
    crumb_flush();

    if (!dir_exists(GAMES_DIR)) {
        sceIoMkdir(GAMES_DIR, 0777);
    }
    settings_load();
    menu_touch_init();
    scan_games();
    inbox_scan();
    fe_scan(fe_root);

    /* tab 0=已安装 1=未安装 2=文件 3=设置 4=退出 */
    int tab = 0;
    int focus = 0;                 /* 0 = content area, 1 = tab column */
    int sel = 0, top = 0;          /* installed tab cursor/scroll */
    int gsel = 0, gtop = 0;        /* grid cursor/scroll */
    int isel = 0, itop = 0;        /* inbox cursor/scroll */
    int fsel = 0, ftop = 0;        /* file browser cursor/scroll */
    int setsel = 0;                /* settings option cursor */
    int settop = 0;                /* settings scroll (setsel clamp) */
    int mode = 0;                  /* 0 = normal, 1 = dialog */
    int dlg_sel = DLG_LAUNCH;
    int confirm_del = 0;
    int have_selection = 0;
    char msg[120] = "";
    unsigned long long msg_us = 0;
    /* v01.78h forensics: the mystery zero-writer that cleared the old
     * saved-register block may strike this frame again. A known pattern
     * checked by the 1 s heartbeat brackets any corruption to a single
     * second - the "menu sen BAD" line names the exact heartbeat.
     * volatile: without it GCC may fold the checks away (sen never has
     * its address taken, so alias analysis assumes calls can't touch it). */
    volatile unsigned sen[8];
    int k;
    for (k = 0; k < 8; k++) sen[k] = 0xC0DE0000u + (unsigned)k;

    for (;;) {
        unsigned int btn = poll_buttons();
        int tap_x = 0, tap_y = 0;
        int tapped = poll_tap(&tap_x, &tap_y);
        int i;
        const int row_h = 30;
        const int lines = 13;
        /* v03.79: visible rows in the 设置 tab. It is a single-column list with
         * 30 px rows starting at y=64 plus a hint block and the storage figures
         * underneath, so 10 rows is what fits in 544 px. Kept separate from
         * `lines` because the list tabs are 2-line rows and must not shrink. */
        const int SET_LINES = 10;
        const int cols = 4;
        const int cell_w = 170, cell_h = 150;
        const int grid_rows = 3;
        int y;

        /* v01.68 black-screen diagnostics: one heartbeat per second in
         * crumb.log. If these appear, the menu LOOP is alive and the
         * question narrows to the display path (flip rc / fb address);
         * if they stop, the loop died and the last one printed marks
         * the vicinity. Remove once the real-hw display issue is
         * closed. */
        {
            unsigned long long now = sceKernelGetProcessTimeWide();
            if (now - menu_hb_us > 1000000ULL) {
                menu_hb_us = now;
                crumb_printf("menu hb: flips=%u rc=0x%08x games=%d btn=0x%x"
                             " tab=%d focus=%d view=%c",
                             menu_flip_count,
                             (unsigned)menu_last_flip_rc,
                             game_count, btn, tab, focus,
                             set_list_grid ? 'g' : 'l');
                /* v01.78h: sentinel check (see sen[] above) */
                for (k = 0; k < 8; k++) {
                    if (sen[k] != 0xC0DE0000u + (unsigned)k) {
                        crumb_printf("menu sen BAD i=%d w=%08x want=%08x",
                                     k, sen[k], 0xC0DE0000u + (unsigned)k);
                        break;
                    }
                }
                crumb_flush();
            }
        }

        /* key-mapping sub page owns the input while open */
        if (km_page) {
            const int kn = vita_keymap_count();
            int leave = 0;
            if (btn & SCE_CTRL_UP) {
                if (km_sel > 0) km_sel--;
            }
            if (btn & SCE_CTRL_DOWN) {
                if (km_sel < kn) km_sel++;
            }
            if (km_sel < kn) {
                if (btn & SCE_CTRL_LEFT) vita_keymap_cycle(km_sel, -1);
                if (btn & (SCE_CTRL_RIGHT | SCE_CTRL_CROSS)) {
                    vita_keymap_cycle(km_sel, 1);
                }
            } else if (btn & SCE_CTRL_CROSS) {
                vita_keymap_reset();
                snprintf(msg, sizeof(msg), "已恢复默认映射");
                msg_us = sceKernelGetProcessTimeWide();
            }
            if (km_sel < km_top) km_top = km_sel;
            if (km_sel >= km_top + KM_LINES) km_top = km_sel - KM_LINES + 1;
            if (tapped && tap_x >= TABBAR_W && tap_y >= 64) {
                int row = (tap_y - 64) / row_h + km_top;
                if (row >= 0 && row < kn) {
                    km_sel = row;
                    vita_keymap_cycle(row, 1);
                } else if (row == kn) {
                    km_sel = kn;
                    vita_keymap_reset();
                }
                tapped = 0;
            }
            if (btn & SCE_CTRL_CIRCLE) leave = 1;
            if (tapped && tap_x < TABBAR_W) leave = 1; /* tab tap passes on */
            if (leave) {
                km_page = 0;
                vita_keymap_save();
                snprintf(msg, sizeof(msg), "按键映射已保存");
                msg_us = sceKernelGetProcessTimeWide();
            }
            btn = 0; /* nothing leaks into the tab/list logic */
            if (km_page) tapped = 0;
        }

        /* ---- tab switching: L/R, dpad left/right, or tap the column ----
         * Two-focus model: the cursor lives in the content area; LEFT
         * pulls it out to the tab column (UP/DOWN then walk the tabs),
         * RIGHT goes back in. The grid only reacts to LEFT when the
         * cursor is already on the edge column, so in-row navigation
         * is never hijacked. L/R remain global shortcuts from either
         * focus (focus stays where it is). */
        if (mode == 0) {
            if (btn & SCE_CTRL_LTRIGGER) {
                if (tab > 0) tab--;
                msg[0] = '\0';
            }
            if (btn & SCE_CTRL_RTRIGGER) {
                if (tab < TAB_N - 1) tab++;
                msg[0] = '\0';
            }
            if (focus == 1) {
                if (btn & SCE_CTRL_UP) {
                    if (tab > 0) tab--;
                    msg[0] = '\0';
                }
                if (btn & SCE_CTRL_DOWN) {
                    if (tab < TAB_N - 1) tab++;
                    msg[0] = '\0';
                }
                if (btn & SCE_CTRL_RIGHT || btn & SCE_CTRL_CROSS) {
                    focus = 0; /* go back into the content */
                    msg[0] = '\0';
                }
            }
            if (tapped && tap_x < TABBAR_W) {
                int ntab = tap_y / TAB_H;
                if (ntab >= 0 && ntab < TAB_N) {
                    if (ntab == TAB_N - 1) {
                        break; /* tap 退出 = quit */
                    }
                    tab = ntab;
                    focus = 0;
                    msg[0] = '\0';
                }
            }
        }

        if (mode == 0) {
            int *psel = &sel, *ptop = &top;
            int nent = game_count;
            int in_content = (focus == 0); /* dpad drives the cursor */
            int has_cursor = 1;            /* current tab has a list */

            if (tab == 0) {
                /* 已安装 has TWO views: list (sel/top) and grid
                 * (gsel/gtop). v01.78f wrote this first branch as
                 * `tab == 0 && GRIDMODE` with a trailing bare `else`
                 * meant for tab 4 - so the LIST case fell through to
                 * that else, has_cursor went 0 and the dpad was dead
                 * on 已安装. 设置 kept working because it owns an
                 * explicit branch. Keep the mode test INSIDE tab 0. */
                if (set_list_grid == TAB_GRIDMODE) {
                    psel = &gsel; ptop = &gtop;
                }
                nent = game_count;
            } else if (tab == 1) {
                psel = &isel; ptop = &itop;
                nent = inbox_count;
            } else if (tab == 2) {
                psel = &fsel; ptop = &ftop;
                nent = fe_count;
            } else if (tab == 3) {
                /* settings: its own cursor - defaulting to sel/top here
                 * made UP/DOWN move the installed-games list instead */
                psel = &setsel; ptop = &settop;
                nent = SET_N;
            } else {
                /* tab 4 (退出): no list, dpad must not move anything */
                has_cursor = 0;
                nent = 0;
            }

            if (in_content && has_cursor && (btn & SCE_CTRL_UP)) {
                if (tab == 0 && set_list_grid == TAB_GRIDMODE && gsel >= cols) {
                    gsel -= cols;
                } else if (*psel > 0) {
                    (*psel)--;
                }
            }
            if (in_content && has_cursor && (btn & SCE_CTRL_DOWN)) {
                if (tab == 0 && set_list_grid == TAB_GRIDMODE &&
                    gsel + cols < nent) {
                    gsel += cols;
                } else if (*psel < nent - 1) {
                    (*psel)++;
                }
            }
            if (btn & SCE_CTRL_LEFT) {
                if (!in_content) {
                    /* tab column focus: L/R edge already handled; LEFT
                     * here is a no-op (already at the edge) */
                } else if (tab == 0 && set_list_grid == TAB_GRIDMODE &&
                           (gsel % cols) != 0) {
                    gsel--;        /* grid: step within the row */
                } else {
                    focus = 1;     /* list view / edge column: out to tabs */
                    msg[0] = '\0';
                }
            }
            if (btn & SCE_CTRL_RIGHT) {
                if (!in_content) {
                    focus = 0;     /* back into the content */
                    msg[0] = '\0';
                } else if (tab == 0 && set_list_grid == TAB_GRIDMODE &&
                           gsel < nent - 1 && (gsel % cols) != cols - 1) {
                    gsel++;        /* grid: step within the row */
                }
                /* list view: RIGHT in content is a no-op (already at
                 * the right edge; LEFT is the only way out) */
            }
            /* scroll clamp: list rows use item units, the grid keeps
             * gtop in ROW units (gsel stays item units) */
            if (tab == 0 && set_list_grid == TAB_GRIDMODE) {
                if (gsel < gtop * cols) gtop = gsel / cols;
                if (gsel >= (gtop + grid_rows) * cols) {
                    gtop = gsel / cols - grid_rows + 1;
                }
            } else if (has_cursor) {
                /* v03.79: the 设置 tab shows SET_LINES rows, not `lines`. */
                int cap = (tab == 3) ? SET_LINES : lines;
                if (*psel < *ptop) *ptop = *psel;
                if (*psel >= *ptop + cap) *ptop = *psel - cap + 1;
            }

            if (btn & SCE_CTRL_SELECT) {
                if (tab == 0) {
                    scan_games();
                    inbox_scan();
                    snprintf(msg, sizeof(msg), "rescanned: %d game(s)",
                             game_count);
                    msg_us = sceKernelGetProcessTimeWide();
                } else if (tab == 2) {
                    if (fe_scan(fe_cwd)) {
                        snprintf(msg, sizeof(msg), "refreshed");
                    } else {
                        snprintf(msg, sizeof(msg), "刷新失败: %.80s", fe_cwd);
                    }
                    msg_us = sceKernelGetProcessTimeWide();
                }
            }

            if (tab == 0) { /* ---- 已安装 ---- */
                if ((btn & SCE_CTRL_SQUARE) && game_count > 0) {
                    /* toggle view mode lazily; persisted on change */
                    set_list_grid = !set_list_grid;
                    settings_save();
                    gsel = sel; /* keep the cursor near the same game */
                    gtop = 0;
                }
                if ((btn & SCE_CTRL_CROSS) && game_count > 0) {
                    if (set_list_grid == TAB_GRIDMODE) {
                        sel = gsel; /* the dialog indexes games[sel] */
                    }
                    mode = 1;
                    dlg_sel = DLG_LAUNCH;
                    confirm_del = 0;
                    /* v02.84: build MIDlet-1..N for this jar once, and
                     * honour a saved choice (game.cfg midlet=N). */
                    midlets_scan(games[sel].jar);
                    if (games[sel].midlet_idx >= 1 &&
                        games[sel].midlet_idx <= midlet_count) {
                        snprintf(games[sel].cls, sizeof(games[sel].cls), "%s",
                                 midlets[games[sel].midlet_idx - 1].cls);
                    }
                }
                if ((btn & SCE_CTRL_CROSS) && game_count == 0) {
                    break; /* fall back to launch.cfg / Hello */
                }
                if (tapped && tap_x >= TABBAR_W && set_list_grid == TAB_GRIDMODE) {
                    int cx = (tap_x - TABBAR_W) / cell_w;
                    int cy = (tap_y - 64) / cell_h;
                    int idx = (gtop + cy) * cols + cx;
                    if (cy >= 0 && cy < grid_rows && cx >= 0 && cx < cols &&
                        idx >= 0 && idx < game_count) {
                        gsel = idx;
                        sel = gsel;
                        mode = 1;
                        dlg_sel = DLG_LAUNCH;
                        confirm_del = 0;
                        midlets_scan(games[sel].jar);
                        if (games[sel].midlet_idx >= 1 &&
                            games[sel].midlet_idx <= midlet_count) {
                            snprintf(games[sel].cls,
                                     sizeof(games[sel].cls), "%s",
                                     midlets[games[sel].midlet_idx - 1].cls);
                        }
                    }
                }
                if (btn & SCE_CTRL_TRIANGLE) {
                    /* Always allow falling back to launch.cfg / bundled
                     * tests even with games installed (needed to run
                     * ToneTest etc. for bring-up). */
                    break;
                }
            } else if (tab == 1) { /* ---- 未安装 ---- */
                if ((btn & SCE_CTRL_START) || (btn & SCE_CTRL_CROSS)) {
                    install_inbox(msg, sizeof(msg));
                    msg_us = sceKernelGetProcessTimeWide();
                    scan_games();
                    inbox_scan();
                    if (isel >= inbox_count) isel = inbox_count - 1;
                    if (isel < 0) isel = 0;
                }
                if (tapped && tap_x >= TABBAR_W && inbox_count > 0) {
                    int row = (tap_y - 64) / row_h;
                    int idx = itop + row;
                    if (row >= 0 && idx >= 0 && idx < inbox_count) {
                        char jp[256];
                        isel = idx;
                        /* install JUST the tapped jar (X installs all) */
                        snprintf(jp, sizeof(jp), "%s/%s", INBOX_DIR,
                                 inbox_names[idx]);
                        install_jar(jp, msg, sizeof(msg));
                        msg_us = sceKernelGetProcessTimeWide();
                        scan_games();
                        inbox_scan();
                        if (isel >= inbox_count) isel = inbox_count - 1;
                        if (isel < 0) isel = 0;
                    }
                }
            } else if (tab == 2) { /* ---- 文件 ---- */
                if (btn & SCE_CTRL_CIRCLE) {
                    char parent[256];
                    if (fe_parent(parent, sizeof(parent))) {
                        if (fe_scan(parent)) {
                            fsel = ftop = 0;
                        } else {
                            snprintf(msg, sizeof(msg), "无法打开: %.80s",
                                     parent);
                            msg_us = sceKernelGetProcessTimeWide();
                        }
                    }
                }
                if (btn & SCE_CTRL_CROSS && fe_count > 0) {
                    if (fsel < 0) fsel = 0;
                    if (fsel >= fe_count) fsel = fe_count - 1;
                    if (fe_activate(fsel, msg, sizeof(msg))) {
                        /* land on the first real entry, not on "..":
                         * otherwise the first X press just walks up */
                        fsel = (fe_count > 1) ? 1 : 0;
                        ftop = 0;
                    }
                    msg_us = sceKernelGetProcessTimeWide();
                }
                if (tapped && tap_x >= TABBAR_W && fe_count > 0) {
                    /* tab 2 content starts at y=80 (below the cwd line) */
                    int row = (tap_y - 80) / row_h;
                    int idx = ftop + row;
                    if (row >= 0 && idx >= 0 && idx < fe_count) {
                        fsel = idx;
                        if (fe_activate(idx, msg, sizeof(msg))) {
                            fsel = (fe_count > 1) ? 1 : 0;
                            ftop = 0;
                        }
                        msg_us = sceKernelGetProcessTimeWide();
                    }
                }
            } else if (tab == 3) { /* ---- 设置 ---- */
                if (btn & SCE_CTRL_CROSS) {
                    settings_toggle(setsel, msg, sizeof(msg));
                    msg_us = sceKernelGetProcessTimeWide();
                }
                if (tapped && tap_x >= TABBAR_W && tap_y >= 64) {
                    int row = (tap_y - 64) / row_h;
                    /* v03.79: the page scrolls, so the tapped row is an offset
                     * into the visible window, not an item index. Without
                     * settop here a tap on the first visible row of a scrolled
                     * page would toggle the wrong option. */
                    if (row >= 0 && row < SET_LINES &&
                        settop + row < SET_N) {
                        setsel = settop + row;
                        settings_toggle(setsel, msg, sizeof(msg));
                        msg_us = sceKernelGetProcessTimeWide();
                    }
                }
            }
            /* tab 4 (退出) unreachable: tapping it breaks the loop and
             * no d-pad navigation enters it (R stops at TAB_N-2 via tap,
             * but RTRIGGER can land on it) */
            if (tab == TAB_N - 1 && (btn & SCE_CTRL_CROSS)) {
                break;
            }
        } else {
            GameEntry *g = &games[sel];

            if (btn & SCE_CTRL_UP) {
                if (dlg_sel > 0) dlg_sel--;
            }
            if (btn & SCE_CTRL_DOWN) {
                if (dlg_sel < DLG_N - 1) dlg_sel++;
            }
            if (btn & SCE_CTRL_CIRCLE) {
                mode = 0;
                confirm_del = 0;
            }
            if (btn & SCE_CTRL_CROSS) {
                if (dlg_sel == DLG_ORIENT) {
                    g->landscape = !g->landscape;
                    /* v02.82: an explicit screen size wins over the
                     * orientation, so swap it too - otherwise flipping
                     * this item would look like a no-op. */
                    if (g->width > 0 && g->height > 0) {
                        int t = g->width;
                        g->width = g->height;
                        g->height = t;
                    }
                    save_cfg(g);
                    snprintf(msg, sizeof(msg), "%s: %s", g->name,
                             g->landscape ? "landscape" : "portrait");
                    msg_us = sceKernelGetProcessTimeWide();
                } else if (dlg_sel == DLG_SCREEN) {
                    screen_cycle(g);
                    save_cfg(g);
                    if (g->width > 0 && g->height > 0) {
                        snprintf(msg, sizeof(msg), "screen: %dx%d",
                                 g->width, g->height);
                    } else {
                        snprintf(msg, sizeof(msg), "screen: auto (%s)",
                                 g->landscape ? "landscape" : "portrait");
                    }
                    msg_us = sceKernelGetProcessTimeWide();
                } else if (dlg_sel == DLG_SCALE) {
                    g->scale_mode = (g->scale_mode + 1) % 3;
                    save_cfg(g);
                    snprintf(msg, sizeof(msg), "scale: %s",
                             scale_name(g->scale_mode));
                    msg_us = sceKernelGetProcessTimeWide();
                } else if (dlg_sel == DLG_PROFILE) {
                    int i = (profile_index(g) + 1) % PROFILE_N;
                    snprintf(g->profile, sizeof(g->profile), "%s",
                             profile_choices[i].name);
                    save_cfg(g);
                    snprintf(msg, sizeof(msg), "profile: %s",
                             profile_choices[i].label);
                    msg_us = sceKernelGetProcessTimeWide();
                } else if (dlg_sel == DLG_MIDLET) {
                    if (midlet_count <= 1) {
                        snprintf(msg, sizeof(msg),
                                 "only one MIDlet in this jar");
                    } else {
                        midlet_cycle(g);
                        save_cfg(g);
                        snprintf(msg, sizeof(msg), "midlet %d/%d: %.40s",
                                 g->midlet_idx, midlet_count, g->cls);
                    }
                    msg_us = sceKernelGetProcessTimeWide();
                } else if (dlg_sel == DLG_LOCALE) {
                    int i = (locale_index(g) + 1) % LOCALE_N;
                    snprintf(g->locale, sizeof(g->locale), "%s",
                             locale_choices[i]);
                    save_cfg(g);
                    snprintf(msg, sizeof(msg), "locale: %s",
                             locale_label(g));
                    msg_us = sceKernelGetProcessTimeWide();
                } else if (dlg_sel == DLG_VKEY) {
                    g->vkey = cycle_follow(g->vkey, 1);
                    save_cfg(g);
                    snprintf(msg, sizeof(msg), "vkey: %s",
                             follow_onoff(g->vkey));
                    msg_us = sceKernelGetProcessTimeWide();
                } else if (dlg_sel == DLG_VIBRATE) {
                    g->vibrate = cycle_follow(g->vibrate, 1);
                    save_cfg(g);
                    snprintf(msg, sizeof(msg), "vibrate: %s",
                             follow_onoff(g->vibrate));
                    msg_us = sceKernelGetProcessTimeWide();
                } else if (dlg_sel == DLG_PERMS) {
                    g->perms = cycle_follow(g->perms, 1);
                    save_cfg(g);
                    snprintf(msg, sizeof(msg), "perms: %s",
                             follow_label(g->perms, "strict", "relaxed"));
                    msg_us = sceKernelGetProcessTimeWide();
                } else if (dlg_sel == DLG_JIT) {
                    g->jit = cycle_follow(g->jit, 2);
                    save_cfg(g);
                    snprintf(msg, sizeof(msg), "jit: %s",
                             jit_label_pergame(g->jit));
                    msg_us = sceKernelGetProcessTimeWide();
                } else if (dlg_sel == DLG_UNINSTALL) {
                    if (!confirm_del) {
                        confirm_del = 1;
                    } else {
                        uninstall_game(g);
                        snprintf(msg, sizeof(msg), "deleted");
                        msg_us = sceKernelGetProcessTimeWide();
                        confirm_del = 0;
                        mode = 0;
                        scan_games();
                        if (sel >= game_count) sel = game_count - 1;
                        if (sel < 0) sel = 0;
                    }
                } else if (dlg_sel == DLG_BACK) {
                    mode = 0;
                    confirm_del = 0;
                } else if (dlg_sel == DLG_LAUNCH) {
                    if (g->cls[0] == '\0') {
                        snprintf(msg, sizeof(msg), "no MIDlet class in jar");
                        msg_us = sceKernelGetProcessTimeWide();
                    } else {
                        save_cfg(g);
                        memset(out, 0, sizeof(*out));
                        snprintf(out->jar, sizeof(out->jar), "%s", g->jar);
                        snprintf(out->cls, sizeof(out->cls), "%s", g->cls);
                        snprintf(out->orient, sizeof(out->orient), "%s",
                                 g->landscape ? "landscape" : "portrait");
                        have_selection = 1;
                        break;
                    }
                }
            }
        }

        /* ---- draw ---- */
        fill_rect(0, 0, FB_W, FB_H, C_BG);
        fill_rect(0, 0, FB_W, 52, C_PANEL);
        draw_text(20, 14, "J2ME Player", 4, C_TITLE);
        draw_textf(300, 22, 2, C_HINT, "%s", VITA_PORT_VERSION_STRING);
        draw_textf(760, 20, 2, C_HINT, "%d game(s)", game_count);

        /* left tab column */
        fill_rect(0, 52, TABBAR_W, FB_H - 52, C_PANEL);
        for (i = 0; i < TAB_N; i++) {
            int ty = 52 + 12 + i * TAB_H;
            const char *label =
                (i == 0) ? "已安装" :
                (i == 1) ? "未安装" :
                (i == 2) ? "文件" :
                (i == 3) ? "设置" : "退出";
            if (i == tab) {
                if (focus == 1) {
                    /* active focus: full highlight + accent bar */
                    fill_rect(0, ty, TABBAR_W, TAB_H - 8, C_SEL);
                    fill_rect(0, ty, 4, TAB_H - 8, C_TITLE);
                } else {
                    /* current tab, focus is in the content area */
                    fill_rect(0, ty, TABBAR_W, TAB_H - 8, C_PANEL2);
                    fill_rect(0, ty, 4, TAB_H - 8, C_HINT);
                }
            }
            draw_text(24, ty + 22, label, 3,
                      (i == tab) ? ((focus == 1) ? C_FG : C_HINT) : C_HINT);
        }
        draw_text(12, FB_H - 20,
                  (focus == 1) ? "dpad walk tabs, RIGHT back"
                               : "L/R or LEFT to tabs", 1, C_HINT);

        /* ---- content area per tab ---- */
        y = 64;
        if (tab == 0 && set_list_grid == TAB_ROWMODE) {
            /* list view (the classic v01.30 layout) */
            for (i = top; i < game_count && i < top + lines; i++) {
                if (i == sel) {
                    fill_rect(TABBAR_W + 8, y - 4, FB_W - TABBAR_W - 24,
                              row_h, (focus == 1) ? C_PANEL2 : C_SEL);
                }
                draw_icon_scaled(TABBAR_W + 20, y, &games[i], row_h - 8);
                draw_text(TABBAR_W + 52, y, games[i].name, 2,
                          games[i].cls[0] ? C_FG : C_WARN);
                /* v02.82: show the effective virtual screen instead of
                 * the bare L/P marker - the size is now per game, and a
                 * device profile changes the default. */
                {
                    int ew, eh;
                    effective_size(&games[i], &ew, &eh);
                    draw_textf(FB_W - 130, y, 2, C_HINT, "%dx%d", ew, eh);
                }
                y += row_h;
            }
            if (game_count == 0) {
                draw_text(TABBAR_W + 20, y + 16, "no games installed", 2, C_WARN);
                draw_text(TABBAR_W + 20, y + 52,
                          "copy .jar files with VitaShell to:", 2, C_WARN);
                draw_text(TABBAR_W + 20, y + 84, INBOX_DIR, 2, C_FG);
                draw_text(TABBAR_W + 20, y + 116,
                          "then open the 未安装 tab and press X", 2, C_WARN);
            }
        } else if (tab == 0) {
            /* grid view: 4 cols x 3 rows of icon+name cells */
            for (i = gtop * cols;
                 i < game_count && i < (gtop + grid_rows) * cols; i++) {
                int cx = (i - gtop * cols) % cols;
                int cy = (i - gtop * cols) / cols;
                int bx = TABBAR_W + 12 + cx * cell_w;
                int by = 64 + cy * cell_h;
                if (i == gsel) {
                    fill_rect(bx, by, cell_w - 8, cell_h - 10,
                              (focus == 1) ? C_PANEL2 : C_SEL);
                }
                draw_icon_scaled(bx + (cell_w - 8 - 84) / 2, by + 8,
                                 &games[i], 84);
                /* name: up to ~7 CJK glyphs, single line */
                draw_text(bx + 8, by + 104, games[i].name, 2,
                          games[i].cls[0] ? C_FG : C_WARN);
            }
            if (game_count == 0) {
                draw_text(TABBAR_W + 20, 84, "no games installed", 2, C_WARN);
                draw_text(TABBAR_W + 20, 120,
                          "install from the 未安装 or 文件 tab", 2, C_HINT);
            }
        } else if (tab == 1) {
            for (i = itop; i < inbox_count && i < itop + lines; i++) {
                if (i == isel) {
                    fill_rect(TABBAR_W + 8, y - 4, FB_W - TABBAR_W - 24,
                              row_h, (focus == 1) ? C_PANEL2 : C_SEL);
                }
                draw_text(TABBAR_W + 20, y, inbox_names[i], 2, C_FG);
                y += row_h;
            }
            if (inbox_count == 0) {
                draw_text(TABBAR_W + 20, y + 16,
                          "inbox empty - copy .jar files with VitaShell to:",
                          2, C_WARN);
                draw_text(TABBAR_W + 20, y + 52, INBOX_DIR, 2, C_FG);
            }
        } else if (tab == 2) {
            /* cwd line: show the Vita pseudo-root as a friendly name */
            draw_text(TABBAR_W + 12, 56,
                      (fe_cwd[0] == '/' && fe_cwd[1] == '\0')
                          ? "(Vita 根目录 / 分区列表)" : fe_cwd,
                      1, C_HINT);
            y = 80;
            {
                int root_view = (fe_cwd[0] == '/' && fe_cwd[1] == '\0');
                int shown = 0;
                for (i = ftop; i < fe_count && shown < lines; i++) {
                    if (i == fsel) {
                        fill_rect(TABBAR_W + 8, y - 4, FB_W - TABBAR_W - 24,
                                  row_h, (focus == 1) ? C_PANEL2 : C_SEL);
                    }
                    draw_text(TABBAR_W + 20, y, fes[i].name, 2,
                              fes[i].type == FE_DIR ? C_TITLE :
                              (fes[i].type == FE_JAR ? C_FG : C_HINT));
                    if (root_view && fes[i].type == FE_DIR) {
                        uint64_t mx = 0, fr = 0;
                        if (sceAppMgrGetDevInfo(fes[i].name, &mx, &fr) == 0 &&
                            mx > 0) {
                            draw_textf(FB_W - 250, y, 1, C_HINT,
                                       "%llu / %llu MB",
                                       (unsigned long long)(fr >> 20),
                                       (unsigned long long)(mx >> 20));
                        }
                    }
                    y += row_h;
                    shown++;
                }
            }
        } else if (tab == 3 && km_page) {
            const int kn = vita_keymap_count();
            y = 64;
            for (i = km_top; i < kn && i < km_top + KM_LINES; i++) {
                if (i == km_sel) {
                    fill_rect(TABBAR_W + 8, y - 4, FB_W - TABBAR_W - 24,
                              row_h, C_SEL);
                }
                draw_text(TABBAR_W + 20, y, vita_keymap_btn_name(i), 2, C_FG);
                draw_text(TABBAR_W + 260, y,
                          vita_keymap_key_name(vita_keymap_get(i)), 2,
                          C_TITLE);
                y += row_h;
            }
            if (km_top + KM_LINES > kn) {
                if (km_sel == kn) {
                    fill_rect(TABBAR_W + 8, y - 4, FB_W - TABBAR_W - 24,
                              row_h, C_SEL);
                }
                draw_text(TABBAR_W + 20, y, "恢复默认", 2, C_FG);
                y += row_h;
            }
            draw_text(TABBAR_W + 20, 64 + KM_LINES * row_h + 4,
                      "左右/X 切换功能机按键  O 保存并返回", 1, C_HINT);
        } else if (tab == 3) {
            uint64_t maxb = 0, freeb = 0;
            /* option rows: UP/DOWN move setsel, X cycles the value.
             * v03.79: SET_N is now larger than the visible area, so the page
             * scrolls by settop. It has to draw its own window (the list tabs
             * use `lines`, which is what the shared clamp knows about), so the
             * clamp below is told SET_LINES for this tab. */
            y = 64;
            for (i = settop;
                 i < SET_N && i < settop + SET_LINES; i++) {
                const char *val =
                      (i == SET_VIEW)    ? (set_list_grid ? "网格" : "列表")
                    : (i == SET_PROFILE) ? profile_choices[set_profile].label
                    : (i == SET_SCALE)   ? scale_name(set_scale)
                    : (i == SET_VKEY)    ? (set_vkey ? "开" : "关")
                    : (i == SET_VIBRATE) ? (set_vibrate ? "开" : "关")
                    : (i == SET_PERMS)   ? (set_perms ? "严格" : "宽松")
                    : (i == SET_KEYMAP)  ? "进入 >"
                    : (i == SET_3D)      ? mode3d_name(set_3d)
                    : (i == SET_DIAG)    ? (vita_diag_visible() ? "开" : "关")
                    : (i == SET_DIAGPOS) ? diagpos_name(vita_diag_get_pos())
                    : (i == SET_DIAGDETAIL) ? (vita_diag_get_detail() ? "开"
                                                                     : "关")
                    : (i == SET_HEAPMB)   ? (heapmb_label())
                                         : jit_name(set_jit);
                if (i == setsel) {
                    fill_rect(TABBAR_W + 8, y - 4, FB_W - TABBAR_W - 24,
                              row_h, (focus == 1) ? C_PANEL2 : C_SEL);
                }
                draw_text(TABBAR_W + 20, y,
                          (i == SET_VIEW)    ? "游戏视图" :
                          (i == SET_PROFILE) ? "默认机型" :
                          (i == SET_SCALE)   ? "默认缩放" :
                          (i == SET_VKEY)    ? "虚拟键盘" :
                          (i == SET_VIBRATE) ? "振动" :
                          (i == SET_PERMS)   ? "权限" :
                          (i == SET_KEYMAP)  ? "按键映射" :
                          (i == SET_3D)      ? "3D 模式" :
                          (i == SET_DIAG)    ? "帧率浮层" :
                          (i == SET_DIAGPOS) ? "浮层位置" :
                          (i == SET_DIAGDETAIL) ? "浮层详细" :
                                                  "Java 堆", 2,
                          C_FG);
                draw_text(TABBAR_W + 260, y, val, 2, C_TITLE);
                y += row_h;
            }
            draw_text(TABBAR_W + 20, y + 8, "X 或点击切换选项值", 1, C_HINT);
            y += 40;
            if (sceAppMgrGetDevInfo("ux0:", &maxb, &freeb) == 0) {
                draw_textf(TABBAR_W + 20, y, 2, C_FG,
                           "ux0: 剩余 %llu MB / 共 %llu MB",
                           (unsigned long long)(freeb >> 20),
                           (unsigned long long)(maxb >> 20));
            }
            y += 34;
            draw_textf(TABBAR_W + 20, y, 2, C_FG, "游戏: %d  收件箱: %d",
                       game_count, inbox_count);
            y += 34;
            draw_text(TABBAR_W + 20, y,
                      "JIT 首轮后崩溃未解(v01.45), 慎开", 1, C_HINT);
            y += 24;
            draw_text(TABBAR_W + 20, y,
                      "硬件3D需libshacccg, 且占用GXM(系统键盘不可用)", 1, C_HINT);
            y += 24;
            draw_text(TABBAR_W + 20, y,
                      "数据目录: ux0:/data/J2ME00001", 1, C_HINT);
        } else {
            /* tab 4 退出 content */
            draw_text(TABBAR_W + 20, 100, "按 X 或点击 退出 离开菜单", 2, C_FG);
            draw_text(TABBAR_W + 20, 140,
                      "(回退到 launch.cfg / 内置测试)", 2, C_HINT);
        }

        if (msg[0] != '\0') {
            unsigned long long now = sceKernelGetProcessTimeWide();
            if (msg_us != 0 && now - msg_us > 4000000ULL) {
                msg[0] = '\0'; /* auto-expire after 4 s */
            } else {
                draw_text(TABBAR_W + 20, FB_H - 40, msg, 2, C_FG);
            }
        }

        if (mode == 1 && game_count > 0) {
            GameEntry *g = &games[sel];
            char screen_label[48];
            char scale_label[32];
            char midlet_label[96];
            char profile_label_buf[64];
            const char *profile_label_str;
            int ew, eh;
            effective_size(g, &ew, &eh);
            snprintf(profile_label_buf, sizeof(profile_label_buf),
                     "profile: %s (%dx%d)", profile_label(g), ew, eh);
            profile_label_str = profile_label_buf;
            if (g->width > 0 && g->height > 0) {
                snprintf(screen_label, sizeof(screen_label), "screen: %dx%d",
                         g->width, g->height);
            } else {
                snprintf(screen_label, sizeof(screen_label),
                         "screen: auto (%dx%d)", ew, eh);
            }
            snprintf(scale_label, sizeof(scale_label), "scale: %s",
                     scale_name(g->scale_mode));
            if (midlet_count > 1) {
                snprintf(midlet_label, sizeof(midlet_label), "midlet: %d/%d",
                         (g->midlet_idx >= 1 && g->midlet_idx <= midlet_count)
                             ? g->midlet_idx : 1,
                         midlet_count);
            } else {
                snprintf(midlet_label, sizeof(midlet_label),
                         "midlet: %d in jar", midlet_count);
            }
            /* v04.34: 13 rows now. 28 px pitch starting at y=96 keeps the
             * whole dialog inside the 544 px panel: 96 + 70 + 13*28 = 530. */
            fill_rect(280, 96, 400, 70 + DLG_N * 28, C_PANEL);
            fill_rect(280, 96, 400, 3, C_TITLE);
            draw_icon_scaled(300, 106, g, 44);
            draw_text(354, 110, g->name, 2, C_TITLE);
            for (i = 0; i < DLG_N; i++) {
                int iy = 96 + 44 + i * 28;
                const char *label;
                char label_buf[96];

                label_buf[0] = '\0';
                if (i == DLG_LAUNCH) {
                    label = "launch game";
                } else if (i == DLG_ORIENT) {
                    label = g->landscape ? "orientation: landscape"
                                         : "orientation: portrait";
                } else if (i == DLG_SCREEN) {
                    label = screen_label;
                } else if (i == DLG_SCALE) {
                    label = scale_label;
                } else if (i == DLG_PROFILE) {
                    label = profile_label_str;
                } else if (i == DLG_MIDLET) {
                    label = midlet_label;
                } else if (i == DLG_LOCALE) {
                    snprintf(label_buf, sizeof(label_buf), "locale: %s",
                             locale_label(g));
                    label = label_buf;
                } else if (i == DLG_VKEY) {
                    snprintf(label_buf, sizeof(label_buf), "keypad: %s",
                             follow_onoff(g->vkey));
                    label = label_buf;
                } else if (i == DLG_VIBRATE) {
                    snprintf(label_buf, sizeof(label_buf), "rumble: %s",
                             follow_onoff(g->vibrate));
                    label = label_buf;
                } else if (i == DLG_PERMS) {
                    snprintf(label_buf, sizeof(label_buf), "permissions: %s",
                             follow_label(g->perms, "strict", "relaxed"));
                    label = label_buf;
                } else if (i == DLG_JIT) {
                    snprintf(label_buf, sizeof(label_buf), "jit: %s",
                             jit_label_pergame(g->jit));
                    label = label_buf;
                } else if (i == DLG_UNINSTALL) {
                    label = confirm_del ? "delete? press again" : "uninstall";
                } else {
                    label = "back";
                }
                if (i == dlg_sel) {
                    fill_rect(290, iy - 4, 380, 24, C_SEL);
                }
                draw_text(306, iy, label, 2, (i == dlg_sel) ? C_FG : C_HINT);
            }
        } else if (tab == 0) {
            draw_text(TABBAR_W + 12, FB_H - 36,
                      "UP/DOWN sel  X menu  SQUARE list/grid  SEL rescan",
                      1, C_HINT);
            draw_text(TABBAR_W + 12, FB_H - 18,
                      "TRIANGLE: launch.cfg / bundled tests",
                      1, C_HINT);
        } else if (tab == 1) {
            draw_text(TABBAR_W + 12, FB_H - 36,
                      "X install all  SEL refresh", 1, C_HINT);
        } else if (tab == 2) {
            draw_text(TABBAR_W + 12, FB_H - 36,
                      "X open/install  O parent  SEL refresh", 1, C_HINT);
        } else if (tab == 3) {
            draw_text(TABBAR_W + 12, FB_H - 36,
                      "X toggle game view", 1, C_HINT);
        }

        menu_flip();
        sceDisplayWaitVblankStart(); /* 60 Hz, no flicker */
    }

    /* v01.78g: deliberately NO icon-cache release here. Freeing the cached
     * textures on the way out of the menu walked the malloc arena with
     * state the VM had already churned and took the emulator down right at
     * the return (PC=0, LR pointing just after the old restore_kept() call)
     * - the same symptom the user hit on the quit tab. scan_games()
     * releases the blocks at a safe point instead, and the resident set is
     * only what the menu already held while it was up. */
    crumb_marker("menu exit");
    crumb_printf("menu exit: have_sel=%d games=%d sen0=%08x",
                 have_selection, game_count, sen[0]);
    crumb_flush();

    /* v01.78h: leave via longjmp, NOT via the epilogue. Forensics on the
     * v01.78g crash (nm + objdump on the unstripped binary): the crash PC
     * was the epilogue `ldmia.w sp!, {r4,r5,r7,r8,r9,sl,fp,pc}` of this
     * function; the crash SP (0x80000328) equals entrySP-32, i.e. the
     * saved-register block itself, and EVERY popped register read as 0
     * while the loop above had been healthy for 20000+ flips and the
     * breadcrumbs just above these lines wrote out fine. So the block was
     * zeroed sometime during the session and only gets read here. Jumping
     * out restores the registers from the .bss jmp_buf instead - main
     * arms it with setjmp(vita_menu_escape) before the call and never
     * returns itself (for(;;)), so no epilogue pop executes at all. */
    vita_menu_result = have_selection;
    longjmp(vita_menu_escape, 1);
    return have_selection; /* not reached */
}
