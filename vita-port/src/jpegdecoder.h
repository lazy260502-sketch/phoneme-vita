/*
 * jpegdecoder.h - the JPEG decode entry points phoneME's image subsystem
 *                 calls (Vita port, v02.89)
 *
 * phoneME's MIDP image decoder (midp/src/lowlevelui/image_decode/...) has
 * one JPEG seam: when ENABLE_JPEG is 1, imgdcd_image_util.c includes
 * <jpegdecoder.h> and calls these five functions. Upstream ships that
 * header (and an IJG libjpeg) under phoneme/jpeg, but those 30+ GPL files
 * are only wired into phoneME's own make system, not into this port.
 *
 * Instead of compiling that tree, the port provides the same interface
 * from a single self-contained file (vita-port/src/vita_jpeg.c) built on
 * stb_image (public domain). Keeping the exact upstream prototypes means
 * no phoneME source has to change - only ENABLE_JPEG flips to 1.
 *
 * Colour order: `pixelData` in MIDP is RGB565 packed as
 *        b + (g << 5) + (r << 11)      see IMGDCD_RGB2PIXEL
 * so red lives in the high bits, blue in the low ones.
 */
#ifndef VITA_JPEGDECODER_H
#define VITA_JPEGDECODER_H

/* Allocate a decoder handle. NULL on out-of-memory. One handle can be
 * used for repeated decodes, but this port's callers create one per
 * image and free it immediately. */
void *JPEG_To_RGB_init(void);

/* Parse the header, report the image size and cache the decoded pixels.
 *
 * @param info       handle from JPEG_To_RGB_init
 * @param inData     JPEG bytes
 * @param inDataLen  byte count
 * @param width      out: image width in pixels
 * @param height     out: image height in pixels
 * @return non-zero on success, 0 on failure
 */
int JPEG_To_RGB_decodeHeader(void *info, char *inData, int inDataLen,
                             int *width, int *height);

/* Copy the cached pixels out as a 24-bit RGB image (3 bytes/pixel).
 * @return bytes written, 0 on failure. */
int JPEG_To_RGB_decodeData(void *info, char *outData);

/* Copy a rectangle of the cached image out, converted to `outPixelSize`
 * bytes per pixel (2 = RGB565, 4 = 32-bit; anything else is treated as
 * 4). `left/top/right/bottom` describe the DESTINATION rectangle in
 * pixels; when it differs from the image size the pixels are resampled
 * nearest-neighbour, which is what the MIDP Image API does for icon-sized
 * copies and costs nothing here.
 *
 * @return bytes written (rows * (right-left) * outPixelSize), 0 on error.
 */
int JPEG_To_RGB_decodeData2(void *info, char *outData, int outPixelSize,
                            int left, int top, int right, int bottom);

/* Decode in one call, allocating a 24-bit RGB buffer the caller frees
 * with free(). Sets *width / *height. NULL on failure. */
char *JPEG_To_RGB_decode(void *info, char *inData, int inDataLen,
                         int *width, int *height);

/* Same, but with a destination rectangle and pixel size (2 or 4). */
char *JPEG_To_RGB_decode2(void *info, char *inData, int inDataLen,
                          int outPixelSize, int left, int top,
                          int right, int bottom, int *width, int *height);

void JPEG_To_RGB_free(void *info);

#endif /* VITA_JPEGDECODER_H */
