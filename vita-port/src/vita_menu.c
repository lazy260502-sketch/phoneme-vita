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

#include <pcsl_string.h>
#include <midpJar.h>
#include <midpMalloc.h>

#include "vita_menu.h"

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

/* pcsl_string.data is a jchar array: expand each byte into one jchar
 * (wrapping a byte pointer directly makes readers stride 2 bytes and
 * garble the path). */
static void make_pcsl_string(const char *utf8, pcsl_string *ps) {
    size_t n = strlen(utf8);
    jchar *w = (jchar *)malloc(n * sizeof(jchar));
    size_t i;
    for (i = 0; i < n; i++) {
        w[i] = (jchar)(unsigned char)utf8[i];
    }
    ps->data = w;
    ps->length = (jsize)n;
    ps->flags = 0;
}

static void free_pcsl_string(pcsl_string *ps) {
    free(ps->data);
    ps->data = NULL;
    ps->length = 0;
}

static char g_manifest_entry[160];

static jboolean manifest_filter(const pcsl_string *name) {
    char tmp[160];
    int i, n = name->length;
    if (n < 20 || n >= (int)sizeof(tmp)) {
        return 0;
    }
    for (i = 0; i < n; i++) {
        char ch = (char)(name->data[i] & 0xFF);
        tmp[i] = (ch >= 'a' && ch <= 'z') ? (char)(ch - 32) : ch;
    }
    tmp[n] = '\0';
    if (strncmp(tmp, "META-INF/", 9) != 0) {
        return 0;
    }
    if (strcmp(tmp + n - 11, "MANIFEST.MF") != 0) {
        return 0;
    }
    memcpy(g_manifest_entry, tmp, n);
    g_manifest_entry[n] = '\0';
    return 1;
}

static jboolean manifest_action(const pcsl_string *name) {
    (void)name;
    return 0;
}

/* The class is the text after the LAST comma on the "MIDlet-1:" line;
 * non-printable characters are dropped (a stray CR makes Class.forName
 * fail with an invisible suffix). */
static void parse_manifest_class(const char *jarpath, char *out, size_t outsz) {
    pcsl_string jar_ps, entry_ps;
    void *handle;
    int jar_err;
    unsigned char *buf = NULL;
    long n = 0, i;
    const char *entry = "META-INF/MANIFEST.MF";
    out[0] = '\0';

    make_pcsl_string(jarpath, &jar_ps);
    handle = midpOpenJar(&jar_err, &jar_ps);
    free_pcsl_string(&jar_ps);
    if (handle == NULL) {
        return;
    }

    make_pcsl_string(entry, &entry_ps);
    n = midpGetJarEntry(handle, &entry_ps, &buf);
    free_pcsl_string(&entry_ps);

    if (n <= 0 || buf == NULL) {
        g_manifest_entry[0] = '\0';
        midpIterateJarEntries(handle, manifest_filter, manifest_action);
        if (g_manifest_entry[0] != '\0') {
            make_pcsl_string(g_manifest_entry, &entry_ps);
            n = midpGetJarEntry(handle, &entry_ps, &buf);
            free_pcsl_string(&entry_ps);
        }
    }
    midpCloseJar(handle);
    if (n <= 0 || buf == NULL) {
        if (buf != NULL) {
            midpFree(buf);
        }
        return;
    }

    for (i = 0; i + 9 <= n; i++) {
        if (buf[i] == 'M' && buf[i + 1] == 'I' && buf[i + 2] == 'D' &&
            buf[i + 3] == 'l' && buf[i + 4] == 'e' && buf[i + 5] == 't' &&
            buf[i + 6] == '-' && buf[i + 7] == '1' && buf[i + 8] == ':') {
            long p = i + 9;
            long lineEnd = p;
            long lastComma = -1;
            int len = 0;
            char tmp[128];
            while (lineEnd < n && buf[lineEnd] != '\n' && buf[lineEnd] != '\r') {
                lineEnd++;
            }
            for (p = i + 9; p < lineEnd; p++) {
                if (buf[p] == ',') {
                    lastComma = p;
                }
            }
            if (lastComma < 0) {
                break;
            }
            p = lastComma + 1;
            while (p < lineEnd && buf[p] == ' ') p++;
            while (p < lineEnd && len < (int)sizeof(tmp) - 1) {
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
    midpFree(buf);
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
        }

        menu_flip();
        sceDisplayWaitVblankStart(); /* 60 Hz, no flicker */
    }

    free(menu_fb);
    menu_fb = NULL;
    return have_selection;
}
