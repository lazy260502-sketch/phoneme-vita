/*
 * vita_icon.c - PNG icon decoding for the native game menu.
 *
 * Uses libpng (already shipped by VitaSDK at arm-vita-eabi/lib) with a
 * memory reader. Everything is normalized to 8-bit RGBA by libpng
 * (png_set_expand + strip16 + palette->RGB), then repacked to the menu
 * framebuffer's 0xAABBGGRR. Unknown chunks are skipped; gamma is left
 * as-is (icons are small UI assets, not photos).
 */
#include "vita_icon.h"

#include <png.h>
#include <stdlib.h>
#include <string.h>

struct mem_reader {
    const unsigned char *data;
    unsigned long len;
    unsigned long off;
};

static void mem_read_fn(png_structp png, png_bytep out, png_size_t n) {
    struct mem_reader *r = (struct mem_reader *)png_get_io_ptr(png);
    if (r == NULL || r->off + n > r->len) {
        png_error(png, "truncated png");
        return;
    }
    memcpy(out, r->data + r->off, n);
    r->off += n;
}

int vita_icon_decode_png(const unsigned char *data, unsigned long len,
                         uint32_t **out_pixels, int *out_w, int *out_h) {
    struct mem_reader reader;
    png_structp png;
    png_infop info;
    png_bytep *rows;
    png_uint_32 w, h;
    int depth, ctype;
    uint32_t *pixels;
    png_uint_32 y;

    if (data == NULL || len < 8 ||
        png_sig_cmp(data, 0, len > 8 ? 8 : len) != 0) {
        return -1;
    }

    reader.data = data;
    reader.len = len;
    reader.off = 0;

    /* libpng < 1.5 needs a user error fn that returns instead of
     * calling exit(); setjmp keeps the error path local. */
    png = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    if (png == NULL) {
        return -1;
    }
    info = png_create_info_struct(png);
    if (info == NULL || setjmp(png_jmpbuf(png))) {
        png_destroy_read_struct(&png, info ? &info : NULL, NULL);
        return -1;
    }

    png_set_read_fn(png, &reader, mem_read_fn);
    png_read_info(png, info);
    png_get_IHDR(png, info, &w, &h, &depth, &ctype, NULL, NULL, NULL);
    if (w == 0 || h == 0 || w > 1024 || h > 1024) {
        png_destroy_read_struct(&png, &info, NULL);
        return -1;
    }

    /* Normalize to 8-bit RGBA */
    if (ctype == PNG_COLOR_TYPE_PALETTE) {
        png_set_palette_to_rgb(png);
    }
    if (ctype == PNG_COLOR_TYPE_GRAY && depth < 8) {
        png_set_expand_gray_1_2_4_to_8(png);
    }
    if (png_get_valid(png, info, PNG_INFO_tRNS)) {
        png_set_tRNS_to_alpha(png);
    }
    if (depth == 16) {
        png_set_strip_16(png);
    }
    if (ctype == PNG_COLOR_TYPE_GRAY ||
        ctype == PNG_COLOR_TYPE_GRAY_ALPHA) {
        png_set_gray_to_rgb(png);
    }
    png_set_filler(png, 0xFF, PNG_FILLER_AFTER); /* RGB -> RGBA */
    png_read_update_info(png, info);

    /* The row stride has to come from libpng, not from the assumption
     * "always RGBA": png_set_filler() is ignored for images that already
     * carry alpha, and palette/tRNS/16-bit combinations can end up with a
     * different channel count. Assuming w*4 made png_read_image() write
     * past `pixels` and smash the malloc arena (v01.78g: the damage only
     * showed up later, inside free(), when the menu exited). Everything is
     * normalized to RGBA above, so anything else is refused. */
    if (png_get_rowbytes(png, info) != (png_uint_32)w * 4) {
        png_destroy_read_struct(&png, &info, NULL);
        return -1;
    }

    pixels = (uint32_t *)malloc((size_t)w * h * 4);
    rows = (png_bytep *)malloc((size_t)h * sizeof(png_bytep));
    if (pixels == NULL || rows == NULL) {
        free(pixels);
        free(rows);
        png_destroy_read_struct(&png, &info, NULL);
        return -1;
    }
    for (y = 0; y < h; y++) {
        rows[y] = (png_byte *)(pixels + (size_t)y * w);
    }

    if (setjmp(png_jmpbuf(png))) {
        free(pixels);
        free(rows);
        png_destroy_read_struct(&png, &info, NULL);
        return -1;
    }
    png_read_image(png, rows);
    png_read_end(png, NULL);
    png_destroy_read_struct(&png, &info, NULL);
    free(rows);

    /* RGBA byte order [R,G,B,A] read as a little-endian word IS
     * 0xAABBGGRR - the menu framebuffer format - so no repacking. */

    *out_pixels = pixels;
    *out_w = (int)w;
    *out_h = (int)h;
    return 0;
}
