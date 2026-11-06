package javax.microedition.m3g;

import java.io.*;

/**
 * Replays ONE gameplay frame of log146/log147 (viewport 0,40 240x180) through the REAL Renderer on a desktop JVM:
 * the camera matrix and the root transform are the values the device logged ("gcam#"/"gmesh#"), the model is a
 * .m3g from the game jar. Prints, per draw, where the vertices land in NDC (see NativeBridgeHost).
 *
 *   java -Dm3gcam=0|1|2 -cp out:<device classes> javax.microedition.m3g.GameplayRun jill_han.m3g
 *
 * Camera rows are the printed r0/r1/r2 = m[0],m[4],m[8] / m[1],m[5],m[9] / m[2],m[6],m[10] of the camera-world matrix.
 */
public class GameplayRun {

    static float f(String hex) {
        return Float.intBitsToFloat((int) Long.parseLong(hex, 16));
    }

    static byte[] readAll(String n) throws IOException {
        File x = new File(n);
        byte[] b = new byte[(int) x.length()];
        DataInputStream in = new DataInputStream(new FileInputStream(x));
        in.readFully(b);
        in.close();
        return b;
    }

    public static void main(String[] a) throws Exception {
        byte[] data = readAll(a[0]);
        com.sun.m3g.M3gFile file = com.sun.m3g.M3gFile.parse(data, 0);
        Object3D[] objs = M3gReader.readAll(data, file);
        Node root = null;
        for (int i = 0; i < objs.length && root == null; i++) {
            if (objs[i] instanceof Node) {
                root = (Node) objs[i];
            }
        }

        /* log146 gcam#1: camera-world rows exactly as logged, translation 0, w row 0,0,0,1. */
        float[] row0 = { f("3eb74000"), f("3f4d1000"), f("3ef56000") };
        float[] row1 = { f("3ef56000"), f("bf193000"), f("3f244000") };
        float[] row2 = { f("3f4d1000"), 0.0f, f("bf193000") };
        float[] mat = {
            row0[0], row0[1], row0[2], 0,
            row1[0], row1[1], row1[2], 0,
            row2[0], row2[1], row2[2], 0,
            0, 0, 0, 1
        };
        Transform cam = new Transform();
        if (System.getProperty("IDENT") != null) {
            cam.setIdentity();   /* objects already in camera space */
        } else if (System.getProperty("CAMROT") != null) {
            /* log147: rotationX(0), rotationY(2048), rotationZ(2048) with the FIXED Transform.postRotate = diag(1,-1,-1). */
            cam.set(new float[] { 1, 0, 0, 0,  0, -1, 0, 0,  0, 0, -1, 0,  0, 0, 0, 1 });
        } else {
            cam.set(mat);
        }

        /* log146 gmesh#3 (Jill): model m=0,38e978d5,0 -> scale (0, 0.0011, 0); t=c0985800,3ea00000,c0895800. The exact
         * per-axis scale/rotation is not recoverable from the diagonal alone, so run the three Jill roots the log shows. */
        float[][] roots = {
            { -4.76f, 0.31f, -4.29f, 0.0055f, 0.0011f, 0.0005f },
            { -4.76f, 0.31f, -4.29f, 0.0011f, 0.0011f, 0.0011f },
            { -4.76f, 0.31f, -4.29f, 0.012f, 0.012f, 0.012f },
        };
        for (int r = 0; r < roots.length; r++) {
            com.sun.m3g.NativeBridge.draws = 0;
            com.sun.m3g.NativeBridge.jit.clear();
            Image2D tgt = new Image2D(Image2D.RGBA, 240, 320);
            Graphics3D g = Graphics3D.getInstance();
            g.bindTarget(tgt, true, 0);
            g.setViewport(0, 40, 240, 180);
            Camera c = new Camera();
            c.setPerspective(70.0f * 7.0f / 9.0f, 1.3333334f, 1.0f, 10000.0f);
            g.setCamera(c, cam);
            Transform t = new Transform();
            t.postTranslate(roots[r][0], roots[r][1], roots[r][2]);
            t.postScale(roots[r][3], roots[r][4], roots[r][5]);
            g.render(root, t);
            g.releaseTarget();
            System.out.println("== root t=(" + roots[r][0] + "," + roots[r][1] + "," + roots[r][2] + ") s=("
                + roots[r][3] + "," + roots[r][4] + "," + roots[r][5] + ")  draws=" + com.sun.m3g.NativeBridge.draws);
            int max = Math.min(com.sun.m3g.NativeBridge.jit.size(), 4);
            for (int i = 0; i < max; i++) {
                System.out.println(com.sun.m3g.NativeBridge.jit.get(i));
            }
        }
    }
}
