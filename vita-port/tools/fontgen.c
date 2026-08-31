/* fontgen.c - offline CJK bitmap font generator (PC tool)
 * Renders a TTF into a packed 1bpp bitmap bank for vita_font.c.
 * Layout: section[0] ASCII 0x20..0x7E, section[1] CJK 0x4E00..0x9FA5.
 * Each glyph: GH rows of STRIDE bytes (MSB-first), vertically placed so
 * the font ascent line sits GH/6 px below the cell top.
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

int main(int argc, char **argv) {
    FILE *f, *out;
    long size;
    unsigned char *buf;
    stbtt_fontinfo info;
    float scale;
    int asc = 0, dsc = 0, lg = 0;
    unsigned char *bank;
    unsigned long out_off = 40;
    int i;
    unsigned long hdr[8];

    if (argc < 3) { printf("usage: fontgen font.ttf out.bin\n"); return 1; }
    f = fopen(argv[1], "rb");
    if (!f) { printf("no font\n"); return 1; }
    fseek(f, 0, SEEK_END); size = ftell(f); fseek(f, 0, SEEK_SET);
    buf = malloc(size);
    if (fread(buf, 1, size, f) != (size_t)size) return 1;
    fclose(f);
    if (stbtt_InitFont(&info, buf, 0) == 0) { printf("init fail\n"); return 1; }
    scale = stbtt_ScaleForPixelHeight(&info, (float)(GH - 2));
    stbtt_GetFontVMetrics(&info, &asc, &dsc, &lg);

    bank = calloc(1, 40 + (unsigned long)TOTAL * GLYPH_BYTES);
    memcpy(bank, "FBMP", 4);
    hdr[0] = 40;                              /* data offset */
    hdr[1] = GW; hdr[2] = GH; hdr[3] = STRIDE;
    hdr[4] = ASCII_FIRST; hdr[5] = ASCII_COUNT;
    hdr[6] = CJK_FIRST;  hdr[7] = CJK_COUNT;
    for (i = 0; i < 8; i++) {
        bank[8 + i*4]  = (unsigned char)hdr[i];
        bank[9 + i*4]  = (unsigned char)(hdr[i] >> 8);
        bank[10 + i*4] = (unsigned char)(hdr[i] >> 16);
        bank[11 + i*4] = (unsigned char)(hdr[i] >> 24);
    }

    for (i = 0; i < TOTAL; i++) {
        int cp = (i < ASCII_COUNT) ? (ASCII_FIRST + i)
                                   : (CJK_FIRST + i - ASCII_COUNT);
        int idx = stbtt_FindGlyphIndex(&info, cp);
        int aw = 0, ah = 0, xo = 0, yo = 0;
        unsigned char *mask = stbtt_GetGlyphBitmap(&info, 0, scale, idx,
                                                   &aw, &ah, &xo, &yo);
        unsigned char *dst = bank + out_off + (unsigned long)i * GLYPH_BYTES;
        if (mask != NULL && aw > 0 && ah > 0) {
            /* place glyph so its baseline sits at cell_y + asc_px */
            int asc_px = (int)(asc * scale + 0.5f);
            int base_y = 2 + asc_px;         /* baseline row in cell */
            int x, y;
            for (y = 0; y < ah; y++) {
                int dy = base_y - yo + y - (ah - (ah)); /* rows below top */
                int sy = y;
                (void)sy;
                dy = base_y - yo + y;
                for (x = 0; x < aw; x++) {
                    unsigned char a = mask[y * aw + x];
                    int dx = xo + x;
                    if (a >= 128 && dy >= 0 && dy < GH && dx >= 0 && dx < GW) {
                        dst[dy * STRIDE + (dx >> 3)] |= (0x80 >> (dx & 7));
                    }
                }
            }
            stbtt_FreeBitmap(mask, NULL);
        }
    }

    out = fopen(argv[2], "wb");
    fwrite(bank, 1, out_off + (unsigned long)TOTAL * GLYPH_BYTES, out);
    fclose(out);
    printf("fontbitmap.bin: %lu bytes\n",
           out_off + (unsigned long)TOTAL * GLYPH_BYTES);
    return 0;
}
