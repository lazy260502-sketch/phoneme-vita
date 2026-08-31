#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GW 20
#define GH 22
#define STRIDE 3
#define GBYTES (STRIDE * GH)

/*
 * Sections: keep vita_font.c in sync only through the self-describing
 * header (magic "J2FB", version 1) - the runtime parses section table
 * from the file, so adding a section here never desyncs the loader.
 */
#define NSEC 9

static const unsigned int sec_first[NSEC] = {
    0x20,   /* ASCII */
    0x4E00, /* CJK Unified (through 0x9FA5) */
    0x3000, /* CJK punctuation */
    0xFF01, /* fullwidth forms */
    0x2010, /* general punctuation */
    0x2460, /* circled numbers: (1)(2)(3)... */
    0x0386, /* Greek */
    0x9FA6, /* CJK tail (0x9FA6-0x9FFF) */
    0xFF61  /* halfwidth katakana */
};
static const unsigned int sec_cnt[NSEC] = {95, 20902, 64, 95, 24, 40, 56, 90, 63};

static void put32(unsigned char *p, unsigned int v) {
    p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF;
    p[2] = (v >> 16) & 0xFF; p[3] = (v >> 24) & 0xFF;
}

int main(int argc, char **argv) {
    FILE *f, *out;
    long fsize;
    unsigned char *fbuf, *bank;
    stbtt_fontinfo info;
    float scale;
    unsigned long total = 0, off, drawn = 0;
    int s, i, r, c;

    if (argc < 3) return 1;
    f = fopen(argv[1], "rb");
    fseek(f, 0, SEEK_END); fsize = ftell(f); fseek(f, 0, SEEK_SET);
    fbuf = malloc(fsize);
    if (fread(fbuf, 1, fsize, f) != (size_t)fsize) return 1;
    fclose(f);
    if (stbtt_InitFont(&info, fbuf, 0) == 0) return 1;
    scale = stbtt_ScaleForPixelHeight(&info, (float)(GH - 4));

    for (s = 0; s < NSEC; s++) total += sec_cnt[s];
    off = 28u + 8u * NSEC; /* header size */

    bank = calloc(1, off + total * GBYTES);

    /* self-describing header */
    bank[0]='J'; bank[1]='2'; bank[2]='F'; bank[3]='B';
    put32(bank + 4, 1);          /* version */
    put32(bank + 8, NSEC);
    put32(bank + 12, GW);
    put32(bank + 16, GH);
    put32(bank + 20, STRIDE);
    put32(bank + 24, off);
    for (s = 0; s < NSEC; s++) {
        put32(bank + 28 + 8 * s, sec_first[s]);
        put32(bank + 28 + 8 * s + 4, sec_cnt[s]);
    }

    for (s = 0; s < NSEC; s++) {
        for (i = 0; i < (int)sec_cnt[s]; i++) {
            unsigned int cp = sec_first[s] + i;
            int gi = stbtt_FindGlyphIndex(&info, (int)cp);
            int aw = 0, ah = 0, xo = 0, yo = 0;
            unsigned char *m = stbtt_GetGlyphBitmap(&info, scale, scale, gi, &aw, &ah, &xo, &yo);
            unsigned char *dst = bank + off + drawn * GBYTES;
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
    printf("drawn=%lu size=%lu\n", drawn, off + total * GBYTES);
    out = fopen(argv[2], "wb");
    fwrite(bank, 1, off + total * GBYTES, out);
    fclose(out);
    free(bank); free(fbuf);
    return 0;
}
