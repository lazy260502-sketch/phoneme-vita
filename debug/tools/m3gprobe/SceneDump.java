package javax.microedition.m3g;

import java.io.*;

/**
 * For every Mesh in a .m3g, prints its world matrix and where its vertices land
 * in NDC under the camera the title builds (eye -9,90,180 / fovy 70 / near 1 /
 * far 10000, viewport 240x140).
 *
 * This exists because the device reported
 *     ndc n=472 behind=0 inside=472 x[0.04,0.04] y[-0.40,-0.40] z[0.98,0.98]
 * i.e. all 472 vertices at ONE point. MeshScan proved the raw vertex extents
 * are fine, so the collapse comes from the node transform - the one thing the
 * earlier host checks never exercised (they always used model = identity).
 */
public class SceneDump {

    static byte[] readAll(String n) throws IOException {
        File f = new File(n);
        byte[] b = new byte[(int) f.length()];
        DataInputStream in = new DataInputStream(new FileInputStream(f));
        in.readFully(b);
        in.close();
        return b;
    }

    static void report(String tag, Mesh m, float[] model, float[] mvp) {
        VertexBuffer vb = m.vertexBuffer;
        if (vb == null || vb.positions == null) {
            System.out.println("  " + tag + " NO POSITIONS");
            return;
        }
        VertexArray pa = vb.positions;
        int n = vb.numVertices;
        float nx0 = 1e30f, ny0 = 1e30f, nz0 = 1e30f;
        float nx1 = -1e30f, ny1 = -1e30f, nz1 = -1e30f;
        int behind = 0, inside = 0;
        for (int k = 0; k < n; k++) {
            float x = pa.componentRaw(k, 0) * vb.positionScale + vb.positionBias[0];
            float y = pa.componentRaw(k, 1) * vb.positionScale + vb.positionBias[1];
            float z = pa.componentRaw(k, 2) * vb.positionScale + vb.positionBias[2];
            float[] q = new float[4];
            com.sun.m3g.Matrix4.transformPoint(q, mvp, x, y, z);
            if (q[3] <= 1e-5f) {
                behind++;
                continue;
            }
            float a = q[0] / q[3], b = q[1] / q[3], c = q[2] / q[3];
            if (a < nx0) nx0 = a;
            if (a > nx1) nx1 = a;
            if (b < ny0) ny0 = b;
            if (b > ny1) ny1 = b;
            if (c < nz0) nz0 = c;
            if (c > nz1) nz1 = c;
            if (Math.abs(a) <= 1 && Math.abs(b) <= 1 && Math.abs(c) <= 1) {
                inside++;
            }
        }
        boolean flat3 = (model[0] == 0 && model[1] == 0 && model[2] == 0
                         && model[4] == 0 && model[5] == 0 && model[6] == 0
                         && model[8] == 0 && model[9] == 0 && model[10] == 0);
        System.out.printf("  %-22s n=%-5d behind=%-4d inside=%-4d"
                          + " ndc x[%7.3f,%7.3f] y[%7.3f,%7.3f] z[%6.3f,%6.3f]%n",
                          tag, n, behind, inside, nx0, nx1, ny0, ny1, nz0, nz1);
        System.out.printf("        world m0=%.4f m5=%.4f m10=%.4f"
                          + " t=(%.3f,%.3f,%.3f)%s%n",
                          model[0], model[5], model[10],
                          model[12], model[13], model[14],
                          flat3 ? "  <<< 3x3 IS ZERO (collapsed)" : "");
    }

    public static void main(String[] args) throws Exception {
        float[] cam = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, -9f, 90f, 180f, 1 };
        float[] view = new float[16];
        com.sun.m3g.Matrix4.invert(view, cam);
        float f = (float) (1.0 / Math.tan(70.0 * 0.5 * Math.PI / 180.0));
        float[] proj = { f / (240f / 140f), 0, 0, 0, 0, f, 0, 0, 0, 0,
                         (10000f + 1f) / (1f - 10000f), -1, 0, 0,
                         2 * 10000f * 1f / (1f - 10000f), 0 };
        float[] vp = new float[16];
        com.sun.m3g.Matrix4.mul(vp, proj, view);

        float[] rootTs = new float[16];
        com.sun.m3g.Matrix4.compose(rootTs, new float[] { 0, 0, 0 },
                                    new float[] { 0, 0, 0, 1 },
                                    new float[] { 0.012f, 0.012f, 0.012f });

        for (int a = 0; a < args.length; a++) {
            byte[] data = readAll(args[a]);
            com.sun.m3g.M3gFile file = com.sun.m3g.M3gFile.parse(data, 0);
            Object3D[] objs = M3gReader.readAll(data, file);
            System.out.println("=== " + new File(args[a]).getName()
                               + " objects=" + objs.length);
            for (int i = 0; i < objs.length; i++) {
                if (!(objs[i] instanceof Mesh)) {
                    continue;
                }
                Mesh m = (Mesh) objs[i];
                float[] world = new float[16];
                m.getWorldMatrix(world);

                float[] mvp = new float[16];
                com.sun.m3g.Matrix4.mul(mvp, vp, world);
                report("mesh#" + i + " world", m, world, mvp);

                float[] mvp2 = new float[16];
                float[] mm = new float[16];
                com.sun.m3g.Matrix4.mul(mm, rootTs, world);
                com.sun.m3g.Matrix4.mul(mvp2, vp, mm);
                report("mesh#" + i + " xS(0.012)", m, mm, mvp2);
            }
        }
    }
}
