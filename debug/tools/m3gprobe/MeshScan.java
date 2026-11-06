package javax.microedition.m3g;

import java.io.*;

/**
 * Lists every Mesh in the given .m3g files with the numbers that decide whether
 * its vertices survive the decode: vertex count, position encode (bias/scale),
 * and the resulting extent. A mesh whose extent is zero is collapsed - all its
 * vertices decode to the same point, every triangle has zero area and nothing
 * can ever be rasterised, no matter how right the camera is.
 */
public class MeshScan {

    static byte[] readAll(String n) throws IOException {
        File f = new File(n);
        byte[] b = new byte[(int) f.length()];
        DataInputStream in = new DataInputStream(new FileInputStream(f));
        in.readFully(b);
        in.close();
        return b;
    }

    static String range(float[] v) {
        return String.format("[%.1f,%.1f]", v[0], v[1]);
    }

    public static void main(String[] args) throws Exception {
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
                VertexBuffer vb = m.vertexBuffer;
                if (vb == null || vb.positions == null) {
                    System.out.println("  mesh#" + i + " NO POSITIONS");
                    continue;
                }
                VertexArray pa = vb.positions;
                int n = vb.numVertices;
                float[] x = { 1e30f, -1e30f };
                float[] y = { 1e30f, -1e30f };
                float[] z = { 1e30f, -1e30f };
                int bad = 0;
                for (int k = 0; k < n; k++) {
                    for (int c = 0; c < 3; c++) {
                        float v = pa.componentRaw(k, c) * vb.positionScale
                                  + vb.positionBias[c];
                        if (Float.isNaN(v) || Float.isInfinite(v)) {
                            bad++;
                        }
                        if (c == 0) {
                            if (v < x[0]) x[0] = v;
                            if (v > x[1]) x[1] = v;
                        } else if (c == 1) {
                            if (v < y[0]) y[0] = v;
                            if (v > y[1]) y[1] = v;
                        } else {
                            if (v < z[0]) z[0] = v;
                            if (v > z[1]) z[1] = v;
                        }
                    }
                }
                boolean flat = (x[1] - x[0] == 0 && y[1] - y[0] == 0
                                && z[1] - z[0] == 0);
                System.out.println("  mesh#" + i
                    + " n=" + n
                    + " sub=" + (m.indexBuffers == null ? 0 : m.indexBuffers.length)
                    + " size=" + pa.componentSize
                    + " comps=" + pa.numComponents
                    + " scale=" + vb.positionScale
                    + " bias=" + vb.positionBias[0] + "," + vb.positionBias[1]
                    + "," + vb.positionBias[2]
                    + " x" + range(x) + " y" + range(y) + " z" + range(z)
                    + (flat ? "  <<< COLLAPSED" : "")
                    + (bad > 0 ? "  <<< BAD VALUES " + bad : ""));
            }
        }
    }
}
