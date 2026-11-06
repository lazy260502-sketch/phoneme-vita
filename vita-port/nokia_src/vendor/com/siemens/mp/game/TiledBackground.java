package com.siemens.mp.game;

import javax.microedition.lcdui.Graphics;
import javax.microedition.lcdui.Image;

/**
 * Siemens API game.TiledBackground (8x8 tile map). Behaviour follows
 * J2ME-Loader's class (Apache-2.0 / MicroEmulator Siemens API, Copyright (C)
 * 2003 Markus Heberling, used under Apache-2.0). Tiles 0 (transparent),
 * 1 (white) and 2 (black) are built in; tile n+3 is row n of the tile image.
 * Loader's non-standard Image.createImage(w,h,argb) is replaced by
 * createRGBImage.
 */
public class TiledBackground extends GraphicObject {
    private Image[] pixels;
    private byte[] map;
    private int widthInTiles;
    private int heightInTiles;
    private int posx;
    private int posy;

    public TiledBackground(byte[] tilePixels, byte[] tileMask, byte[] map,
                           int widthInTiles, int heightInTiles) {
        this(com.siemens.mp.ui.Image.createImageFromBitmap(
                 tilePixels, tileMask, 8, tilePixels.length),
             null, map, widthInTiles, heightInTiles);
    }

    public TiledBackground(ExtendedImage tilePixels, ExtendedImage tileMask,
                           byte[] map, int widthInTiles, int heightInTiles) {
        this(tilePixels.getImage(),
             tileMask == null ? null : tileMask.getImage(),
             map, widthInTiles, heightInTiles);
    }

    public TiledBackground(Image tilePixels, Image tileMask, byte[] map,
                           int widthInTiles, int heightInTiles) {
        this.map = map;
        this.heightInTiles = heightInTiles;
        this.widthInTiles = widthInTiles;
        if (tileMask != null) {
            tilePixels = com.siemens.mp.ui.Image.createTransparentImageFromMask(
                tilePixels, tileMask);
        }
        int tiles = tilePixels.getHeight() / 8;
        pixels = new Image[tiles + 3];
        pixels[0] = solid(0x00000000);   /* transparent */
        pixels[1] = solid(0xFFFFFFFF);   /* white */
        pixels[2] = solid(0xFF000000);   /* black */
        int w = tilePixels.getWidth();
        int[] buf = new int[w * 8];
        for (int i = 0; i < tiles; i++) {
            tilePixels.getRGB(buf, 0, w, 0, i * 8, w, 8);
            pixels[i + 3] = Image.createRGBImage(buf, w, 8, true);
        }
    }

    private static Image solid(int argb) {
        int[] p = new int[64];
        for (int i = 0; i < p.length; i++) {
            p[i] = argb;
        }
        return Image.createRGBImage(p, 8, 8, true);
    }

    public void setPositionInMap(int x, int y) {
        posx = x;
        posy = y;
    }

    protected void paint(Graphics g) {
        for (int y = posy / 8; y < heightInTiles; y++) {
            for (int x = posx / 8; x < widthInTiles; x++) {
                int t = map[y * widthInTiles + x] & 0xFF;
                if (t >= pixels.length) {
                    continue;   /* map refers to a tile the image lacks */
                }
                g.drawImage(pixels[t], -posx + x * 8, -posy + y * 8,
                            Graphics.TOP | Graphics.LEFT);
            }
        }
    }

    void paintTo(Graphics g) {
        paint(g);
    }
}
