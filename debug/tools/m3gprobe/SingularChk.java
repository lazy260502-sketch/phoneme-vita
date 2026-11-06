package javax.microedition.m3g;

/**
 * Behaviour of Transform.invert() on a degenerate matrix, and of the renderer
 * when the camera is unusable.
 *
 * BACKGROUND (log189, 极品飞车, 黑屏):
 *   The title builds a camera basis from two 3D points and normalises it:
 *   ai.f() bci=236..283 -> ab.a(Transform,int x6) -> 1/sqrt(|p1-p0|^2).
 *   When the two points coincide, the length is 0, the scale is +Infinity and
 *   the matrix rows become NaN. invert() used to throw ArithmeticException;
 *   the title has no handler on that path, TimerThread cancelled the task and
 *   the screen stayed black.
 *
 * WHAT IS ASSERTED HERE:
 *   1. invert() does NOT throw for a singular matrix (matches the reference
 *      implementation, where it is a bare native call and the Java layer never
 *      inspects a result) and leaves the matrix untouched.
 *   2. invert() still works for a healthy matrix (no regression).
 *   3. Matrix4.invert() reports NaN/Inf matrices as not-invertible instead of
 *      returning true plus an all-NaN inverse (that was a real bug: det comes
 *      out NaN and `det == 0.0f` is false for NaN).
 */
public class SingularChk {
    static int fails = 0;
    static void chk(String what, boolean ok) {
        System.out.println((ok ? "  ok   " : "  FAIL ") + what);
        if (!ok) fails++;
    }

    /** The matrix ab.a(Transform,int x6) builds when p0 == p1. */
    static float[] degenerate() {
        float invLen = 1.0f / 0.0f;            /* +Inf */
        float d = 0.0f * invLen;               /* NaN  */
        float[] m = new float[16];
        m[0] = 1f; m[2] = 1f;
        m[4] = d;  m[5] = d;  m[6] = d;
        m[8] = -d; m[9] = -d; m[10] = -d;
        m[15] = 1f;
        return m;
    }

    public static void main(String[] a) {
        /* 1. singular matrix: must not throw, must stay unchanged */
        float[] bad = degenerate();
        float[] before = new float[16];
        System.arraycopy(bad, 0, before, 0, 16);
        Transform t = new Transform();
        t.set(bad);
        boolean threw = false;
        try {
            t.invert();
        } catch (Throwable e) {
            threw = true;
            System.out.println("       threw " + e);
        }
        chk("invert() on NaN matrix does not throw", !threw);
        float[] after = new float[16];
        t.get(after);
        boolean unchanged = true;
        for (int i = 0; i < 16; i++) {
            /* NaN != NaN, so compare bit patterns */
            if (Float.floatToIntBits(after[i]) != Float.floatToIntBits(before[i])) {
                unchanged = false;
            }
        }
        chk("  matrix left untouched", unchanged);

        /* 2. healthy matrix still inverts */
        Transform ok = new Transform();
        ok.postTranslate(3f, 4f, 5f);
        ok.postRotate(30f, 0f, 1f, 0f);
        float[] orig = new float[16];
        ok.get(orig);
        boolean t2 = false;
        try {
            ok.invert();
        } catch (Throwable e) {
            t2 = true;
        }
        chk("invert() on healthy matrix does not throw", !t2);
        /* inverse * original should be ~identity: check the translation cancels */
        float[] inv = new float[16];
        ok.get(inv);
        Transform prod = new Transform();
        prod.set(orig);
        prod.postMultiply(ok);          /* orig * inv */
        float[] p = new float[16];
        prod.get(p);
        boolean ident = Math.abs(p[12]) < 1e-3f && Math.abs(p[13]) < 1e-3f
                     && Math.abs(p[14]) < 1e-3f && Math.abs(p[0] - 1f) < 1e-3f
                     && Math.abs(p[5] - 1f) < 1e-3f && Math.abs(p[10] - 1f) < 1e-3f;
        chk("  orig * inverse == identity", ident);

        /* 3. Matrix4.invert must reject NaN, not hand back an all-NaN inverse */
        float[] out = new float[16];
        boolean okDet = com.sun.m3g.Matrix4.invert(out, bad);
        chk("Matrix4.invert(NaN matrix) == false", !okDet);

        float[] inf = new float[16];
        inf[0] = Float.POSITIVE_INFINITY;
        inf[5] = 1f; inf[10] = 1f; inf[15] = 1f;
        boolean okInf = com.sun.m3g.Matrix4.invert(new float[16], inf);
        chk("Matrix4.invert(Inf matrix) == false", !okInf);

        /* identity still fine */
        float[] id = new float[16];
        com.sun.m3g.Matrix4.identity(id);
        chk("Matrix4.invert(identity) == true",
            com.sun.m3g.Matrix4.invert(out, id));

        System.out.println(fails == 0 ? "\nALL PASS" : "\nFAILURES: " + fails);
        if (fails != 0) System.exit(1);
    }
}
