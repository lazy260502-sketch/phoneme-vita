/*
 * vita_font.c - TrueType text rendering for phoneME MIDP on PS Vita.
 *
 * Replaces the empty gxjport_text.o stub inside libobj.a (the CMake build
 * deletes that archive member; this file supplies the same three symbols).
 * The stub returned KNI_FALSE/-1 so gxj_text.c fell back to the built-in
 * 5x7 ASCII-only bitmap font; we instead rasterize any Unicode char
 * (incl. CJK) with stb_truetype from a TTF shipped in the VPK at
 * app0:/data/J2ME00001/font.ttf, overridable by the user via
 *   ux0:/data/J2ME00001/font.ttf
 */

#include <kni.h>
#include <gxjport_text.h>
#include <gxj_putpixel.h>

#include <psp2/io/fcntl.h>

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define STB_TRUETYPE_IMPLEMENTATION
#include <stb_truetype.h>

/* ------------------------------------------------------------------ */
/* Font file loading                                                   */

#define MAX_FONT_BYTES (40 * 1024 * 1024)

static unsigned char *font_data = NULL;
static stbtt_fontinfo font_info;
static int font_loaded = 0;

static int try_load_font(const char *path) {
    fprintf(stderr, "[font] trying %s\n", fflush(stderr), path), fflush(stderr);
    SceUID fd = sceIoOpen(path, SCE_O_RDONLY, 0);
    long size, total = 0;
    unsigned char *buf;

    if (fd < 0) {
        return 0;
    }

    size = (long)sceIoLseek(fd, 0, SCE_SEEK_END);
    sceIoLseek(fd, 0, SCE_SEEK_SET);

    if (size <= 0 || size > MAX_FONT_BYTES) {
        sceIoClose(fd);
        return 0;
    }

    buf = (unsigned char *)malloc((size_t)size);
    if (buf == NULL) {
        sceIoClose(fd);
        return 0;
    }

    while (total < size) {
        int nread = sceIoRead(fd, buf + total, (SceSize)(size - total));
        if (nread <= 0) break;
        total += (long)nread;
    }
    sceIoClose(fd);

    fprintf(stderr, "[font] read %s: %ld/%ld bytes\n", path, (long)total, (long)size);
    fflush(stderr);
    if (total != size || stbtt_InitFont(&font_info, buf, 0) == 0) {
        fprintf(stderr, "[font] stbtt_InitFont FAILED\n");
        fflush(stderr);
        free(buf);
        return 0;
    }

    font_data = buf;
    font_loaded = 1;
    return 1;
}

void vita_font_init(void) {
    if (font_loaded) {
        return;
    }
    /* User override first, then the packaged copy. */
    if (try_load_font("ux0:/data/J2ME00001/font.ttf")) return;
    try_load_font("app0:/data/J2ME00001/font.ttf");
}

/* ------------------------------------------------------------------ */
/* Glyph cache                                                         */

#define FONT_SIZE_MEDIUM_PX 20

#define GLYPH_HT_SIZE 1024 /* power of two */

typedef struct Glyph {
    struct Glyph *next;
    unsigned int codepoint;
    unsigned char size_code; /* 0 = small (16px), 1 = medium (20px),
                              * 2 = large (24px) */
    unsigned char style;
    short width, height;
    short xoff, yoff;    /* bitmap offset from origin (stb convention) */
    short advance;       /* font-unit advance */
    unsigned char *mask; /* width*height coverage 0..255; NULL = blank */
} Glyph;

static Glyph *glyph_buckets[GLYPH_HT_SIZE];

static int px_for_size_code(int size_code) {
    switch (size_code) {
    case 0:  return 16;
    case 2:  return 24;
    default: return FONT_SIZE_MEDIUM_PX;
    }
}

/* MIDP logical size constants (Font.java): SIZE_SMALL 8, SIZE_MEDIUM 16,
 * SIZE_LARGE 24. gxj passes those raw through to us; map to pixel sizes. */
