package javax.microedition.m3g;

import java.io.*;

/**
 * render(node, T) with the log150 camera and root transform; prints the NDC extent of every draw so the size of the
 * model on screen can be compared before and after the node-transformation rule (v03.85).
 * Usage: NodeScaleRun file.m3g
 */
public class NodeScaleRun {
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
            if (objs[i] instanceof SkinnedMesh) root = (Node) objs[i];
        }
        float[] own = new float[16];
        root.getCompositeMatrix(own);
        System.out.println("node own scale m0=" + own[0] + " m5=" + own[5] + " m10=" + own[10]);

        Image2D tgt = new Image2D(Image2D.RGBA, 240, 320);
        Graphics3D g = Graphics3D.getInstance();
        Camera c = new Camera();
        c.setPerspective(54.0f, 1.3333334f, 1.0f, 10000.0f);
        Transform cam = new Transform();
        cam.set(new float[] { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 });
        g.bindTarget(tgt, true, 0);
        g.setViewport(0, 40, 240, 180);
        g.setCamera(c, cam);
        Transform t = new Transform();
        t.postTranslate(-1.92f, 0.83f, -6.07f);
        t.postScale(0.012f, 0.012f, 0.012f);
        g.render(root, t);
        g.releaseTarget();
        for (int i = 0; i < com.sun.m3g.NativeBridge.jit.size(); i++) {
            String s = (String) com.sun.m3g.NativeBridge.jit.get(i);
            if (s.indexOf("draw#") >= 0) System.out.println(s);
        }
    }
}
