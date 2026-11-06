package javax.microedition.m3g;

import com.sun.m3g.Matrix4;

/**
 * Translation / orientation / scale plus an optional explicit matrix.
 * The composite is T * R * S * M, matching the JSR 184 definition.
 */
public abstract class Transformable extends Object3D {

    final float[] translation = { 0.0f, 0.0f, 0.0f };
    final float[] scale = { 1.0f, 1.0f, 1.0f };
    final float[] orientation = { 0.0f, 0.0f, 1.0f, 0.0f }; /* angle, ax, ay, az */
    Transform userTransform = null;

    Transformable() {
    }

    public void setTranslation(float tx, float ty, float tz) {
        translation[0] = tx;
        translation[1] = ty;
        translation[2] = tz;
    }

    public void getTranslation(float[] t) {
        requireLen(t, 3);
        System.arraycopy(translation, 0, t, 0, 3);
    }

    public void setScale(float sx, float sy, float sz) {
        scale[0] = sx;
        scale[1] = sy;
        scale[2] = sz;
    }

    public void getScale(float[] s) {
        requireLen(s, 3);
        System.arraycopy(scale, 0, s, 0, 3);
    }

    public void setOrientation(float angle, float ax, float ay, float az) {
        orientation[0] = angle;
        orientation[1] = ax;
        orientation[2] = ay;
        orientation[3] = az;
    }

    public void getOrientation(float[] o) {
        requireLen(o, 4);
        System.arraycopy(orientation, 0, o, 0, 4);
    }

    /** Orientation from a quaternion (x, y, z, w): the encoding of ORIENTATION keyframes (not angle/axis). */
    void setOrientationQuat(float x, float y, float z, float w) {
        float len = (float)Math.sqrt(x * x + y * y + z * z + w * w);
        if (len < 1e-12f) {
            orientation[0] = 0.0f;
            return;
        }
        x /= len;
        y /= len;
        z /= len;
        w /= len;
        if (w < 0.0f) {
            x = -x;
            y = -y;
            z = -z;
            w = -w;
        }
        float s = (float)Math.sqrt(x * x + y * y + z * z);
        if (s < 1e-6f) {
            orientation[0] = 0.0f;
            return;
        }
        orientation[0] = (float)(2.0 * Matrix4.atan2(s, w) * (180.0 / Math.PI));
        orientation[1] = x / s;
        orientation[2] = y / s;
        orientation[3] = z / s;
    }

    public void setTransform(Transform t) {
        userTransform = t;
    }

    public void getTransform(Transform t) {
        if (t == null) {
            throw com.sun.m3g.M3gTrace.fail(new NullPointerException());
        }
        if (userTransform == null) {
            t.setIdentity();
        } else {
            t.set(userTransform);
        }
    }

    /* v03.30: JSR-184 Transformable.getCompositeTransform(Transform): the
     * node's T*R*S*M. Neither game calls it, but it is public API and the
     * only way a test can read the composed matrix back. */
    public void getCompositeTransform(Transform t) {
        if (t == null) {
            throw com.sun.m3g.M3gTrace.fail(new NullPointerException());
        }
        getCompositeMatrix(t.m);
    }

    /* v04.10: the composite of the last call and the exact inputs it was built from. The inputs are compared, not
     * flagged dirty: the TRS arrays and userTransform.m are written in place from many places (loader, setters,
     * animation, game code holding the Transform). A NaN input never compares equal, so it just recomputes. */
    private float[] ccKey = null;      /* t(3) s(3) o(4) */
    private float[] ccUser = null;     /* userTransform.m at that time */
    private float[] ccOut = null;
    private boolean ccHasUser = false;
    private boolean ccValid = false;

    private boolean sameInputs() {
        float[] k = ccKey;
        float[] t = translation, s = scale, o = orientation;
        if (k[0] != t[0] || k[1] != t[1] || k[2] != t[2]
            || k[3] != s[0] || k[4] != s[1] || k[5] != s[2]
            || k[6] != o[0] || k[7] != o[1] || k[8] != o[2] || k[9] != o[3]) {
            return false;
        }
        if (userTransform == null) {
            return !ccHasUser;
        }
        if (!ccHasUser) {
            return false;
        }
        float[] m = userTransform.m;
        float[] u = ccUser;
        for (int i = 0; i < 16; i++) {
            if (m[i] != u[i]) {
                return false;
            }
        }
        return true;
    }

    /** Composite local transform into out.m (column-major). */
    void getCompositeMatrix(float[] out) {
        if (ccKey == null) {
            ccKey = new float[10];
            ccUser = new float[16];
            ccOut = new float[16];
        } else if (ccValid && sameInputs()) {
            System.arraycopy(ccOut, 0, out, 0, 16);
            return;
        }
        float[] res = new float[16];
        if (userTransform != null) {
            float[] trs = new float[16];
            Matrix4.compose(trs, translation, orientation, scale);
            Matrix4.mul(res, trs, userTransform.m);
        } else {
            Matrix4.compose(res, translation, orientation, scale);
        }
        ccValid = false;
        System.arraycopy(res, 0, ccOut, 0, 16);
        System.arraycopy(translation, 0, ccKey, 0, 3);
        System.arraycopy(scale, 0, ccKey, 3, 3);
        System.arraycopy(orientation, 0, ccKey, 6, 4);
        ccHasUser = userTransform != null;
        if (ccHasUser) {
            System.arraycopy(userTransform.m, 0, ccUser, 0, 16);
        }
        ccValid = true;
        System.arraycopy(res, 0, out, 0, 16);
    }

    static void requireLen(float[] a, int n) {
        if (a == null) {
            throw com.sun.m3g.M3gTrace.fail(new NullPointerException());
        }
        if (a.length < n) {
            throw com.sun.m3g.M3gTrace.fail(new IllegalArgumentException("need " + n + " elements"));
        }
    }
}
