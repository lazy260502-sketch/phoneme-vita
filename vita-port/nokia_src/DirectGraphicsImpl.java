package com.nokia.mid.ui;

import javax.microedition.lcdui.Graphics;
import javax.microedition.lcdui.Image;
import javax.microedition.lcdui.game.Sprite;

/**
 * DirectGraphics on top of a plain MIDP Graphics (delegation: Graphics is a
 * concrete CLDC class and cannot implement the Nokia interface itself).
 * It draws into the SAME Graphics, so translate/clip/color state is shared
 * with the game's own MIDP calls.
 *
 * Written against J2ME-Loader's DirectGraphicsImp (Apache-2.0; itself derived
 * from MicroEmulator's Nokia API, Copyright (C) 2003 Markus Heberling, dual
 * LGPL-2.1 / Apache-2.0, used here under Apache-2.0) for behaviour and
 * argument checking, but re-implemented on this tree's CLDC/MIDP classes:
 * Loader's version needs android.graphics and a Graphics.setColorAlpha()
 * extension that do not exist here.
 *
 * Known limits (NOT verified on a real Nokia device or on hardware):
 *  - Graphics has no alpha state: setARGBColor and the fill/draw methods
 *    draw opaque, or nothing when alpha is 0. Partial alpha is not blended.
 *  - fillPolygon is a fan from point 0: correct for convex polygons only.
 *  - getPixels only works on an Image's Graphics (a screen Graphics has no
 *    readable pixel buffer here); on the screen it throws
 *    IllegalStateException.
 *  - The flip+rotate order follows Loader's table (rotate first, then flip).
 *    Loader, MicroEmulator and FreeJ2ME disagree with each other on
 *    "one flip + 90/270"; no spec text was available to settle it.
 */
final class DirectGraphicsImpl implements DirectGraphics {
    final Graphics g;
    /** The Image this Graphics draws into, if known (set by DirectUtils). */
    Image target;
    private int argb = 0xFF000000;

    DirectGraphicsImpl(Graphics g) {
        this.g = g;
    }

    /* rows: flip none / horizontal / vertical / both;
     * cols: ROTATE 0 / 90 / 180 / 270 (Nokia rotation is counter-clockwise).
     * Same table as J2ME-Loader's MANIPULATION2TRANSFORM. */
    private static final int[][] M2T = {
        { Sprite.TRANS_NONE, Sprite.TRANS_ROT270, Sprite.TRANS_ROT180,
          Sprite.TRANS_ROT90 },
        { Sprite.TRANS_MIRROR, Sprite.TRANS_MIRROR_ROT90,
          Sprite.TRANS_MIRROR_ROT180, Sprite.TRANS_MIRROR_ROT270 },
        { Sprite.TRANS_MIRROR_ROT180, Sprite.TRANS_MIRROR_ROT270,
          Sprite.TRANS_MIRROR, Sprite.TRANS_MIRROR_ROT90 },
        { Sprite.TRANS_ROT180, Sprite.TRANS_ROT90, Sprite.TRANS_NONE,
          Sprite.TRANS_ROT270 }
    };

    static int toTransform(int manipulation) {
        int flip = manipulation >>> 13;
        if (flip > 3) {
            throw new IllegalArgumentException("manipulation " + manipulation);
        }
        int rotation = manipulation & 0x1FFF;
        int idx = rotation / 90;
        if (rotation - idx * 90 != 0 || idx > 3) {
            throw new IllegalArgumentException("manipulation " + manipulation);
        }
        return M2T[flip][idx];
    }

    public void setARGBColor(int argbColor) {
        argb = argbColor;
        g.setColor(argbColor & 0x00FFFFFF);
    }

    public int getAlphaComponent() {
        return (argb >>> 24) & 0xFF;
    }

    public int getNativePixelFormat() {
        return TYPE_USHORT_565_RGB;
    }

