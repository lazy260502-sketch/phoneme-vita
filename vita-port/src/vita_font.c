/*
 * vita_font.c - bitmap-bank CJK/Latin text rendering for phoneME MIDP.
 *
 * Loads a pre-rendered bitmap bank (tools/fontgen.c output) from VPK or
 * ux0 override, and draws glyphs by table lookup + blit.  The bank header
 * is self-describing (magic "J2FB"), so fontgen can change coverage or
 * glyph format without touching this file.
 *
 * Version 2 banks carry per section a cell size, a bit depth (1bpp for
 * the CJK sections, 8bpp alpha for the anti aliased Latin ones) and a
 * per-glyph advance table, where a zero advance means "no glyph here".
 * That is what makes Latin proportional: version 1 advanced the pen by
 * the cell width for every character, so Latin text came out in a 20 px
 * monospace grid.
 *
 * Glyphs are baked with the baseline inside the cell (see fontgen.c), so
 * the cell's top left corner is the pen position and the ink lands on the
 * line's baseline without any per-glyph offset here.
 */
#include <kni.h>
#include <gxj_putpixel.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Fixed-cell geometry: the version 1 fallback, and the nominal cell size
 * reported to the native menu. */
#define GW 20
#define GH 22
#define STRIDE 3

#define FB_MAXSEC 16

static unsigned char *fb_data = NULL;
static unsigned int fb_size = 0;
static int fb_ready = 0;

/* Line metrics, taken from the bank header when it carries them */
static int fb_ascent = 18, fb_descent = 4, fb_leading = 0;
/* Nominal cell: the alpha fallback geometry, and what the native menu uses
 * to lay out its columns */
static int fb_gw = GW, fb_gh = GH, fb_stride = STRIDE;

typedef struct {
    unsigned int first, cnt;
    unsigned int gw, gh, stride, bpp;
    const unsigned char *adv;   /* cnt entries, 0 == no glyph */
    const unsigned char *bmp;   /* cnt * stride * gh bytes */
} fb_sec;

static fb_sec fb_sec_tab[FB_MAXSEC];
static int fb_nsec = 0;

/* Tiny LE readers */
static unsigned int rd32(const unsigned char *p) {
    return p[0] | (p[1] << 8) | (p[2] << 16) | ((unsigned int)p[3] << 24);
}
static int rd16s(const unsigned char *p) {
    int v = p[0] | (p[1] << 8);
    return (v & 0x8000) ? v - 0x10000 : v;
}

static void fb_fail(const char *path, const char *why) {
    fprintf(stderr, "vita_font: %s: %s\n", path, why);
    free(fb_data);
    fb_data = NULL;
    fb_ready = 0;
}

/*
 * Parse a J2FB bank.
 *
 *  version 1: header 28 bytes, then nsec * (first, count) u32 pairs; one
 *             global cell size, 1bpp, fixed advance, bitmaps right after
 *             the section table.
 *  version 2: header 32 bytes (line metrics, glyph count, data offset),
 *             then nsec * 28 byte section records; each section has its
 *             own cell size, bit depth and advance table.  Advance
 *             tables sit before the bitmap blob.
 */
