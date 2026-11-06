package javax.microedition.m3g;

/**
 * Blending and depth-buffer state. Mapped onto the backend's
 * v3d_set_state(cull, blend) plus the depth flags, which the backends
 * apply per draw.
 */
public class CompositingMode extends Object3D {

    public static final int ALPHA = 64;
    public static final int ALPHA_ADD = 65;
    public static final int MODULATE = 66;
    public static final int MODULATE_X2 = 67;
    public static final int REPLACE = 68;

    int blending = REPLACE;
    boolean depthTest = true;
    boolean depthWrite = true;
    float alphaThreshold = 0.0f;
    int blendColor = 0;
    boolean colorWrite = true;

    public void setBlending(int mode) {
        if (mode < ALPHA || mode > REPLACE) {
            throw com.sun.m3g.M3gTrace.fail(new IllegalArgumentException("bad blending mode"));
        }
        blending = mode;
    }

    public int getBlending() {
        return blending;
    }

    public void setAlphaThreshold(float threshold) {
        if (threshold < 0.0f || threshold > 1.0f) {
            throw com.sun.m3g.M3gTrace.fail(new IllegalArgumentException("threshold out of range"));
        }
        alphaThreshold = threshold;
    }

    public float getAlphaThreshold() {
        return alphaThreshold;
    }

    public void setBlendColor(int rgb) {
        blendColor = rgb & 0x00FFFFFF;
    }

    public int getBlendColor() {
        return blendColor;
    }

    public void setDepthTestEnable(boolean enable) {
        depthTest = enable;
    }

    public boolean isDepthTestEnabled() {
        return depthTest;
    }

    public void setDepthWriteEnable(boolean enable) {
        depthWrite = enable;
    }

    public boolean isDepthWriteEnabled() {
        return depthWrite;
    }

    public void setColorWriteEnable(boolean enable) {
        colorWrite = enable;
    }

    public boolean isColorWriteEnabled() {
        return colorWrite;
    }
}