static int size_code_for(int midp_size) {
    switch (midp_size) {
    case 8:  return 0;
    case 24: return 2;
    default: return 1;
    }
}

static float scale_for(int size_code) {
    return stbtt_ScaleForPixelHeight(&font_info,
                                     (float)px_for_size_code(size_code));
}

static unsigned int hash_glyph(unsigned int cp, int sc, int st) {
    return (cp * 31u + (unsigned)sc * 7u + (unsigned)st) & (GLYPH_HT_SIZE - 1);
}

static Glyph *glyph_get(unsigned int cp, int sc, int st) {
    Glyph *g;
    unsigned int h;
    int idx, aw = 0, ah = 0, xo = 0, yo = 0;
    short adv = 0;
    unsigned char *mask;

    h = hash_glyph(cp, sc, st);
    for (g = glyph_buckets[h]; g != NULL; g = g->next) {
        if (g->codepoint == cp && g->size_code == (unsigned char)sc &&
            g->style == (unsigned char)st) {
            return g;
        }
    }

    vita_font_init();
    if (!font_loaded) {
        return NULL;
    }

    g = (Glyph *)calloc(1, sizeof(Glyph));
    if (g == NULL) {
        return NULL;
    }

    {
        static int first_trace = 1;
        unsigned long long t0 = sceKernelGetProcessTimeWide();
        idx = stbtt_FindGlyphIndex(&font_info, (int)cp);
        mask = stbtt_GetGlyphBitmap(&font_info, scale_for(sc),
                                    scale_for(sc), idx, &aw, &ah, &xo, &yo);
        stbtt_GetGlyphHMetrics(&font_info, idx, &adv, NULL);
        if (first_trace || aw == 0 || idx == 0) {
            fprintf(stderr,
                "[font] glyph cp=0x%04x idx=%d bitmap=%dx%d took=%llums\n",
                cp, idx, aw, ah,
                (sceKernelGetProcessTimeWide() - t0) / 1000ULL);
            fflush(stderr);
            first_trace = 0;
        }
    }

    if (mask != NULL && aw > 0 && ah > 0) {
        if (st & 1) { /* bold: cheap one-pixel alpha smear to the right */
            int xx, yy;
            for (yy = 0; yy < ah; yy++) {
                unsigned char *row = mask + yy * aw;
                for (xx = aw - 1; xx >= 1; xx--) {
                    int v = (row[xx] + row[xx - 1]) >> 1;
                    row[xx] = (unsigned char)(v > 255 ? 255 : v);
                }
            }
        }
        g->width  = (short)aw;
        g->height = (short)ah;
        g->xoff   = (short)xo;
        g->yoff   = (short)yo;
        g->mask   = mask;
    } else if (mask != NULL) {
        stbtt_FreeBitmap(mask, NULL);
    }
    /* Missing outline (space, unrenderable): mask stays NULL. Draw marks
     * a tofu box only for CJK-range codepoints so gaps stay visible. */

    g->codepoint = cp;
    g->size_code = (unsigned char)sc;
    g->style     = (unsigned char)st;
    g->advance   = adv;

    h = hash_glyph(cp, sc, st);
    g->next = glyph_buckets[h];
    glyph_buckets[h] = g;
    return g;
}

static int cp_is_cjk(unsigned int cp) {
    return (cp >= 0x2E80 && cp <= 0x9FFF) ||
           (cp >= 0xF900 && cp <= 0xFAFF) ||
           (cp >= 0xFF00 && cp <= 0xFFEF) ||
           (cp >= 0x20000 && cp <= 0x2FA1F);
}

/* ------------------------------------------------------------------ */
/* gxjport_text.h implementation                                       */

