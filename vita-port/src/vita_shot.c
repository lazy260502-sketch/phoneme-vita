/*
 * vita_shot.c - screenshot capture for the PS Vita J2ME port (v02.92)
 *
 * Design notes
 * ============
 * Trigger:
 *   - physical pad: SELECT + START. Detected in vita_input.c (fast path,
 *     so the two keys can be swallowed and never reach the MIDlet as
 *     '#' / '*') and, independently, by the poller thread below so a
 *     shot still works when the VM thread is frozen.
 *   - virtual key : a small camera button drawn over the frame. The
 *     poller thread does the hit test on the touch panel; vita_input.c
 *     only has to consume the pen event so the game sees nothing.
 *
 * Capture:
 *   The render thread is the only writer of the framebuffer, so the
 *   cleanest image is the back buffer just before sceDisplaySetFrameBuf
 *   (vita_display.c calls vita_shot_service() there). The poller thread
 *   covers the frozen-VM case: if nobody serviced the request within
 *   500 ms it grabs the front buffer (= the last submitted frame) - the
 *   image that is on the panel, which is exactly what a freeze report
 *   wants.
 *
 * Encoding:
 *   PNG, self-contained (zlib's compress2 + crc32, both already linked
 *   for the jar reader). Nothing else writes PNG on this target and a
 *   hand-rolled writer keeps the frame path independent of libpng's
 *   FILE* I/O (all port I/O goes through sceIo).
 *   Level 1 (Z_BEST_SPEED): a full 960x544 frame compresses in a few
 *   tens of milliseconds on the Vita CPU, i.e. one dropped frame - fine
 *   for a manual screenshot.
 *
 * Everything is written to ux0:/data/J2ME00001/shots/ with a wall-clock
 * name so a shot can be matched to a log line; shot.log records every
 * attempt (success, failure, why, size).
 */
#include <psp2/ctrl.h>
#include <psp2/touch.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/processmgr.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <zlib.h>

#include "vita_shot.h"

#define SHOT_DIR "ux0:/data/J2ME00001/shots"
#define SHOT_CFG "ux0:/data/J2ME00001/shot.cfg"
#define SHOT_LOG "ux0:/data/J2ME00001/shot.log"

#define PHYS_W 960
#define PHYS_H 544

/* On-screen button geometry, physical pixels. Sits in the left
 * letterbox bar just below the virtual left soft key: free in every
 * layout (bar mode: the digit panel starts at y~104; overlay/stretch
 * mode: it is the only thing in that corner). */
#define BTN_X 6
#define BTN_Y 56
#define BTN_W 56
#define BTN_H 36

/* How long the button stays highlighted after a successful save. */
#define FLASH_US 700000ULL
/* How long the result banner stays on screen. */
#define TOAST_US 3000000ULL
/* Wait this long after a save for the normal render path to paint the
 * banner before submitting a frame of our own. */
#define TOAST_GRACE_US 200000ULL
/* No flip for this long after the banner expired = the panel still shows
 * it and nothing else will repaint, so the poller wipes it. */
#define TOAST_CLEAR_IDLE_US 200000ULL
/* A request older than this means the render thread is not flipping. */
#define STALE_US 500000ULL
/* Two triggers within this window are the same shot (the input layer and
 * the poller thread both see the SELECT+START press). */
#define DEBOUNCE_US 300000ULL

/* ---- state ---- */
static int cfg_loaded = 0;
static int enabled = 1;
static int button_on = 1;
static int combo_on = 1;

static volatile int g_pending = 0;
static volatile int g_busy = 0;
static volatile int g_combo = 0;
static volatile int g_thread_started = 0;
/* 1 between lfjport_ui_init() and lfjport_ui_finalize(); the poller must
 * not touch the framebuffer outside that window. */
static volatile int g_display_up = 0;
static unsigned long long g_last_req_us = 0;
static unsigned long long g_saved_us = 0;
static unsigned int g_saved_count = 0;

/* v02.93: result banner. Written by whichever thread saved the shot,
 * read by the display layer when it paints. `toast_text`/`toast_err` are
 * published by the release-store of `toast_until_us`. */
static char toast_text[112];
static volatile unsigned long long toast_until_us = 0;
static volatile int toast_err = 0;
/* 0 = this save has not been shown yet; the poller sets it once a frame
 * carrying the banner exists (either the VM painted it or we did). */
