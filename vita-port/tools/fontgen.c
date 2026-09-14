#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * fontgen - bake the glyph bank read by vita-port/src/vita_font.c.
 *
 * Usage: fontgen <cjk.ttf> <latin.ttf> <out.bin>
 *
 * Two source fonts, because one CJK font's Latin is a compromise: the
 * Latin/ASCII sections are rasterised from a dedicated Latin face with
 * real side bearings and proportional advances, the CJK sections from the
 * CJK face with a fixed full-width advance.
 *
 * Cell geometry.  Every glyph gets a CW x CH cell whose origin is the pen
 * position; the baseline sits ASCENT rows below the top of the cell.  The
 * runtime reports exactly those numbers through gxjport_get_font_info()
 * (ascent 18, descent 4, height 22), so a glyph baked at row
 * (ASCENT - yoff) lands on the line's baseline.
 *
 * This is the fix for the old bank's two visible defects: it blitted the
 * ink box at the cell's top-left corner (ignoring stb's xoff/yoff), which
 * left every glyph hanging from the top of the line - commas, periods and
 * descenders ended up at the top instead of on the baseline; and it
 * advanced the pen by the cell width for every character, so Latin text
 * was forced into a 20 px monospace grid.
 */

#define CW      20  /* cell width, px */
#define CH      22  /* cell height, px */
#define ASCENT  18  /* baseline, rows below the cell top (== descent 4) */

/* Em size handed to stbtt_ScaleForPixelHeight.  CJK_PX is the value the
 * previous single-font bank used, so ideograph size is unchanged. */
#define CJK_PX   18
#define LATIN_PX 20

#define NSEC 11

/* One bank section: a contiguous code point range rasterised from one of
 * the two fonts.  The latin sections are stored anti aliased (8bpp alpha)
 * and carry per-glyph advances; the CJK sections stay 1bpp and use the
 * cell width as the advance. */
typedef struct {
    unsigned int first;
    unsigned int count;
    int          latin;
} Sec;

static const Sec secs[NSEC] = {
    /* Latin text: proportional, anti aliased. */
    { 0x0020,    95, 1 },   /* ASCII */
    { 0x00A0,    96, 1 },   /* Latin-1 supplement */
    { 0x0100,   128, 1 },   /* Latin Extended-A */
    { 0x0386,    56, 1 },   /* Greek */
    { 0x2010,    24, 1 },   /* general punctuation: dashes, quotes */
    { 0x2460,    40, 1 },   /* circled numbers (1)(2)(3)... */
    /* CJK text: full width, monospace, plain 1bpp. */
    { 0x3000,    64, 0 },   /* CJK punctuation */
    { 0xFF01,    95, 0 },   /* fullwidth forms */
    { 0xFF61,    63, 0 },   /* halfwidth katakana */
    { 0x4E00, 20902, 0 },   /* CJK Unified Ideographs (through 0x9FA5) */
    { 0x9FA6,    90, 0 }    /* CJK tail 0x9FA6-0x9FFF */
};

/*
 * File layout (little endian, version 2):
 *
 *   0  char[4] "J2FB"
 *   4  u8   version (2)
 *   5  u8   flags (0)
 *   6  u16  reserved
 *   8  u32  nsec
 *  12  u32  dataOff      start of the bitmap blob
 *  16  s16  ascent
 *  18  s16  descent
 *  20  s16  leading
 *  22  u16  reserved
 *  24  u32  nglyphs      sum of all section counts
 *  28  u32  reserved
 *
 * then nsec section records of 28 bytes:
 *
 *  +0  u32  first        first code point
 *  +4  u32  count
 *  +8  u32  gw           cell width
 *  +12 u32  gh           cell height
 *  +16 u32  stride       bytes per bitmap row (1bpp: (gw+7)/8, 8bpp: gw)
 *  +20 u32  bpp          bits per pixel: 1 or 8
 *  +24 u32  advOff       absolute offset of `count` u8 advances,
 *                        0 meaning "no glyph"
 *
 * then the advance tables, then at dataOff the section bitmaps back to
 * back, each section holding count * stride * gh bytes in code point
 * order.  Every section has an advance table, so a zero advance is the
 * single representation of "this code point has no glyph" regardless of
 * section type.
 */

