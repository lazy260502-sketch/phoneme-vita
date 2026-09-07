/*
 * vita_font.c - bitmap-bank CJK/ASCII text rendering for phoneME MIDP.
 *
 * Loads a pre-rendered 1bpp bitmap bank (tools/fontgen.c output) from
 * VPK or ux0 override, and draws glyphs by table lookup + blit.
 * The bank header is self-describing (magic "J2FB", version 1):
 * section table is parsed at load time, so fontgen can add coverage
 * without touching this file. Glyph bitmaps are 1bpp, MSB-first.
 */
#include <kni.h>
#include <gxj_putpixel.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Legacy fallback dims if a headerless (very old) bank is loaded */
#define GW 20
#define GH 22
#define STRIDE 3

#include <stdarg.h>
static SceUID flog_fd = -2;
static void flog(const char *fmt, ...) {
    char buf[256];
    va_list ap;
    int n;
    if (flog_fd == -2) {
        flog_fd = sceIoOpen("ux0:/data/J2ME00001/font_debug.log",
                            SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
    }
    if (flog_fd < 0) return;
    va_start(ap, fmt);
    n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n > 0) sceIoWrite(flog_fd, buf, n);
}

static unsigned char *fb_data = NULL;
static unsigned int fb_size = 0;
static int fb_ready = 0;

/* Parsed from the bank header */
static int fb_gw = GW, fb_gh = GH, fb_stride = STRIDE;
static unsigned int fb_data_off = 0;
static int fb_nsec = 0;
static unsigned int fb_sec_first[16], fb_sec_cnt[16];
static const unsigned char *fb_sec_base[16];

/* Tiny LE readers */
static unsigned int rd32(const unsigned char *p) {
    return p[0] | (p[1] << 8) | (p[2] << 16) | ((unsigned int)p[3] << 24);
}

static void fb_load(const char *path) {
    SceUID fd = sceIoOpen(path, SCE_O_RDONLY, 0);
    if (fd < 0) {
        return;
    }
    fb_size = (unsigned int)sceIoLseek(fd, 0, SCE_SEEK_END);
    sceIoLseek(fd, 0, SCE_SEEK_SET);
    fb_data = (unsigned char *)malloc(fb_size);
    if (fb_data == NULL) {
        sceIoClose(fd);
        return;
    }
    if (sceIoRead(fd, fb_data, fb_size) != (int)fb_size) {
        free(fb_data);
        fb_data = NULL;
        sceIoClose(fd);
        return;
    }
    sceIoClose(fd);

    /* Parse the self-describing header (J2FB v1) */
    if (fb_size < 28 || memcmp(fb_data, "J2FB", 4) != 0 || fb_data[4] != 1) {
        flog("fb_load %s: bad header\n", path);
        free(fb_data);
        fb_data = NULL;
        return;
    }
    fb_nsec = (int)rd32(fb_data + 8);
    fb_gw = (int)rd32(fb_data + 12);
    fb_gh = (int)rd32(fb_data + 16);
    fb_stride = (int)rd32(fb_data + 20);
    fb_data_off = rd32(fb_data + 24);
    if (fb_nsec <= 0 || fb_nsec > 16 ||
        fb_gw <= 0 || fb_gw > 64 || fb_gh <= 0 || fb_gh > 64 ||
        fb_stride != (fb_gw + 7) / 8) {
        flog("fb_load %s: bad dims nsec=%d %dx%d/%d\n",
             path, fb_nsec, fb_gw, fb_gh, fb_stride);
        free(fb_data);
        fb_data = NULL;
        return;
    }
    for (int s = 0; s < fb_nsec; s++) {
        fb_sec_first[s] = rd32(fb_data + 28 + 8 * s);
        fb_sec_cnt[s] = rd32(fb_data + 28 + 8 * s + 4);
    }
    {
        unsigned long total = 0;
        for (int s = 0; s < fb_nsec; s++) total += fb_sec_cnt[s];
        if (fb_data_off + total * (unsigned long)fb_stride * fb_gh > fb_size) {
            flog("fb_load %s: truncated\n", path);
            free(fb_data);
            fb_data = NULL;
            return;
        }
    }
    fb_ready = 1;
}

/* Lazy-load the bank on first use: ux0 override first, then VPK copy */
static void fb_ensure(void) {
    if (fb_ready) return;
    fb_load("ux0:/data/J2ME00001/fontbitmap.bin");
    if (!fb_ready) fb_load("app0:/data/J2ME00001/fontbitmap.bin");
    flog("fb_load done size=%u ready=%d nsec=%d %dx%d/%d\n",
         fb_size, fb_ready, fb_nsec, fb_gw, fb_gh, fb_stride);
}