static volatile int toast_flip_done = 1;
/* v03.01: 0 = the expired banner may still be on the panel and has to be
 * wiped by the poller; set once that happened. */
static volatile int toast_clear_done = 1;

/* Exported by vita_display.c: the last frame submitted to the screen. */
extern const unsigned int *vita_display_get_front(int *w, int *h);
/* Exported by vita_display.c: repaint the current front buffer with the
 * banner on top. Only used when the VM thread stopped flipping. */
extern void vita_display_toast_flip(void);
/* Exported by vita_display.c: 1 while the panel still shows the banner,
 * and the repaint that takes it off again (v03.01). */
extern int vita_display_toast_on_panel(void);
extern void vita_display_toast_clear(void);
/* Exported by vita_display.c: timestamp of the last submitted frame. */
extern unsigned long long vita_display_last_flip_us(void);
/* Exported by vita_font.c: UTF-8 text into a 32bpp framebuffer. */
extern int vita_menu_draw_utf8(uint32_t *fb, int w, int h, int x, int y,
                               const char *utf8, uint32_t color);
extern int vita_menu_font_gw(void);
extern int vita_menu_font_gh(void);

/* ------------------------------------------------------------------ */
/* Tiny log helper (sceIo, no FILE*, same style as the other diag logs) */
/* ------------------------------------------------------------------ */
static void shot_log(const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    int n;
    SceUID fd;

    va_start(ap, fmt);
    n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n <= 0) {
        return;
    }
    if (n > (int)sizeof(buf) - 1) {
        n = (int)sizeof(buf) - 1;
    }
    fd = sceIoOpen(SHOT_LOG, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0777);
    if (fd >= 0) {
        sceIoWrite(fd, buf, (SceSize)n);
        sceIoClose(fd);
    }
    /* Deliberately no stdio here: this runs on the poller thread too and
     * the port's rule is sceIo-only on background/diagnostic paths (see
     * vita_crumb.c). shot.log carries everything. */
}

/* ------------------------------------------------------------------ */
/* Config                                                             */
/* ------------------------------------------------------------------ */
static void shot_cfg_load(void)
{
    char buf[192];
    char *p, *nl;
    SceUID fd;
    int n;

    if (cfg_loaded) {
        return;
    }
    cfg_loaded = 1;

    fd = sceIoOpen(SHOT_CFG, SCE_O_RDONLY, 0);
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

    for (p = buf; p != NULL; p = (nl != NULL) ? nl + 1 : NULL) {
        nl = strpbrk(p, "\r\n");
        if (nl != NULL) {
            *nl = '\0';
        }
        if (strncmp(p, "on=", 3) == 0) {
            enabled = (p[3] == '1' || p[3] == 'y' || p[3] == 'Y');
        } else if (strncmp(p, "button=", 7) == 0) {
            button_on = (p[7] == '1' || p[7] == 'y' || p[7] == 'Y');
        } else if (strncmp(p, "combo=", 6) == 0) {
            combo_on = (p[6] == '1' || p[6] == 'y' || p[6] == 'Y');
        }
    }
}

int vita_shot_enabled(void)
{
    shot_cfg_load();
    return enabled;
}

int vita_shot_button_enabled(void)
{
    shot_cfg_load();
    return button_on;
}

int vita_shot_combo_enabled(void)
{
    shot_cfg_load();
    return combo_on;
}

int vita_shot_pending(void)
{
    return __atomic_load_n(&g_pending, __ATOMIC_SEQ_CST);
}

int vita_shot_combo_held(void)
{
    return __atomic_load_n(&g_combo, __ATOMIC_SEQ_CST);
}

void vita_shot_set_combo(int held)
{
    __atomic_store_n(&g_combo, held ? 1 : 0, __ATOMIC_SEQ_CST);
}

/* ------------------------------------------------------------------ */
/* PNG writer                                                         */
/* ------------------------------------------------------------------ */
static void be32(unsigned char *p, unsigned int v)
{
    p[0] = (unsigned char)(v >> 24);
    p[1] = (unsigned char)(v >> 16);
    p[2] = (unsigned char)(v >> 8);
    p[3] = (unsigned char)v;
}

