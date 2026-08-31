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
    /* layout as written by tools/fontgen.c (verified by byte dump):
     *   u16@12 GW, u16@16 GH, u16@20 STRIDE, u32@24 data_off,
     *   u32@28 ascii_count, u32@32 cjk_first(0x4E00), u32@36 cjk_count
     */
    fb_gw     = hdr[12] | (hdr[13] << 8);
    fb_gh     = hdr[16] | (hdr[17] << 8);
    fb_stride = hdr[20] | (hdr[21] << 8);
    data_off  = (unsigned)hdr[24] | ((unsigned)hdr[25] << 8) |
                ((unsigned)hdr[26] << 16) | ((unsigned)hdr[27] << 24);
    fb_ascii_count = (unsigned)hdr[28] | ((unsigned)hdr[29] << 8) |
                     ((unsigned)hdr[30] << 16) | ((unsigned)hdr[31] << 24);
    fb_cjk_first   = (unsigned)hdr[32] | ((unsigned)hdr[33] << 8) |
                     ((unsigned)hdr[34] << 16) | ((unsigned)hdr[35] << 24);
    fb_cjk_count   = (unsigned)hdr[36] | ((unsigned)hdr[37] << 8) |
                     ((unsigned)hdr[38] << 16) | ((unsigned)hdr[39] << 24);
    fb_ascii_first = 0x20;

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

    dest = gxj_get_image_screen_buffer_impl(
        (const java_imagedata *)dst, &tmp, NULL);
    if (dest == NULL) {
        return KNI_FALSE;
    }
    /* Same rule as gxj_text.c: a null imagedata means "the system screen
     * buffer" - our tmp would then be garbage (height=-2 billion), and
     * all glyph pixels were being written to wild memory. */
    dest = (gxj_screen_buffer *)getScreenBuffer(dest);

    clipX1 = clip[0]; clipY1 = clip[1];
    clipX2 = clip[2]; clipY2 = clip[3];
    if (clipX1 < 0) clipX1 = 0;
    if (clipY1 < 0) clipY1 = 0;
    if (clipX2 > dest->width)  clipX2 = dest->width;
    if (clipY2 > dest->height) clipY2 = dest->height;

    pixel_color_cache = (gxj_pixel_type)pixel;
    pen_x = x;

    /* one-time diagnostics: prove draw_chars runs and show its inputs */
    {
        static int diag_done = 0;
        if (!diag_done) {
            diag_done = 1;
            flog("draw_chars entry: fb_ready=%d n=%d first_cp=0x%04x "
                 "clip=(%d,%d,%d,%d) dest=%dx%d x=%d y=%d\n",
                 fb_ready, n, (unsigned)chararray[0],
                 clip[0], clip[1], clip[2], clip[3],
                 dest->width, dest->height, x, y);
        }
    }

    for (i = 0; i < n; i++) {
        unsigned int cp = (unsigned)chararray[i];
        const unsigned char *g = fb_glyph(cp);
        {
            static int char_diag = 40;
            if (char_diag > 0) {
                char_diag--;
                flog("ch cp=0x%04x glyph=%s\n", cp, g ? "ok" : "MISSING");
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