static void fb_load(const char *path) {
    SceUID fd;
    int ver, s;
    unsigned long blob;

    fd = sceIoOpen(path, SCE_O_RDONLY, 0);
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
        fb_fail(path, "short read");
        sceIoClose(fd);
        return;
    }
    sceIoClose(fd);

    if (fb_size < 28 || memcmp(fb_data, "J2FB", 4) != 0) {
        fb_fail(path, "bad header");
        return;
    }
    ver = fb_data[4];
    fb_nsec = (int)rd32(fb_data + 8);
    if (fb_nsec <= 0 || fb_nsec > FB_MAXSEC) {
        fb_fail(path, "bad section count");
        return;
    }

    if (ver == 1) {
        unsigned long off;
        fb_gw = (int)rd32(fb_data + 12);
        fb_gh = (int)rd32(fb_data + 16);
        fb_stride = (int)rd32(fb_data + 20);
        off = rd32(fb_data + 24);
        if (fb_gw <= 0 || fb_gw > 64 || fb_gh <= 0 || fb_gh > 64 ||
            fb_stride != (fb_gw + 7) / 8) {
            fb_fail(path, "bad v1 dimensions");
            return;
        }
        fb_ascent = fb_gh - 4;
        fb_descent = 4;
        fb_leading = 0;
        blob = off + (unsigned long)fb_nsec * 8;
        for (s = 0; s < fb_nsec; s++) {
            fb_sec *sc = &fb_sec_tab[s];
            sc->first = rd32(fb_data + 28 + 8 * s);
            sc->cnt = rd32(fb_data + 32 + 8 * s);
            sc->gw = (unsigned int)fb_gw;
            sc->gh = (unsigned int)fb_gh;
            sc->stride = (unsigned int)fb_stride;
            sc->bpp = 1;
            sc->adv = NULL;         /* v1: advance == cell width */
            sc->bmp = fb_data + blob;
            blob += (unsigned long)sc->cnt * sc->stride * sc->gh;
        }
        if (blob > fb_size) {
            fb_fail(path, "truncated v1 bank");
            return;
        }
    } else if (ver == 2) {
        if (fb_size < 32 + (unsigned int)fb_nsec * 28) {
            fb_fail(path, "truncated v2 header");
            return;
        }
        fb_ascent = rd16s(fb_data + 16);
        fb_descent = rd16s(fb_data + 18);
        fb_leading = rd16s(fb_data + 20);
        blob = rd32(fb_data + 12);
        for (s = 0; s < fb_nsec; s++) {
            const unsigned char *rec = fb_data + 32 + 28 * s;
            fb_sec *sc = &fb_sec_tab[s];
            unsigned long advoff;
            sc->first = rd32(rec + 0);
            sc->cnt = rd32(rec + 4);
            sc->gw = rd32(rec + 8);
            sc->gh = rd32(rec + 12);
            sc->stride = rd32(rec + 16);
            sc->bpp = rd32(rec + 20);
            advoff = rd32(rec + 24);
            if (sc->cnt == 0 || sc->gw == 0 || sc->gw > 64 ||
                sc->gh == 0 || sc->gh > 64 ||
                (sc->bpp != 1 && sc->bpp != 8) ||
                sc->stride != (sc->bpp == 8 ? sc->gw : (sc->gw + 7) / 8)) {
                fb_fail(path, "bad v2 section");
                return;
            }
            if (advoff != 0) {
                if (advoff + sc->cnt > fb_size) {
                    fb_fail(path, "advance table out of range");
                    return;
                }
                sc->adv = fb_data + advoff;
            } else {
                sc->adv = NULL;
            }
            if (blob + (unsigned long)sc->cnt * sc->stride * sc->gh > fb_size) {
                fb_fail(path, "truncated v2 bank");
                return;
            }
            sc->bmp = fb_data + blob;
            blob += (unsigned long)sc->cnt * sc->stride * sc->gh;
            if (s == 0) {
                fb_gw = (int)sc->gw;
                fb_gh = (int)sc->gh;
                fb_stride = (int)sc->stride;
            }
        }
    } else {
        fb_fail(path, "unsupported version");
        return;
    }

    fb_ready = 1;
}

/* Lazy-load the bank on first use: ux0 override first, then VPK copy */
static void fb_ensure(void) {
    if (fb_ready) return;
    fb_load("ux0:/data/J2ME00001/fontbitmap.bin");
    if (!fb_ready) fb_load("app0:/data/J2ME00001/fontbitmap.bin");
}

/* One resolved glyph: its bitmap (NULL when the code point is missing),
 * the pen advance that goes with it, and the section's cell format. */
typedef struct {
    const unsigned char *bits;
    int adv;
    int gw, gh, stride, bpp;
} fb_glyph_t;

static void fb_lookup(unsigned int cp, fb_glyph_t *out) {
    int s;

    out->bits = NULL;
    out->adv = 0;
    out->gw = fb_gw;
    out->gh = fb_gh;
    out->stride = fb_stride;
    out->bpp = 1;
    if (!fb_ready) {
        return;
    }
    for (s = 0; s < fb_nsec; s++) {
        const fb_sec *sc = &fb_sec_tab[s];
        if (cp >= sc->first && cp < sc->first + sc->cnt) {
            unsigned int idx = cp - sc->first;
            out->gw = (int)sc->gw;
            out->gh = (int)sc->gh;
            out->stride = (int)sc->stride;
            out->bpp = (int)sc->bpp;
            out->adv = (sc->adv != NULL) ? (int)sc->adv[idx] : (int)sc->gw;
            if (out->adv == 0) {
                return;             /* code point has no glyph */
            }
            out->bits = sc->bmp + (unsigned long)idx * sc->stride * sc->gh;
            return;
        }
    }
}

