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
 *   ux0:/data/J2ME00001/inbox/*.jar              drop jars here, START
 *                                                installs them
 *
 * Keys: UP/DOWN select, CROSS open dialog (launch/orientation/uninstall/
 * back), CIRCLE close, START install inbox, SELECT rescan. Empty list +
 * CROSS quits the menu (falls back to launch.cfg / bundled Hello).
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <psp2/ctrl.h>
#include <psp2/display.h>
#include <psp2/io/dirent.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/threadmgr.h>

#include <zlib.h>

#include "vita_menu.h"
#include "vita_version.h"

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
#define C_FG    0xFFF0F0F0u
#define C_SEL   0xFF3050A0u
#define C_TITLE 0xFF40B0E0u
#define C_HINT  0xFF908878u
#define C_WARN  0xFF5050E0u

static uint32_t *menu_fb = NULL;

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

/* 5x7 glyph scaled 3x -> 15x21 px characters */
static void draw_text(int x, int y, const char *s, int scale, uint32_t color) {
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
    memset(&fb, 0, sizeof(fb));
    fb.size = sizeof(SceDisplayFrameBuf);
    fb.base = menu_fb;
    fb.pitch = FB_W;
    fb.pixelformat = SCE_DISPLAY_PIXELFORMAT_A8B8G8R8;
    fb.width = FB_W;
    fb.height = FB_H;
    sceDisplaySetFrameBuf(&fb, SCE_DISPLAY_SETBUF_IMMEDIATE);
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

/* ------------------------------------------------------------------ */
/* Game catalogue                                                      */
/* ------------------------------------------------------------------ */

typedef struct {
    char dir[160];
    char name[128];
    char jar[176];
    char cls[128];
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
                if (a >= 'a' && a <= 'z') {
                    a = (char)(a - 32);
                }
                if (a != want[k]) {
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

static void scan_games(void) {
    SceUID d;
    SceIoDirent ent;
    game_count = 0;
    memset(games, 0, sizeof(games));

    d = sceIoDopen(GAMES_DIR);
    if (d < 0) {
        return;
    }
    while (sceIoDread(d, &ent) > 0 && game_count < MAX_GAMES) {
        GameEntry *g;
        char cfgpath[256];
        SceUID fd;

        if (!SCE_S_ISDIR(ent.d_stat.st_mode)) {
            continue;
        }
        if (strcmp(ent.d_name, ".") == 0 || strcmp(ent.d_name, "..") == 0) {
            continue;
        }
        g = &games[game_count];
        snprintf(g->dir, sizeof(g->dir), GAMES_DIR "/%s", ent.d_name);
        strncpy(g->name, ent.d_name, sizeof(g->name) - 1);
        snprintf(g->jar, sizeof(g->jar), "%s/" JAR_NAME, g->dir);

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
        /* fix jars whose MANIFEST class does not exist (FileManagerMIDl
         * lesson): verify against the central directory and fall back
         * to the best MIDlet-ish class; persist the fix in game.cfg */
        validate_game_class(g, 1);
        game_count++;
    }
    sceIoDclose(d);
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
    while (sceIoDread(d, &ent) > 0) {
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
        base[blen - 4] = '\0';

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

int vita_menu_run(VitaGameSel *out) {
    int sel = 0;
    int top = 0;
    int mode = 0;      /* 0 = list, 1 = dialog */
    int dlg_sel = DLG_LAUNCH;
    int confirm_del = 0;
    int have_selection = 0;
    char msg[120] = "";

    menu_fb = (uint32_t *)memalign(0x100, FB_W * FB_H * 4);
    if (menu_fb == NULL) {
        return 0;
    }

    if (!dir_exists(GAMES_DIR)) {
        sceIoMkdir(GAMES_DIR, 0777);
    }
    scan_games();

    for (;;) {
        unsigned int btn = poll_buttons();
        int i;
        const int row_h = 30;
        const int lines = 12;
        int y;

        if (mode == 0) {
            if (btn & SCE_CTRL_UP) {
                if (sel > 0) sel--;
            }
            if (btn & SCE_CTRL_DOWN) {
                if (sel < game_count - 1) sel++;
            }
            if (sel < top) top = sel;
            if (sel >= top + lines) top = sel - lines + 1;

            if (btn & SCE_CTRL_START) {
                install_inbox(msg, sizeof(msg));
                scan_games();
                if (sel >= game_count) sel = game_count - 1;
                if (sel < 0) sel = 0;
            }
            if (btn & SCE_CTRL_SELECT) {
                scan_games();
                snprintf(msg, sizeof(msg), "rescanned: %d game(s)", game_count);
            }
            if ((btn & SCE_CTRL_CROSS) && game_count > 0) {
                mode = 1;
                dlg_sel = DLG_LAUNCH;
                confirm_del = 0;
            }
            if ((btn & SCE_CTRL_CROSS) && game_count == 0) {
                break; /* fall back to launch.cfg / Hello */
            }
            if (btn & SCE_CTRL_TRIANGLE) {
                /* Always allow falling back to launch.cfg / bundled
                 * tests even with games installed (needed to run
                 * ToneTest etc. for bring-up). */
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
                } else if (dlg_sel == DLG_UNINSTALL) {
                    if (!confirm_del) {
                        confirm_del = 1;
                    } else {
                        uninstall_game(g);
                        snprintf(msg, sizeof(msg), "deleted");
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

        y = 64;
        for (i = top; i < game_count && i < top + lines; i++) {
            if (i == sel) {
                fill_rect(16, y - 4, FB_W - 32, row_h, C_SEL);
            }
            draw_text(28, y, games[i].name, 2,
                      games[i].cls[0] ? C_FG : C_WARN);
            draw_text(880, y, games[i].landscape ? "L" : "P", 2, C_HINT);
            if (games[i].cls[0] == '\0') {
                draw_text(908, y, "?", 2, C_WARN);
            }
            y += row_h;
        }
        if (game_count == 0) {
            draw_text(28, y + 16, "no games installed", 2, C_WARN);
            draw_text(28, y + 52, "copy .jar files with VitaShell to:", 2, C_WARN);
            draw_text(28, y + 84, INBOX_DIR, 2, C_FG);
            draw_text(28, y + 116, "then press START to install", 2, C_WARN);
            draw_text(28, y + 148, "press X to run bundled Hello", 2, C_HINT);
        }

        if (msg[0] != '\0') {
            draw_text(20, 470, msg, 2, C_FG);
        }

        if (mode == 1 && game_count > 0) {
            GameEntry *g = &games[sel];
            fill_rect(280, 120, 400, 70 + DLG_N * 40, C_PANEL);
            fill_rect(280, 120, 400, 3, C_TITLE);
            draw_text(300, 134, g->name, 2, C_TITLE);
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
        } else {
            draw_text(20, 512,
                      "UP/DOWN select  X open  START install inbox  SEL rescan",
                      1, C_HINT);
            draw_text(20, 530,
                      "TRIANGLE: run launch.cfg / bundled tests",
                      1, C_HINT);
        }

        menu_flip();
        sceDisplayWaitVblankStart(); /* 60 Hz, no flicker */
    }

    /* Keep menu_fb allocated after the menu exits (no free): the display
     * still scans it out until the VM installs its own framebuffer, and
     * handing those pages back to malloc let the VM heap overwrite them -
     * garbage on screen during startup (v01.27 "no picture" symptom).
     * 2MB once per process is an acceptable cost of the round-loop menu. */
    return have_selection;
}
