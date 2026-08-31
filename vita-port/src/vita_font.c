/*
 * vita_font.c - bitmap-bank CJK/ASCII text rendering for phoneME MIDP.
 *
 * Loads a pre-rendered 1bpp bitmap bank (tools/fontgen.c output) from
 * VPK or ux0 override, and draws glyphs by table lookup + blit.
 * Covers: ASCII 0x20-0x7E, CJK 0x4E00-0x9FA5,
 *         CJK punct 0x3000-0x303F, fullwidth 0xFF01-0xFF5E,
 *         general punct 0x2010-0x2027.
 */
#include <kni.h>
#include <gxj_putpixel.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GW 20
#define GH 22
#define STRIDE ((GW + 7) / 8)
#define GLYPH_BYTES (STRIDE * GH)

static unsigned char *fb_data = NULL;
static unsigned int fb_size = 0;
static int fb_ready = 0;
static int fb_gw = GW, fb_gh = GH, fb_stride = STRIDE;
static int fb_ascent = 18;

static void fb_load(const char *path) {
    SceUID fd = sceIoOpen(path, SCE_O_RDONLY, 0);
    SceIoStat st;
    if (fd < 0) {
        return;
    }
    if (sceIoGetstatByFd(fd, &st) < 0 || st.st_size < 40) {
        sceIoClose(fd);
        return;
    }
    fb_size = (unsigned int)st.st_size;
    fb_data = (unsigned char *)malloc(fb_size);
    if (fb_data == NULL) {
        sceIoClose(fd);
        return;
    }
    sceIoLseek(fd, 0, SCE_SEEK_SET);
    if (sceIoRead(fd, fb_data, fb_size) != (int)fb_size) {
        free(fb_data);
        fb_data = NULL;
        sceIoClose(fd);
        return;
    }
    sceIoClose(fd);
    if (memcmp(fb_data, "FBMP", 4) != 0) {
        free(fb_data);
        fb_data = NULL;
        return;
    }
    fb_ready = 1;
    fprintf(stderr, "[font] bitmap bank loaded (%u bytes)\n", fb_size);
    fflush(stderr);
}

/* Section descriptors in the bank.
 * Each section: first_codepoint, count, then bitmaps sequentially.
 * Must match tools/fontgen.c section generation order. */
#define NSEC 5
static const unsigned int sec_first[NSEC] = {
    0x20,   /* ASCII */
    0x4E00, /* CJK Unified Ideographs */
    0x3000, /* CJK Symbols and Punctuation */
    0xFF01, /* Fullwidth Forms */
    0x2010, /* General Punctuation dash/quotes */
};
static const unsigned int sec_count[NSEC] = {
    95,   /* 0x20-0x7E */
    20902,/* 0x4E00-0x9FA5 */
    64,   /* 0x3000-0x303F */
    95,   /* 0xFF01-0xFF5E */
    24,   /* 0x2010-0x2027 */
};

static const unsigned char *fb_glyph(unsigned int cp) {
    static const unsigned char *sections[NSEC];
    static int sections_init = 0;

    if (!fb_ready) {
        return NULL;
    }
    if (!sections_init) {
        const unsigned char *p = fb_data + 40; /* data starts at byte 40 */
        int s;
        for (s = 0; s < NSEC; s++) {
            sections[s] = p;
            p += (unsigned int)sec_count[s] * GLYPH_BYTES;
        }
        sections_init = 1;
    }

    for (int s = 0; s < NSEC; s++) {
        if (cp >= sec_first[s] && cp < sec_first[s] + sec_count[s]) {
            return sections[s] +
                   (unsigned)(cp - sec_first[s]) * GLYPH_BYTES;
        }
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* gxjport_text.h implementation                                       */

int gxjport_get_font_info(int face, int style, int size,
                          int *ascent, int *descent, int *leading) {
    (void)face; (void)style; (void)size;
    vita_font_init();
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
    vita_font_init();
    if (!fb_ready) {
        return -1;
    }
    return n * GW; /* fixed-width bank */
}

int gxjport_draw_chars(int pixel, const jshort *clip, void *dst, int dotted,
                       int face, int style, int size,
                       int x, int y, int anchor,
                       const jchar *chararray, int n) {
    gxj_screen_buffer tmp;
    gxj_screen_buffer *dest;
    int i, pen_x;
    int clipX1, clipY1, clipX2, clipY2;
    gxj_pixel_type color = (gxj_pixel_type)pixel;

    (void)dotted; (void)face; (void)style; (void)size; (void)anchor;
    vita_font_init();

    dest = (gxj_screen_buffer *)dst;
    if (dest == NULL || dest->pixelData == NULL) {
        return KNI_FALSE;
    }

    clipX1 = clip[0]; clipY1 = clip[1];
    clipX2 = clip[2]; clipY2 = clip[3];
    if (clipX1 < 0) clipX1 = 0;
    if (clipY1 < 0) clipY1 = 0;
    if (clipX2 > dest->width)  clipX2 = dest->width;
    if (clipY2 > dest->height) clipY2 = dest->height;

    pen_x = x;

    for (i = 0; i < n; i++) {
        unsigned int cp = (unsigned)chararray[i];
        const unsigned char *g = fb_glyph(cp);

        if (g != NULL) {
            /* draw 20x22 1bpp bitmap, MSB-first per row */
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
                    }
                }
            }
        } else if (cp >= 0x2E80) {
            /* tofu box: 18x18 hollow square for missing CJK */
            int r, c;
            for (r = 0; r < 18; r++) {
                int py = y + r;
                if (py < clipY1 || py >= clipY2) continue;
                for (c = 0; c < 18; c++) {
                    int pxx = pen_x + c;
                    if (pxx < clipX1 || pxx >= clipX2) continue;
                    if (r == 0 || r == 17 || c == 0 || c == 17) {
                        dest->pixelData[py * dest->width + pxx] = color;
                    }
                }
            }
        }
        pen_x += GW;
    }

    return KNI_TRUE;
}
