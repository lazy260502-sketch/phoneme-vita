package javax.microedition.m3g;

import java.io.*;

/**
 * HOST-side repro of the device probe. Loads a real .m3g with the SAME reader
 * the port uses, then evaluates the exact camera the title's bytecode builds,
 * so the "behind / inside / winding" numbers can be known before flashing.
 */
public class HostProbe {

    static byte[] readAll(String n) throws IOException {
        File f = new File(n);
        byte[] b = new byte[(int) f.length()];
        DataInputStream in = new DataInputStream(new FileInputStream(f));
        in.readFully(b);
        in.close();
        return b;
    }

    static float[] mul(float[] a, float[] b) {
        float[] r = new float[16];
        for (int c = 0; c < 4; c++) {
            for (int rw = 0; rw < 4; rw++) {
                float s = 0;
                for (int k = 0; k < 4; k++) {
                    s += a[k * 4 + rw] * b[c * 4 + k];
                }
                r[c * 4 + rw] = s;
            }
        }
        return r;
    }

    static float[] translate(float x, float y, float z) {
        float[] m = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
        m[12] = x;
        m[13] = y;
        m[14] = z;
        return m;
    }

    /** JSR-184 setPerspective: fovy in DEGREES, standard GL matrix. */
    static float[] proj(float fovyDeg, float aspect, float near, float far) {
        float[] m = new float[16];
        float f = 1.0f / (float) Math.tan(fovyDeg * 0.5 * Math.PI / 180.0);
        m[0] = f / aspect;
        m[5] = f;
        m[10] = (far + near) / (near - far);
        m[11] = -1.0f;
        m[14] = 2.0f * far * near / (near - far);
        return m;
    }

    static void eval(String tag, float[] mvp, float[] pos, int n, int[] idx) {
        float nx0 = 1e30f, ny0 = 1e30f, nz0 = 1e30f;
        float nx1 = -1e30f, ny1 = -1e30f, nz1 = -1e30f;
        int behind = 0, inside = 0;
        for (int i = 0; i < n; i++) {
            float x = pos[i * 3], y = pos[i * 3 + 1], z = pos[i * 3 + 2];
            float cx = mvp[0] * x + mvp[4] * y + mvp[8] * z + mvp[12];
            float cy = mvp[1] * x + mvp[5] * y + mvp[9] * z + mvp[13];
            float cz = mvp[2] * x + mvp[6] * y + mvp[10] * z + mvp[14];
            float cw = mvp[3] * x + mvp[7] * y + mvp[11] * z + mvp[15];
            if (cw <= 1e-5f) {
                behind++;
                continue;
            }
            float a = cx / cw, b = cy / cw, c = cz / cw;
            if (a < nx0) nx0 = a;
            if (a > nx1) nx1 = a;
            if (b < ny0) ny0 = b;
            if (b > ny1) ny1 = b;
            if (c < nz0) nz0 = c;
            if (c > nz1) nz1 = c;
            if (a >= -1 && a <= 1 && b >= -1 && b <= 1 && c >= -1 && c <= 1) {
                inside++;
            }
        }
        int posArea = 0, negArea = 0;
        if (idx != null) {
            for (int t = 0; t + 2 < idx.length; t += 3) {
                float[] v = new float[6];
                boolean ok = true;
                for (int k = 0; k < 3; k++) {
                    int vi = idx[t + k];
                    float x = pos[vi * 3], y = pos[vi * 3 + 1], z = pos[vi * 3 + 2];
                    float w = mvp[3] * x + mvp[7] * y + mvp[11] * z + mvp[15];
                    if (w <= 1e-5f) {
                        ok = false;
                        break;
                    }
                    v[k * 2] = (mvp[0] * x + mvp[4] * y + mvp[8] * z + mvp[12]) / w;
                    v[k * 2 + 1] = (mvp[1] * x + mvp[5] * y + mvp[9] * z + mvp[13]) / w;
                }
                if (!ok) {
                    continue;
                }
                float cross = (v[2] - v[0]) * (v[5] - v[1])
                            - (v[3] - v[1]) * (v[4] - v[0]);
                if (cross > 0) {
                    posArea++;
                } else if (cross < 0) {
                    negArea++;
                }
            }
        }
        System.out.printf(
            "  %-26s behind=%-4d inside=%-4d x[%8.2f,%8.2f] y[%8.2f,%8.2f] "
            + "z[%6.2f,%6.2f] posArea=%-4d negArea=%-4d%n",
            tag, behind, inside, nx0, nx1, ny0, ny1, nz0, nz1, posArea, negArea);
    }