/* Advance to use when the bank has no glyph at all: full width for CJK
 * (where the caller draws a tofu box) and roughly proportional for the
 * rest, so a stray code point cannot overlap its neighbour. */
static int fb_default_advance(unsigned int cp) {
    if (cp >= 0x2E80) {
        return fb_gw;
    }
    if (cp == ' ') {
        return fb_gw / 3;
    }
    return fb_gw / 2;
}

/* Blend `color` (RGB565) over `dest` with 0..255 coverage (8bpp glyphs) */
static gxj_pixel_type fb_blend565(gxj_pixel_type color,
                                  gxj_pixel_type dest, unsigned int a) {
    unsigned int ia = 255u - a;
    unsigned int r = (((color >> 11) & 0x1F) * a + ((dest >> 11) & 0x1F) * ia) / 255u;
    unsigned int g = (((color >> 5) & 0x3F) * a + ((dest >> 5) & 0x3F) * ia) / 255u;
    unsigned int b = ((color & 0x1F) * a + (dest & 0x1F) * ia) / 255u;
    return (gxj_pixel_type)((r << 11) | (g << 5) | b);
}

/* Same, for the 32bpp 0xAABBGGRR framebuffer used by the native menu */
static uint32_t fb_blend8888(uint32_t color, uint32_t dest, unsigned int a) {
    unsigned int ia = 255u - a;
    unsigned int r = ((color & 0xFF) * a + (dest & 0xFF) * ia) / 255u;
    unsigned int g = (((color >> 8) & 0xFF) * a + ((dest >> 8) & 0xFF) * ia) / 255u;
    unsigned int b = (((color >> 16) & 0xFF) * a + ((dest >> 16) & 0xFF) * ia) / 255u;
    return (dest & 0xFF000000u) | (r & 0xFF) | ((g & 0xFF) << 8) |
           ((b & 0xFF) << 16);
}

/* Blit one glyph into the 565 screen buffer, clipped */
static int fb_blit565(gxj_screen_buffer *dest, const fb_glyph_t *gl,
                      int pen_x, int y, gxj_pixel_type color,
                      int clipX1, int clipY1, int clipX2, int clipY2) {
    int r, c, drawn = 0;
    for (r = 0; r < gl->gh; r++) {
        int py = y + r;
        const unsigned char *row;
        if (py < clipY1 || py >= clipY2) continue;
        row = gl->bits + (unsigned long)r * gl->stride;
        for (c = 0; c < gl->gw; c++) {
            int px = pen_x + c;
            gxj_pixel_type *dst;
            if (px < clipX1 || px >= clipX2) continue;
            dst = &dest->pixelData[py * dest->width + px];
            if (gl->bpp == 8) {
                unsigned int a = row[c];
                if (a == 0) continue;
                *dst = (a == 255) ? color : fb_blend565(color, *dst, a);
            } else if (row[c >> 3] & (0x80 >> (c & 7))) {
                *dst = color;
            } else {
                continue;
            }
            drawn++;
        }
    }
    return drawn;
}

/* Outline box for a code point the bank has no glyph for */
static int fb_tofu565(gxj_screen_buffer *dest, int pen_x, int y, int side,
                      gxj_pixel_type color,
                      int clipX1, int clipY1, int clipX2, int clipY2) {
    int r, c, drawn = 0;
    int top = y + fb_ascent - side + 1;
    for (r = 0; r < side; r++) {
        int py = top + r;
        if (py < clipY1 || py >= clipY2) continue;
        for (c = 0; c < side; c++) {
            int px = pen_x + c;
            if (px < clipX1 || px >= clipX2) continue;
            if (r == 0 || r == side - 1 || c == 0 || c == side - 1) {
                dest->pixelData[py * dest->width + px] = color;
                drawn++;
            }
        }
    }
    return drawn;
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
    if (ascent)  *ascent  = fb_ascent;
    if (descent) *descent = fb_descent;
    if (leading) *leading = fb_leading;
    return KNI_TRUE;
}

