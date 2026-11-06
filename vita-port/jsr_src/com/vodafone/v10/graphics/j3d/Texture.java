/*
 * Texture.java - VSCL 1.0 (see Graphics3D.java for the scope note).
 *
 * The image is kept so the game's setTexture() call succeeds; the placeholder
 * renderer does not sample it. "sphere map" textures carry an index table.
 */
package com.vodafone.v10.graphics.j3d;

import javax.microedition.lcdui.Image;

public class Texture {

    private final Image image;
    private final int[] sphereMapIndex;
    private final boolean forModel;

    public Texture(Image image, boolean isForModel) {
        this(image, null, isForModel);
    }

    public Texture(Image image, int[] sphereMapIndex, boolean isForModel) {
        if (image == null) {
            throw new NullPointerException("image");
        }
        this.image = image;
        this.sphereMapIndex = sphereMapIndex;
        this.forModel = isForModel;
    }

    public boolean isSphereMap() {
        return sphereMapIndex != null;
    }

    public boolean isForModel() {
        return forModel;
    }

    public Image getImage() {
        return image;
    }
}
