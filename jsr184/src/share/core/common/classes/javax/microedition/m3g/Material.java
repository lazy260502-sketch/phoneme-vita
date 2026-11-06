package javax.microedition.m3g;

/**
 * Material parameters used by Renderer's per-vertex lighting.
 */
public class Material extends Object3D {

    public static final int AMBIENT = 1024;
    public static final int DIFFUSE = 1025;
    public static final int EMISSIVE = 1026;
    public static final int SPECULAR = 1027;

    int ambient = 0x00333333;
    int diffuse = 0x00CCCCCC;
    int emissive = 0x00000000;
    int specular = 0x00000000;
    float shininess = 0.0f;
    boolean vertexColorTracking = false;

    public void setColor(int target, int argb) {
        int rgb = argb & 0x00FFFFFF;
        switch (target) {
        case AMBIENT: ambient = rgb; break;
        case DIFFUSE: diffuse = rgb; break;
        case EMISSIVE: emissive = rgb; break;
        case SPECULAR: specular = rgb; break;
        default: throw com.sun.m3g.M3gTrace.fail(new IllegalArgumentException("bad target"));
        }
    }

    public int getColor(int target) {
        switch (target) {
        case AMBIENT: return ambient;
        case DIFFUSE: return diffuse;
        case EMISSIVE: return emissive;
        case SPECULAR: return specular;
        default: throw com.sun.m3g.M3gTrace.fail(new IllegalArgumentException("bad target"));
        }
    }

    public void setShininess(float s) {
        if (s < 0.0f || s > 128.0f) {
            throw com.sun.m3g.M3gTrace.fail(new IllegalArgumentException("shininess out of range"));
        }
        shininess = s;
    }

    public float getShininess() {
        return shininess;
    }

    public void setVertexColorTrackingEnable(boolean enable) {
        vertexColorTracking = enable;
    }

    public boolean isVertexColorTrackingEnabled() {
        return vertexColorTracking;
    }
}