int gxjport_get_chars_width(int face, int style, int size,
                            const jchar *charArray, int n) {
    int i, w = 0;
    fb_glyph_t gl;

    (void)face; (void)style; (void)size;
    fb_ensure();
    if (!fb_ready) {
        return -1;
    }
    /* Proportional: sum the bank's per-glyph advances instead of
     * multiplying the cell width, so Latin text measures like Latin. */
    for (i = 0; i < n; i++) {
        unsigned int cp = (unsigned)charArray[i];
        fb_lookup(cp, &gl);
        w += (gl.adv != 0) ? gl.adv : fb_default_advance(cp);
    }
    return w;
}

int gxjport_draw_chars(int pixel, const jshort *clip, void *dst, int dotted,
                       int face, int style, int size,
                       int x, int y, int anchor,
                       const jchar *chararray, int n) {
    gxj_screen_buffer *dest = (gxj_screen_buffer *)dst;
    int i, pen_x;
    int clipX1, clipY1, clipX2, clipY2;
    gxj_pixel_type color = (gxj_pixel_type)pixel;
    fb_glyph_t gl;

    (void)dotted; (void)face; (void)style; (void)size; (void)anchor;

    if (dest == NULL || dest->pixelData == NULL) {
        return KNI_FALSE;
    }

    fb_ensure();

    clipX1 = clip[0]; clipY1 = clip[1];
    clipX2 = clip[2]; clipY2 = clip[3];

    /* `y` is the top of the line box, the cell's top row is the pen row */
    pen_x = x;
    for (i = 0; i < n; i++) {
        unsigned int cp = (unsigned)chararray[i];
        fb_lookup(cp, &gl);
        if (gl.bits != NULL) {
            fb_blit565(dest, &gl, pen_x, y, color,
                       clipX1, clipY1, clipX2, clipY2);
        } else if (cp >= 0x2E80) {
            fb_tofu565(dest, pen_x, y, fb_gw - 2, color,
                       clipX1, clipY1, clipX2, clipY2);
        }
        pen_x += (gl.adv != 0) ? gl.adv : fb_default_advance(cp);
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

/* Blit one glyph into the 32bpp menu framebuffer, clipped */
static void fb_blit8888(uint32_t *fb, int w, int h, const fb_glyph_t *gl,
                        int pen_x, int y, uint32_t color) {
    int r, c;
    for (r = 0; r < gl->gh; r++) {
        int py = y + r;
        const unsigned char *row;
        if (py < 0 || py >= h) continue;
        row = gl->bits + (unsigned long)r * gl->stride;
        for (c = 0; c < gl->gw; c++) {
            int px = pen_x + c;
            uint32_t *dst;
            if (px < 0 || px >= w) continue;
            dst = &fb[py * w + px];
            if (gl->bpp == 8) {
                unsigned int a = row[c];
                if (a == 0) continue;
                *dst = (a == 255) ? color : fb_blend8888(color, *dst, a);
            } else if (row[c >> 3] & (0x80 >> (c & 7))) {
                *dst = color;
            }
        }
    }
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
        fb_glyph_t gl;
        int used = utf8_decode(s, avail, &cp);
        int r, c;

        fb_lookup(cp, &gl);
        if (gl.bits != NULL) {
            fb_blit8888(fb, w, h, &gl, pen_x, y, color);
        } else if (cp >= 0x2E80) {
            int side = fb_gw - 2;
            int top = y + fb_ascent - side + 1;
            for (r = 0; r < side; r++) {
                int py = top + r;
                if (py < 0 || py >= h) continue;
                for (c = 0; c < side; c++) {
                    int px = pen_x + c;
                    if (px < 0 || px >= w) continue;
                    if (r == 0 || r == side - 1 || c == 0 || c == side - 1) {
                        fb[py * w + px] = color;
                    }
                }
            }
        }
        pen_x += (gl.adv != 0) ? gl.adv : fb_default_advance(cp);
        s += used;
        avail -= used;
    }
    return pen_x;
}