int gxjport_get_font_info(int face, int style, int size,
                          int *ascent, int *descent, int *leading) {
    int sc, a = 0, d = 0, lg = 0;
    float scale;

    (void)face;
    (void)style;
    vita_font_init();
    if (!font_loaded) {
        return KNI_FALSE;
    }

    sc = size_code_for(size);
    scale = scale_for(sc);

    stbtt_GetFontVMetrics(&font_info, &a, &d, &lg);
    if (ascent)  *ascent  = (int)(a * scale + 0.5f);
    if (descent) *descent = (int)(d * scale + 0.5f);
    if (leading) *leading = (int)(lg * scale + 0.5f);
    return KNI_TRUE;
}

int gxjport_get_chars_width(int face, int style, int size,
                            const jchar *charArray, int n) {
    int sc, i, total = 0;
    float scale;

    (void)face;
    vita_font_init();
    if (!font_loaded || charArray == NULL || n <= 0) {
        return -1;
    }

    sc = size_code_for(size);
    scale = scale_for(sc);

    for (i = 0; i < n; i++) {
        Glyph *g = glyph_get(charArray[i], sc, style);
        if (g != NULL) {
            total += (int)(g->advance * scale + 0.5f);
        }
    }
    return total > 0 ? total : -1;
}

/* Compose the alpha-plane value approximating this RGB565 text colour. */
static gxj_alpha_type alpha_level_for(gxj_pixel_type p) {
    unsigned r5 = (p >> 11) & 0x1F;
    unsigned g6 = (p >> 5) & 0x3F;
    unsigned b5 = p & 0x1F;
    unsigned lum = (r5 * 8 + g6 * 4 + b5 * 8) / 5; /* ~0..255 */
    if (lum > 255) lum = 255;
    return (gxj_alpha_type)lum;
}

