/*
 * vita_font.c - bitmap-bank CJK/ASCII text rendering for phoneME MIDP.
 *
 * Replaces the gxjport_text.o stub inside libobj.a (the CMake build
 * deletes that archive member). Instead of rasterizing on the Vita
 * (stb_truetype hung on ARM), this reads a pre-rendered 1bpp bitmap bank
 * generated on the PC by tools/fontgen.c:
 *   section[0] ASCII 0x20..0x7E, section[1] CJK 0x4E00..0x9FA5
 * Loaded from ux0:/data/J2ME00001/fontbitmap.bin, then app0:.
 */
#include <kni.h>
#include <gxj_putpixel.h>
#include "gxj_intern_graphics.h" /* getScreenBuffer: dst==NULL means the
                                    system screen buffer on this port */
#include <psp2/io/fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Direct-to-file diagnostics: the CLDC VM rebinds stdio to tty0 at
 * startup, so fprintf(stderr) from inside the VM never reaches
 * midp_stderr.log. This logger keeps its own fd. */
static SceUID font_log_fd = -2;
static void flog(const char *fmt, ...) {
    char buf[256];
    va_list ap;
    int n;
    if (font_log_fd == -2) {
        font_log_fd = sceIoOpen("ux0:/data/J2ME00001/font_debug.log",
                                SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
    }
    if (font_log_fd < 0) {
        return;
    }
    va_start(ap, fmt);
    n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n > 0) {
        sceIoWrite(font_log_fd, buf, n);
    }
}

#define GW 20
#define GH 22
#define STRIDE ((GW + 7) / 8)
#define GLYPH_BYTES (STRIDE * GH)

static unsigned char *fb_data = NULL;
static unsigned int fb_size = 0;
static int fb_ready = 0;
static int fb_gw = GW, fb_gh = GH, fb_stride = STRIDE;
static unsigned int fb_ascii_first, fb_ascii_count;
static unsigned int fb_cjk_first, fb_cjk_count;
static const unsigned char *fb_ascii, *fb_cjk;
static int fb_ascent = 18;

static void fb_load(const char *path) {
    SceUID fd = sceIoOpen(path, SCE_O_RDONLY, 0);
    unsigned char hdr[40];
    unsigned int data_off;
    if (fd < 0) {
        return;
    }
    if (sceIoRead(fd, hdr, sizeof(hdr)) != sizeof(hdr) ||
        memcmp(hdr, "FBMP", 4) != 0) {
        sceIoClose(fd);
        return;
    }
    /* Exact layout dumped from fontbitmap.bin:
     *   u16@6  = GW, byte@8 = GH, u16@10 = STRIDE,
     *   u32@12 = data_off, u32@16 = ascii_first(0x20),
     *   u32@20 = ascii_count(95), u32@24 = cjk_data_off,
     *   u32@28 = cjk_first(0x4E00), u32@32 = cjk_count(20902)
     */
    fb_gw     = hdr[6]  | (hdr[7]  << 8);
    fb_gh     = hdr[8];
    fb_stride = hdr[10] | (hdr[11] << 8);
    data_off  = (unsigned)hdr[12] | ((unsigned)hdr[13] << 8) |
                ((unsigned)hdr[14] << 16) | ((unsigned)hdr[15] << 24);
    fb_ascii_first = (unsigned)hdr[16] | ((unsigned)hdr[17] << 8) |
                     ((unsigned)hdr[18] << 16) | ((unsigned)hdr[19] << 24);
    fb_ascii_count = (unsigned)hdr[20] | ((unsigned)hdr[21] << 8) |
                     ((unsigned)hdr[22] << 16) | ((unsigned)hdr[23] << 24);
    fb_cjk_first   = (unsigned)hdr[28] | ((unsigned)hdr[29] << 8) |
                     ((unsigned)hdr[30] << 16) | ((unsigned)hdr[31] << 24);
    fb_cjk_count   = (unsigned)hdr[32] | ((unsigned)hdr[33] << 8) |
                     ((unsigned)hdr[34] << 16) | ((unsigned)hdr[35] << 24);

    fb_size = data_off + (unsigned)fb_ascii_count * GLYPH_BYTES +
              (unsigned)fb_cjk_count * GLYPH_BYTES;
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
    fb_ascii = fb_data + data_off;
    fb_cjk = fb_ascii + (unsigned)fb_ascii_count * GLYPH_BYTES;
    fb_ready = 1;
    fprintf(stderr,
        "[font] bitmap bank loaded from %s (gw=%d gh=%d stride=%d "
        "ascii=%u cjk=%u)\n",
        path, fb_gw, fb_gh, fb_stride, fb_ascii_count, fb_cjk_count);
    fflush(stderr);
}

