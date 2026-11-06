package javax.microedition.m3g;

/**
 * Constant-integrity test for the JSR-184 API surface.
 *
 * WHY THIS EXISTS
 * ===============
 * This port has now been bitten three times by the SAME defect class: a wrong
 * or missing API constant that makes a perfectly legal call THROW. When such a
 * call sits inside a title's periodic task, TimerThread cancels the task and
 * the app freezes with no visible error - the hardest possible symptom to
 * diagnose from a device log.
 *
 *   v03.29  Texture2D.FUNC_BLEND (228) was missing
 *           -> 生化危机 called setBlending(228) -> IllegalArgumentException
 *   v04.15  PolygonMode.WINDING_CCW/CW were 176/177 instead of 168/169
 *           -> 极品飞车 called setWinding(169) -> IllegalArgumentException
 *              ("bad winding", log180), task cancelled, loading screen froze
 *   v04.15  Light.SPOT (131) was missing -> any spot-lit title would crash
 *
 * THE SOURCES OF TRUTH (do not "fix" these numbers from memory)
 * ============================================================
 *   1. Every .m3g file stores the API constant VALUE, not a compact file-code.
 *      Measured on 极品飞车's /aq: culling=162 (CULL_NONE), shading=165
 *      (SHADE_SMOOTH), winding=168 (WINDING_CCW). Measured across the sample
 *      set: Texture2D levelFilter=208, imageFilter=210.
 *   2. A title's own bytecode passes the same numbers (setWinding(169),
 *      Texture2D.setBlending(228)).
 *   So when a constant is in doubt, check those two - not a remembered table.
 *
 * Note the deliberate overlap: WINDING_CCW == POINT == 168 and
 * WINDING_CW == LINE == 169. That is the specification; the values live in
 * different method namespaces. Do NOT de-duplicate them.
 *
 * WHAT IT DOES
 * ============
 * Calls every documentable setter with every defined value and insists that
 * none throws. No .m3g file and no device needed.
 *
 * HOW TO RUN (host)
 * =================
 *   WORK=/tmp/m3gchk
 *   rm -rf $WORK && mkdir -p $WORK/src $WORK/out
 *   cp -r jsr184/src/share/core/common/classes/* $WORK/src/
 *   cp debug/tools/m3gprobe/NativeBridgeHost.java $WORK/src/com/sun/m3g/NativeBridge.java
 *   cp debug/tools/m3gprobe/SetterChk.java $WORK/src/javax/microedition/m3g/
 *   cd $WORK/src
 *   javac -nowarn -encoding UTF-8 -cp <midp classes> -d $WORK/out \
 *       $(find . -name '*.java')
 *   java -cp $WORK/out:<midp classes> javax.microedition.m3g.SetterChk
 *
 * IMPORTANT javac TRAP: `static final int` constants are INLINED at compile
 * time, so this file must be compiled against the SAME classes it will run
 * against. Compiling it with a classpath that has an older PolygonMode.class
 * produces a test that reports the OLD values while the runtime uses the new
 * ones - it happened while writing this file and produced a false "still 176".
 *
 * Exit code 0 = every legal call accepted.
 */
public class SetterChk {

    static int fails = 0;

    static void tryIt(String what, Runnable r) {
        try {
            r.run();
        } catch (Throwable t) {
            fails++;
            System.out.println("THROWS " + what + " -> " + t);
        }
    }

