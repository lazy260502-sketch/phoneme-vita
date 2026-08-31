/* fontgen.c - offline CJK bitmap font generator (PC tool)
 * Renders a TTF into a packed 1bpp bitmap bank for vita_font.c.
 *
 * File layout (all little-endian):
 *   [0..3]   "FBMP"
 *   [4]      version = 1
 *   [5]      reserved
 *   [6]      glyph width  (u8)
 *   [7]      reserved
 *   [8]      glyph height (u8)
 *   [9]      reserved
 *   [10]     stride bytes/row (u8)
 *   [11]     reserved
 *   [12..15] data offset (u32) = 40
 *   [16..19] ascii first codepoint (u32) = 0x20
 *   [20..23] ascii count (u32) = 95
 *   [24..27] cjk first codepoint (u32) = 0x4E00
 *   [28..31] cjk count (u32) = 20902
 *   [32..39] reserved
 *   [40...]  glyph bitmaps, 1bpp rows MSB-first
 *
 * Sections: [0] ASCII 0x20..0x7E, [1] CJK 0x4E00..0x9FA5
 */
#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GW 20
#define GH 22
#define STRIDE ((GW + 7) / 8)
#define GLYPH_BYTES (STRIDE * GH)
#define ASCII_FIRST 0x20
#define ASCII_COUNT 95
#define CJK_FIRST 0x4E00
#define CJK_COUNT 20902
#define TOTAL (ASCII_COUNT + CJK_COUNT)
#define DATA_OFF 40

static unsigned int w32(const unsigned char *p) {
    return (unsigned int)p[0] | ((unsigned int)p[1] << 8) |
           ((unsigned int)p[2] << 16) | ((unsigned int)p[3] << 24);
}

int main(int argc, char **argv) {
    FILE *f, *out;
    long size;
    unsigned char *buf;
    stbtt_fontinfo info;
    float scale;
    int asc = 0, dsc = 0, lg = 0;
    unsigned char *bank;
    unsigned long out_off = DATA_OFF;
    unsigned char hdr[40];
    int i, ascii_drawn = 0, cjk_drawn = 0;

    if (argc < 3) { printf("usage: fontgen font.ttf out.bin\n"); return 1; }
    f = fopen(argv[1], "rb");
    if (!f) { printf("no font\n"); return 1; }
    fseek(f, 0, SEEK_END); size = ftell(f); fseek(f, 0, SEEK_SET);
    buf = malloc(size);
    if (fread(buf, 1, size, f) != (size_t)size) return 1;
    fclose(f);
    if (stbtt_InitFont(&info, buf, 0) == 0) { printf("init fail\n"); return 1; }
    scale = stbtt_ScaleForPixelHeight(&info, (float)(GH - 4));
    stbtt_GetFontVMetrics(&info, &asc, &dsc, &lg);
    printf("unitsPerEm ascent=%d scale=%.5f asc_px=%.1f\n",
           1, (double)scale, (double)(asc * scale));

    bank = calloc(1, DATA_OFF + (unsigned long)TOTAL * GLYPH_BYTES);
    memcpy(bank, "FBMP", 4);
    bank[4] = 1;
    bank[6] = (unsigned char)GW;
    bank[8] = (unsigned char)GH;
    bank[10] = (unsigned char)STRIDE;
    {
        unsigned long v;
        unsigned char *p = bank + 12;
        v = DATA_OFF;                        p[0]=v&255; p[1]=(v>>8)&255; p[2]=(v>>16)&255; p[3]=(v>>24)&255;
        v = ASCII_FIRST;                     p[4]=v&255; p[5]=(v>>8)&255; p[6]=(v>>16)&255; p[7]=(v>>24)&255;
        v = ASCII_COUNT;                     p[8]=v&255; p[9]=(v>>8)&255; p[10]=(v>>16)&255; p[11]=(v>>24)&255;
        v = DATA_OFF + (unsigned long)ASCII_COUNT * GLYPH_BYTES;
                                             p[12]=v&255; p[13]=(v>>8)&255; p[14]=(v>>16)&255; p[15]=(v>>24)&255;
        v = CJK_FIRST;                       p[16]=v&255; p[17]=(v>>8)&255; p[18]=(v>>16)&255; p[19]=(v>>24)&255;
        v = CJK_COUNT;                       p[20]=v&255; p[21]=(v>>8)&255; p[22]=(v>>16)&255; p[23]=(v>>24)&255;
    }

    for (i = 0; i < TOTAL; i++) {
        int cp = (i < ASCII_COUNT) ? (ASCII_FIRST + i)
                                   : (CJK_FIRST + i - ASCII_COUNT);
        int idx = stbtt_FindGlyphIndex(&info, cp);
        int aw = 0, ah = 0, xo = 0, yo = 0;
        unsigned char *mask = stbtt_GetGlyphBitmap(&info, scale, scale, idx,
                                                   &aw, &ah, &xo, &yo);
        unsigned char *dst = bank + out_off + (unsigned long)i * GLYPH_BYTES;
        if (mask != NULL && aw > 0 && ah > 0) {
            int x, y;
            /* Place glyph from cell top-left, no baseline math.
             * CJK glyphs fill the em square so top-left placement
             * is correct for a fixed-size bitmap cell. */
            for (y = 0; y < ah && y < GH; y++) {
                for (x = 0; x < aw && x < GW; x++) {
                    unsigned char a = mask[y * aw + x];
                    if (a >= 100) {
                        dst[y * STRIDE + (x >> 3)] |= (0x80 >> (x & 7));
                    }
                }
            }

            stbtt_FreeBitmap(mask, NULL);
            if (i < ASCII_COUNT) ascii_drawn++; else cjk_drawn++;
        }
    }
    printf("glyphs drawn: ascii=%d cjk=%d\n", ascii_drawn, cjk_drawn);

    out = fopen(argv[2], "wb");
    fwrite(bank, 1, DATA_OFF + (unsigned long)TOTAL * GLYPH_BYTES, out);
    fclose(out);
    return 0;
}
