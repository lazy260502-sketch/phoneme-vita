package javax.microedition.m3g;

import java.io.*;

/**
 * Runs the REAL port code (Graphics3D + Renderer) on a desktop JVM against a
 * .m3g file, with com.sun.m3g.NativeBridge replaced by a recording stub.
 *
 * Why this exists: every "draws but nothing appears" report so far needed a
 * device round trip to get one line of evidence. This walks the same Java path
 * the Vita runs and prints, per draw call, where the vertices land in NDC plus
 * the first/last raw vertex - so a collapsed vertex array or a degenerate model
 * matrix is visible without flashing anything.
 *
 * Build (see README.md in this folder): compile a copy of
 * jsr184/src/share/core/common/classes with NativeBridgeHost.java installed as
 * com/sun/m3g/NativeBridge.java.
 */
public class HostRun {

    static byte[] readAll(String n) throws IOException {
        File f = new File(n);
        byte[] b = new byte[(int) f.length()];
        DataInputStream in = new DataInputStream(new FileInputStream(f));
        in.readFully(b);
        in.close();
        return b;
    }

    static void run(String tag, Node root, Transform camT, Transform rootT,
                    float fovy, int vpY, int vpH) {
        com.sun.m3g.NativeBridge.draws = 0;
        com.sun.m3g.NativeBridge.jit.clear();

        Image2D tgt = new Image2D(Image2D.RGBA, 240, 320);
        Graphics3D g = Graphics3D.getInstance();
        g.bindTarget(tgt, true, 0);
        g.setViewport(0, vpY, 240, vpH);

        Camera cam = new Camera();
        cam.setPerspective(fovy, 1.3333334f, 1.0f, 10000.0f);
        g.setCamera(cam, camT);

        Background bg = new Background();
        bg.setColor(0x000000);
        g.clear(bg);

        g.render(root, rootT);
        g.releaseTarget();

        System.out.println("== " + tag + "  draws=" + com.sun.m3g.NativeBridge.draws);
        int max = Math.min(com.sun.m3g.NativeBridge.jit.size(), 24);
        for (int i = 0; i < max; i++) {
            System.out.println(com.sun.m3g.NativeBridge.jit.get(i));
        }
        if (com.sun.m3g.NativeBridge.jit.size() > max) {
            System.out.println("    ... " + (com.sun.m3g.NativeBridge.jit.size() - max)
                               + " more lines");
        }
    }

    /* v04: sweep EVERY model the title loads and report which draw collapses.
     * The device reported n=472 collapsing onto ONE NDC point while a host
     * sweep of missionmap (the only model with 472 vertices) shows a spread, so
     * either another model also has 472 vertices or the collapse depends on the
     * node matrix. Running all of them with identity root settles it. */
    static void sweep(String path) throws Exception {
        byte[] data = readAll(path);
        com.sun.m3g.M3gFile file = com.sun.m3g.M3gFile.parse(data, 0);
        Object3D[] objs = M3gReader.readAll(data, file);
        Image2D tgt = new Image2D(Image2D.RGBA, 240, 320);
        Graphics3D g = Graphics3D.getInstance();
        g.bindTarget(tgt, true, 0);
        g.setViewport(0, 105, 240, 140);
        Camera cam = new Camera();
        cam.setPerspective(70.0f, 1.3333334f, 1.0f, 10000.0f);
        Transform camT = new Transform();
        camT.postTranslate(-9.0f, 90.0f, 180.0f);
        g.setCamera(cam, camT);
        Background bg = new Background();
        bg.setColor(0);
        com.sun.m3g.NativeBridge.draws = 0;
        com.sun.m3g.NativeBridge.jit.clear();
        for (int i = 0; i < objs.length; i++) {
            if (!(objs[i] instanceof Node)) {
                continue;
            }
            g.clear(bg);
            g.render((Node) objs[i], null);
        }
        g.releaseTarget();
        String name = new File(path).getName();
        for (int i = 0; i < com.sun.m3g.NativeBridge.jit.size(); i++) {
            String line = (String) com.sun.m3g.NativeBridge.jit.get(i);
            if (line.indexOf("draw#") < 0) {
                continue;
            }
            /* flag collapsed draws: extent of x/y/z all zero after formatting */
            java.util.StringTokenizer st = new java.util.StringTokenizer(line, "[,]");
            if (line.indexOf("x[  0.000,  0.000]") >= 0
                || line.indexOf("inside=0 ") >= 0) {
                System.out.println(name + " COLLAPSED? " + line);
            }
            if (line.indexOf("n=472 ") >= 0) {
                System.out.println(name + " n=472      " + line);
            }
        }
    }

    public static void main(String[] args) throws Exception {
        if (args.length > 1 && args[0].equals("sweep")) {
            for (int i = 1; i < args.length; i++) {
                try {
                    sweep(args[i]);
                } catch (Throwable t) {
                    System.out.println(new File(args[i]).getName() + " THREW " + t);
                }
            }
            return;
        }
        byte[] data = readAll(args[0]);
        com.sun.m3g.M3gFile file = com.sun.m3g.M3gFile.parse(data, 0);
        Object3D[] objs = M3gReader.readAll(data, file);
        Node root = null;
        for (int i = 0; i < objs.length && root == null; i++) {
            if (objs[i] instanceof Node) {
                root = (Node) objs[i];
            }
        }
        System.out.println("file=" + new File(args[0]).getName()
                           + " objects=" + objs.length
                           + " root=" + root.getClass().getName());

        /* Camera transform exactly as log140 reported it:
         *     [m3g] cam xform eye=-9.0,90.0,180.0 m00=1.0 m11=1.0 m22=1.0
         * fovy from the bytecode: camFOV*7/9 with the game's FOV. 70*7/9 is
         * one candidate; the other call site uses a plain 70. Try both. */
        Transform camT = new Transform();
        camT.postTranslate(-9.0f, 90.0f, 180.0f);

        /* Root transform exactly as log140 reported it:
         *     [m3g] root t=0.0,0.0,0.0 s=0.67,0.67,0.67  */
        Transform rootDevice = new Transform();
        rootDevice.postScale(0.67f, 0.67f, 0.67f);

        /* And the transform the bytecode builds for the overlay mesh:
         *     postTranslate; postScale(0.012)   */
        Transform rootTitle = new Transform();
        rootTitle.postTranslate(0.0f, 0.0f, 0.0f);
        rootTitle.postScale(0.012f, 0.012f, 0.012f);

        for (int i = 0; i < 2; i++) {
            float fovy = (i == 0) ? 70.0f : 70.0f * 7.0f / 9.0f;
            System.out.println("\n---- fovy=" + fovy + " ----");
            run("root=identity", root, camT, null, fovy, 105, 140);
            run("root=S(0.67) [device-reported]", root, camT, rootDevice, fovy, 105, 140);
            run("root=T*S(0.012) [title overlay]", root, camT, rootTitle, fovy, 105, 140);
        }
    }
}
