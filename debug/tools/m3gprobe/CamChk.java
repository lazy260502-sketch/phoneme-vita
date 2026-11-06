package javax.microedition.m3g;

/** Every documented Camera usage must not throw. */
public class CamChk {
    static int fails = 0;
    static void chk(String what, boolean ok) {
        System.out.println((ok ? "  ok   " : "  FAIL ") + what);
        if (!ok) fails++;
    }
    public static void main(String[] a) {
        /* 1. The exact log185 call: PERSPECTIVE camera + real Transform. */
        Camera c = new Camera();
        c.setPerspective(70.0f, 0.75f, 1.0f, 1000.0f);
        Transform t = new Transform();
        try {
            int type = c.getProjection(t);
            chk("getProjection(Transform) on PERSPECTIVE (log185 site)", type == Camera.PERSPECTIVE);
            chk("  matrix is not identity", t.m[0] != 1.0f || t.m[5] != 1.0f);
            chk("  matrix is finite", finite(t));
            /* fovy=70 deg -> f = 1/tan(35deg) = 1.4281; m[5] = f */
            chk("  m5 == 1/tan(35deg) = " + t.m[5], Math.abs(t.m[5] - 1.42815f) < 0.001f);
            /* m0 = f/aspect = 1.4281/0.75 = 1.9042 */
            chk("  m0 == f/aspect = " + t.m[0], Math.abs(t.m[0] - 1.90420f) < 0.001f);
        } catch (Throwable e) {
            chk("getProjection(Transform) on PERSPECTIVE threw " + e, false);
        }

        /* 2. PARALLEL too. */
        Camera p = new Camera();
        p.setParallel(2.0f, 0.75f, 1.0f, 100.0f);
        try {
            int type = p.getProjection(new Transform());
            chk("getProjection(Transform) on PARALLEL", type == Camera.PARALLEL);
        } catch (Throwable e) { chk("PARALLEL threw " + e, false); }

        /* 3. GENERIC round-trip must be exact. */
        Transform g = new Transform();
        g.postRotate(30.0f, 0, 0, 1);
        Camera gc = new Camera();
        gc.setGeneric(g);
        Transform out = new Transform();
        try {
            chk("getProjection(Transform) on GENERIC", gc.getProjection(out) == Camera.GENERIC);
            boolean same = true;
            for (int i = 0; i < 16; i++) if (Math.abs(out.m[i] - g.m[i]) > 1e-6f) same = false;
            chk("  GENERIC matrix round-trips exactly", same);
        } catch (Throwable e) { chk("GENERIC threw " + e, false); }

        /* 4. null Transform is legal (only asks for the type). */
        try { chk("getProjection(null) legal", c.getProjection((Transform)null) == Camera.PERSPECTIVE); }
        catch (Throwable e) { chk("getProjection(null) threw " + e, false); }

        /* 5. float[] form: GENERIC leaves the array untouched. */
        float[] prm = { -1, -2, -3, -4 };
        gc.getProjection(prm);
        chk("getProjection(float[]) on GENERIC leaves array untouched",
            prm[0] == -1 && prm[1] == -2 && prm[2] == -3 && prm[3] == -4);
        c.getProjection(prm);
        chk("getProjection(float[]) on PERSPECTIVE fills 70/0.75/1/1000",
            prm[0] == 70.0f && prm[1] == 0.75f && prm[2] == 1.0f && prm[3] == 1000.0f);

        /* 6. setPerspective must REJECT near == far (spec: "Implementations
         * must detect this rather than trying to construct the projection
         * matrix, as that would result in a divide by zero error"), so a camera
         * with near == far cannot be built through the setter at all - and
         * therefore getProjection()'s documented ArithmeticException is
         * unreachable from Java. Assert the setter behaves, and that the
         * reachable path (a legal camera) never throws. */
        boolean rejected = false;
        try { new Camera().setPerspective(45.0f, 1.0f, 5.0f, 5.0f); }
        catch (IllegalArgumentException e) { rejected = true; }
        chk("setPerspective rejects near == far", rejected);
        boolean parRejected = false;
        try { new Camera().setParallel(5.0f, 1.0f, 3.0f, 3.0f); }
        catch (IllegalArgumentException e) { parRejected = true; }
        chk("setParallel rejects near == far", parRejected);

        System.out.println(fails == 0 ? "\nALL PASS" : "\nFAILURES: " + fails);
    }
    static boolean finite(Transform t) {
        for (int i = 0; i < 16; i++) if (Float.isNaN(t.m[i]) || Float.isInfinite(t.m[i])) return false;
        return true;
    }
}