/* Write one PNG chunk: length, type, data, CRC(type+data). */
static int png_chunk(SceUID fd, const char *type, const unsigned char *data,
                     unsigned int len)
{
    unsigned char head[8];
    unsigned char crcb[4];
    unsigned int crc;

    be32(head, len);
    memcpy(head + 4, type, 4);
    if (sceIoWrite(fd, head, sizeof(head)) != (SceSSize)sizeof(head)) {
        return -1;
    }
    if (len > 0 &&
        sceIoWrite(fd, data, (SceSize)len) != (SceSSize)len) {
        return -1;
    }
    crc = crc32(0, (const Bytef *)type, 4);
    if (len > 0) {
        crc = crc32(crc, (const Bytef *)data, len);
    }
    be32(crcb, crc);
    return (sceIoWrite(fd, crcb, sizeof(crcb)) == (SceSSize)sizeof(crcb))
               ? 0 : -1;
}

/* `fb` is A8B8G8R8 as stored by the display (0xAABBGGRR little endian,
 * i.e. bytes R,G,B,A in memory). The PNG is plain 8-bit RGB. */
static int shot_write_png(const char *path, const unsigned int *fb,
                          int w, int h)
{
    static const unsigned char sig[8] = {
        0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A
    };
    unsigned char ihdr[13];
    unsigned char *raw = NULL;
    unsigned char *comp = NULL;
    unsigned long raw_len;
    uLongf comp_len;
    SceUID fd;
    int x, y, rc = -1;

    raw_len = (unsigned long)h * (1UL + (unsigned long)w * 3UL);
    raw = (unsigned char *)malloc((size_t)raw_len);
    if (raw == NULL) {
        return -1;
    }
    for (y = 0; y < h; y++) {
        unsigned char *row = raw + (size_t)y * (1UL + (unsigned long)w * 3UL);
        const unsigned int *src = fb + (size_t)y * (size_t)w;
        row[0] = 0;                      /* filter type: none */
        for (x = 0; x < w; x++) {
            unsigned int p = src[x];
            row[1 + x * 3 + 0] = (unsigned char)(p & 0xFF);         /* R */
            row[1 + x * 3 + 1] = (unsigned char)((p >> 8) & 0xFF);  /* G */
            row[1 + x * 3 + 2] = (unsigned char)((p >> 16) & 0xFF); /* B */
        }
    }

    comp_len = compressBound(raw_len);
    comp = (unsigned char *)malloc((size_t)comp_len);
    if (comp == NULL) {
        free(raw);
        return -1;
    }
    if (compress2(comp, &comp_len, raw, raw_len, Z_BEST_SPEED) != Z_OK) {
        free(comp);
        free(raw);
        return -2;
    }
    free(raw);

    fd = sceIoOpen(path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
    if (fd < 0) {
        free(comp);
        return -3;
    }

    be32(ihdr, (unsigned int)w);
    be32(ihdr + 4, (unsigned int)h);
    ihdr[8] = 8;    /* bit depth      */
    ihdr[9] = 2;    /* colour type: RGB */
    ihdr[10] = 0;   /* compression    */
    ihdr[11] = 0;   /* filter         */
    ihdr[12] = 0;   /* interlace      */

    if (sceIoWrite(fd, sig, sizeof(sig)) == (SceSSize)sizeof(sig) &&
        png_chunk(fd, "IHDR", ihdr, sizeof(ihdr)) == 0 &&
        png_chunk(fd, "IDAT", comp, (unsigned int)comp_len) == 0 &&
        png_chunk(fd, "IEND", NULL, 0) == 0) {
        rc = 0;
    }
    sceIoClose(fd);
    free(comp);

    if (rc != 0) {
        /* do not leave a half-written file behind */
        sceIoRemove(path);
    }
    return rc;
}

/* ------------------------------------------------------------------ */
/* File naming                                                        */
/* ------------------------------------------------------------------ */
static int shot_path_exists(const char *path)
{
    SceIoStat st;
    return sceIoGetstat(path, &st) >= 0;
}

static void shot_make_path(char *out, size_t out_size)
{
    char base[192];
    time_t t = time(NULL);
    struct tm tmv;
    int i;

    memset(&tmv, 0, sizeof(tmv));
    if (t > 0 && localtime_r(&t, &tmv) != NULL &&
        tmv.tm_year + 1900 >= 2020) {
        snprintf(base, sizeof(base),
                 SHOT_DIR "/shot_%04d%02d%02d_%02d%02d%02d",
                 tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
                 tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
    } else {
        /* No usable RTC: monotonic sequence number, still sortable. */
        snprintf(base, sizeof(base), SHOT_DIR "/shot_seq_%06u",
                 g_saved_count);
    }
    snprintf(out, out_size, "%s.png", base);
    for (i = 2; i < 100; i++) {
        if (!shot_path_exists(out)) {
            break;
        }
        snprintf(out, out_size, "%s_%d.png", base, i);
    }
}

static int shot_save(const unsigned int *fb, int w, int h, const char *why)
{
    char path[288];
    const char *name;
    int rc;

    if (fb == NULL || w <= 0 || h <= 0) {
        return -1;
    }
    sceIoMkdir(SHOT_DIR, 0777);      /* EEXIST is fine */
    shot_make_path(path, sizeof(path));

    rc = shot_write_png(path, fb, w, h);
    name = strrchr(path, '/');
    name = (name != NULL) ? name + 1 : path;
    if (rc == 0) {
        __atomic_store_n(&g_saved_us, sceKernelGetProcessTimeWide(),
                         __ATOMIC_RELAXED);
        g_saved_count++;
        shot_log("[SHOT] ok %s %dx%d why=%s\n", path, w, h,
                 (why != NULL) ? why : "?");
        /* v02.93: tell the user. The display layer paints this on the
         * next frame (i.e. immediately, since we run inside the flip). */
        snprintf(toast_text, sizeof(toast_text),
                 "已保存截图  %.40s", name);
        toast_err = 0;
        toast_flip_done = 0;
        toast_clear_done = 0;    /* watch for its expiry */
        __atomic_store_n(&toast_until_us,
                         sceKernelGetProcessTimeWide() + TOAST_US,
                         __ATOMIC_RELEASE);
    } else {
        shot_log("[SHOT] FAIL rc=%d path=%s why=%s\n", rc, path,
                 (why != NULL) ? why : "?");
        snprintf(toast_text, sizeof(toast_text),
                 "截图失败  rc=%d", rc);
        toast_err = 1;
        toast_flip_done = 0;
        toast_clear_done = 0;
        __atomic_store_n(&toast_until_us,
                         sceKernelGetProcessTimeWide() + TOAST_US,
                         __ATOMIC_RELEASE);
    }
    return rc;
}

/* ------------------------------------------------------------------ */
/* Capture entry points                                               */
/* ------------------------------------------------------------------ */
void vita_shot_request(const char *why)
{
    unsigned long long now;

    shot_cfg_load();
    if (!enabled) {
        return;
    }
    now = sceKernelGetProcessTimeWide();
    if (g_last_req_us != 0 && now - g_last_req_us < DEBOUNCE_US) {
        return;
    }
    g_last_req_us = now;
    __atomic_store_n(&g_pending, 1, __ATOMIC_SEQ_CST);
    shot_log("[SHOT] request why=%s\n", (why != NULL) ? why : "?");
}

void vita_shot_service(const unsigned int *fb, int w, int h)
{
    if (!__atomic_load_n(&g_display_up, __ATOMIC_SEQ_CST)) {
        return;
    }
    if (!__atomic_load_n(&g_pending, __ATOMIC_SEQ_CST)) {
        return;
    }
    if (__atomic_exchange_n(&g_busy, 1, __ATOMIC_SEQ_CST)) {
        return;      /* the poller is writing; the next frame retries */
    }
    __atomic_store_n(&g_pending, 0, __ATOMIC_SEQ_CST);
    shot_save(fb, w, h, "frame");
    __atomic_store_n(&g_busy, 0, __ATOMIC_SEQ_CST);
}

int vita_shot_capture_stale(void)
{
    const unsigned int *fb;
    int w = 0, h = 0;

    if (__atomic_exchange_n(&g_busy, 1, __ATOMIC_SEQ_CST)) {
        return 0;
    }
    if (!__atomic_load_n(&g_pending, __ATOMIC_SEQ_CST)) {
        __atomic_store_n(&g_busy, 0, __ATOMIC_SEQ_CST);
        return 0;
    }
    __atomic_store_n(&g_pending, 0, __ATOMIC_SEQ_CST);

    if (!__atomic_load_n(&g_display_up, __ATOMIC_SEQ_CST)) {
        __atomic_store_n(&g_busy, 0, __ATOMIC_SEQ_CST);
        return 1;
    }
    fb = vita_display_get_front(&w, &h);
    if (fb != NULL) {
        shot_save(fb, w, h, "stale");
    } else {
        shot_log("[SHOT] drop: no framebuffer yet\n");
    }
    __atomic_store_n(&g_busy, 0, __ATOMIC_SEQ_CST);
    return 1;
}

void vita_shot_quiesce(void)
{
    __atomic_store_n(&g_display_up, 0, __ATOMIC_SEQ_CST);
    __atomic_store_n(&g_pending, 0, __ATOMIC_SEQ_CST);
    while (__atomic_load_n(&g_busy, __ATOMIC_SEQ_CST)) {
        sceKernelDelayThread(1000);
    }
}

/* ------------------------------------------------------------------ */
/* On-screen camera button                                            */
/* ------------------------------------------------------------------ */
static inline void s_px(unsigned int *fb, int w, int h, int x, int y,
                        unsigned int c)
{
    if (x >= 0 && x < w && y >= 0 && y < h) {
        fb[(size_t)y * (size_t)w + (size_t)x] = c;
    }
}

static void s_rect(unsigned int *fb, int w, int h, int x, int y, int rw,
                   int rh, unsigned int c)
{
    int r, k;
    for (r = 0; r < rh; r++) {
        for (k = 0; k < rw; k++) {
            s_px(fb, w, h, x + k, y + r, c);
        }
    }
}

static void s_circle(unsigned int *fb, int w, int h, int cx, int cy, int rad,
                     unsigned int c)
{
    int dy, dx;
    for (dy = -rad; dy <= rad; dy++) {
        for (dx = -rad; dx <= rad; dx++) {
            if (dx * dx + dy * dy <= rad * rad) {
                s_px(fb, w, h, cx + dx, cy + dy, c);
            }
        }
    }
}

void vita_shot_draw_button(unsigned int *fb, int w, int h)
{
    unsigned int face, edge, icon, dark;
    unsigned long long now;
    int flash;

    shot_cfg_load();
    if (!enabled || !button_on || fb == NULL ||
        !__atomic_load_n(&g_display_up, __ATOMIC_SEQ_CST)) {
        return;
    }
    now = sceKernelGetProcessTimeWide();
    flash = (g_saved_us != 0 && now - g_saved_us < FLASH_US);

    face = flash ? 0xFF30A0F0u : 0xFF282430u;   /* amber when just saved */
    edge = 0xFFB8B8C0u;
    icon = 0xFFFFFFFFu;
    dark = face;

    /* rounded-ish body: outer border then inset face */
    s_rect(fb, w, h, BTN_X, BTN_Y, BTN_W, BTN_H, edge);
    s_rect(fb, w, h, BTN_X + 2, BTN_Y + 2, BTN_W - 4, BTN_H - 4, dark);

    /* camera: top bump, body outline, lens */
    s_rect(fb, w, h, BTN_X + 22, BTN_Y + 9, 12, 5, icon);
    s_rect(fb, w, h, BTN_X + 14, BTN_Y + 13, 28, 16, icon);
    s_rect(fb, w, h, BTN_X + 16, BTN_Y + 15, 24, 12, dark);
    s_circle(fb, w, h, BTN_X + 28, BTN_Y + 21, 4, icon);
}

int vita_shot_button_hit(int px, int py)
{
    shot_cfg_load();
    if (!enabled || !button_on) {
        return 0;
    }
    return (px >= BTN_X && px < BTN_X + BTN_W &&
            py >= BTN_Y && py < BTN_Y + BTN_H) ? 1 : 0;
}

/* ------------------------------------------------------------------ */
/* v02.93: result banner                                              */
/* ------------------------------------------------------------------ */
int vita_shot_toast_active(void)
{
    unsigned long long until =
        __atomic_load_n(&toast_until_us, __ATOMIC_ACQUIRE);
    return (until != 0 && sceKernelGetProcessTimeWide() < until) ? 1 : 0;
}

/* Rough advance of a UTF-8 string for box sizing: CJK full width, ASCII
 * half. Only used to centre the banner, so an approximation is fine. */
static int utf8_width_est(const char *s, int gw)
{
    int pen = 0;
    if (gw <= 0) {
        return 0;
    }
    while (*s != '\0') {
        unsigned char c = (unsigned char)*s;
        if (c < 0x80) {
            pen += (gw + 1) / 2;
            s++;
        } else if ((c & 0xE0) == 0xC0) {
            pen += gw;
            s += 2;
        } else if ((c & 0xF0) == 0xE0) {
            pen += gw;
            s += 3;
        } else {
            pen += gw;
            s += 4;
        }
    }
    return pen;
}

/* Blend `c` into the existing pixel with weight num/256 (the display
 * ignores alpha, so "translucent" has to be mixed on the CPU). */
static void s_blend(unsigned int *fb, int w, int h, int x, int y,
                    unsigned int c, int num)
{
    unsigned int d;
    int r, g, b;
    if (x < 0 || x >= w || y < 0 || y >= h) {
        return;
    }
    d = fb[(size_t)y * (size_t)w + (size_t)x];
    r = (int)(d & 0xFF) * (256 - num) + (int)(c & 0xFF) * num;
    g = (int)((d >> 8) & 0xFF) * (256 - num) + (int)((c >> 8) & 0xFF) * num;
    b = (int)((d >> 16) & 0xFF) * (256 - num) + (int)((c >> 16) & 0xFF) * num;
    fb[(size_t)y * (size_t)w + (size_t)x] =
        0xFF000000u | (((unsigned int)(b >> 8) & 0xFF) << 16) |
        (((unsigned int)(g >> 8) & 0xFF) << 8) | ((unsigned int)(r >> 8) & 0xFF);
}

static void s_line(unsigned int *fb, int w, int h, int x0, int y0, int x1, int y1,
                   unsigned int c, int thick)
{
    int dx = x1 - x0, dy = y1 - y0;
    int steps = (dx < 0 ? -dx : dx);
    int sy = (dy < 0 ? -dy : dy);
    int i, t;

    if (sy > steps) {
        steps = sy;
    }
    if (steps == 0) {
        s_px(fb, w, h, x0, y0, c);
        return;
    }
    for (i = 0; i <= steps; i++) {
        int px = x0 + dx * i / steps;
        int py = y0 + dy * i / steps;
        for (t = 0; t < thick; t++) {
            s_px(fb, w, h, px + t, py, c);
            s_px(fb, w, h, px, py + t, c);
        }
    }
}

int vita_shot_draw_toast(unsigned int *fb, int w, int h)
{
    unsigned long long until;
    int gw, gh, text_w, box_w, box_h, box_x, box_y;
    int icon_x, icon_y, icon_s;
    int text_x, text_y;
    unsigned int edge, fill, text_col;
    char text[sizeof(toast_text)];

    if (fb == NULL) {
        return 0;
    }
    until = __atomic_load_n(&toast_until_us, __ATOMIC_ACQUIRE);
    if (until == 0 || sceKernelGetProcessTimeWide() >= until) {
        return 0;
    }
    /* Copy under the same acquire/release pair: a torn banner would be a
     * half-updated string, which is only cosmetic but avoidable. */
    memcpy(text, toast_text, sizeof(text));
    text[sizeof(text) - 1] = '\0';

    gw = vita_menu_font_gw();
    gh = vita_menu_font_gh();
    if (gh <= 0) {
        gh = 16;
    }
    text_w = utf8_width_est(text, gw);
    box_w = text_w + 64;                  /* icon + padding */
    if (box_w > w - 32) {
        box_w = w - 32;
    }
    if (box_w < 200) {
        box_w = 200;
    }
    box_h = gh + 18;
    if (box_h < 34) {
        box_h = 34;
    }
    box_x = (w - box_w) / 2;
    box_y = 8;
    if (box_y + box_h > h) {
        box_y = h - box_h - 2;
    }
    if (box_x < 2) {
        box_x = 2;
    }

    edge = __atomic_load_n(&toast_err, __ATOMIC_ACQUIRE) ? 0xFF5050E0u
                                                         : 0xFF60E050u;
    fill = 0xFF1A1A20u;
    text_col = 0xFFFFFFFFu;

    /* translucent black body + 2 px accent border */
    {
        int r, c;
        for (r = 0; r < box_h; r++) {
            for (c = 0; c < box_w; c++) {
                s_blend(fb, w, h, box_x + c, box_y + r, fill, 210);
            }
        }
    }
    {
        int c;
        for (c = 0; c < box_w; c++) {
            s_px(fb, w, h, box_x + c, box_y, edge);
            s_px(fb, w, h, box_x + c, box_y + 1, edge);
            s_px(fb, w, h, box_x + c, box_y + box_h - 1, edge);
            s_px(fb, w, h, box_x + c, box_y + box_h - 2, edge);
        }
        for (c = 0; c < box_h; c++) {
            s_px(fb, w, h, box_x, box_y + c, edge);
            s_px(fb, w, h, box_x + 1, box_y + c, edge);
            s_px(fb, w, h, box_x + box_w - 1, box_y + c, edge);
            s_px(fb, w, h, box_x + box_w - 2, box_y + c, edge);
        }
    }

    /* check mark / cross */
    icon_s = 16;
    icon_x = box_x + 16;
    icon_y = box_y + (box_h - icon_s) / 2;
    if (__atomic_load_n(&toast_err, __ATOMIC_ACQUIRE)) {
        s_line(fb, w, h, icon_x, icon_y, icon_x + icon_s, icon_y + icon_s,
               edge, 3);
        s_line(fb, w, h, icon_x + icon_s, icon_y, icon_x, icon_y + icon_s,
               edge, 3);
    } else {
        s_line(fb, w, h, icon_x, icon_y + icon_s * 5 / 8,
               icon_x + icon_s * 5 / 12, icon_y + icon_s, edge, 3);
        s_line(fb, w, h, icon_x + icon_s * 5 / 12, icon_y + icon_s,
               icon_x + icon_s, icon_y, edge, 3);
    }

    /* text (falls back to "no text" when fontbitmap.bin is missing: the
     * coloured banner + icon still tells the user something happened) */
    text_x = box_x + 16 + icon_s + 10;
    text_y = box_y + (box_h - gh) / 2;
    vita_menu_draw_utf8((uint32_t *)fb, w, h, text_x, text_y, text,
                        text_col);
    return 1;
}

/* ------------------------------------------------------------------ */
/* Poller thread: pad combo + touch button, and the frozen-VM fallback */
/* ------------------------------------------------------------------ */

/* v03.01: should the poller wipe the expired banner?
 * Yes only when ALL of these hold: the banner is no longer active, it has
 * not been wiped yet, the frame on the panel still carries it, and there
 * has been no flip for TOAST_CLEAR_IDLE_US (i.e. the MIDlet is not
 * repainting - otherwise its own next frame does the cleanup). */
static int shot_toast_needs_clear(unsigned long long now)
{
    if (vita_shot_toast_active() || toast_clear_done) {
        return 0;
    }
    if (!vita_display_toast_on_panel()) {
        return 0;
    }
    return (now - vita_display_last_flip_us() >= TOAST_CLEAR_IDLE_US);
}

static int shot_thread(SceSize args, void *argp)
{
    SceCtrlData pad;
    SceTouchData touch;
    SceTouchPanelInfo panel;
    int max_x = 1919, max_y = 1087;
    int prev_combo = 0;
    int prev_btn = 0;
    unsigned long long req_us = 0;

    (void)args;
    (void)argp;

    /* Touch may not be sampled yet (vita_input.c normally does this on its
     * first pump); the button has to work even if the VM never pumps. */
    {
        SceTouchSamplingState st = SCE_TOUCH_SAMPLING_STATE_STOP;
        if (sceTouchGetSamplingState(SCE_TOUCH_PORT_FRONT, &st) == 0 &&
            st != SCE_TOUCH_SAMPLING_STATE_START) {
            sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT,
                                     SCE_TOUCH_SAMPLING_STATE_START);
        }
    }
    memset(&panel, 0, sizeof(panel));
    if (sceTouchGetPanelInfo(SCE_TOUCH_PORT_FRONT, &panel) == 0 &&
        panel.maxAaX > 0 && panel.maxAaY > 0) {
        max_x = panel.maxAaX;
        max_y = panel.maxAaY;
    }
    shot_log("[SHOT] poller up: button=%d combo=%d touch_grid=%dx%d\n",
             button_on, combo_on, max_x + 1, max_y + 1);

    for (;;) {
        unsigned long long now = sceKernelGetProcessTimeWide();

        /* Physical SELECT+START. vita_input.c normally catches this first
         * (and swallows the keys); this copy exists for the frozen case. */
        if (combo_on && sceCtrlPeekBufferPositive(0, &pad, 1) >= 0) {
            int both =
                ((pad.buttons & (SCE_CTRL_SELECT | SCE_CTRL_START)) ==
                 (SCE_CTRL_SELECT | SCE_CTRL_START));
            if (both && !prev_combo) {
                vita_shot_request("keys");
            }
            prev_combo = both;
        }

        /* Virtual camera button. */
        if (button_on) {
            int down = 0;
            if (sceTouchPeek(SCE_TOUCH_PORT_FRONT, &touch, 1) > 0 &&
                touch.reportNum > 0) {
                int px = touch.report[0].x * PHYS_W / (max_x + 1);
                int py = touch.report[0].y * PHYS_H / (max_y + 1);
                down = vita_shot_button_hit(px, py);
            }
            if (down && !prev_btn) {
                vita_shot_request("touch");
            }
            prev_btn = down;
        }

        /* Frozen-VM fallback: nobody serviced the request in time. */
        if (vita_shot_pending()) {
            if (req_us == 0) {
                req_us = now;
            } else if (now - req_us >= STALE_US) {
                if (vita_shot_capture_stale()) {
                    req_us = 0;
                }
            }
        } else {
            req_us = 0;
        }

        /* v02.93: make sure the banner is actually visible. Normally the
         * save happens inside the render thread's flip, so a newer frame
         * exists right away and there is nothing to do. If the picture is
         * static (no repaints) or the VM is frozen, no frame will ever
         * carry it - submit one ourselves after a short grace period. */
        if (!toast_flip_done && vita_shot_toast_active()) {
            unsigned long long saved = __atomic_load_n(&g_saved_us,
                                                       __ATOMIC_RELAXED);
            if (vita_display_last_flip_us() >= saved) {
                toast_flip_done = 1;       /* the VM painted it */
            } else if (saved != 0 && now - saved >= TOAST_GRACE_US) {
                vita_display_toast_flip();
                toast_flip_done = 1;
                toast_clear_done = 0;      /* it is on the panel now */
            }
        } else if (!vita_shot_toast_active()) {
            toast_flip_done = 1;
        }

        /* v03.01: the banner expired - take it off the panel ourselves if
         * no frame followed. Reported symptom: the green box stayed on
         * screen. Any new frame would have repainted over it (the display
         * layer tracks that in g_toast_on_panel), so an idle screen means
         * the last submitted frame still shows it.
         * The 200 ms idle gate is what keeps a live MIDlet in charge: as
         * long as it keeps flipping, its own next frame does the cleanup. */
        if (shot_toast_needs_clear(now)) {
            vita_display_toast_clear();
            toast_clear_done = 1;
        }

        sceKernelDelayThread(50 * 1000);   /* 20 Hz is plenty */
    }
    return 0;
}

static void shot_start_thread(void)
{
    SceUID t;

    shot_cfg_load();
    if (g_thread_started || !enabled) {
        return;
    }
    g_thread_started = 1;

    t = sceKernelCreateThread("j2me_shot", shot_thread, 0x10000100, 0x4000,
                              0, SCE_KERNEL_THREAD_CPU_AFFINITY_MASK_DEFAULT,
                              NULL);
    if (t < 0) {
        shot_log("[SHOT] thread create FAILED rc=0x%08x\n", t);
        g_thread_started = 0;
        return;
    }
    if (sceKernelStartThread(t, 0, NULL) < 0) {
        shot_log("[SHOT] thread start FAILED\n");
        g_thread_started = 0;
    }
}

void vita_shot_init(void)
{
    shot_cfg_load();
    sceIoMkdir(SHOT_DIR, 0777);
    __atomic_store_n(&g_display_up, 1, __ATOMIC_SEQ_CST);
    if (!enabled) {
        shot_log("[SHOT] disabled by shot.cfg\n");
        return;
    }
    shot_start_thread();
}

void vita_shot_set_enabled(int on)
{
    shot_cfg_load();
    enabled = on ? 1 : 0;
    if (enabled) {
        shot_start_thread();
    }
}
