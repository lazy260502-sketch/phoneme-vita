/*
 * vita_font.c - bitmap-bank CJK/ASCII text rendering for phoneME MIDP.
 *
 * Loads a pre-rendered 1bpp bitmap bank (tools/fontgen.c output).
 * No header parsing - dimensions are hardcoded to match fontgen.c.
 * Bank layout:
 *   bytes 0-39:  header (ignored by this code)
 *   bytes 40+:   sequential 1bpp glyph bitmaps (20x22, 3 bytes/row)
 *                in section order: ASCII, CJK, punct, fullwidth, gen
 */
#include <kni.h>
#include <gxj_putpixel.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Hardcoded dimensions - MUST match tools/fontgen.c */
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

#define GW 20
#define GH 22
#define STRIDE 3
#define GLYPH_BYTES (STRIDE * GH) /* 66 */
#define DATA_OFF 40

/* Section definitions - must match fontgen.c */
#define NSEC 5
static const unsigned int sec_first[NSEC] = { 0x20, 0x4E00, 0x3000, 0xFF01, 0x2010 };
static const unsigned int sec_cnt[NSEC]   = { 95, 20902, 64, 95, 24 };

static unsigned char *fb_data = NULL;
static unsigned int fb_size = 0;
static int fb_ready = 0;

/* Section base pointers into fb_data (computed after load) */
static const unsigned char *sec_base[NSEC];

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
    fb_ready = 1;
}

/* Lazy-load the bank on first use: ux0 override first, then VPK copy */
static void fb_ensure(void) {
    if (fb_ready) return;
    fb_load("ux0:/data/J2ME00001/fontbitmap.bin");
    if (!fb_ready) fb_load("app0:/data/J2ME00001/fontbitmap.bin");
    flog("fb_load done size=%u ready=%d\n", fb_size, fb_ready);
}

/* Returns pointer to the 1bpp glyph bitmap for codepoint cp,
 * or NULL if not found. Each glyph is GH rows of STRIDE bytes. */
static const unsigned char *fb_glyph(unsigned int cp) {
    static int init = 0;
    static const unsigned char *base[NSEC];
    if (!fb_ready) {
        return NULL;
    }
    if (!init) {
        const unsigned char *p = fb_data + DATA_OFF;
        int s;
        for (s = 0; s < NSEC; s++) {
            base[s] = p;
            p += (unsigned int)sec_cnt[s] * GLYPH_BYTES;
        }
        init = 1;
    }
    for (int s = 0; s < NSEC; s++) {
        if (cp >= sec_first[s] && cp < sec_first[s] + sec_cnt[s]) {
            return base[s] + (unsigned)(cp - sec_first[s]) * GLYPH_BYTES;
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
    if (ascent)  *ascent  = GH - 4;
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
    return n * GW;
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
            for (r = 0; r < GH; r++) {
                int py = y + r;
                const unsigned char *row = g + r * STRIDE;
                if (py < clipY1 || py >= clipY2) continue;
                for (c = 0; c < GW; c++) {
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
        pen_x += GW;
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