    public static void main(String[] args) throws Exception {
        byte[] data = readAll(args[0]);
        com.sun.m3g.M3gFile file = com.sun.m3g.M3gFile.parse(data, 0);
        Object3D[] objs = M3gReader.readAll(data, file);
        System.out.println("objects=" + objs.length + " from " + args[0]);

        /* The first mesh with a position array - the same one the device probe
         * reported as "ndc n=472 tris=708". */
        Mesh mesh = null;
        for (int i = 0; i < objs.length && mesh == null; i++) {
            if (objs[i] instanceof Mesh) {
                Mesh m = (Mesh) objs[i];
                if (m.vertexBuffer != null && m.vertexBuffer.positions != null
                    && m.indexBuffers != null && m.indexBuffers.length > 0) {
                    mesh = m;
                }
            }
        }
        if (mesh == null) {
            System.out.println("no mesh found");
            return;
        }
        VertexBuffer vb = mesh.vertexBuffer;
        int n = vb.numVertices;
        float[] pos = new float[n * 3];
        VertexArray vp = vb.positions;
        float ps = vb.positionScale;
        float[] pb = vb.positionBias;
        float x0 = 1e30f, x1 = -1e30f, y0 = 1e30f, y1 = -1e30f, z0 = 1e30f, z1 = -1e30f;
        for (int i = 0; i < n; i++) {
            pos[i * 3] = vp.componentRaw(i, 0) * ps + pb[0];
            pos[i * 3 + 1] = vp.componentRaw(i, 1) * ps + pb[1];
            pos[i * 3 + 2] = vp.componentRaw(i, 2) * ps + pb[2];
            if (pos[i * 3] < x0) x0 = pos[i * 3];
            if (pos[i * 3] > x1) x1 = pos[i * 3];
            if (pos[i * 3 + 1] < y0) y0 = pos[i * 3 + 1];
            if (pos[i * 3 + 1] > y1) y1 = pos[i * 3 + 1];
            if (pos[i * 3 + 2] < z0) z0 = pos[i * 3 + 2];
            if (pos[i * 3 + 2] > z1) z1 = pos[i * 3 + 2];
        }
        System.out.printf("n=%d submeshes=%d model extent x[%.1f,%.1f] y[%.1f,%.1f] z[%.1f,%.1f]%n",
                          n, mesh.indexBuffers.length, x0, x1, y0, y1, z0, z1);
        int[] idx = mesh.indexBuffers[0].triangleList();
        System.out.println("submesh0 triangles=" + (idx.length / 3));

        float[] p1 = proj(70f, 1.7142857f, 1f, 10000f);   /* in-game, bytecode */
        float[] p2 = proj(70f, 1.2f, 1f, 10000f);          /* map view, bytecode */

        System.out.println("--- hypothesis A: setCamera transform is camera->world"
                           + " (view = inverse) [v03.39 / v03.35] ---");
        eval("in-game eye(-9,90,180)", mul(p1, translate(9f, -90f, -180f)), pos, n, idx);
        eval("map     eye(-8,16,32)", mul(p2, translate(8f, -16f, -32f)), pos, n, idx);
        eval("map2    eye(-15,18,30)", mul(p2, translate(15f, -18f, -30f)), pos, n, idx);

        System.out.println("--- hypothesis B: transform used directly [v03.36/v03.37] ---");
        eval("in-game eye(-9,90,180)", mul(p1, translate(-9f, 90f, 180f)), pos, n, idx);
        eval("map     eye(-8,16,32)", mul(p2, translate(-8f, 16f, 32f)), pos, n, idx);
    }
}