    public void drawImage(Image img, int x, int y, int anchor,
                          int manipulation) {
        if (img == null) {
            throw new NullPointerException();
        }
        if ((anchor & -64) != 0) {
            throw new IllegalArgumentException("anchor " + anchor);
        }
        int transform = toTransform(manipulation);
        /* Nokia games pass anchor 0 meaning the default; MIDP drawRegion
         * rejects 0 with IllegalArgumentException. */
        if (anchor == 0) {
            anchor = Graphics.TOP | Graphics.LEFT;
        }
        g.drawRegion(img, 0, 0, img.getWidth(), img.getHeight(), transform,
                     x, y, anchor);
    }

    /* ---------------------------------------------------------- drawPixels */

    private void blit(int[] colors, int width, int height, boolean alpha,
                      int manipulation, int x, int y) {
        int transform = toTransform(manipulation);
        Image im = Image.createRGBImage(colors, width, height, alpha);
        g.drawRegion(im, 0, 0, width, height, transform, x, y,
                     Graphics.TOP | Graphics.LEFT);
    }

    /** 1-bit pixel: set bit = black, clear = white (Nokia convention). */
    private static int bit1(byte[] pixels, byte[] mask, int idx, int shift) {
        int p = (((pixels[idx] >> shift) & 1) ^ 1) * 0x00FFFFFF;
        if (mask == null) {
            return 0xFF000000 | p;
        }
        return (((mask[idx] >> shift) & 1) != 0 ? 0xFF000000 : 0) | p;
    }

    public void drawPixels(byte[] pixels, byte[] transparencyMask, int offset,
                           int scanlength, int x, int y, int width,
                           int height, int manipulation, int format) {
        if (pixels == null) {
            throw new NullPointerException();
        }
        if (width < 0 || height < 0 || scanlength < width) {
            throw new IllegalArgumentException();
        }
        if (offset < 0) {
            throw new ArrayIndexOutOfBoundsException();
        }
        if (width == 0 || height == 0) {
            return;
        }
        int[] colors = new int[width * height];
        switch (format) {
            case TYPE_BYTE_1_GRAY: {
                int space = scanlength - width;
                int di = 0;
                for (int yi = 0; yi < height; yi++, offset += space) {
                    for (int xi = 0; xi < width; xi++, offset++) {
                        colors[di++] = bit1(pixels, transparencyMask,
                                            offset >> 3, 7 - (offset & 7));
                    }
                }
                break;
            }
            case TYPE_BYTE_1_GRAY_VERTICAL: {
                int ods = offset / scanlength;
                int oms = offset % scanlength;
                int shift = ods & 7;
                int di = 0;
                for (int yi = 0; yi < height; yi++) {
                    int idx = ((ods + yi) >> 3) * scanlength + oms;
                    for (int xi = 0; xi < width; xi++) {
                        colors[di++] = bit1(pixels, transparencyMask, idx++,
                                            shift);
                    }
                    shift = (shift + 1) & 7;
                }
                break;
            }
            case TYPE_BYTE_2_GRAY:
            case TYPE_BYTE_4_GRAY:
            case TYPE_BYTE_8_GRAY:
            case TYPE_BYTE_332_RGB:
                throw new IllegalArgumentException("Unsupported format: "
                                                   + format);
            default:
                throw new IllegalArgumentException("Illegal format: "
                                                   + format);
        }
        blit(colors, width, height, transparencyMask != null, manipulation,
             x, y);
    }

    public void drawPixels(int[] pixels, boolean transparency, int offset,
                           int scanlength, int x, int y, int width,
                           int height, int manipulation, int format) {
        if (pixels == null) {
            throw new NullPointerException();
        }
        if (format != TYPE_INT_888_RGB && format != TYPE_INT_8888_ARGB) {
            throw new IllegalArgumentException("Illegal format: " + format);
        }
        if (width < 0 || height < 0 || scanlength < width) {
            throw new IllegalArgumentException();
        }
        if (offset < 0) {
            throw new ArrayIndexOutOfBoundsException();
        }
        if (width == 0 || height == 0) {
            return;
        }
        int[] colors = new int[width * height];
        int space = scanlength - width;
        int di = 0;
        for (int yi = 0; yi < height; yi++, offset += space) {
            for (int xi = 0; xi < width; xi++) {
                colors[di++] = pixels[offset++];
            }
        }
        blit(colors, width, height,
             format != TYPE_INT_888_RGB && transparency, manipulation, x, y);
    }

