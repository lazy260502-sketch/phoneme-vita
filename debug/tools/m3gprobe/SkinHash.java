package javax.microedition.m3g;

import java.io.*;

/**
 * Bit-exact regression check for the skinning path: loads a .m3g, animates it through a fixed time sequence (with
 * repeated times and a mid-run TRS edit, so cached local matrices must hit AND invalidate) and prints a checksum of
 * the skinned positions after every step. Run it against a baseline build and a modified build; the output must be
 * identical. Usage: SkinHash file.m3g
 */
public class SkinHash {
    public static void main(String[] a) throws Exception {
        File x = new File(a[0]);
        byte[] data = new byte[(int) x.length()];
        DataInputStream in = new DataInputStream(new FileInputStream(x));
        in.readFully(data);
        in.close();
        com.sun.m3g.M3gFile file = com.sun.m3g.M3gFile.parse(data, 0);
        Object3D[] objs = M3gReader.readAll(data, file);
        SkinnedMesh sm = null;
        for (int i = 0; i < objs.length && sm == null; i++) {
            if (objs[i] instanceof SkinnedMesh) sm = (SkinnedMesh) objs[i];
        }
        VertexBuffer vb = sm.vertexBuffer;
        int n = vb.numVertices;
        float ps = vb.positionScale;
        float[] pb = vb.positionBias;
        int[] times = { 0, 0, 400, 400, 1500, 1500, 1500, 3000, 4300, 4300, 5733, 100, 100, 2500 };
        long all = 1469598103934665603L;
        for (int s = 0; s < times.length; s++) {
            sm.animate(times[s]);
            /* edits after animate(), or the animation tracks would overwrite them */
            if (s == 9) {
                Node b = sm.bones[sm.bones.length / 2];
                b.setTranslation(b.translation[0] + 1.5f, b.translation[1], b.translation[2] - 0.25f);
                b.setOrientation(37.0f, 0.0f, 1.0f, 0.0f);
            }
            if (s == 12) {
                Node b = sm.bones[sm.bones.length / 3];
                Transform ut = new Transform();
                ut.postScale(1.0f, 1.1f, 1.0f);
                b.setTransform(ut);
            }
            float[] pos = new float[n * 3];
            for (int i = 0; i < n; i++) {
                for (int c = 0; c < 3; c++) {
                    pos[i * 3 + c] = vb.positions.componentRaw(i, c) * ps + pb[c];
                }
            }
            sm.applySkin(pos, n);
            long h = 1469598103934665603L;
            for (int i = 0; i < pos.length; i++) {
                float v = pos[i];
                if (v == 0.0f) v = 0.0f;               /* -0 and +0 are the same position */
                h = (h ^ Float.floatToIntBits(v)) * 1099511628211L;
            }
            all = (all ^ h) * 1099511628211L;
            System.out.println("step " + s + " t=" + times[s] + " hash=" + Long.toHexString(h));
        }
        System.out.println("ALL " + Long.toHexString(all));
    }
}
