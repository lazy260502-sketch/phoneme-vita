package javax.microedition.m3g;

import com.sun.m3g.Matrix4;

/**
 * A 4x4 transform. Internally column-major; get(float[]) hands out the
 * row-major array the JSR 184 API specifies.
 */
public class Transform {

    final float[] m = new float[16];

    public Transform() {
        Matrix4.identity(m);
    }

    public Transform(Transform other) {
        if (other == null) {
            throw com.sun.m3g.M3gTrace.fail(new NullPointerException());
        }
        Matrix4.copy(m, other.m);
    }

    public void setIdentity() {
        Matrix4.identity(m);
    }

    public void set(Transform other) {
        if (other == null) {
            throw com.sun.m3g.M3gTrace.fail(new NullPointerException());
        }
        Matrix4.copy(m, other.m);
    }

    public void set(float[] matrix) {
        if (matrix == null) {
            throw com.sun.m3g.M3gTrace.fail(new NullPointerException());
        }
        if (matrix.length < 16) {
            throw com.sun.m3g.M3gTrace.fail(new IllegalArgumentException("need 16 elements"));
        }
        for (int row = 0; row < 4; row++) {
            for (int col = 0; col < 4; col++) {
                m[col * 4 + row] = matrix[row * 4 + col];
            }
        }
    }

    public void get(float[] matrix) {
        if (matrix == null) {
            throw com.sun.m3g.M3gTrace.fail(new NullPointerException());
        }
        if (matrix.length < 16) {
            throw com.sun.m3g.M3gTrace.fail(new IllegalArgumentException("need 16 elements"));
        }
        for (int row = 0; row < 4; row++) {
            for (int col = 0; col < 4; col++) {
                matrix[row * 4 + col] = m[col * 4 + row];
            }
        }
    }

    /* v03.40: THE bug that keeps the 3D scenes black.
     *
     * Matrix4.mul(out, a, b) computes out = a * b. JSR-184 says
     *     postMultiply(t)  ->  M = M * t      (post-multiply)
     *     preMultiply(t)   ->  M = t * M      (pre-multiply)
     * but every post* method below passed the operands the other way round
     * (Matrix4.mul(m, x, m) == m = x * m), i.e. post* was implemented as
     * PRE-multiply and pre* did not exist at all.
     *
     * Why that is fatal rather than cosmetic: the CAPCOM title builds every
     * scene transform as postTranslate then postScale:
     *     t.postTranslate(m03/409.6f, m13/409.6f, m23/409.6f);
     *     t.postScale(0.012f, 0.012f, 0.012f);
     * By the spec that composes T*S, so a vertex becomes 0.012*v + p and the
     * translation survives intact. Composed as S*T it becomes
     * 0.012*(v + p) = 0.012*v + 0.012*p -- the translation is divided by 83 as
     * well, so every node collapses onto the origin no matter where the game
     * put it. That is exactly what log139 showed:
     *     ndc n=472 behind=0 inside=472 x[0.04,0.04] y[-0.40,-0.40] z[0.98,0.98]
     * 472 vertices of the room overlay at a SINGLE NDC point, and the readback
     * probe counting only 268 pixels whose alpha is zero.
     *
     * Cameras were unaffected (setCameraTransform uses Transform.set(), not
     * post*), which is why the camera looked right all along. */
    public void postMultiply(Transform other) {
        if (other == null) {
            throw com.sun.m3g.M3gTrace.fail(new NullPointerException());
        }
        Matrix4.mul(m, m, other.m);
    }

    public void preMultiply(Transform other) {
        if (other == null) {
            throw com.sun.m3g.M3gTrace.fail(new NullPointerException());
        }
        Matrix4.mul(m, other.m, m);
    }

