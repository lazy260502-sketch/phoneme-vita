package javax.microedition.m3g;

/**
 * Polygon rasterisation mode.
 */
public class PolygonMode extends Object3D {

    public static final int POINT = 168;
    public static final int LINE = 169;
    public static final int FILL = 170;

    public static final int CULL_BACK = 160;
    public static final int CULL_FRONT = 161;
    public static final int CULL_NONE = 162;

    /* v04.17: WINDING_CCW/CW were 176/177. That is WRONG, and it cost a
     * device round trip (log180).
     *
     * JSR-184 really does define WINDING_CCW == POINT == 168 and
     * WINDING_CW == LINE == 169. The overlap is harmless because the values
     * live in different method namespaces (setWinding vs setFillMode). Someone
     * saw the duplicate 168 and "fixed" the collision by renumbering winding
     * to 176/177, which broke both directions:
     *
     *   1. THE .m3g FILE FORMAT stores the API constant directly. Measured on
     *      极品飞车's /aq: all 22 PolygonModes carry culling=162 (CULL_NONE),
     *      shading=165, winding=168. So a file's CCW is literally 168, and
     *      our 176 never matched it. Renderer's
     *      `getWinding() == PolygonMode.WINDING_CW` could therefore never be
     *      true, i.e. the CW flip was dead code for every file-loaded mesh.
     *   2. THE API CALL throws. The same title calls
     *      `PolygonMode.setWinding(169)` (ab.a(Node,int) bci 202..206,
     *      literally `sipush 169`), which is CW. Our validation rejected it:
     *          IllegalArgumentException: bad winding
     *          -> [TIMEREXC] task v@1be3a289 cancelled
     *          -> the 15 ms main loop stopped -> "loading screen froze again".
     *
     * Two independent sources (file contents and a title's own bytecode) agree
     * on 168/169, so this is not a guess.
     *
     * DO NOT "de-duplicate" these numbers again: 168/169 appearing twice is
     * the specification, not a mistake. */
    public static final int WINDING_CCW = 168;
    public static final int WINDING_CW = 169;

    /* v04.17: the shading constants were missing entirely (the field carried a
     * bare `0` and a comment). Files store 165, which is SHADE_SMOOTH, and
     * nothing in this port reads the field yet, so declaring the real values
     * and defaulting to SMOOTH is documentation plus a correct starting state
     * rather than a behaviour change. */
    public static final int SHADE_FLAT = 164;
    public static final int SHADE_SMOOTH = 165;

    int culling = CULL_BACK;
    int shading = SHADE_SMOOTH;
    int winding = WINDING_CCW;
    boolean twoSided = false;
    boolean localCamera = false;
    int mode = FILL;
    float pointSize = 1.0f;

    public void setCulling(int mode) {
        if (mode < CULL_BACK || mode > CULL_NONE) {
            throw com.sun.m3g.M3gTrace.fail(new IllegalArgumentException("bad culling"));
        }
        culling = mode;
    }

    public int getCulling() {
        return culling;
    }

    public void setWinding(int mode) {
        if (mode != WINDING_CCW && mode != WINDING_CW) {
            throw com.sun.m3g.M3gTrace.fail(new IllegalArgumentException("bad winding"));
        }
        winding = mode;
    }

    public int getWinding() {
        return winding;
    }

    public void setShading(int mode) {
        shading = mode;
    }

    public int getShading() {
        return shading;
    }

    public void setTwoSidedLightingEnable(boolean enable) {
        twoSided = enable;
    }

    public boolean isTwoSidedLightingEnabled() {
        return twoSided;
    }

    public void setLocalCameraLightingEnable(boolean enable) {
        localCamera = enable;
    }

    public boolean isLocalCameraLightingEnabled() {
        return localCamera;
    }

    public void setPerspectiveCorrectionEnable(boolean enable) {
        /* always on in both backends */
    }

    public void setPointSpriteEnable(boolean enable) {
    }

    public void setFillMode(int mode) {
        if (mode != POINT && mode != LINE && mode != FILL) {
            throw com.sun.m3g.M3gTrace.fail(new IllegalArgumentException("bad fill mode"));
        }
        this.mode = mode;
    }

    public int getFillMode() {
        return mode;
    }

    public void setPointSize(float size) {
        pointSize = size;
    }

    public float getPointSize() {
        return pointSize;
    }
}
