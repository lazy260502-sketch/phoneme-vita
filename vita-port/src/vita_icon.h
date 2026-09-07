/*
 * vita_icon.h - PNG icon decoding for the native game menu.
 *
 * MIDlet icons live inside the game jar (path from MANIFEST "MIDlet-1:
 * name, icon.png, class"). The menu needs them as 32bpp pixels matching
 * its A8B8G8R8 framebuffer (0xAABBGGRR on little-endian ARM), so the
 * decoder normalizes every PNG color type (gray / palette / RGB / RGBA,
 * 8/16-bit, interlaced) into that layout via libpng.
 */
#ifndef VITA_ICON_H
#define VITA_ICON_H

#include <stdint.h>

/* Decode a PNG image. On success returns 0 and sets *out_pixels to a
 * malloc'd w*h buffer of 0xAABBGGRR pixels (caller frees), *out_w/*out_h.
 * On failure returns -1 and leaves outputs untouched. */
int vita_icon_decode_png(const unsigned char *data, unsigned long len,
                         uint32_t **out_pixels, int *out_w, int *out_h);

#endif /* VITA_ICON_H */