#define HDR_SIZE 32
#define SEC_SIZE 28

static void put32(unsigned char *p, unsigned int v) {
    p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF;
    p[2] = (v >> 16) & 0xFF; p[3] = (v >> 24) & 0xFF;
}
static void put16(unsigned char *p, int v) {
    p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF;
}

typedef struct {
    stbtt_fontinfo info;
    unsigned char *buf;
    float          scale;
    const char    *path;
} Face;

static int face_open(Face *f, const char *path, int px) {
    FILE *fp;
    long  n;

    fp = fopen(path, "rb");
    if (fp == NULL) {
        fprintf(stderr, "fontgen: cannot open %s\n", path);
        return 1;
    }
    fseek(fp, 0, SEEK_END);
    n = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    f->buf = (unsigned char *)malloc((size_t)n);
    if (f->buf == NULL || fread(f->buf, 1, (size_t)n, fp) != (size_t)n) {
        fprintf(stderr, "fontgen: cannot read %s\n", path);
        fclose(fp);
        return 1;
    }
    fclose(fp);
    if (stbtt_InitFont(&f->info, f->buf, 0) == 0) {
        fprintf(stderr, "fontgen: not a TrueType font: %s\n", path);
        return 1;
    }
    f->scale = stbtt_ScaleForPixelHeight(&f->info, (float)px);
    f->path = path;
    return 0;
}

/* Advance in pixels, clamped to the 1..255 range the table can hold.
 * 0 means "no glyph". */
static unsigned char glyph_advance(const Face *f, int gi) {
    int aw = 0, lsb = 0, adv;
    if (gi == 0) {
        return 0;
    }
    stbtt_GetGlyphHMetrics(&f->info, gi, &aw, &lsb);
    adv = (int)(f->scale * (float)aw + 0.5f);
    if (adv < 1) {
        adv = 1;
    }
    if (adv > 255) {
        adv = 255;
    }
    return (unsigned char)adv;
}

/* Probe report: proves the xoff/yoff convention and the resulting rows.
 * 'A' must sit above the baseline, '.' right on it. */
static void probe(const Face *f, const char *what) {
    static const struct { int cp; const char *n; } p[] = {
        { 'A', "A" }, { 'x', "x" }, { '.', "." }, { ',', "," }, { 'g', "g" }
    };
    int i;
    printf("  probe %-5s (%s):\n", what, f->path);
    for (i = 0; i < (int)(sizeof(p) / sizeof(p[0])); i++) {
        int gi = stbtt_FindGlyphIndex(&f->info, p[i].cp);
        int aw = 0, ah = 0, xo = 0, yo = 0;
        unsigned char *m;
        if (gi == 0) {
            printf("    %-2s : absent\n", p[i].n);
            continue;
        }
        m = stbtt_GetGlyphBitmap(&f->info, f->scale, f->scale, gi,
                                 &aw, &ah, &xo, &yo);
        printf("    %-2s : adv=%d ink=%dx%d xoff=%d yoff=%d -> rows %d..%d\n",
               p[i].n, glyph_advance(f, gi), aw, ah, xo, yo,
               ASCENT + yo, ASCENT + yo + ah - 1);
        if (m != NULL) {
            stbtt_FreeBitmap(m, NULL);
        }
    }
}

