package javax.microedition.m3g;

import com.sun.m3g.NativeBridge;

/**
 * v04.02: asserts that Renderer actually forwards CompositingMode to the
 * backend. Before v04.02 setState(bool cull, bool blend) collapsed five blend
 * equations into "blend on/off", so this test could not exist.
 *
 * Builds a single triangle twice - once with default CompositingMode, once
 * with a non-default one - and prints what NativeBridgeHost recorded.
 *
 * No .m3g file needed: the scene is built through the public API, which is the
 * same path a game takes.
 *
 * Usage: BlendStateRun
 */
public class BlendStateRun {

    private static Mesh triangle(CompositingMode cm) {
        VertexBuffer vb = new VertexBuffer();
        VertexArray pos = new VertexArray(3, 3, 2);
        pos.set(0, 1, new byte[] { 1, 0, 0, 1, 0, 0, 1, 0, 0 });
        vb.setPositions(pos, 1.0f, new float[] { 0, 0, 0 });

        TriangleStripArray idx = new TriangleStripArray(0, new int[] { 3 });

        Appearance ap = new Appearance();
        if (cm != null) {
            ap.setCompositingMode(cm);
        }
        Mesh m = new Mesh(vb, (IndexBuffer)idx, ap);
        return m;
    }

    private static void runCase(String name, CompositingMode cm) {
        Mesh m = triangle(cm);
        Image2D tgt = new Image2D(Image2D.RGBA, 64, 64);
        Graphics3D g = Graphics3D.getInstance();
        Camera c = new Camera();
        c.setPerspective(45.0f, 1.0f, 1.0f, 100.0f);
        Transform cam = new Transform();
        g.bindTarget(tgt, true, 0);
        g.setViewport(0, 0, 64, 64);
        g.setCamera(c, cam);
        Transform t = new Transform();
        t.postTranslate(0.0f, 0.0f, -5.0f);
        g.render(m, t);
        g.releaseTarget();
        System.out.println(name
            + " blendMode=" + NativeBridge.lastBlendMode
            + " depthTest=" + NativeBridge.lastDepthTest
            + " depthWrite=" + NativeBridge.lastDepthWrite
            + " colorWrite=" + NativeBridge.lastColorWrite
            + " alphaThreshold=" + NativeBridge.lastAlphaThreshold
            + " blendColor=0x" + Integer.toHexString(NativeBridge.lastBlendColor));
    }

    public static void main(String[] args) {
        int fails = 0;

        /* 1. No CompositingMode at all -> M3G defaults. */
        NativeBridge.lastBlendMode = -1;
        runCase("default", null);
        if (NativeBridge.lastBlendMode != CompositingMode.REPLACE) {
            System.out.println("FAIL default blendMode=" + NativeBridge.lastBlendMode);
            fails++;
        }
        if (!NativeBridge.lastDepthTest || !NativeBridge.lastDepthWrite
                || !NativeBridge.lastColorWrite
                || NativeBridge.lastAlphaThreshold != 0.0f) {
            System.out.println("FAIL default flags");
            fails++;
        }

        /* 2. Every blending mode must reach the backend unchanged. */
        int[] modes = { CompositingMode.ALPHA, CompositingMode.ALPHA_ADD,
                        CompositingMode.MODULATE, CompositingMode.MODULATE_X2,
                        CompositingMode.REPLACE };
        for (int i = 0; i < modes.length; i++) {
            CompositingMode cm = new CompositingMode();
            cm.setBlending(modes[i]);
            runCase("mode" + modes[i], cm);
            if (NativeBridge.lastBlendMode != modes[i]) {
                System.out.println("FAIL mode " + modes[i] + " -> " + NativeBridge.lastBlendMode);
                fails++;
            }
        }

        /* 3. Depth flags + alpha test + blend colour must survive the trip. */
        CompositingMode cm = new CompositingMode();
        cm.setBlending(CompositingMode.MODULATE);
        cm.setDepthTestEnable(false);
        cm.setDepthWriteEnable(false);
        cm.setColorWriteEnable(false);
        cm.setAlphaThreshold(0.5f);
        cm.setBlendColor(0x00FF8000);
        runCase("flags", cm);
        if (NativeBridge.lastDepthTest || NativeBridge.lastDepthWrite
                || NativeBridge.lastColorWrite) {
            System.out.println("FAIL flags not forwarded");
            fails++;
        }
        if (NativeBridge.lastAlphaThreshold != 0.5f) {
            System.out.println("FAIL threshold=" + NativeBridge.lastAlphaThreshold);
            fails++;
        }
        if (NativeBridge.lastBlendColor != 0x00FF8000) {
            System.out.println("FAIL blendColor=0x"
                + Integer.toHexString(NativeBridge.lastBlendColor));
            fails++;
        }

        System.out.println(fails == 0 ? "ALL PASS" : ("FAILURES: " + fails));
        System.exit(fails == 0 ? 0 : 1);
    }
}
