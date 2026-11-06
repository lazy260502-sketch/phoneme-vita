/*
 * vita_jpeg.c - JPEG decoding for phoneME MIDP's image path (v02.89)
 *
 * Why this file exists: MIDP only requires PNG, and this port shipped with
 * USE_JPEG=false, so `Image.createImage()` on a JPEG threw
 * IllegalArgumentException and any game using JPEG art (common in 2005+
 * titles for backgrounds/photos, and in vendor APIs like
 * DirectUtils.createImage) drew nothing or died.
 *
 * Decoder choice: stb_image (public domain, single header, already the
 * style used here for minimp3/tsf/tml). Upstream phoneME ships an IJG
 * libjpeg under phoneme/jpeg, but it is wired only into phoneME's own
 * make system and would add 30+ files to the MIDP build; stb_image is one
 * file, decodes baseline *and* progressive JPEG, and needs no libc beyond
 * memcpy/malloc.
 *
 * Only the JPEG path is compiled (#define STBI_ONLY_JPEG), which keeps the
 * code size in the same range as one extra font rather than a whole image
 * library - the TEXT segment only has a couple of hundred KB of headroom
 * before it runs into the RW segment (see the __sce_headroom note in
 * CMakeLists.txt).
 *
 * The interface implemented here is phoneME's own (see jpegdecoder.h), so
 * no upstream source is modified: only ENABLE_JPEG flips to 1 and this
 * file joins the link.
 *
 * Output conversion: MIDP stores images as RGB565 packed
 *     b | (g << 5) | (r << 11)
 * i.e. an ABGR1555-style word with red in the top bits.
 */
#include <stdlib.h>
#include <string.h>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG          /* no PNG/BMP/TGA/GIF/HDR/PSD code        */
#define STBI_NO_STDIO           /* only the memory entry points are used */
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STBI_ASSERT(x)          /* a corrupt image must not abort()       */
/* phoneME's MIDP build compiles with -Werror. stb_image is third-party
 * code we deliberately do not touch, so scoped pragmas are the honest way
 * to keep its warnings from failing the build. */
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wtype-limits"
#endif
#include "stb_image.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#include "jpegdecoder.h"

/* One decoded image, cached between decodeHeader() and decodeData2().
 * MIDP always calls the pair back to back, so decoding once and then
 * converting is both simpler and faster than parsing twice. */
typedef struct {
    unsigned char *rgb;   /* width*height*3, from stb_image          */
    int width;
    int height;
    int comp;             /* channels stb produced (always 3 here)   */
    int live;             /* decodeHeader succeeded                  */
} VitaJpeg;

void *JPEG_To_RGB_init(void) {
    VitaJpeg *j = (VitaJpeg *)calloc(1, sizeof(VitaJpeg));
    return j;
}

/* Drop the cached pixels but keep the handle usable. */
static void jpeg_reset(VitaJpeg *j) {
    if (j->rgb != NULL) {
        stbi_image_free(j->rgb);
        j->rgb = NULL;
    }
    j->width = 0;
    j->height = 0;
    j->comp = 0;
    j->live = 0;
}

int JPEG_To_RGB_decodeHeader(void *info, char *inData, int inDataLen,
                             int *width, int *height) {
    VitaJpeg *j = (VitaJpeg *)info;
    int w = 0, h = 0;

    if (j == NULL || inData == NULL || inDataLen <= 0) {
        return 0;
    }
    jpeg_reset(j);

    /* stbi_load_from_memory wants "req_comp"; ask for 3 so the conversion
     * below never has to guess. A greyscale JPEG is expanded by stb. */
    j->rgb = stbi_load_from_memory((const stbi_uc *)inData, inDataLen,
                                   &w, &h, &j->comp, 3);
    if (j->rgb == NULL || w <= 0 || h <= 0) {
        /* stb reports a reason through stbi_failure_reason(); there is no
         * logging channel from here (this is called on the VM thread and
         * a synchronous log write costs milliseconds), so the failure just
         * becomes "decoding error" for the Java caller. */
        jpeg_reset(j);
        return 0;
    }
    j->width = w;
    j->height = h;
    j->live = 1;
    if (width != NULL) *width = w;
    if (height != NULL) *height = h;
    return 1;
}

/* Nearest-neighbour resample of one row of the cached image into the
 * destination rect, converting to RGB565 or 32-bit as asked. */