    public void drawPixels(short[] pixels, boolean transparency, int offset,
                           int scanlength, int x, int y, int width,
                           int height, int manipulation, int format) {
        if (pixels == null) {
            throw new NullPointerException();
        }
        if (width < 0 || height < 0 || scanlength < width) {
            throw new IllegalArgumentException();
        }
        if (offset < 0) {
            throw new ArrayIndexOutOfBoundsException();
        }
        if (width == 0 || height == 0) {
            return;
        }
        int[] colors = new int[width * height];
        int space = scanlength - width;
        int di = 0;
        switch (format) {
            case TYPE_USHORT_4444_ARGB:
                for (int yi = 0; yi < height; yi++, offset += space) {
                    for (int xi = 0; xi < width; xi++) {
                        int s = pixels[offset++] & 0xFFFF;
                        int a = (s & 0xF000) << 12;
                        int r = (s & 0x0F00) << 8;
                        int gg = (s & 0x00F0) << 4;
                        int b = s & 0x000F;
                        int v = a | r | gg | b;
                        colors[di++] = v | (v << 4);
                    }
                }
                break;
            case TYPE_USHORT_444_RGB:
                for (int yi = 0; yi < height; yi++, offset += space) {
                    for (int xi = 0; xi < width; xi++) {
                        int s = pixels[offset++] & 0xFFFF;
                        int rgb = ((s & 0x0F00) << 8) | ((s & 0x00F0) << 4)
                                  | (s & 0x000F);
                        colors[di++] = 0xFF000000 | rgb | (rgb << 4);
                    }
                }
                break;
            case TYPE_USHORT_565_RGB:
                for (int yi = 0; yi < height; yi++, offset += space) {
                    for (int xi = 0; xi < width; xi++) {
                        int s = pixels[offset++] & 0xFFFF;
                        int r = ((s & 0xF800) << 8) | ((s & 0xE000) << 3);
                        int gg = ((s & 0x07E0) << 5) | ((s & 0x0600) >> 1);
                        int b = ((s & 0x001F) << 3) | ((s & 0x001C) >> 2);
                        colors[di++] = 0xFF000000 | r | gg | b;
                    }
                }
                break;
            case TYPE_USHORT_555_RGB:
            case TYPE_USHORT_1555_ARGB:
                throw new IllegalArgumentException("Unsupported format: "
                                                   + format);
            default:
                throw new IllegalArgumentException("Illegal format: "
                                                   + format);
        }
        /* 565/444 carry 0xFF alpha so they stay opaque; 4444 has its own. */
        blit(colors, width, height, true, manipulation, x, y);
    }

    /* ----------------------------------------------------------- getPixels */

    /** Read ARGB from the target Image (the only readable surface). */
    private void readArgb(int[] out, int x, int y, int w, int h) {
        if (target == null) {
            throw new IllegalStateException(
                "getPixels: Graphics is not backed by a readable Image");
        }
        int iw = target.getWidth();
        int ih = target.getHeight();
        /* Nokia coordinates are in the translated system */
        x += g.getTranslateX();
        y += g.getTranslateY();
        int cw = Math.min(w, iw - x);
        int ch = Math.min(h, ih - y);
        if (cw <= 0 || ch <= 0) {
            return;
        }
        int[] row = new int[cw];
        for (int yy = 0; yy < ch; yy++) {
            target.getRGB(row, 0, cw, x, y + yy, cw, 1);
            System.arraycopy(row, 0, out, yy * w, cw);
        }
    }

