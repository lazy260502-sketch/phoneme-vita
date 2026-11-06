package com.sun.m3g;

/**
 * HOST STUB of the KNI seam. Used only by debug/tools/m3gprobe/HostRun, which
 * drives the REAL Renderer/Graphics3D code on a desktop JVM so a device round
 * trip is not needed to see where vertices land.
 *
 * Instead of calling vita_3d.c it records the MVP and the vertex arrays and
 * computes clip/NDC itself - the same arithmetic the device probe in
 * Renderer.drawMesh() does, but with full precision and no line budget.
 */
public final class NativeBridge {

    private NativeBridge() {}

    public static float[] lastMvp = null;
    public static int draws = 0;
    public static int dupCalls = 0;
    public static int lastN = 0;
    public static String lastTexInfo = "";
    public static java.util.ArrayList jit = new java.util.ArrayList();

    public static int init(int width, int height) { return 1; }
    public static int backend() { return 1; }
    public static int camMode() {
        return Integer.getInteger("m3gcam", 0).intValue() | (Integer.getInteger("m3gskin", 0).intValue() << 8);
    }
    public static int textureCreate(int[] argb, int width, int height) { return 1; }
    public static void textureFree(int handle) {}
    public static void clear(int argb, boolean clearDepth, boolean clearColor) {}
    public static void setScissor(int x, int y, int width, int height) {}
    public static int screenWidth() { return 240; }
    public static int screenHeight() { return 320; }

    public static void setMvp(float[] m16ColumnMajor) {
        lastMvp = (float[]) m16ColumnMajor.clone();
    }

    /* v04.02: records the CompositingMode the Renderer asked for so a host run
     * can assert the wiring (blend mode, depth flags, alpha threshold) without
     * a device round trip. The old stub was setState(bool, bool) and dropped
     * everything except "is blending on". */
    public static int lastBlendMode = -1;
    public static boolean lastDepthTest = true;
    public static boolean lastDepthWrite = true;
    public static boolean lastColorWrite = true;
    public static float lastAlphaThreshold = 0.0f;
    public static int lastBlendColor = 0;

    public static void setState(boolean cull, int blendMode,
                                boolean depthTest, boolean depthWrite,
                                boolean colorWrite, float alphaThreshold,
                                int blendColor) {
        lastBlendMode = blendMode;
        lastDepthTest = depthTest;
        lastDepthWrite = depthWrite;
        lastColorWrite = colorWrite;
        lastAlphaThreshold = alphaThreshold;
        lastBlendColor = blendColor;
    }

    /* v04.41: Fog. Must exist or the host tree will not compile against
     * Renderer - keeping the stub in step with com/sun/m3g/NativeBridge.java is
     * part of shipping a new NativeBridge entry point. */
    public static int lastFogMode = 0;
    public static int lastFogColor = 0;
    public static int lastFogNear256 = 0;
    public static int lastFogFar256 = 0;
    public static int lastFogDensity1000 = 0;

    public static void setFog(int mode, int color, int near256,
                              int far256, int density1000) {
        lastFogMode = mode;
        lastFogColor = color;
        lastFogNear256 = near256;
        lastFogFar256 = far256;
        lastFogDensity1000 = density1000;
    }

