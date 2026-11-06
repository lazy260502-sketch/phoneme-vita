package com.siemens.mp.color_game;

import javax.microedition.lcdui.Graphics;
import javax.microedition.lcdui.Image;

/**
 * Siemens color_game TiledLayer: wraps javax.microedition.lcdui.game.TiledLayer
 * (standard tile/animated-tile/cell handling and drawing). Position and
 * visibility live in this package's own Layer and are copied onto the wrapped
 * layer before painting. Member list follows J2ME-Loader's class (Apache-2.0).
 */
public class TiledLayer extends Layer {
    private javax.microedition.lcdui.game.TiledLayer t;

    public TiledLayer(int columns, int rows, Image image, int tileWidth,
                      int tileHeight) {
        super(columns < 1 || tileWidth < 1 ? -1 : columns * tileWidth,
              rows < 1 || tileHeight < 1 ? -1 : rows * tileHeight);
        t = new javax.microedition.lcdui.game.TiledLayer(columns, rows, image,
                                                         tileWidth,
                                                         tileHeight);
    }

    javax.microedition.lcdui.game.TiledLayer impl() {
        t.setPosition(x, y);
        t.setVisible(visible);
        return t;
    }

    public int createAnimatedTile(int staticTileIndex) {
        return t.createAnimatedTile(staticTileIndex);
    }

    public void setAnimatedTile(int animatedTileIndex, int staticTileIndex) {
        t.setAnimatedTile(animatedTileIndex, staticTileIndex);
    }

    public int getAnimatedTile(int animatedTileIndex) {
        return t.getAnimatedTile(animatedTileIndex);
    }

    public void setCell(int col, int row, int tileIndex) {
        t.setCell(col, row, tileIndex);
    }

    public int getCell(int col, int row) {
        return t.getCell(col, row);
    }

    public void fillCells(int col, int row, int numCols, int numRows,
                          int tileIndex) {
        t.fillCells(col, row, numCols, numRows, tileIndex);
    }

    public final int getCellWidth() {
        return t.getCellWidth();
    }

    public final int getCellHeight() {
        return t.getCellHeight();
    }

    public final int getColumns() {
        return t.getColumns();
    }

    public final int getRows() {
        return t.getRows();
    }

    public void setStaticTileSet(Image image, int tileWidth, int tileHeight) {
        t.setStaticTileSet(image, tileWidth, tileHeight);
        width = t.getWidth();
        height = t.getHeight();
    }

    public final void paint(Graphics g) {
        if (g == null) {
            throw new NullPointerException();
        }
        if (visible) {
            impl().paint(g);
        }
    }
}
