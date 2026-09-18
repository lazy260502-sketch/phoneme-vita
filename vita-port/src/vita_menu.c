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
 *   ux0:/data/J2ME00001/games/<name>/game.cfg    line1=class ('-' = auto),
 *                                                line2=portrait|landscape
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
#include <psp2/rtc.h>
#include <psp2/touch.h>
#include <psp2/appmgr.h>

#include <zlib.h>

#include "vita_menu.h"
#include "vita_icon.h"
#include "vita_version.h"
#include "vita_fbmem.h"
#include "vita_crumb.h"

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
} GameEntry;

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
                char ch = (char)buf[q];
                if (ch >= 0x20) out[o++] = ch;
            }
            while (o > 0 && out[o - 1] == ' ') o--;
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
                    char ch = (char)buf[s++];
                    if (ch >= 0x20) out[len++] = ch;
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
                char ch = (char)buf[p++];
                if (ch >= 0x20) {
                    tmp[len++] = ch;
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
    SceUID fd;
    snprintf(path, sizeof(path), "%s/" CFG_NAME, g->dir);
    fd = sceIoOpen(path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
    if (fd >= 0) {
        char line[160];
        int n = snprintf(line, sizeof(line), "%s\n%s\n",
                         g->cls[0] ? g->cls : "-",
                         g->landscape ? "landscape" : "portrait");
        sceIoWrite(fd, line, n);
        sceIoClose(fd);
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
    SceUID d = sceIoDopen(path);
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
        hdr[0] == CACHE_MAGIC && hdr[1] == 1 &&
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
    hdr[1] = 1;
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
        char cfgpath[256];
        SceUID fd;
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
             * newer manual class override / orientation - refresh both. */
            g->landscape = 0;
            snprintf(cfgpath, sizeof(cfgpath), "%s/" CFG_NAME, g->dir);
            fd = sceIoOpen(cfgpath, SCE_O_RDONLY, 0);
            if (fd >= 0) {
                char buf[256];
                int n = sceIoRead(fd, buf, sizeof(buf) - 1);
                sceIoClose(fd);
                if (n > 0) {
                    char *nl, *p2;
                    buf[n] = '\0';
                    nl = strpbrk(buf, "\r\n");
                    if (nl != NULL) {
                        *nl = '\0';
                        p2 = nl + 1 + strspn(nl + 1, "\r\n");
                        if (strncmp(p2, "landscape", 9) == 0) {
                            g->landscape = 1;
                        }
                    }
                    if (buf[0] != '\0' && strcmp(buf, "-") != 0) {
                        strncpy(g->cls, buf, sizeof(g->cls) - 1);
                        g->cls[sizeof(g->cls) - 1] = '\0';
                    }
                }
            }
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

            /* game.cfg: line1 class ('-' = auto), line2 orientation */
            g->landscape = 0;
            snprintf(cfgpath, sizeof(cfgpath), "%s/" CFG_NAME, g->dir);
            fd = sceIoOpen(cfgpath, SCE_O_RDONLY, 0);
            if (fd >= 0) {
                char buf[256];
                int n = sceIoRead(fd, buf, sizeof(buf) - 1);
                sceIoClose(fd);
                if (n > 0) {
                    char *nl, *p2;
                    buf[n] = '\0';
                    nl = strpbrk(buf, "\r\n");
                    if (nl != NULL) {
                        *nl = '\0';
                        p2 = nl + 1 + strspn(nl + 1, "\r\n");
                        if (strncmp(p2, "landscape", 9) == 0) {
                            g->landscape = 1;
                        }
                    }
                    if (buf[0] != '\0' && strcmp(buf, "-") != 0) {
                        strncpy(g->cls, buf, sizeof(g->cls) - 1);
                    }
                }
            }
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

enum { DLG_LAUNCH = 0, DLG_ORIENT, DLG_UNINSTALL, DLG_BACK, DLG_N };

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
enum { SET_VIEW = 0, SET_JIT, SET_N };
static int set_list_grid = 0;      /* 0 = list, 1 = grid */
static int set_jit = 0;            /* 0/1/2, mirrors launch.cfg "jit=" */

static const char *jit_name(int j) {
    return (j == 0) ? "关闭" : (j == 1) ? "仅首轮" : "每轮";
}

static void settings_load(void) {
    SceUID fd = sceIoOpen(MENU_CFG, SCE_O_RDONLY, 0);
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

static void fe_scan(const char *path) {
    SceUID d;
    SceIoDirent ent;

    snprintf(fe_cwd, sizeof(fe_cwd), "%s", path);
    fe_count = 0;
    /* virtual ".." parent (except at the sandbox root) so up/down/tap
     * all treat going up like opening any other directory */
    if (strcmp(path, fe_root) != 0) {
        char parent[256];
        fe_parent(parent, sizeof(parent));
        fe_insert("..", parent, FE_DIR);
    }
    if (strcmp(path, "/") == 0) {
        /* Root: build the partition list by probing (see fe_devices),
         * then merge anything sceIoDopen("/") returns that we did not
         * already add. The old code early-returned when sceIoDopen
         * failed, which skipped the fallback and left the tab empty. */
        int i;
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
        return;
    }
    d = sceIoDopen(path);
    if (d < 0) {
        return;
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
            const char *sep = (path[strlen(path) - 1] == '/') ? "" : "/";
            snprintf(full, sizeof(full), "%s%s%s", path, sep, ent.d_name);
        }
        if (SCE_S_ISDIR(ent.d_stat.st_mode)) {
            type = FE_DIR;
        } else {
            type = (strstr(ent.d_name, ".jar") != NULL) ? FE_JAR : FE_FILE;
        }
        fe_insert(ent.d_name, full, type);
    }
    sceIoDclose(d);
}

static int fe_parent(char *out, size_t outsz) {
    char *slash;
    if (fe_cwd[0] == '\0' || strcmp(fe_cwd, "/") == 0) {
        return 0; /* Vita pseudo-root - no parent */
    }
    snprintf(out, outsz, "%s", fe_cwd);
    slash = strrchr(out, '/');
    if (slash == NULL) {
        snprintf(out, outsz, "/"); /* "ux0:" -> "/" */
        return 1;
    }
    if (slash == out) {
        slash[1] = '\0'; /* "/ux0:" -> "/" */
    } else {
        *slash = '\0';
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
                if (*psel < *ptop) *ptop = *psel;
                if (*psel >= *ptop + lines) *ptop = *psel - lines + 1;
            }

            if (btn & SCE_CTRL_SELECT) {
                if (tab == 0) {
                    scan_games();
                    inbox_scan();
                    snprintf(msg, sizeof(msg), "rescanned: %d game(s)",
                             game_count);
                    msg_us = sceKernelGetProcessTimeWide();
                } else if (tab == 2) {
                    fe_scan(fe_cwd);
                    snprintf(msg, sizeof(msg), "refreshed");
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
                        fe_scan(parent);
                        fsel = ftop = 0;
                    }
                }
                if (btn & SCE_CTRL_CROSS && fe_count > 0) {
                    FileEntry *e = &fes[fsel];
                    if (e->type == FE_DIR) {
                        fe_scan(e->full);
                        fsel = ftop = 0;
                    } else if (e->type == FE_JAR) {
                        if (install_jar(e->full, msg, sizeof(msg))) {
                            msg_us = sceKernelGetProcessTimeWide();
                            scan_games();
                            fe_scan(fe_cwd);
                            if (fsel >= fe_count) fsel = fe_count - 1;
                            if (fsel < 0) fsel = 0;
                        }
                    }
                }
                if (tapped && tap_x >= TABBAR_W && fe_count > 0) {
                    /* tab 2 content starts at y=80 (below the cwd line) */
                    int row = (tap_y - 80) / row_h;
                    int idx = ftop + row;
                    if (row >= 0 && idx >= 0 && idx < fe_count) {
                        FileEntry *e = &fes[idx];
                        if (e->type == FE_DIR) {
                            fe_scan(e->full);
                            fsel = ftop = 0;
                        } else if (e->type == FE_JAR) {
                            if (install_jar(e->full, msg, sizeof(msg))) {
                                msg_us = sceKernelGetProcessTimeWide();
                                scan_games();
                                fe_scan(fe_cwd);
                                if (fsel >= fe_count) fsel = fe_count - 1;
                                if (fsel < 0) fsel = 0;
                            }
                        }
                    }
                }
            } else if (tab == 3) { /* ---- 设置 ---- */
                if (btn & SCE_CTRL_CROSS) {
                    settings_toggle(setsel, msg, sizeof(msg));
                    msg_us = sceKernelGetProcessTimeWide();
                }
                if (tapped && tap_x >= TABBAR_W && tap_y >= 64) {
                    int row = (tap_y - 64) / row_h;
                    if (row >= 0 && row < SET_N) {
                        setsel = row;
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
                    save_cfg(g);
                    snprintf(msg, sizeof(msg), "%s: %s", g->name,
                             g->landscape ? "landscape" : "portrait");
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
                draw_text(FB_W - 60, y, games[i].landscape ? "L" : "P",
                          2, C_HINT);
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
        } else if (tab == 3) {
            uint64_t maxb = 0, freeb = 0;
            /* option rows: UP/DOWN move setsel, X cycles the value */
            y = 64;
            for (i = 0; i < SET_N; i++) {
                const char *val = (i == SET_VIEW)
                                      ? (set_list_grid ? "网格" : "列表")
                                      : jit_name(set_jit);
                if (i == setsel) {
                    fill_rect(TABBAR_W + 8, y - 4, FB_W - TABBAR_W - 24,
                              row_h, (focus == 1) ? C_PANEL2 : C_SEL);
                }
                draw_text(TABBAR_W + 20, y,
                          (i == SET_VIEW) ? "游戏视图" : "JIT 编译", 2,
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
            fill_rect(280, 120, 400, 70 + DLG_N * 40, C_PANEL);
            fill_rect(280, 120, 400, 3, C_TITLE);
            draw_icon_scaled(300, 130, g, 44);
            draw_text(354, 134, g->name, 2, C_TITLE);
            for (i = 0; i < DLG_N; i++) {
                int iy = 120 + 56 + i * 40;
                const char *label;
                if (i == DLG_LAUNCH) {
                    label = "launch game";
                } else if (i == DLG_ORIENT) {
                    label = g->landscape ? "orientation: landscape"
                                         : "orientation: portrait";
                } else if (i == DLG_UNINSTALL) {
                    label = confirm_del ? "delete? press again" : "uninstall";
                } else {
                    label = "back";
                }
                if (i == dlg_sel) {
                    fill_rect(290, iy - 6, 380, 34, C_SEL);
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