    public void getPixels(int[] pixels, int offset, int scanlength, int x,
                          int y, int width, int height, int format) {
        if (pixels == null) {
            throw new NullPointerException();
        }
        if (x < 0 || y < 0 || width < 0 || height < 0 || scanlength < width) {
            throw new IllegalArgumentException();
        }
        if (offset < 0 || offset + width + (height - 1) * scanlength
                > pixels.length) {
            throw new IllegalArgumentException();
        }
        if (format != TYPE_INT_888_RGB && format != TYPE_INT_8888_ARGB) {
            throw new IllegalArgumentException("Illegal format: " + format);
        }
        if (width == 0 || height == 0) {
            return;
        }
        int[] tmp = new int[width * height];
        readArgb(tmp, x, y, width, height);
        int si = 0;
        for (int yi = 0; yi < height; yi++, offset += scanlength) {
            for (int xi = 0; xi < width; xi++) {
                int c = tmp[si++];
                pixels[offset + xi] =
                    format == TYPE_INT_888_RGB ? (c & 0xFFFFFF) : c;
            }
        }
    }

    public void getPixels(short[] pixels, int offset, int scanlength, int x,
                          int y, int width, int height, int format) {
        if (pixels == null) {
            throw new NullPointerException();
        }
        if (x < 0 || y < 0 || width < 0 || height < 0 || scanlength < width) {
            throw new IllegalArgumentException();
        }
        if (offset < 0 || offset + width + (height - 1) * scanlength
                > pixels.length) {
            throw new IllegalArgumentException();
        }
        if (width == 0 || height == 0) {
            return;
        }
        if (format != TYPE_USHORT_4444_ARGB && format != TYPE_USHORT_444_RGB
                && format != TYPE_USHORT_565_RGB) {
            throw new IllegalArgumentException("Unsupported format: "
                                               + format);
        }
        int[] tmp = new int[width * height];
        readArgb(tmp, x, y, width, height);
        int si = 0;
        for (int yi = 0; yi < height; yi++, offset += scanlength) {
            for (int xi = 0; xi < width; xi++) {
                int c = tmp[si++];
                int v;
                switch (format) {
                    case TYPE_USHORT_4444_ARGB:
                        v = ((c >> 16) & 0xF000) | ((c >> 12) & 0x0F00)
                            | ((c >> 8) & 0x00F0) | ((c >> 4) & 0x000F);
                        break;
                    case TYPE_USHORT_444_RGB:
                        v = ((c >> 12) & 0x0F00) | ((c >> 8) & 0x00F0)
                            | ((c >> 4) & 0x000F);
                        break;
                    default:
                        v = ((c >> 8) & 0xF800) | ((c >> 5) & 0x07E0)
                            | ((c >> 3) & 0x001F);
                        break;
                }
                pixels[offset + xi] = (short) v;
            }
        }
    }

    public void getPixels(byte[] pixels, byte[] transparencyMask, int offset,
                          int scanlength, int x, int y, int width,
                          int height, int format) {
        if (pixels == null) {
            throw new NullPointerException();
        }
        if (x < 0 || y < 0 || width < 0 || height < 0 || scanlength < width) {
            throw new IllegalArgumentException();
        }
        if (offset < 0) {
            throw new ArrayIndexOutOfBoundsException();
        }
        if (width == 0 || height == 0) {
            return;
        }
        if (format != TYPE_BYTE_1_GRAY && format != TYPE_BYTE_1_GRAY_VERTICAL) {
            throw new IllegalArgumentException("Unsupported format: "
                                               + format);
        }
        int[] tmp = new int[width * height];
        readArgb(tmp, x, y, width, height);
        int si = 0;
        if (format == TYPE_BYTE_1_GRAY) {
            int bits = offset + width + (height - 1) * scanlength;
            if (bits > (pixels.length << 3) || (transparencyMask != null
                    && bits > (transparencyMask.length << 3))) {
                throw new ArrayIndexOutOfBoundsException();
            }
            int space = scanlength - width;
            for (int yi = 0; yi < height; yi++, offset += space) {
                for (int xi = 0; xi < width; xi++, offset++) {
                    setBit(pixels, transparencyMask, offset >> 3,
                           7 - (offset & 7), tmp[si++]);
                }
            }
        } else {
            int ods = offset / scanlength;
            int oms = offset % scanlength;
            int shift = ods & 7;
            int maxIndex = ((ods + height - 1) >> 3) * scanlength + oms;
            if (maxIndex >= pixels.length || (transparencyMask != null
                    && maxIndex >= transparencyMask.length)) {
                throw new ArrayIndexOutOfBoundsException();
            }
            for (int yi = 0; yi < height; yi++) {
                int idx = ((ods + yi) >> 3) * scanlength + oms;
                for (int xi = 0; xi < width; xi++) {
                    setBit(pixels, transparencyMask, idx++, shift, tmp[si++]);
                }
                shift = (shift + 1) & 7;
            }
        }
    }

