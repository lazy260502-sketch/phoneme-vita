package javax.microedition.m3g;

/**
 * Linear fog. Parameters are stored for Renderer's per-vertex fog factor;
 * the backends modulate the vertex colour with it.
 */
public class Fog extends Object3D {

    public static final int EXPONENTIAL = 80;
    public static final int LINEAR = 81;

    int mode = LINEAR;
    int color = 0x00000000;
    float near = 1.0f;
    float far = 100.0f;
    float density = 1.0f;

    public int getMode() {
        return mode;
    }

    public void setMode(int m) {
        if (m != EXPONENTIAL && m != LINEAR) {
            throw com.sun.m3g.M3gTrace.fail(new IllegalArgumentException("bad fog mode"));
        }
        mode = m;
    }

    public int getColor() {
        return color;
    }

    public void setColor(int rgb) {
        color = rgb & 0x00FFFFFF;
    }

    public float getNearDistance() {
        return near;
    }

    public float getFarDistance() {
        return far;
    }

    public void setLinear(float near, float far) {
        if (near < 0.0f || far <= near) {
            throw com.sun.m3g.M3gTrace.fail(new IllegalArgumentException("bad fog range"));
        }
        mode = LINEAR;
        this.near = near;
        this.far = far;
    }

    public float getDensity() {
        return density;
    }

    public void setDensity(float density) {
        if (density < 0.0f) {
            throw com.sun.m3g.M3gTrace.fail(new IllegalArgumentException("negative density"));
        }
        mode = EXPONENTIAL;
        this.density = density;
    }
}
