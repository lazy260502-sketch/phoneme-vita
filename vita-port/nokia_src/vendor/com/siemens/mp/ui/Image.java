package com.siemens.mp.ui;

import java.io.IOException;

/**
 * Siemens API ui.Image: builds lcdui Images from the phone's bitmap format
 * (1 bit per pixel, MSB first within each byte, set bit = black) with an
 * optional 1-bit mask. Behaviour follows J2ME-Loader's class (Apache-2.0 /
 * MicroEmulator Siemens API, Copyright (C) 2003 Markus Heberling, used under
 * Apache-2.0); its non-standard Image.setBlackWhiteAlpha() hint is dropped
 * (this MIDP has no such method) - the pixels already carry their alpha.
 */
public class Image extends com.siemens.mp.misc.NativeMem {
    public static javax.microedition.lcdui.Image createImageFromBitmap(
            byte[] imageData, int imageWidth, int imageHeight) {
        return createImageFromBitmap(imageData, null, imageWidth, imageHeight);
    }

    public static javax.microedition.lcdui.Image createImageFromBitmap(
            byte[] imageData, byte[] alpha, int imageWidth, int imageHeight) {
        if (imageData == null) {
            return null;
        }
        if (imageWidth < 8) {
            imageWidth = 8;
        }
        int[] pix = new int[imageHeight * imageWidth];
        for (int y = 0; y < imageHeight; y++) {
            for (int x = 0; x < imageWidth / 8; x++) {
                for (int b = 0; b < 8; b++) {
                    pix[x * 8 + 7 - b + y * imageWidth] =
                        doAlpha(imageData, alpha, y * imageWidth / 8 + x, b);
                }
            }
        }
        return javax.microedition.lcdui.Image.createRGBImage(
            pix, imageWidth, imageHeight, true);
    }

    public static javax.microedition.lcdui.Image createImageWithoutScaling(
            String name) throws IOException {
        return javax.microedition.lcdui.Image.createImage(name);
    }

    /** 2 bits per pixel: black / transparent. */
    public static javax.microedition.lcdui.Image createTransparentImageFromBitmap(
            byte[] imageData, int imageWidth, int imageHeight) {
        if (imageData == null) {
            return null;
        }
        if (imageWidth < 4) {
            imageWidth = 4;
        }
        int[] pix = new int[imageHeight * imageWidth];
        for (int y = 0; y < imageHeight; y++) {
            for (int x = 0; x < imageWidth / 4; x++) {
                for (int b = 7; b >= 0; b -= 2) {
                    pix[x * 4 + 3 - b / 2 + y * imageWidth] =
                        doAlpha(imageData, y * imageWidth / 4 + x, b);
                }
            }
        }
        return javax.microedition.lcdui.Image.createRGBImage(
            pix, imageWidth, imageHeight, true);
    }

    /** Pixels of image where mask is opaque white become transparent. */
    public static javax.microedition.lcdui.Image createTransparentImageFromMask(
            javax.microedition.lcdui.Image image,
            javax.microedition.lcdui.Image mask) {
        int width = image.getWidth();
        int height = image.getHeight();
        int[] imagePixels = new int[width * height];
        int[] maskPixels = new int[width * height];
        image.getRGB(imagePixels, 0, width, 0, 0, width, height);
        mask.getRGB(maskPixels, 0, width, 0, 0, width, height);
        for (int i = 0; i < imagePixels.length; i++) {
            if (maskPixels[i] == 0xFFFFFFFF) {
                imagePixels[i] = 0;
            }
        }
        return javax.microedition.lcdui.Image.createRGBImage(
            imagePixels, width, height, true);
    }

    private static boolean isBitSet(byte b, int pos) {
        return pos >= 0 && ((b >> pos) & 1) != 0;
    }

    private static int doAlpha(byte[] pix, int pos, int shift) {
        int p = isBitSet(pix[pos], shift) ? 0 : 0x00FFFFFF;
        int a = (isBitSet(pix[pos], shift) || isBitSet(pix[pos], shift - 1))
                ? 0xFF000000 : 0;
        return p | a;
    }

    private static int doAlpha(byte[] pix, byte[] alpha, int pos, int shift) {
        int p = isBitSet(pix[pos], shift) ? 0 : 0x00FFFFFF;
        int a = (alpha == null || isBitSet(alpha[pos], shift))
                ? 0xFF000000 : 0;
        return p | a;
    }
}
