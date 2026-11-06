package javax.microedition.m3g;

/**
 * Appearance = material + texture + the two polygon/compositing modes.
 */
public class Appearance extends Object3D {

    /* JSR-184 §Appearance.setLayer: layer must be in [-1, 15] (-1 = unlayered).
     * These are NOT part of the API - the spec only states the range in prose,
     * so they stay private to avoid inventing constants a title might trip on. */
    private static final int LAYER_MIN = -1;
    private static final int LAYER_MAX = 15;

    private Material material = null;
    private Texture2D texture = null;
    private CompositingMode compositing = null;
    private PolygonMode polygon = null;
    private Fog fog = null;
    private int layer = 0;

    public void setMaterial(Material m) {
        material = m;
    }

    /* v04.22: setLayer() was declared PACKAGE-PRIVATE while JSR-184 makes it
     * public API. A title calls it with `invokevirtual`, so the access check in
     * JavaClass::check_access() failed and the VM threw IllegalAccessError -
     * which on this build prints as the useless "java.lang.Error:  95" because
     * USE_VERBOSE_ERROR_MSG is off and ErrorMsgTag is then just an enum index
     * (illegal_access_error == 95). 极品飞车 hit exactly that at
     * ab.a(Node,int) bci=255 -> setLayer(2), the Timer task got cancelled and
     * the loading screen froze (log182).
     * This is the FOURTH defect of the same shape (a legal API call that
     * throws): FUNC_BLEND v03.29, WINDING_CCW/CW v04.17, Light.SPOT v04.17,
     * and now a visibility mistake. debug/tools/m3gprobe/vischk.py now cross-
     * checks every m3g member a title's bytecode references against the
     * visibility of the member we actually built - run it before shipping. */
    public void setLayer(int layer) {
        if (layer < LAYER_MIN || layer > LAYER_MAX) {
            throw com.sun.m3g.M3gTrace.fail(new IllegalArgumentException(
                "bad layer: " + layer));
        }
        this.layer = layer;
    }

    public int getLayer() {
        return layer;
    }

    public Material getMaterial() {
        return material;
    }

    public void setTexture(int index, Texture2D t) {
        if (index != 0) {
            throw com.sun.m3g.M3gTrace.fail(new IllegalArgumentException("only one texture unit"));
        }
        texture = t;
    }

    public Texture2D getTexture(int index) {
        if (index != 0) {
            throw com.sun.m3g.M3gTrace.fail(new IllegalArgumentException("only one texture unit"));
        }
        return texture;
    }

    public int getTextureCount() {
        return texture == null ? 0 : 1;
    }

    public void setCompositingMode(CompositingMode c) {
        compositing = c;
    }

    public CompositingMode getCompositingMode() {
        return compositing;
    }

    public void setPolygonMode(PolygonMode p) {
        polygon = p;
    }

    public PolygonMode getPolygonMode() {
        return polygon;
    }

    public void setFog(Fog f) {
        fog = f;
    }

    public Fog getFog() {
        return fog;
    }

    void collectReferences(java.util.Vector out) {
        if (material != null) out.addElement(material);
        if (texture != null) out.addElement(texture);
        if (compositing != null) out.addElement(compositing);
        if (polygon != null) out.addElement(polygon);
        if (fog != null) out.addElement(fog);
    }
}