    public void postTranslate(float tx, float ty, float tz) {
        float[] t = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, tx, ty, tz, 1 };
        Matrix4.mul(m, m, t);
    }

    public void preTranslate(float tx, float ty, float tz) {
        float[] t = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, tx, ty, tz, 1 };
        Matrix4.mul(m, t, m);
    }

    public void postRotate(float angle, float ax, float ay, float az) {
        float[] r = rotation(angle, ax, ay, az);
        if (r != null) {
            Matrix4.mul(m, m, r);
        }
    }

    public void preRotate(float angle, float ax, float ay, float az) {
        float[] r = rotation(angle, ax, ay, az);
        if (r != null) {
            Matrix4.mul(m, r, m);
        }
    }

    /* v03.63: log153 - the game's facing matrix is sometimes diag(0,1,0), i.e. cos and sin of a rotationY would both be 0. Log what the real
     * calls get: every 16th call plus any call whose cos and sin are both 0 or NaN (cap 160 lines). rotQuiet mutes it for the self-test. */
    static boolean rotQuiet = false;
    private static int rotCalls = 0;
    private static int rotLogged = 0;

    private static float[] rotation(float angle, float ax, float ay, float az) {
        float len = (float)Math.sqrt(ax * ax + ay * ay + az * az);
        if (len == 0.0f) {
            return null;
        }
        ax /= len;
        ay /= len;
        az /= len;
        /* JSR-184: angle is in DEGREES (log147: AffineTrans.rotationX passes i*360/4096). Using it as radians made every rotation wrong. */
        double rad = angle * (Math.PI / 180.0);
        float c = (float)Math.cos(rad);
        float s = (float)Math.sin(rad);
        if (!rotQuiet) {
            rotCalls++;
            boolean zero = (c == 0.0f && s == 0.0f);
            boolean nan = (c != c || s != s);
            if (rotLogged < 160 && (zero || nan || (rotCalls & 15) == 1)) {
                rotLogged++;
                com.sun.m3g.M3gTrace.log("rot#" + rotCalls + " ms=" + System.currentTimeMillis() + " ang*100=" + (int)(angle * 100.0f)
                    + " axis*100=" + (int)(ax * 100.0f) + "," + (int)(ay * 100.0f) + "," + (int)(az * 100.0f)
                    + " cos=" + Integer.toHexString(Float.floatToIntBits(c)) + " sin=" + Integer.toHexString(Float.floatToIntBits(s))
                    + (zero ? " ZERO" : (nan ? " NAN" : "")));
            }
        }
        float t = 1.0f - c;
        return new float[] {
            t * ax * ax + c,       t * ax * ay + s * az, t * ax * az - s * ay, 0,
            t * ax * ay - s * az,  t * ay * ay + c,      t * ay * az + s * ax, 0,
            t * ax * az + s * ay,  t * ay * az - s * ax, t * az * az + c,      0,
            0, 0, 0, 1
        };
    }

    public void postScale(float sx, float sy, float sz) {
        float[] s = { sx, 0, 0, 0, 0, sy, 0, 0, 0, 0, sz, 0, 0, 0, 0, 1 };
        Matrix4.mul(m, m, s);
    }

    public void preScale(float sx, float sy, float sz) {
        float[] s = { sx, 0, 0, 0, 0, sy, 0, 0, 0, 0, sz, 0, 0, 0, 0, 1 };
        Matrix4.mul(m, s, m);
    }

    /**
     * Inverts this transformation.
     *
     * v04.49: DOES NOT THROW ANY MORE. This is a deliberate deviation from the
     * letter of JSR-184, and it is the behaviour real devices have.
     *
     * WHY. log189 (极品飞车): the title builds a camera basis from two 3D
     * points (ai.f() bci=236..283 -> ab.a(Transform,int x6)) and does
     * `1/sqrt(|p1-p0|^2)`. When the two points coincide the length is 0, so the
     * scale factor is +Infinity, so the matrix rows become NaN, so the matrix
     * is not invertible BY CONSTRUCTION - the title's own data produced it.
     * `invert()` then threw ArithmeticException, the title has NO exception
     * handler anywhere on that path (ai.class has no table entry covering
     * bci=283), the exception escaped the periodic task, TimerThread cancelled
     * it, and the screen went black and stayed black.
     *
     * THE SPEC vs THE PLATFORM. JSR-184 documents
     *     Throws: ArithmeticException - if this transformation is not invertible
     * but the reference implementation wraps Nokia's m3g core, and there
     * `Transform.invert()` is just `_invert(matrix)` - a native call that
     * returns void. The Java layer never inspects a result, so a singular
     * matrix leaves the matrix untouched and execution continues. That is why
     * this title shipped and worked on real hardware: on the devices it was
     * written for, the singular case was SILENT. Throwing is technically
     * compliant but it takes a rendering glitch and turns it into a dead frame
     * loop, which is strictly worse for compatibility - and compatibility is
     * the goal of this port.
     *
     * WHAT HAPPENS NOW: on a singular (or NaN/Inf) matrix the transform is left
     * EXACTLY as it was, which matches the reference's "untouched" outcome (a
     * C routine that bails out before writing `out`). The failure is still
     * visible: M3gTrace logs it once per site so a device log can tell
     * "singular matrix happened" from "singular matrix never happened". The
     * caller sees a stale matrix for one frame - a wrong-looking camera, not a
     * frozen title.
     *
     * Callers INSIDE this port never relied on the throw: every internal use
     * goes through Matrix4.invert(float[],float[]) and checks the boolean
     * (Node.getTransformTo, Renderer.view, SkinnedMesh.bind pose). Only the
     * title's own call was ever exposed to the exception.
     */
    public void invert() {
        float[] out = new float[16];
        if (!Matrix4.invert(out, m)) {
            com.sun.m3g.M3gTrace.log(
                "Transform.invert: matrix is not invertible; left unchanged");
            return;
        }
        Matrix4.copy(m, out);
    }

    /** True when the matrix is a pure translation (fast path for points). */
    public boolean isIdentity() {
        for (int i = 0; i < 16; i++) {
            float want = (i % 5 == 0) ? 1.0f : 0.0f;
            if (m[i] != want) {
                return false;
            }
        }
        return true;
    }

    /** transform(src) -> dst; returns dst. */
    public void transform(float[] src, float[] dst) {
        if (src == null || dst == null) {
            throw com.sun.m3g.M3gTrace.fail(new NullPointerException());
        }
        int n = src.length;
        if (n % 4 != 0 || dst.length < n) {
            throw com.sun.m3g.M3gTrace.fail(new IllegalArgumentException());
        }
        for (int i = 0; i < n; i += 4) {
            float x = src[i], y = src[i + 1], z = src[i + 2], w = src[i + 3];
            dst[i]     = m[0] * x + m[4] * y + m[8] * z + m[12] * w;
            dst[i + 1] = m[1] * x + m[5] * y + m[9] * z + m[13] * w;
            dst[i + 2] = m[2] * x + m[6] * y + m[10] * z + m[14] * w;
            dst[i + 3] = m[3] * x + m[7] * y + m[11] * z + m[15] * w;
        }
    }

    /** transform(src) in place. */
    public void transform(float[] vectors) {
        transform(vectors, vectors);
    }
}