static void jpeg_emit_rows(VitaJpeg *j, unsigned char *out, int pixelSize,
                           int left, int top, int right, int bottom) {
    int dw = right - left;
    int dh = bottom - top;
    int y, x;

    if (dw <= 0 || dh <= 0) {
        return;
    }
    for (y = 0; y < dh; y++) {
        /* destination row -> source row */
        int sy = (dh == j->height) ? (y + top) : (y * j->height) / dh;
        const unsigned char *srow;
        unsigned char *drow;

        if (sy < 0) sy = 0;
        if (sy >= j->height) sy = j->height - 1;
        srow = j->rgb + (size_t)sy * j->width * 3;
        drow = out + (size_t)y * dw * pixelSize;

        for (x = 0; x < dw; x++) {
            int sx = (dw == j->width) ? (x + left) : (x * j->width) / dw;
            const unsigned char *p;
            int r, g, b;

            if (sx < 0) sx = 0;
            if (sx >= j->width) sx = j->width - 1;
            p = srow + (size_t)sx * 3;
            r = p[0];
            g = p[1];
            b = p[2];

            if (pixelSize == 2) {
                /* RGB565: b | (g << 5) | (r << 11) - see IMGDCD_RGB2PIXEL */
                unsigned int v = (unsigned int)((b >> 3) & 0x1F) |
                                 (unsigned int)(((g >> 2) & 0x3F) << 5) |
                                 (unsigned int)(((r >> 3) & 0x1F) << 11);
                drow[x * 2 + 0] = (unsigned char)(v & 0xFF);
                drow[x * 2 + 1] = (unsigned char)((v >> 8) & 0xFF);
            } else {
                /* 32-bit: keep MIDP's channel order (r high, b low). The
                 * alpha byte is set to 0xFF: the MIDP blitters ignore it,
                 * but an 0x00 would look transparent to anything that
                 * honours it. */
                drow[x * 4 + 0] = (unsigned char)b;
                drow[x * 4 + 1] = (unsigned char)g;
                drow[x * 4 + 2] = (unsigned char)r;
                drow[x * 4 + 3] = 0xFF;
            }
        }
    }
}

int JPEG_To_RGB_decodeData2(void *info, char *outData, int outPixelSize,
                            int left, int top, int right, int bottom) {
    VitaJpeg *j = (VitaJpeg *)info;
    int dw, dh;

    if (j == NULL || !j->live || outData == NULL) {
        return 0;
    }
    if (outPixelSize != 2) outPixelSize = 4;
    dw = right - left;
    dh = bottom - top;
    if (dw <= 0 || dh <= 0) {
        return 0;
    }
    jpeg_emit_rows(j, (unsigned char *)outData, outPixelSize,
                   left, top, right, bottom);
    return dw * dh * outPixelSize;
}

int JPEG_To_RGB_decodeData(void *info, char *outData) {
    VitaJpeg *j = (VitaJpeg *)info;
    int y;
    unsigned char *d;

    if (j == NULL || !j->live || outData == NULL) {
        return 0;
    }
    /* 24-bit output is the one case jpeg_emit_rows() does not cover (it
     * writes 2- or 4-byte pixels), and this helper always wants the whole
     * image unscaled, so it is a straight copy of the cached RGB. */
    d = (unsigned char *)outData;
    for (y = 0; y < j->height; y++) {
        const unsigned char *s = j->rgb + (size_t)y * j->width * 3;
        memcpy(d, s, (size_t)j->width * 3);
        d += (size_t)j->width * 3;
    }
    return j->width * j->height * 3;
}

char *JPEG_To_RGB_decode(void *info, char *inData, int inDataLen,
                         int *width, int *height) {
    unsigned char *out;
    int w = 0, h = 0;

    if (!JPEG_To_RGB_decodeHeader(info, inData, inDataLen, &w, &h)) {
        return NULL;
    }
    out = (unsigned char *)malloc((size_t)w * h * 3);
    if (out == NULL) {
        return NULL;
    }
    if (JPEG_To_RGB_decodeData(info, (char *)out) <= 0) {
        free(out);
        return NULL;
    }
    if (width != NULL) *width = w;
    if (height != NULL) *height = h;
    return (char *)out;
}

char *JPEG_To_RGB_decode2(void *info, char *inData, int inDataLen,
                          int outPixelSize, int left, int top,
                          int right, int bottom, int *width, int *height) {
    unsigned char *out;
    int dw, dh, bytes;

    if (!JPEG_To_RGB_decodeHeader(info, inData, inDataLen, width, height)) {
        return NULL;
    }
    if (outPixelSize != 2) outPixelSize = 4;
    dw = right - left;
    dh = bottom - top;
    if (dw <= 0 || dh <= 0) {
        return NULL;
    }
    bytes = dw * dh * outPixelSize;
    out = (unsigned char *)malloc((size_t)bytes);
    if (out == NULL) {
        return NULL;
    }
    if (JPEG_To_RGB_decodeData2(info, (char *)out, outPixelSize,
                                left, top, right, bottom) != bytes) {
        free(out);
        return NULL;
    }
    return (char *)out;
}

void JPEG_To_RGB_free(void *info) {
    VitaJpeg *j = (VitaJpeg *)info;

    if (j == NULL) {
        return;
    }
    jpeg_reset(j);
    free(j);
}
