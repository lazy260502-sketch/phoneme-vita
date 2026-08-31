#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GW 20
#define GH 22
#define STRIDE 3
#define GBYTES (STRIDE * GH)
#define DATA_OFF 40
#define NSEC 5

static const unsigned int sec_first[NSEC] = {0x20, 0x4E00, 0x3000, 0xFF01, 0x2010};
static const unsigned int sec_cnt[NSEC] = {95, 20902, 64, 95, 24};
static const unsigned int total_glyphs = 95 + 20902 + 64 + 95 + 24;

int main(int argc, char **argv) {
    FILE *f, *out;
    long fsize;
    unsigned char *fbuf, *bank;
    stbtt_fontinfo info;
    float scale;
    int asc, dsc, lg;
    unsigned long off, drawn = 0;
    int s, i, r, c;

    if (argc < 3) return 1;
    f = fopen(argv[1], "rb");
    fseek(f, 0, SEEK_END); fsize = ftell(f); fseek(f, 0, SEEK_SET);
    fbuf = malloc(fsize);
    if (fread(fbuf, 1, fsize, f) != (size_t)fsize) return 1;
    fclose(f);
    if (stbtt_InitFont(&info, fbuf, 0) == 0) return 1;
    scale = stbtt_ScaleForPixelHeight(&info, (float)(GH - 4));

    bank = calloc(1, DATA_OFF + (unsigned long)total_glyphs * GBYTES);

    for (s = 0; s < NSEC; s++) {
        for (i = 0; i < (int)sec_cnt[s]; i++) {
            unsigned int cp = sec_first[s] + i;
            int gi = stbtt_FindGlyphIndex(&info, (int)cp);
            int aw = 0, ah = 0, xo = 0, yo = 0;
            unsigned char *m = stbtt_GetGlyphBitmap(&info, scale, scale, gi, &aw, &ah, &xo, &yo);
            unsigned char *dst = bank + DATA_OFF + (unsigned long)drawn * GBYTES;
            if (m) {
                for (r = 0; r < ah && r < GH; r++)
                    for (c = 0; c < aw && c < GW; c++)
                        if (m[r * aw + c] >= 100)
                            dst[r * STRIDE + (c >> 3)] |= 0x80 >> (c & 7);
                stbtt_FreeBitmap(m, NULL);
            }
            drawn++;
        }
    }
    printf("drawn=%lu\n", drawn);
    out = fopen(argv[2], "wb");
    fwrite(bank, 1, DATA_OFF + (unsigned long)total_glyphs * GBYTES, out);
    fclose(out);
    free(bank); free(fbuf);
    return 0;
}
