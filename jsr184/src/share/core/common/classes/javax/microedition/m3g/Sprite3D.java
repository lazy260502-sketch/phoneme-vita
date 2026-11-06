package javax.microedition.m3g;

/**
 * Camera-facing textured quad. Rendered by Renderer as a billboard at the
 * node position.
 */
public class Sprite3D extends Node {

    public static final int MODE_SCALED = 85;
    public static final int MODE_UNSCALED = 86;

    boolean scaled;
    int cropX, cropY, cropW, cropH;
    Image2D image;
    Appearance appearance;

    Sprite3D(boolean placeholder) {
    }

    public Sprite3D(boolean scaled, Image2D image, Appearance appearance) {
        if (image == null) {
            throw com.sun.m3g.M3gTrace.fail(new NullPointerException());
        }
        this.scaled = scaled;
        this.image = image;
        this.appearance = appearance;
    }

    public boolean isScaled() {
        return scaled;
    }

    public void setCrop(int x, int y, int width, int height) {
        if (width < 0 || height < 0) {
            throw com.sun.m3g.M3gTrace.fail(new IllegalArgumentException("negative crop"));
        }
        cropX = x;
        cropY = y;
        cropW = width;
        cropH = height;
    }

    public int getCropX() {
        return cropX;
    }

    public int getCropY() {
        return cropY;
    }

    public int getCropWidth() {
        return cropW;
    }

    public int getCropHeight() {
        return cropH;
    }

    void collectReferences(java.util.Vector out) {
        if (image != null) out.addElement(image);
        if (appearance != null) out.addElement(appearance);
    }

    public Image2D getImage() {
        return image;
    }

    public Appearance getAppearance() {
        return appearance;
    }
}
