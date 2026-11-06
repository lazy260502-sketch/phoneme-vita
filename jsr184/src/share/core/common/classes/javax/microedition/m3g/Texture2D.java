package javax.microedition.m3g;

/**
 * Texture mapping parameters. The backend implements the common
 * "modulate with vertex colour, bilinear/nearest filter, repeat/border"
 * case; the rest of the state is validated and stored.
 */
public class Texture2D extends Transformable {

    public static final int FILTER_BASE_LEVEL = 208;
    public static final int FILTER_NEAREST = 209;
    public static final int FILTER_LINEAR = 210;

    public static final int FUNC_ADD = 224;
    public static final int FUNC_SUBTRACT = 225;
    public static final int FUNC_MODULATE = 226;
    public static final int FUNC_REPLACE = 227;
    /* v03.29: JSR-184 also defines FUNC_BLEND = 228 (blendColour * texture).
     * Without it setBlending(228) threw IllegalArgumentException; log134 shows
     * the CAPCOM title calling exactly that
     * (BioCtrl ... sipush 228; invokevirtual Texture2D.setBlending). */
    public static final int FUNC_BLEND = 228;

    public static final int WRAP_CLAMP = 240;
    public static final int WRAP_REPEAT = 241;

    Image2D image = null;
    int blendColor = 0x00000000;
    int blendMode = FUNC_MODULATE;
    int wrapS = WRAP_REPEAT;
    int wrapT = WRAP_REPEAT;
    int levelFilter = FILTER_BASE_LEVEL;
    int imageFilter = FILTER_NEAREST;   /* M3G: new Texture2D(image) defaults to FILTER_NEAREST; M3gReader overwrites it from the file */

    Texture2D(boolean placeholder) {
    }

    public Texture2D(Image2D image) {
        if (image == null) {
            throw com.sun.m3g.M3gTrace.fail(new NullPointerException());
        }
        this.image = image;
    }

    public void setImage(Image2D img) {
        if (img == null) {
            throw com.sun.m3g.M3gTrace.fail(new NullPointerException());
        }
        image = img;
    }

    void collectReferences(java.util.Vector out) {
        if (image != null) out.addElement(image);
    }

    public Image2D getImage() {
        return image;
    }

    public void setBlendColor(int rgb) {
        blendColor = rgb & 0x00FFFFFF;
    }

    public int getBlendColor() {
        return blendColor;
    }

    public void setBlending(int func) {
        if (func < FUNC_ADD || func > FUNC_BLEND) {
            throw com.sun.m3g.M3gTrace.fail(new IllegalArgumentException("bad blending"));
        }
        blendMode = func;
    }

    public int getBlending() {
        return blendMode;
    }

    public void setWrapping(int wrapS, int wrapT) {
        if ((wrapS != WRAP_CLAMP && wrapS != WRAP_REPEAT)
                || (wrapT != WRAP_CLAMP && wrapT != WRAP_REPEAT)) {
            throw com.sun.m3g.M3gTrace.fail(new IllegalArgumentException("bad wrapping"));
        }
        this.wrapS = wrapS;
        this.wrapT = wrapT;
    }

    public int getWrappingS() {
        return wrapS;
    }

    public int getWrappingT() {
        return wrapT;
    }

    public void setFiltering(int levelFilter, int imageFilter) {
        if (levelFilter < FILTER_BASE_LEVEL || levelFilter > FILTER_LINEAR
                || imageFilter < FILTER_NEAREST || imageFilter > FILTER_LINEAR) {
            throw com.sun.m3g.M3gTrace.fail(new IllegalArgumentException("bad filtering"));
        }
        this.levelFilter = levelFilter;
        this.imageFilter = imageFilter;
    }

    public int getLevelFilter() {
        return levelFilter;
    }

    public int getImageFilter() {
        return imageFilter;
    }
}