int gxjport_draw_chars(int pixel, const jshort *clip, void *dst, int dotted,
                       int face, int style, int size,
                       int x, int y, int anchor,
                       const jchar *chararray, int n) {
    gxj_screen_buffer tmp;
    gxj_screen_buffer *dest;
    int sc, i, pen_x, pen_y;
    int clipX1, clipY1, clipX2, clipY2;
    gxj_pixel_type color565;
    gxj_alpha_type color_alpha;
    float scale;

    (void)dotted;
    vita_font_init();
    if (!font_loaded || chararray == NULL || n <= 0) {
        fprintf(stderr,
            "[font] draw_chars FALLBACK to builtin (loaded=%d n=%d)\n",
            font_loaded, n);
        fflush(stderr);
        return KNI_FALSE; /* let gxj_text.c draw ASCII via built-in font */
    }
    fprintf(stderr,
        "[font] draw_chars n=%d first_cp=0x%04x clip=(%d,%d,%d,%d)\n",
        n, chararray[0], clip[0], clip[1], clip[2], clip[3]);
    fflush(stderr);

    dest = gxj_get_image_screen_buffer_impl(
        (const java_imagedata *)dst, &tmp, NULL);
    if (dest == NULL) {
        return KNI_FALSE;
    }

    sc = size_code_for(size);
    scale = scale_for(sc);

    color565 = (gxj_pixel_type)pixel;
    color_alpha = alpha_level_for(color565);

    clipX1 = clip[0]; clipY1 = clip[1];
    clipX2 = clip[2]; clipY2 = clip[3];
    if (clipX1 < 0) clipX1 = 0;
    if (clipY1 < 0) clipY1 = 0;
    if (clipX2 > dest->width)  clipX2 = dest->width;
    if (clipY2 > dest->height) clipY2 = dest->height;
    if (clipX1 >= clipX2 || clipY1 >= clipY2) {
        return KNI_TRUE;
    }

    /* gxj_text.c already resolved LEFT/RIGHT/HCENTER/TOP/BOTTOM/BASELINE
     * anchors before calling us: (x,y) is the top-left of the line box. */
    pen_x = x;
    pen_y = y;

    for (i = 0; i < n; i++) {
        Glyph *g = glyph_get(chararray[i], sc, style);
        if (g == NULL) {
            return KNI_FALSE;
        }

        if (g->mask != NULL) {
            int gx = pen_x + g->xoff;
            /* stbtt yoff is baseline->bitmap-top (up positive). (pen_y) is
             * the line-box top, so the baseline sits at pen_y + ascent and
             * the bitmap top lands at baseline - yoff. Writing pen_y+yoff
             * pushed glyphs ~one-line down, outside the clip box. */
            /* stbtt: ascent in font units * current scale = pixels
             * above the baseline where the line-box top maps */
            int a0, d0, lg0;
            float sc_scale = scale_for(sc);
            stbtt_GetFontVMetrics(&font_info, &a0, &d0, &lg0);
            int gy = pen_y + (int)(a0 * sc_scale + 0.5f) - g->yoff;
            int row, col;
            for (row = 0; row < g->height; row++) {
                int dy = gy + row;
                if (dy < clipY1 || dy >= clipY2) continue;
                for (col = 0; col < g->width; col++) {
                    int dx = gx + col;
                    unsigned char a;
                    if (dx < clipX1 || dx >= clipX2) continue;
                    a = g->mask[row * g->width + col];
                    if (a == 0) continue;
                    if (a >= 255) {
                        dest->pixelData[dy * dest->width + dx] = color565;
                        if (dest->alphaData != NULL) {
                            dest->alphaData[dy * dest->width + dx] = color_alpha;
                        }
                    } else {
                        gxj_pixel_type old =
                            dest->pixelData[dy * dest->width + dx];
                        unsigned r5o = (old >> 11) & 0x1F;
                        unsigned g6o = (old >> 5) & 0x3F;
                        unsigned b5o = old & 0x1F;
                        unsigned r5n = ((unsigned)color565 >> 11) & 0x1F;
                        unsigned g6n = ((unsigned)color565 >> 5) & 0x3F;
                        unsigned b5n = (unsigned)color565 & 0x1F;
                        unsigned r = (r5o * (255 - a) + r5n * a) / 255;
                        unsigned gg = (g6o * (255 - a) + g6n * a) / 255;
                        unsigned b = (b5o * (255 - a) + b5n * a) / 255;
                        dest->pixelData[dy * dest->width + dx] =
                            (gxj_pixel_type)((r << 11) | (gg << 5) | b);
                        if (dest->alphaData != NULL) {
                            gxj_alpha_type oa =
                                dest->alphaData[dy * dest->width + dx];
                            unsigned v =
                                (unsigned)(oa * (255 - a) +
                                           (unsigned)color_alpha * a) / 255;
                            dest->alphaData[dy * dest->width + dx] =
                                (gxj_alpha_type)v;
                        }
                    }
                }
            }
        } else if (cp_is_cjk(chararray[i])) {
            /* Tofu box: one-em hollow square so missing glyphs show. */
            int em = px_for_size_code(sc) - 2;
            int row;
            for (row = 0; row < em; row++) {
                int dy = pen_y + row;
                int cxx;
                if (dy < clipY1 || dy >= clipY2) continue;
                if (row == 0 || row == em - 1) {
                    for (cxx = 0; cxx < em; cxx++) {
                        int dx = pen_x + cxx;
                        if (dx >= clipX1 && dx < clipX2) {
                            dest->pixelData[dy * dest->width + dx] = color565;
                        }
                    }
                } else {
                    int dx;
                    dx = pen_x;
                    if (dx >= clipX1 && dx < clipX2) {
                        dest->pixelData[dy * dest->width + dx] = color565;
                    }
                    dx = pen_x + em - 1;
                    if (dx >= clipX1 && dx < clipX2) {
                        dest->pixelData[dy * dest->width + dx] = color565;
                    }
                }
            }
        }

        pen_x += (int)(g->advance * scale + 0.5f);
    }

    fprintf(stderr, "[font] draw_chars done, pen_x=%d\n", pen_x);
    fflush(stderr);
    return KNI_TRUE;
}
