package javax.microedition.m3g;

import com.sun.m3g.NativeBridge;

/**
 * 2D image usable as a texture or a background.
 *
 * Pixels are kept as 0xAARRGGBB ints (the Java layout) and the backend
 * texture is created lazily, so an Image2D that is only ever a render
 * target never allocates a GPU resource.
 *
 * Constructors follow JSR 184 exactly:
 *   Image2D(int format, int width, int height)            mutable, blank
 *   Image2D(int format, int width, int height, byte[] px) packed
 *   Image2D(int format, Object image)                     from a MIDP Image
 */
public class Image2D extends Object3D {

    public static final int ALPHA = 96;
    public static final int LUMINANCE = 97;
    public static final int LUMINANCE_ALPHA = 98;
    public static final int RGB = 99;
    public static final int RGBA = 100;

    int format;
    int width;
    int height;
    int[] pixels;
    boolean mutable;

    private int textureHandle = 0;
    boolean textureDirty = true;

    Image2D() {
    }

    public Image2D(int format, int width, int height) {
        checkFormat(format);
        if (width <= 0 || height <= 0) {
            throw com.sun.m3g.M3gTrace.fail(new IllegalArgumentException("bad size"));
        }
        this.format = format;
        this.width = width;
        this.height = height;
        this.pixels = new int[width * height];
        this.mutable = true;
    }

    public Image2D(int format, int width, int height, byte[] image) {
        this(format, width, height);
        if (image == null) {
            throw com.sun.m3g.M3gTrace.fail(new NullPointerException());
        }
        int per = bytesPerPixel(format);
        if (image.length < width * height * per) {
            throw com.sun.m3g.M3gTrace.fail(new IllegalArgumentException("pixel buffer too small"));
        }
        unpack(image, 0, 0, width, height, 0, 0, per);
    }

    public Image2D(int format, Object image) {
        this(format, widthOf(image), heightOf(image));
        javax.microedition.lcdui.Image img =
            (javax.microedition.lcdui.Image)image;
        img.getRGB(pixels, 0, this.width, 0, 0, this.width, this.height);
        textureDirty = true;
    }

    private static void checkFormat(int format) {
        if (format < ALPHA || format > RGBA) {
            throw com.sun.m3g.M3gTrace.fail(new IllegalArgumentException("bad format"));
        }
    }

    private static int widthOf(Object image) {
        if (image == null) {
            throw com.sun.m3g.M3gTrace.fail(new NullPointerException());
        }
        return ((javax.microedition.lcdui.Image)image).getWidth();
    }

    private static int heightOf(Object image) {
        if (image == null) {
            throw com.sun.m3g.M3gTrace.fail(new NullPointerException());
        }
        return ((javax.microedition.lcdui.Image)image).getHeight();
    }

    static int bytesPerPixel(int format) {
        switch (format) {
        case ALPHA:
        case LUMINANCE: return 1;
        case LUMINANCE_ALPHA: return 2;
        case RGB: return 3;
        default: return 4;
        }
    }

    private void unpack(byte[] src, int sx, int sy, int w, int h,
                        int dx, int dy, int per) {
        int o = 0;
        for (int row = 0; row < h; row++) {
            for (int col = 0; col < w; col++, o += per) {
                int a, r, g, b;
                switch (format) {
                case ALPHA:
                    a = src[o] & 0xFF; r = g = b = 0xFF; break;
                case LUMINANCE:
                    r = g = b = src[o] & 0xFF; a = 0xFF; break;
                case LUMINANCE_ALPHA:
                    r = g = b = src[o] & 0xFF; a = src[o + 1] & 0xFF; break;
                case RGB:
                    r = src[o] & 0xFF; g = src[o + 1] & 0xFF;
                    b = src[o + 2] & 0xFF; a = 0xFF; break;
                default:
                    r = src[o] & 0xFF; g = src[o + 1] & 0xFF;
                    b = src[o + 2] & 0xFF; a = src[o + 3] & 0xFF; break;
                }
                pixels[(dy + row) * this.width + (dx + col)] =
                    (a << 24) | (r << 16) | (g << 8) | b;
            }
        }
        textureDirty = true;
    }

    public int getFormat() {
        return format;
    }

    public int getWidth() {
        return width;
    }

    public int getHeight() {
        return height;
    }

    public boolean isMutable() {
        return mutable;
    }

    public void set(int x, int y, int width, int height, byte[] image) {
        if (!mutable) {
            throw com.sun.m3g.M3gTrace.fail(new IllegalStateException("immutable image"));
        }
        if (image == null) {
            throw com.sun.m3g.M3gTrace.fail(new NullPointerException());
        }
        if (x < 0 || y < 0 || x + width > this.width
                || y + height > this.height) {
            throw com.sun.m3g.M3gTrace.fail(new IllegalArgumentException("out of bounds"));
        }
        unpack(image, 0, 0, width, height, x, y, bytesPerPixel(format));
    }

    public void get(int x, int y, int width, int height, byte[] out) {
        if (out == null) {
            throw com.sun.m3g.M3gTrace.fail(new NullPointerException());
        }
        if (x < 0 || y < 0 || x + width > this.width
                || y + height > this.height) {
            throw com.sun.m3g.M3gTrace.fail(new IllegalArgumentException("out of bounds"));
        }
        int per = bytesPerPixel(format);
        int o = 0;
        for (int row = 0; row < height; row++) {
            for (int col = 0; col < width; col++, o += per) {
                int p = pixels[(y + row) * this.width + (x + col)];
                int r = (p >> 16) & 0xFF, g = (p >> 8) & 0xFF, b = p & 0xFF;
                int a = (p >> 24) & 0xFF;
                switch (format) {
                case ALPHA: out[o] = (byte)a; break;
                case LUMINANCE: out[o] = (byte)r; break;
                case LUMINANCE_ALPHA:
                    out[o] = (byte)r; out[o + 1] = (byte)a; break;
                case RGB:
                    out[o] = (byte)r; out[o + 1] = (byte)g;
                    out[o + 2] = (byte)b; break;
                default:
                    out[o] = (byte)r; out[o + 1] = (byte)g;
                    out[o + 2] = (byte)b; out[o + 3] = (byte)a; break;
                }
            }
        }
    }

    /** Texture handle for the backend, created on first use. */
    int getTextureHandle() {
        if (textureDirty) {
            if (textureHandle != 0) {
                NativeBridge.textureFree(textureHandle);
            }
            textureHandle = NativeBridge.textureCreate(pixels, width, height);
            textureDirty = false;
        }
        return textureHandle;
    }

    int pixelAt(int x, int y) {
        return pixels[y * width + x];
    }
}
