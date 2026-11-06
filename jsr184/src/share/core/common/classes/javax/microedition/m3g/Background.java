package javax.microedition.m3g;

import com.sun.m3g.NativeBridge;

/**
 * Flat background: a clear colour, optionally an Image2D drawn behind the
 * scene. The image modes are accepted and stored; this port only implements
 * the straightforward "stretch the whole image over the viewport" case.
 */
public class Background extends Object3D {

    public static final int BORDER = 32;
    public static final int REPEAT = 33;

    public static final int TOP_LEFT = 0;
    public static final int BOTTOM_LEFT = 1;
    public static final int TOP_RIGHT = 2;
    public static final int BOTTOM_RIGHT = 3;

    int color = 0;
    int cropX, cropY, cropW, cropH;
    int imageModeX = REPEAT;
    int imageModeY = REPEAT;
    boolean colorClear = true;
    boolean depthClear = true;
    Image2D image = null;

    public void setColor(int argb) {
        color = argb;
        colorClear = true;
    }

    public int getColor() {
        return color;
    }

    public void setColorClearEnable(boolean enable) {
        colorClear = enable;
    }

    public boolean isColorClearEnabled() {
        return colorClear;
    }

    public void setDepthClearEnable(boolean enable) {
        depthClear = enable;
    }

    public boolean isDepthClearEnabled() {
        return depthClear;
    }

    public void setImage(Image2D img) {
        image = img;
    }

    public Image2D getImage() {
        return image;
    }

    public void setImageMode(int modeX, int modeY) {
        if ((modeX != BORDER && modeX != REPEAT)
                || (modeY != BORDER && modeY != REPEAT)) {
            throw com.sun.m3g.M3gTrace.fail(new IllegalArgumentException("bad image mode"));
        }
        imageModeX = modeX;
        imageModeY = modeY;
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
    }

    boolean hasImage() {
        return image != null;
    }
}