int main(int argc, char **argv) {
    Face cjk, latin;
    unsigned char *bank, *adv_base;
    unsigned long *sec_bmp_off;
    unsigned long total = 0, adv_off, data_off, bmp_off, bank_size;
    unsigned long drawn = 0, clipped_up = 0, clipped_dn = 0, clipped_x = 0;
    FILE *out;
    int s, i, r, c;

    if (argc < 3) {
        fprintf(stderr, "usage: %s <cjk.ttf> [latin.ttf] <out.bin>\n", argv[0]);
        return 1;
    }
    /* Two-font form, or the older one-font form. */
    if (argc >= 4) {
        if (face_open(&cjk, argv[1], CJK_PX) != 0 ||
            face_open(&latin, argv[2], LATIN_PX) != 0) {
            return 1;
        }
        out = fopen(argv[3], "wb");
    } else {
        if (face_open(&cjk, argv[1], CJK_PX) != 0) {
            return 1;
        }
        if (face_open(&latin, argv[1], LATIN_PX) != 0) {
            return 1;
        }
        out = fopen(argv[2], "wb");
    }
    if (out == NULL) {
        fprintf(stderr, "fontgen: cannot write the output file\n");
        return 1;
    }

    for (s = 0; s < NSEC; s++) {
        total += secs[s].count;
    }

    /* Advance tables first (one byte per glyph), bitmaps after them. */
    adv_off = HDR_SIZE + (unsigned long)SEC_SIZE * NSEC;
    data_off = adv_off + total;
    data_off = (data_off + 3u) & ~3ul;  /* keep the blob 4-byte aligned */

    sec_bmp_off = (unsigned long *)calloc(NSEC, sizeof(*sec_bmp_off));
    bmp_off = data_off;
    for (s = 0; s < NSEC; s++) {
        unsigned long gw = CW, gh = CH;
        unsigned long stride = secs[s].latin ? gw : (gw + 7) / 8;
        sec_bmp_off[s] = bmp_off;
        bmp_off += (unsigned long)secs[s].count * stride * gh;
    }
    bank_size = bmp_off;

    bank = (unsigned char *)calloc(1, bank_size);
    if (bank == NULL || sec_bmp_off == NULL) {
        fprintf(stderr, "fontgen: out of memory\n");
        return 1;
    }
    adv_base = bank + adv_off;

    /* Self-describing header. */
    bank[0] = 'J'; bank[1] = '2'; bank[2] = 'F'; bank[3] = 'B';
    bank[4] = 2;                    /* version */
    bank[5] = 0;                    /* flags */
    put32(bank + 8,  NSEC);
    put32(bank + 12, (unsigned int)data_off);
    put16(bank + 16, ASCENT);
    put16(bank + 18, CH - ASCENT);
    put16(bank + 20, 0);            /* leading */
    put32(bank + 24, (unsigned int)total);

    for (s = 0; s < NSEC; s++) {
        unsigned char *rec = bank + HDR_SIZE + SEC_SIZE * s;
        unsigned long stride = secs[s].latin ? CW : (CW + 7) / 8;
        unsigned long adv_of_this_sec;
        const Face *f = secs[s].latin ? &latin : &cjk;
        int bpp = secs[s].latin ? 8 : 1;
        int adv_min = 256, adv_max = 0, ink_lo = 99, ink_hi = -99;

        /* This section's advance table. */
        adv_of_this_sec = adv_off;
        adv_off += secs[s].count;

        put32(rec + 0,  secs[s].first);
        put32(rec + 4,  secs[s].count);
        put32(rec + 8,  CW);
        put32(rec + 12, CH);
        put32(rec + 16, (unsigned int)stride);
        put32(rec + 20, (unsigned int)bpp);
        put32(rec + 24, (unsigned int)adv_of_this_sec);

        for (i = 0; i < (int)secs[s].count; i++) {
            unsigned int cp = secs[s].first + (unsigned int)i;
            unsigned char *adv = bank + adv_of_this_sec + i;
            unsigned char *dst = bank + sec_bmp_off[s]
                               + (unsigned long)i * stride * CH;
            int gi = stbtt_FindGlyphIndex(&f->info, (int)cp);
            int aw = 0, ah = 0, xo = 0, yo = 0;
            unsigned char *m;
            int cx, cy;

            *adv = glyph_advance(f, gi);
            if (*adv != 0) {
                adv_min = *adv < adv_min ? *adv : adv_min;
                adv_max = *adv > adv_max ? *adv : adv_max;
            }
            if (gi == 0) {
                continue;           /* code point absent from this face */
            }
            m = stbtt_GetGlyphBitmap(&f->info, f->scale, f->scale, gi,
                                     &aw, &ah, &xo, &yo);
            if (m == NULL) {
                continue;           /* blank glyph, e.g. space */
            }
            if (xo < ink_lo) {
                ink_lo = xo;
            }
            if (xo + aw > ink_hi) {
                ink_hi = xo + aw;
            }

            /* Cell origin is the pen position, baseline ASCENT rows down.
             * stb reports the ink box in screen coordinates with y growing
             * downwards and the baseline at 0, so yo is negative for ink
             * above the baseline; the ink box therefore starts at
             * (xoff, ASCENT + yoff). */
            cx = xo;
            cy = ASCENT + yo;
            for (r = 0; r < ah; r++) {
                int py = cy + r;
                if (py < 0) { clipped_up++; continue; }
                if (py >= CH) { clipped_dn++; continue; }
                for (c = 0; c < aw; c++) {
                    int px = cx + c;
                    unsigned char a = m[r * aw + c];
                    if (a == 0 || *adv == 0) {
                        continue;
                    }
                    if (px < 0 || px >= CW) { clipped_x++; continue; }
                    if (bpp == 8) {
                        dst[(unsigned long)py * (CW) + px] = a;
                    } else if (a >= 128) {
                        dst[(unsigned long)py * stride + (px >> 3)]
                            |= (unsigned char)(0x80 >> (px & 7));
                    }
                }
            }
            stbtt_FreeBitmap(m, NULL);
            drawn++;
        }

        if (adv_min == 256) adv_min = 0;
        if (ink_hi == -99) { ink_lo = 0; ink_hi = 0; }
        /*
         * The CJK sections are monospace: every present glyph must advance
         * by the same amount, and no cell may overrun the next one.  The
         * face's own advance can be one pixel short of its ink box (both
         * come from integer rounding of the same em), so widen the whole
         * section to the larger of the two.  Zero stays zero: that is how
         * "no glyph" is spelled.
         */
        if (!secs[s].latin) {
            int a = ink_hi;   /* ink_hi is already "rightmost column + 1" */
            if (a < adv_max) {
                a = adv_max;
            }
            for (i = 0; i < (int)secs[s].count; i++) {
                if (bank[adv_of_this_sec + i] != 0) {
                    bank[adv_of_this_sec + i] = (unsigned char)a;
                }
            }
            if (adv_max > 0) {
                adv_min = adv_max = a;
            }
        }
        printf("  sec %2d: U+%04X..U+%04X %5u %-9s adv %d..%d ink cols %d..%d\n",
               s, secs[s].first, secs[s].first + secs[s].count - 1,
               secs[s].count, bpp == 8 ? "8bpp" : "1bpp",
               adv_min, adv_max, ink_lo, ink_hi - 1);
    }

    printf("fontgen: %d sections, %lu code points, %lu drawn, %lu bytes\n",
           NSEC, total, drawn, bank_size);
    printf("  cell %dx%d, baseline row %d, advance tables at %lu,"
           " bitmaps at %lu\n", CW, CH, ASCENT, (unsigned long)(HDR_SIZE + SEC_SIZE * NSEC),
           data_off);
    if (clipped_up || clipped_dn || clipped_x) {
        printf("  WARNING clipped: %lu above, %lu below, %lu sideways\n",
               clipped_up, clipped_dn, clipped_x);
    } else {
        printf("  no ink clipped by the cell\n");
    }
    probe(&latin, "latin");
    probe(&cjk, "cjk");

    if (fwrite(bank, 1, bank_size, out) != bank_size) {
        fprintf(stderr, "fontgen: short write\n");
        return 1;
    }
    fclose(out);
    free(bank);
    free(sec_bmp_off);
    free(cjk.buf);
    if (latin.buf != cjk.buf) {
        free(latin.buf);
    }
    return 0;
}