static void vita_font_init(void) {
    if (fb_ready) {
        return;
    }
    fb_load("ux0:/data/J2ME00001/fontbitmap.bin");
    if (!fb_ready) {
        fb_load("app0:/data/J2ME00001/fontbitmap.bin");
    }
    if (fb_ready) {
        fb_ascent = fb_gh - 4;
    }
}

/* returns glyph bitmap (1bpp, STRIDE bytes/row) or NULL */
static const unsigned char *fb_glyph(unsigned int cp) {
    if (!fb_ready) {
        return NULL;
    }
    if (cp >= fb_ascii_first && cp < fb_ascii_first + fb_ascii_count) {
        return fb_ascii + (unsigned)(cp - fb_ascii_first) * GLYPH_BYTES;
    }
    if (cp >= fb_cjk_first && cp < fb_cjk_first + fb_cjk_count) {
        return fb_cjk + (unsigned)(cp - fb_cjk_first) * GLYPH_BYTES;
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
    if (ascent)  *ascent  = fb_ascent;
    if (descent) *descent = fb_gh - fb_ascent;
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

static gxj_pixel_type pixel_color_cache;

/* Draw a 1bpp bank glyph with the current color (foreground only). */
static void fb_blit(gxj_screen_buffer *dest, int dx, int dy,
                    const unsigned char *glyph) {
    int x, y;
    for (y = 0; y < fb_gh; y++) {
        int py = dy + y;
        const unsigned char *row = glyph + y * fb_stride;
        if (py < 0 || py >= dest->height) {
            continue;
        }
        for (x = 0; x < fb_gw; x++) {
            int px = dx + x;
            if (px < 0 || px >= dest->width) {
                continue;
            }
            if (row[x >> 3] & (0x80 >> (x & 7))) {
                dest->pixelData[py * dest->width + px] =
                    (gxj_pixel_type)(pixel_color_cache & 0xFFFF);
            }
        }
    }
}

int gxjport_draw_chars(int pixel, const jshort *clip, void *dst, int dotted,
                       int face, int style, int size,
                       int x, int y, int anchor,
                       const jchar *chararray, int n) {
    gxj_screen_buffer tmp;
    gxj_screen_buffer *dest;
    int i, pen_x;
    int clipX1, clipY1, clipX2, clipY2;

    (void)dotted; (void)face; (void)style; (void)size; (void)anchor;
    vita_font_init();

    /* gxj_text.c already resolved dst into a gxj_screen_buffer* (stack
     * copy for images, &gxj_system_screen_buffer for the screen) before
     * calling us - re-interpreting it as java_imagedata produced garbage
     * (dest=320x-2 billion) and all pixels went to wild memory. Just
     * cast it. */
    (void)tmp;
    dest = (gxj_screen_buffer *)dst;
    if (dest == NULL) {
        return KNI_FALSE;
    }

    clipX1 = clip[0]; clipY1 = clip[1];
    clipX2 = clip[2]; clipY2 = clip[3];
    if (clipX1 < 0) clipX1 = 0;
    if (clipY1 < 0) clipY1 = 0;
    if (clipX2 > dest->width)  clipX2 = dest->width;
    if (clipY2 > dest->height) clipY2 = dest->height;

    pixel_color_cache = (gxj_pixel_type)pixel;
    pen_x = x;

    /* ---- ONE-SHOT DECISIVE DIAGNOSTIC ----
     * 1) log pointer identity: is dst the system screen buffer?
     * 2) paint 4 corner color markers DIRECTLY into dest->pixelData.
     *    If they show up on screen, the buffer is the displayed one and
     *    any missing text is a glyph-content issue. If not, dest is not
     *    the displayed buffer. No more guessing. */
    {
        static int diag_done = 0;
        if (!diag_done) {
            extern gxj_screen_buffer gxj_system_screen_buffer;
            gxj_pixel_type *px = dest->pixelData;
            diag_done = 1;
            flog("dst=%p sysbuf=%p same=%d\n",
                 dst, (void *)&gxj_system_screen_buffer,
                 dst == (void *)&gxj_system_screen_buffer);
            flog("dest=%dx%d pixelData=%p alphaData=%p\n",
                 dest->width, dest->height,
                 (void *)dest->pixelData, (void *)dest->alphaData);
            flog("entry: fb_ready=%d n=%d first_cp=0x%04x clip=(%d,%d,%d,%d) "
                 "x=%d y=%d\n",
                 fb_ready, n, (unsigned)chararray[0],
                 clip[0], clip[1], clip[2], clip[3], x, y);
            if (px != NULL && dest->width > 80 && dest->height > 80) {
                int m, k;
                /* corners: TL=white TR=red BL=green BR=blue (RGB565) */
                const gxj_pixel_type col[4] =
                    { 0xFFFF, 0xF800, 0x07E0, 0x001F };
                const int cx[4] = { 0, dest->width - 40,
                                    0, dest->width - 40 };
                const int cy[4] = { 0, 0,
                                    dest->height - 40, dest->height - 40 };
                for (m = 0; m < 4; m++) {
                    int written = 0;
                    for (k = 0; k < 40; k++) {
                        int yy = cy[m] + k;
                        int xx;
                        if (yy < 0 || yy >= dest->height) continue;
                        for (xx = 0; xx < 40; xx++) {
                            int xpos = cx[m] + xx;
                            if (xpos < 0 || xpos >= dest->width) continue;
                            px[yy * dest->width + xpos] = col[m];
                            written++;
                        }
                    }
                    flog("marker[%d] wrote %d px at (%d,%d)\n",
                         m, written, cx[m], cy[m]);
                }
                flog("first glyph bitcount check next; screen NOW has "
                     "4 corner squares if buffer is displayed\n");
            }
        }
    }

    for (i = 0; i < n; i++) {
        unsigned int cp = (unsigned)chararray[i];
        const unsigned char *g = fb_glyph(cp);
        {
            static int char_diag = 8;
            if (char_diag > 0 && g != NULL) {
                char_diag--;
                int row, bits = 0;
                for (row = 0; row < fb_gh; row++) {
                    const unsigned char *r8 = g + row * fb_stride;
                    bits += (r8[0] ? 1 : 0) + (r8[1] ? 1 : 0) +
                            (fb_stride > 2 && r8[2] ? 1 : 0);
                }
                flog("ch cp=0x%04x glyph_rows_nonblank=%d/22\n", cp, bits);
            }
        }
        if (g == NULL) {
            /* tofu box for CJK, blank space otherwise */
            if ((cp >= 0x2E80 && cp <= 0x9FFF) ||
                (cp >= 0xFF00 && cp <= 0xFFEF)) {
                int r, c2;
                for (r = 0; r < 18; r++) {
                    int py = y + r;
                    if (py < clipY1 || py >= clipY2) continue;
                    for (c2 = 0; c2 < 18; c2++) {
                        int px = pen_x + c2;
                        if (px < clipX1 || px >= clipX2) continue;
                        if (r == 0 || r == 17 || c2 == 0 || c2 == 17) {
                            dest->pixelData[py * dest->width + px] =
                                pixel_color_cache;
                        }
                    }
                }
            }
        } else {
            int r, c2;
            for (r = 0; r < fb_gh; r++) {
                int py = y + r;
                const unsigned char *row = g + r * fb_stride;
                if (py < clipY1 || py >= clipY2) continue;
                for (c2 = 0; c2 < fb_gw; c2++) {
                    int px = pen_x + c2;
                    if (px < clipX1 || px >= clipX2) continue;
                    if (row[c2 >> 3] & (0x80 >> (c2 & 7))) {
                        static int first_px_logged = 0;
                        if (!first_px_logged) {
                            first_px_logged = 1;
                            flog("first glyph px written: addr=%p "
                                 "(x=%d y=%d) color=0x%04x\n",
                                 (void *)&dest->pixelData[
                                     py * dest->width + px],
                                 px, py, (unsigned)pixel_color_cache);
                        }
                        dest->pixelData[py * dest->width + px] =
                            pixel_color_cache;
                    }
                }
            }
        }
        pen_x += GW;
    }

    return KNI_TRUE;
}