    /** Inverse of bit1(): an opaque dark pixel becomes a set bit. */
    private static void setBit(byte[] pixels, byte[] mask, int idx, int shift,
                               int color) {
        int a = color >>> 31;
        int r = (color >> 16) & 0xFF;
        int gg = (color >> 8) & 0xFF;
        int b = color & 0xFF;
        /* luminance (BT.601, 16.16 fixed point): bright pixel -> bit clear */
        int lum = (0x4CB2 * r + 0x9691 * gg + 0x1D3E * b) >> 23;
        int bit = (lum ^ 1) & a;
        if (bit == 1) {
            pixels[idx] |= (byte) (1 << shift);
        } else {
            pixels[idx] &= (byte) ~(1 << shift);
        }
        if (mask != null) {
            if (a == 1) {
                mask[idx] |= (byte) (1 << shift);
            } else {
                mask[idx] &= (byte) ~(1 << shift);
            }
        }
    }

    /* ----------------------------------------------------- triangles etc. */

    private static void checkPoly(int[] xPoints, int xOffset, int[] yPoints,
                                  int yOffset, int nPoints) {
        if (xPoints == null || yPoints == null) {
            throw new NullPointerException();
        }
        if (nPoints < 0 || xOffset < 0 || yOffset < 0
                || xOffset + nPoints > xPoints.length
                || yOffset + nPoints > yPoints.length) {
            throw new IllegalArgumentException();
        }
    }

    public void fillTriangle(int x1, int y1, int x2, int y2, int x3, int y3,
                             int argbColor) {
        if ((argbColor >>> 24) == 0) {
            argb = argbColor;
            return;
        }
        int saved = g.getColor();
        setARGBColor(argbColor);
        g.fillTriangle(x1, y1, x2, y2, x3, y3);
        g.setColor(saved);
    }

    public void drawTriangle(int x1, int y1, int x2, int y2, int x3, int y3,
                             int argbColor) {
        if ((argbColor >>> 24) == 0) {
            argb = argbColor;
            return;
        }
        int saved = g.getColor();
        setARGBColor(argbColor);
        g.drawLine(x1, y1, x2, y2);
        g.drawLine(x2, y2, x3, y3);
        g.drawLine(x3, y3, x1, y1);
        g.setColor(saved);
    }

    public void fillPolygon(int[] xPoints, int xOffset, int[] yPoints,
                            int yOffset, int nPoints, int argbColor) {
        checkPoly(xPoints, xOffset, yPoints, yOffset, nPoints);
        if (nPoints < 3 || (argbColor >>> 24) == 0) {
            argb = argbColor;
            return;
        }
        int saved = g.getColor();
        setARGBColor(argbColor);
        int x0 = xPoints[xOffset];
        int y0 = yPoints[yOffset];
        for (int i = 1; i < nPoints - 1; i++) {
            g.fillTriangle(x0, y0,
                           xPoints[xOffset + i], yPoints[yOffset + i],
                           xPoints[xOffset + i + 1], yPoints[yOffset + i + 1]);
        }
        g.setColor(saved);
    }

    public void drawPolygon(int[] xPoints, int xOffset, int[] yPoints,
                            int yOffset, int nPoints, int argbColor) {
        checkPoly(xPoints, xOffset, yPoints, yOffset, nPoints);
        if (nPoints < 2 || (argbColor >>> 24) == 0) {
            argb = argbColor;
            return;
        }
        int saved = g.getColor();
        setARGBColor(argbColor);
        for (int i = 0; i < nPoints; i++) {
            int j = (i + 1) % nPoints;
            g.drawLine(xPoints[xOffset + i], yPoints[yOffset + i],
                       xPoints[xOffset + j], yPoints[yOffset + j]);
        }
        g.setColor(saved);
    }
}