    public static void main(String[] a) {
        /* ---- PolygonMode -------------------------------------------------
         * winding and fill deliberately share 168/169 (see the header). */
        final PolygonMode pm = new PolygonMode();
        final int[] cull = { PolygonMode.CULL_BACK, PolygonMode.CULL_FRONT,
                             PolygonMode.CULL_NONE };
        for (int i = 0; i < cull.length; i++) {
            final int v = cull[i];
            tryIt("PolygonMode.setCulling(" + v + ")",
                  new Runnable() { public void run() { pm.setCulling(v); } });
        }
        final int[] wind = { PolygonMode.WINDING_CCW, PolygonMode.WINDING_CW };
        for (int i = 0; i < wind.length; i++) {
            final int v = wind[i];
            tryIt("PolygonMode.setWinding(" + v + ")",
                  new Runnable() { public void run() { pm.setWinding(v); } });
        }
        final int[] fill = { PolygonMode.POINT, PolygonMode.LINE,
                             PolygonMode.FILL };
        for (int i = 0; i < fill.length; i++) {
            final int v = fill[i];
            tryIt("PolygonMode.setFillMode(" + v + ")",
                  new Runnable() { public void run() { pm.setFillMode(v); } });
        }
        final int[] shade = { PolygonMode.SHADE_FLAT, PolygonMode.SHADE_SMOOTH };
        for (int i = 0; i < shade.length; i++) {
            final int v = shade[i];
            tryIt("PolygonMode.setShading(" + v + ")",
                  new Runnable() { public void run() { pm.setShading(v); } });
        }

        /* ---- Texture2D ---------------------------------------------------
         * imageFilter may be NEAREST or LINEAR only; FILTER_BASE_LEVEL is a
         * levelFilter value. Passing BASE_LEVEL as an imageFilter is NOT a
         * legal call and this test must not claim otherwise. */
        final Texture2D t = new Texture2D(new Image2D(Image2D.RGB, 8, 8));
        final int[] wrap = { Texture2D.WRAP_CLAMP, Texture2D.WRAP_REPEAT };
        for (int i = 0; i < wrap.length; i++) {
            for (int j = 0; j < wrap.length; j++) {
                final int x = wrap[i], y = wrap[j];
                tryIt("Texture2D.setWrapping(" + x + "," + y + ")",
                      new Runnable() { public void run() { t.setWrapping(x, y); } });
            }
        }
        final int[] filt = { Texture2D.FILTER_NEAREST, Texture2D.FILTER_LINEAR };
        for (int i = 0; i < filt.length; i++) {
            final int v = filt[i];
            tryIt("Texture2D.setFiltering(BASE_LEVEL," + v + ")",
                  new Runnable() { public void run() {
                      t.setFiltering(Texture2D.FILTER_BASE_LEVEL, v); } });
        }
        final int[] blend = { Texture2D.FUNC_ADD, Texture2D.FUNC_SUBTRACT,
                              Texture2D.FUNC_MODULATE, Texture2D.FUNC_REPLACE,
                              Texture2D.FUNC_BLEND };
        for (int i = 0; i < blend.length; i++) {
            final int v = blend[i];
            tryIt("Texture2D.setBlending(" + v + ")",
                  new Runnable() { public void run() { t.setBlending(v); } });
        }

        /* ---- Light ------------------------------------------------------- */
        final Light lt = new Light();
        final int[] lmode = { Light.AMBIENT, Light.DIRECTIONAL, Light.OMNI,
                              Light.SPOT };
        for (int i = 0; i < lmode.length; i++) {
            final int v = lmode[i];
            tryIt("Light.setMode(" + v + ")",
                  new Runnable() { public void run() { lt.setMode(v); } });
        }

        /* ---- CompositingMode --------------------------------------------- */
        final CompositingMode cm = new CompositingMode();
        final int[] bl = { CompositingMode.ALPHA, CompositingMode.ALPHA_ADD,
                           CompositingMode.MODULATE, CompositingMode.MODULATE_X2,
                           CompositingMode.REPLACE };
        for (int i = 0; i < bl.length; i++) {
            final int v = bl[i];
            tryIt("CompositingMode.setBlending(" + v + ")",
                  new Runnable() { public void run() { cm.setBlending(v); } });
        }

        /* ---- Fog --------------------------------------------------------- */
        final Fog fog = new Fog();
        final int[] fm = { Fog.EXPONENTIAL, Fog.LINEAR };
        for (int i = 0; i < fm.length; i++) {
            final int v = fm[i];
            tryIt("Fog.setMode(" + v + ")",
                  new Runnable() { public void run() { fog.setMode(v); } });
        }

        System.out.println(fails == 0 ? "ALL PASS" : ("FAILURES: " + fails));
        System.exit(fails == 0 ? 0 : 1);
    }
}
