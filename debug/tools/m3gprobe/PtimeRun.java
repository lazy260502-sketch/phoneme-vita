package javax.microedition.m3g;

import java.io.*;

public class PtimeRun {
    public static void main(String[] a) throws Exception {
        File x = new File(a[0]);
        byte[] data = new byte[(int) x.length()];
        DataInputStream in = new DataInputStream(new FileInputStream(x));
        in.readFully(data);
        in.close();
        com.sun.m3g.M3gFile file = com.sun.m3g.M3gFile.parse(data, 0);
        Object3D[] objs = M3gReader.readAll(data, file);
        Node root = null;
        for (int i = 0; i < objs.length && root == null; i++) {
            if (objs[i] instanceof Mesh) root = (Node) objs[i];
        }
        Image2D tgt = new Image2D(Image2D.RGBA, 240, 320);
        Graphics3D g = Graphics3D.getInstance();
        Camera c = new Camera();
        c.setPerspective(54.0f, 1.3333334f, 1.0f, 10000.0f);
        Transform cam = new Transform();
        for (int f = 0; f < 45; f++) {
            g.bindTarget(tgt, true, 0);
            g.setViewport(0, 40, 240, 180);
            g.setCamera(c, cam);
            Transform t = new Transform();
            /* walks 0.03 per frame along x, so trackRoot's 100/1000 threshold is crossed every ~4 frames */
            t.postTranslate(-4.76f + 0.03f * f, 0.31f, -4.29f);
            t.postScale(0.012f, 0.012f, 0.012f);
            if (f == 0) System.out.println("ROOT=" + root.getClass().getName()); g.render(root, t);
            g.releaseTarget();
            Thread.sleep(30);
        }
    }
}
