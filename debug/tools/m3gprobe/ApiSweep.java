package javax.microedition.m3g;

/**
 * Calls every JSR-184 API entry point that 极品飞车 (and the other sample
 * titles) actually reference, with legal arguments, and fails on any throw.
 *
 * WHY: five device-only freezes in this port were all "a legal API call threw"
 * (FUNC_BLEND, WINDING, SPOT, setLayer, and now Camera.getProjection). The list
 * of calls below was extracted from the titles' own constant pools
 * (`javap -v | grep '// javax/microedition/m3g/'`), so it covers exactly what a
 * real game touches - not what the spec has.
 *
 * Run it before shipping any change to javax.microedition.m3g.
 */
public class ApiSweep {
    static int fails = 0;
    static int calls = 0;

    static void c(String what, Runnable r) {
        calls++;
        try {
            r.run();
        } catch (Throwable t) {
            fails++;
            System.out.println("THROWS  " + what + "  ->  " + t);
        }
    }

    public static void main(String[] a) {
        final Camera cam = new Camera();
        final Transform tr = new Transform();
        final World world = new World();
        final Group grp = new Group();
        final Mesh mesh = null;   /* only exercised through loaded files */
        final Image2D img = new Image2D(Image2D.RGB, 8, 8);
        final Texture2D tex = new Texture2D(img);
        final Appearance app = new Appearance();
        final CompositingMode cm = new CompositingMode();
        final PolygonMode pm = new PolygonMode();
        final Background bg = new Background();
        final Node node = new Group();

        /* Camera -- the log185 site is getProjection(Transform) on a
         * PERSPECTIVE camera; the rest is what the titles filter on. */
        c("Camera.setPerspective", new Runnable() { public void run() { cam.setPerspective(70f, 0.75f, 1f, 1000f); } });
        c("Camera.getProjection(Transform) after setPerspective",
          new Runnable() { public void run() { cam.getProjection(new Transform()); } });
        c("Camera.getProjection(float[]) after setPerspective",
          new Runnable() { public void run() { cam.getProjection(new float[4]); } });
        c("Camera.getProjection(null)", new Runnable() { public void run() { cam.getProjection((Transform)null); } });
        c("Camera.setParallel", new Runnable() { public void run() { cam.setParallel(2f, 0.75f, 1f, 100f); } });
        c("Camera.getProjection(Transform) after setParallel",
          new Runnable() { public void run() { cam.getProjection(new Transform()); } });
        c("Camera.setGeneric", new Runnable() { public void run() { cam.setGeneric(new Transform()); } });
        c("Camera.getProjection(Transform) after setGeneric",
          new Runnable() { public void run() { cam.getProjection(new Transform()); } });

        /* Appearance / modes */
        c("Appearance.setLayer(2)", new Runnable() { public void run() { app.setLayer(2); } });
        c("Appearance.setCompositingMode", new Runnable() { public void run() { app.setCompositingMode(cm); } });
        c("Appearance.getCompositingMode", new Runnable() { public void run() { app.getCompositingMode(); } });
        c("Appearance.setPolygonMode", new Runnable() { public void run() { app.setPolygonMode(pm); } });
        c("Appearance.getPolygonMode", new Runnable() { public void run() { app.getPolygonMode(); } });
        c("Appearance.setTexture(0,..)", new Runnable() { public void run() { app.setTexture(0, tex); } });
        c("Appearance.getTexture(0)", new Runnable() { public void run() { app.getTexture(0); } });

        c("CompositingMode.setBlending(ALPHA)", new Runnable() { public void run() { cm.setBlending(CompositingMode.ALPHA); } });
        c("CompositingMode.getBlending", new Runnable() { public void run() { cm.getBlending(); } });
        c("CompositingMode.setAlphaThreshold(0)", new Runnable() { public void run() { cm.setAlphaThreshold(0f); } });
        c("CompositingMode.setDepthTestEnable", new Runnable() { public void run() { cm.setDepthTestEnable(true); } });
        c("CompositingMode.setDepthWriteEnable", new Runnable() { public void run() { cm.setDepthWriteEnable(false); } });

        c("PolygonMode.setWinding(CW)", new Runnable() { public void run() { pm.setWinding(PolygonMode.WINDING_CW); } });
        c("PolygonMode.setWinding(CCW)", new Runnable() { public void run() { pm.setWinding(PolygonMode.WINDING_CCW); } });
        c("PolygonMode.setCulling(ALL)", new Runnable() { public void run() {
            pm.setCulling(PolygonMode.CULL_BACK); pm.setCulling(PolygonMode.CULL_FRONT); pm.setCulling(PolygonMode.CULL_NONE); } });
        c("PolygonMode.setShading(ALL)", new Runnable() { public void run() {
            pm.setShading(PolygonMode.SHADE_FLAT); pm.setShading(PolygonMode.SHADE_SMOOTH); } });
        c("PolygonMode.setFillMode(ALL)", new Runnable() { public void run() {
            pm.setFillMode(PolygonMode.POINT); pm.setFillMode(PolygonMode.LINE); pm.setFillMode(PolygonMode.FILL); } });

        c("Texture2D.setImage", new Runnable() { public void run() { tex.setImage(img); } });
        c("Texture2D.setBlending(FUNC_BLEND)", new Runnable() { public void run() { tex.setBlending(Texture2D.FUNC_BLEND); } });

        c("Background.setColor", new Runnable() { public void run() { bg.setColor(0x102030); } });
        c("Background.setColorClearEnable", new Runnable() { public void run() { bg.setColorClearEnable(true); } });
        c("Background.setDepthClearEnable", new Runnable() { public void run() { bg.setDepthClearEnable(true); } });

        /* Graph */
        c("World.setActiveCamera", new Runnable() { public void run() { world.setActiveCamera(cam); } });
        c("World.setBackground", new Runnable() { public void run() { world.setBackground(bg); } });
        c("Group.addChild", new Runnable() { public void run() { grp.addChild(node); } });
        c("Group.getChildCount", new Runnable() { public void run() { grp.getChildCount(); } });
        c("Group.getChild(0)", new Runnable() { public void run() { grp.getChild(0); } });
        c("Group.removeChild", new Runnable() { public void run() { grp.removeChild(node); } });
        c("Node.getParent", new Runnable() { public void run() { node.getParent(); } });
        c("Node.setAlphaFactor", new Runnable() { public void run() { node.setAlphaFactor(1.0f); } });
        c("Node.setRenderingEnable", new Runnable() { public void run() { node.setRenderingEnable(false); node.setRenderingEnable(true); } });
        c("Node.getTransformTo(self)", new Runnable() { public void run() { node.getTransformTo(node, new Transform()); } });

        /* Object3D */
        c("Object3D.setUserID/getUserID", new Runnable() { public void run() { node.setUserID(7); node.getUserID(); } });
        c("Object3D.find(7)", new Runnable() { public void run() { node.find(7); } });
        c("Object3D.duplicate", new Runnable() { public void run() { node.duplicate(); } });
        c("Object3D.getAnimationTrackCount", new Runnable() { public void run() { node.getAnimationTrackCount(); } });
        c("Object3D.animate", new Runnable() { public void run() { node.animate(0); } });

        /* Transformable */
        c("Transformable.setTranslation/getTranslation", new Runnable() { public void run() {
            node.setTranslation(1f, 2f, 3f); node.getTranslation(new float[3]); } });
        c("Transformable.setScale/getScale", new Runnable() { public void run() {
            node.setScale(2f, 2f, 2f); node.getScale(new float[3]); } });
        c("Transformable.setOrientation/getOrientation", new Runnable() { public void run() {
            node.setOrientation(30f, 0f, 0f, 1f); node.getOrientation(new float[4]); } });
        c("Transformable.setTransform", new Runnable() { public void run() { node.setTransform(new Transform()); } });

        /* Transform */
        c("Transform.set/getIdentity", new Runnable() { public void run() { tr.setIdentity(); tr.set(new Transform()); } });
        c("Transform.postTranslate/Rotate/Scale", new Runnable() { public void run() {
            tr.postTranslate(1f, 2f, 3f); tr.postRotate(45f, 0f, 1f, 0f); tr.postScale(1f, 1f, 1f); } });
        c("Transform.postMultiply", new Runnable() { public void run() { tr.postMultiply(new Transform()); } });
        c("Transform.invert", new Runnable() { public void run() { tr.invert(); } });
        c("Transform.transform", new Runnable() { public void run() { tr.transform(new float[12]); } });

        /* Image2D */
        c("Image2D.set", new Runnable() { public void run() { img.set(0, 0, 8, 8, new byte[8 * 8 * 3]); } });

        System.out.println(calls + " calls, " + fails + " threw");
        System.out.println(fails == 0 ? "ALL PASS" : "FAILURES: " + fails);
        if (fails != 0) System.exit(1);
    }
}
