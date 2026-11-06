package javax.microedition.m3g;

/**
 * Light node. Per-vertex shading is computed in Renderer, so only the
 * parameters live here.
 */
public class Light extends Node {

    public static final int AMBIENT = 128;
    public static final int DIRECTIONAL = 129;
    public static final int OMNI = 130;
    /* v04.17: SPOT was missing entirely, and setMode() rejected it. That is the
     * same failure shape as the PolygonMode.WINDING bug fixed in this build: a
     * title that calls setMode(Light.SPOT) (131) got
     *     IllegalArgumentException: bad light mode
     * and, if it happened inside a periodic task, the task was cancelled and
     * the app froze with no visible error.
     * The renderer does not implement the spot cone yet (see the roadmap), so
     * a spot light is currently shaded like OMNI - a strictly better outcome
     * than a crash. The angle/exponent values are read from the file and kept
     * so the cone can be implemented without touching the loader again.
     *
     * NOTE: every one of the 43 .m3g files in debug/3d_game uses only 128 and
     * 129, which is why this never surfaced here. */
    public static final int SPOT = 131;

    int mode = AMBIENT;
    int color = 0x00FFFFFF;
    float intensity = 1.0f;
    float constant = 1.0f;
    float linear = 0.0f;
    float quadratic = 0.0f;

    public int getMode() {
        return mode;
    }

    public void setMode(int m) {
        if (m != AMBIENT && m != DIRECTIONAL && m != OMNI && m != SPOT) {
            throw com.sun.m3g.M3gTrace.fail(new IllegalArgumentException("bad light mode"));
        }
        mode = m;
    }

    public int getColor() {
        return color;
    }

    public void setColor(int rgb) {
        color = rgb & 0x00FFFFFF;
    }

    public float getIntensity() {
        return intensity;
    }

    public void setIntensity(float i) {
        intensity = i;
    }

    public void setAttenuation(float c, float l, float q) {
        if (c < 0.0f || l < 0.0f || q < 0.0f) {
            throw com.sun.m3g.M3gTrace.fail(new IllegalArgumentException("negative attenuation"));
        }
        constant = c;
        linear = l;
        quadratic = q;
    }

    public void getAttenuation(float[] out) {
        if (out == null || out.length < 3) {
            throw com.sun.m3g.M3gTrace.fail(new IllegalArgumentException());
        }
        out[0] = constant;
        out[1] = linear;
        out[2] = quadratic;
    }
}