/* Returns pointer to the 1bpp glyph bitmap for codepoint cp,
 * or NULL if not found. Each glyph is GH rows of STRIDE bytes. */
static const unsigned char *fb_glyph(unsigned int cp) {
    if (!fb_ready) {
        return NULL;
    }
    for (int s = 0; s < fb_nsec; s++) {
        if (cp >= fb_sec_first[s] && cp < fb_sec_first[s] + fb_sec_cnt[s]) {
            unsigned long idx = 0;
            for (int t = 0; t < s; t++) idx += fb_sec_cnt[t];
            idx += cp - fb_sec_first[s];
            return fb_data + fb_data_off
                 + idx * (unsigned long)fb_stride * fb_gh;
        }
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* gxjport_text.h implementation                                       */

int gxjport_get_font_info(int face, int style, int size,
                          int *ascent, int *descent, int *leading) {
    (void)face; (void)style; (void)size;
    fb_ensure();
    if (!fb_ready) {
        return KNI_FALSE;
    }
    if (ascent)  *ascent  = fb_gh - 4;
    if (descent) *descent = 4;
    if (leading) *leading = 0;
    return KNI_TRUE;
}

int gxjport_get_chars_width(int face, int style, int size,
                            const jchar *charArray, int n) {
    (void)face; (void)style; (void)size; (void)charArray;
    fb_ensure();
    if (!fb_ready) {
        return -1;
    }
    return n * fb_gw;
}

int gxjport_draw_chars(int pixel, const jshort *clip, void *dst, int dotted,
                       int face, int style, int size,
                       int x, int y, int anchor,
                       const jchar *chararray, int n) {
    gxj_screen_buffer *dest = (gxj_screen_buffer *)dst;
    int i, pen_x;
    int clipX1, clipY1, clipX2, clipY2;
    gxj_pixel_type color = (gxj_pixel_type)pixel;

    (void)dotted; (void)face; (void)style; (void)size; (void)anchor;

    if (dest == NULL || dest->pixelData == NULL) {
        return KNI_FALSE;
    }

    fb_ensure();

    clipX1 = clip[0]; clipY1 = clip[1];
    clipX2 = clip[2]; clipY2 = clip[3];

    pen_x = x;
    {
        static int clip_logged = 0;
        if (!clip_logged) {
            clip_logged = 1;
            flog("first draw clip=[%d,%d,%d,%d] dest=%dx%d fb_ready=%d\n",
                 clip[0], clip[1], clip[2], clip[3],
                 dest->width, dest->height, fb_ready);
        }
    }

    /* SCREEN DUMP: save dest buffer on FIRST draw_chars call */
    {
        static int dumped = 0;
        if (!dumped && dest->pixelData != NULL) {
            dumped = 1;
            SceUID dfd = sceIoOpen("ux0:/data/J2ME00001/screen_dump.bin",
                                   SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
            if (dfd >= 0) {
                sceIoWrite(dfd, dest->pixelData,
                           dest->width * dest->height * 2);
                sceIoClose(dfd);
            }
            flog("screen dumped %dx%d\n", dest->width, dest->height);
        }
    }

    int pixels_drawn = 0;
    for (i = 0; i < n; i++) {
        unsigned int cp = (unsigned)chararray[i];
        const unsigned char *g = fb_glyph(cp);

        if (g != NULL) {
            int r, c;
            for (r = 0; r < fb_gh; r++) {
                int py = y + r;
                const unsigned char *row = g + r * fb_stride;
                if (py < clipY1 || py >= clipY2) continue;
                for (c = 0; c < fb_gw; c++) {
                    int pxx = pen_x + c;
                    if (pxx < clipX1 || pxx >= clipX2) continue;
                    if (row[c >> 3] & (0x80 >> (c & 7))) {
                        dest->pixelData[py * dest->width + pxx] = color;
                        pixels_drawn++;
                    }
                }
            }
        } else if (cp >= 0x2E80) {
            /* tofu box for missing CJK glyphs */
            int rr, cc;
            for (rr = 0; rr < 18; rr++) {
                int py = y + rr;
                if (py < clipY1 || py >= clipY2) continue;
                for (cc = 0; cc < 18; cc++) {
                    int px = pen_x + cc;
                    if (px < clipX1 || px >= clipX2) continue;
                    if (rr == 0 || rr == 17 || cc == 0 || cc == 17) {
                        dest->pixelData[py * dest->width + px] = color;
                        pixels_drawn++;
                    }
                }
            }
        }
        pen_x += fb_gw;
    }

    {
        static int calls = 0;
        calls++;
        if (calls <= 10) {
            flog("draw n=%d cp=0x%04x x=%d y=%d pixels_drawn=%d\n",
                 n, (unsigned)chararray[0], x, y, pixels_drawn);
        }
    }

    return KNI_TRUE;
}

/* ------------------------------------------------------------------ */
/* Native-menu rendering (UTF-8 -> codepoints -> 1bpp glyph blit).     */
/* Used by vita_menu.c so game names from MANIFEST.MF (often GBK/UTF-8 */
/* Chinese) draw with the same bank the Java layer uses.               */
/* ------------------------------------------------------------------ */

/* Decode one UTF-8 sequence; returns bytes consumed, *cp = codepoint.
 * GBK bytes (lead 0x81-0xFE) are NOT valid UTF-8 - callers that hit
 * them get *cp = cp with the high bytes preserved via gb fallback. */
static int utf8_decode(const unsigned char *s, int avail, unsigned int *cp) {
    unsigned int c = s[0];
    if (c < 0x80) {
        *cp = c;
        return 1;
    }
    if ((c & 0xE0) == 0xC0 && avail >= 2 && (s[1] & 0xC0) == 0x80) {
        *cp = ((c & 0x1F) << 6) | (s[1] & 0x3F);
        return 2;
    }
    if ((c & 0xF0) == 0xE0 && avail >= 3 && (s[1] & 0xC0) == 0x80 &&
        (s[2] & 0xC0) == 0x80) {
        *cp = ((c & 0x0F) << 12) | ((s[1] & 0x3F) << 6) | (s[2] & 0x3F);
        return 3;
    }
    if ((c & 0xF8) == 0xF0 && avail >= 4 && (s[1] & 0xC0) == 0x80 &&
        (s[2] & 0xC0) == 0x80 && (s[3] & 0xC0) == 0x80) {
        *cp = ((c & 0x07) << 18) | ((s[1] & 0x3F) << 12) |
              ((s[2] & 0x3F) << 6) | (s[3] & 0x3F);
        return 4;
    }
    *cp = c; /* invalid byte: pass through (draws tofu/ascii) */
    return 1;
}

int vita_menu_font_gw(void) {
    fb_ensure();
    return fb_ready ? fb_gw : 0;
}

int vita_menu_font_gh(void) {
    fb_ensure();
    return fb_ready ? fb_gh : 0;
}

/* Draw a UTF-8 string into a 32bpp framebuffer (0xAABBGGRR), clipped to
 * [0,w)x[0,h). Returns the pen x after the last glyph. Missing glyphs
 * draw a tofu box; ASCII without the bank falls back to caller. */
int vita_menu_draw_utf8(uint32_t *fb, int w, int h,
                        int x, int y, const char *utf8, uint32_t color) {
    const unsigned char *s = (const unsigned char *)utf8;
    int avail = (int)strlen(utf8);
    int pen_x = x;

    fb_ensure();
    if (fb == NULL || !fb_ready) {
        return pen_x;
    }

    while (avail > 0) {
        unsigned int cp;
        const unsigned char *g;
        int used = utf8_decode(s, avail, &cp);
        int r, c;

        g = fb_glyph(cp);
        if (g != NULL) {
            for (r = 0; r < fb_gh; r++) {
                int py = y + r;
                const unsigned char *row = g + r * fb_stride;
                if (py < 0 || py >= h) continue;
                for (c = 0; c < fb_gw; c++) {
                    int px = pen_x + c;
                    if (px < 0 || px >= w) continue;
                    if (row[c >> 3] & (0x80 >> (c & 7))) {
                        fb[py * w + px] = color;
                    }
                }
            }
        } else if (cp >= 0x2E80) {
            /* tofu box for missing CJK glyphs */
            for (r = 0; r < fb_gh; r++) {
                int py = y + r;
                if (py < 0 || py >= h) continue;
                for (c = 0; c < fb_gw; c++) {
                    int px = pen_x + c;
                    if (px < 0 || px >= w) continue;
                    if (r == 0 || r == fb_gh - 1 || c == 0 ||
                        c == fb_gw - 1) {
                        fb[py * w + px] = color;
                    }
                }
            }
        }
        pen_x += fb_gw;
        s += used;
        avail -= used;
    }
    return pen_x;
}