    public static void draw(float[] pos3, float[] uv2, byte[] rgba,
                            int[] indices, int indexCount, int texHandle) {
        draws++;
        lastN = (pos3 == null) ? 0 : pos3.length / 3;
        lastTexInfo = "idx=" + (indices == null ? -1 : indexCount)
                    + " tex=" + texHandle;
        if (lastMvp == null || pos3 == null) {
            return;
        }
        int n = pos3.length / 3;
        float nx0 = 1e30f, ny0 = 1e30f, nz0 = 1e30f;
        float nx1 = -1e30f, ny1 = -1e30f, nz1 = -1e30f;
        int behind = 0, inside = 0;
        for (int i = 0; i < n; i++) {
            float x = pos3[i * 3], y = pos3[i * 3 + 1], z = pos3[i * 3 + 2];
            float cx = lastMvp[0] * x + lastMvp[4] * y + lastMvp[8] * z + lastMvp[12];
            float cy = lastMvp[1] * x + lastMvp[5] * y + lastMvp[9] * z + lastMvp[13];
            float cz = lastMvp[2] * x + lastMvp[6] * y + lastMvp[10] * z + lastMvp[14];
            float cw = lastMvp[3] * x + lastMvp[7] * y + lastMvp[11] * z + lastMvp[15];
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
            if (Math.abs(a) <= 1 && Math.abs(b) <= 1 && Math.abs(c) <= 1) {
                inside++;
            }
        }
        /* winding sign, as in Renderer's probe */
        int posArea = 0, negArea = 0;
        if (indices != null) {
            for (int t = 0; t + 2 < indices.length; t += 3) {
                float[] v = new float[6];
                boolean ok = true;
                for (int k = 0; k < 3; k++) {
                    int vi = indices[t + k];
                    if (vi < 0 || vi >= n) { ok = false; break; }
                    float x = pos3[vi * 3], y = pos3[vi * 3 + 1], z = pos3[vi * 3 + 2];
                    float w = lastMvp[3] * x + lastMvp[7] * y + lastMvp[11] * z + lastMvp[15];
                    if (w <= 1e-5f) { ok = false; break; }
                    v[k * 2] = (lastMvp[0] * x + lastMvp[4] * y + lastMvp[8] * z + lastMvp[12]) / w;
                    v[k * 2 + 1] = (lastMvp[1] * x + lastMvp[5] * y + lastMvp[9] * z + lastMvp[13]) / w;
                }
                if (!ok) { continue; }
                float cross = (v[2] - v[0]) * (v[5] - v[1])
                            - (v[3] - v[1]) * (v[4] - v[0]);
                if (cross > 0) { posArea++; } else if (cross < 0) { negArea++; }
            }
        }
        jit.add(String.format(
            "    draw#%-3d n=%-5d %-18s behind=%-4d inside=%-4d"
            + " ndc x[%7.3f,%7.3f] y[%7.3f,%7.3f] z[%6.3f,%6.3f] pos=%d neg=%d",
            new Object[] { new Integer(draws), new Integer(n), lastTexInfo,
                           new Integer(behind), new Integer(inside),
                           new Float(nx0), new Float(nx1),
                           new Float(ny0), new Float(ny1),
                           new Float(nz0), new Float(nz1),
                           new Integer(posArea), new Integer(negArea) }));
        /* first three raw vertices, so a collapsed array is unmistakable */
        if (n >= 3) {
            jit.add(String.format(
                "      v0=(%.4f,%.4f,%.4f) v1=(%.4f,%.4f,%.4f) vLast=(%.4f,%.4f,%.4f)",
                new Object[] { new Float(pos3[0]), new Float(pos3[1]), new Float(pos3[2]),
                               new Float(pos3[3]), new Float(pos3[4]), new Float(pos3[5]),
                               new Float(pos3[(n - 1) * 3]), new Float(pos3[(n - 1) * 3 + 1]),
                               new Float(pos3[(n - 1) * 3 + 2]) }));
        }
        jit.add(String.format("      mvp m0=%.4f m5=%.4f m10=%.4f m11=%.4f t=(%.3f,%.3f,%.3f)",
            new Object[] { new Float(lastMvp[0]), new Float(lastMvp[5]),
                           new Float(lastMvp[10]), new Float(lastMvp[11]),
                           new Float(lastMvp[12]), new Float(lastMvp[13]),
                           new Float(lastMvp[14]) }));
    }

    public static void present(int[] argb, int width, int height) {}
    public static boolean skin(float[] pos, int n, float[] mats, int[] ent, int nent) { return false; }

    /** What the last decode() / bones() calls were given; the call itself is refused so the Java loop runs. */
    public static Object[] lastDecodeArgs = null;
    public static Object[] lastBonesArgs = null;
    public static boolean decode(float[] out, int n, int stride, int[] iData, float[] fData, int comps, float[] sb) {
        lastDecodeArgs = new Object[] { new Integer(n), new Integer(stride), iData, fData, new Integer(comps), sb.clone() };
        return false;
    }
    public static boolean bones(float[] local, int[] parent, float[] bindInv, byte[] used, int nk, float[] bm, byte[] state) {
        lastBonesArgs = new Object[] { local.clone(), parent.clone(), bindInv.clone(), used.clone(), new Integer(nk) };
        return false;
    }

    /** The arguments of the last shade() call (what Renderer packed), so a test can hand them to the C kernel. */
    public static Object[] lastShadeArgs = null;
    public static boolean shade(byte[] out, int n, float[] prm, int[] nI, float[] nF, int nComps, int nSize,
                                int[] cI, float[] cF, int cComps, int cSize, int nl) {
        lastShadeArgs = new Object[] { new Integer(n), prm, nI, nF, new Integer(nComps), new Integer(nSize),
                                       cI, cF, new Integer(cComps), new Integer(cSize), new Integer(nl) };
        return false;
    }
}
