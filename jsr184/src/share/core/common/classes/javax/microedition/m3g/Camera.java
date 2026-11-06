package javax.microedition.m3g;

/**
 * Camera node. Projection is stored as parameters and turned into a matrix
 * by Renderer once the viewport (and therefore the aspect correction) is
 * known.
 */
public class Camera extends Node {

    public static final int GENERIC = 48;
    public static final int PARALLEL = 49;
    public static final int PERSPECTIVE = 50;

    int type = GENERIC;
    float fovy = 45.0f;
    float aspect = 1.0f;
    float near = 0.1f;
    float far = 100.0f;
    final Transform projection = new Transform();

    public int getProjection(float[] params) {
        if (params != null) {
            if (params.length < 4) {
                throw com.sun.m3g.M3gTrace.fail(new IllegalArgumentException());
            }
            if (type != GENERIC) {
                params[0] = fovy;
                params[1] = aspect;
                params[2] = near;
                params[3] = far;
            }
        }
        return type;
    }

    /**
     * Returns the projection matrix and type.
     *
     * v04.29: this used to THROW `IllegalArgumentException("parametric camera")`
     * whenever the camera was PERSPECTIVE or PARALLEL and the caller passed a
     * non-null Transform. That was invented behaviour, and JSR-184 says the
     * opposite in so many words:
     *
     *   "Gets the current projection matrix and type. This method is available
     *    REGARDLESS OF THE TYPE OF PROJECTION, since parallel and perspective
     *    projections can always be returned in the 4x4 matrix form."
     *   Throws: ArithmeticException "if the transformation matrix cannot be
     *    computed due to illegal perspective or parallel projection parameters
     *    (that is, if near == far)".
     *
     * log185 (极品飞车) hit exactly that: it builds a PERSPECTIVE camera and then
     * does `cam.getProjection(t)` with a real Transform, because it wants the
     * matrix to pre-multiply into its own camera-space transform
     * (ab.a(Camera,Transform,Transform) bci=2). We threw, the exception escaped
     * paint(), and the frame died - the same failure shape as FUNC_BLEND,
     * WINDING, SPOT, setLayer and the Loader node, but this time introduced by
     * this port rather than inherited.
     *
     * The matrix construction lives in Renderer.buildProjection() so there is
     * ONE definition of it. Renderer's own per-frame path accepts the same
     * near==far case and simply produces an unusable matrix; here the spec
     * explicitly asks for an exception, so that case (and only that case)
     * throws.
     */
    public int getProjection(Transform t) {
        if (t != null) {
            if (type != GENERIC && near == far) {
                throw com.sun.m3g.M3gTrace.fail(new ArithmeticException(
                    "near == far: projection matrix is undefined"));
            }
            float aspectRatio = aspect != 0.0f ? aspect : 1.0f;
            float[] m = Renderer.buildProjection(type, fovy, aspectRatio,
                                                 near, far, projection.m);
            System.arraycopy(m, 0, t.m, 0, 16);
        }
        return type;
    }

    public void setParallel(float fovy, float aspectRatio, float nearPlane,
                            float farPlane) {
        /* JSR-184: setParallel only rejects aspectRatio == 0 and far <= near.
         * fovy and the near plane are NOT constrained - a negative near plane
         * is normal for an orthographic camera, and fovy may be <= 0 to flip
         * the projection. log129: 极品飞车13's scene builder calls
         *     camera.setParallel(90.0f, 1.0f, -1.0f, 1.0f)
         * (see class `r`, the object created by `ab`'s static initializer), so
         * v03.21 and earlier threw IllegalArgumentException, the static
         * initializer died with ExceptionInInitializerError and the MIDlet
         * never started. Renderer's orthographic matrix already handles a
         * negative near plane correctly (m[10] = -2/(far-near), m[14] =
         * -(far+near)/(far-near)). */
        if (aspectRatio == 0.0f || farPlane <= nearPlane) {
            throw com.sun.m3g.M3gTrace.fail(new IllegalArgumentException("bad parallel projection"
                + " fovy=" + fovy + " aspect=" + aspectRatio
                + " near=" + nearPlane + " far=" + farPlane));
        }
        type = PARALLEL;
        this.fovy = fovy;
        aspect = aspectRatio;
        near = nearPlane;
        far = farPlane;
    }

    public void setPerspective(float fovy, float aspectRatio, float nearPlane,
                               float farPlane) {
        /* JSR-184: fovy <= 0, aspectRatio == 0, near <= 0 and far <= near are
         * all illegal for a perspective projection. */
        if (fovy <= 0.0f || aspectRatio == 0.0f || nearPlane <= 0.0f
            || farPlane <= nearPlane) {
            throw com.sun.m3g.M3gTrace.fail(new IllegalArgumentException("bad perspective projection"
                + " fovy=" + fovy + " aspect=" + aspectRatio
                + " near=" + nearPlane + " far=" + farPlane));
        }
        type = PERSPECTIVE;
        this.fovy = fovy;
        this.aspect = aspectRatio;
        near = nearPlane;
        far = farPlane;
    }

    public void setGeneric(Transform transform) {
        if (transform == null) {
            throw com.sun.m3g.M3gTrace.fail(new NullPointerException());
        }
        type = GENERIC;
        projection.set(transform);
    }
}
