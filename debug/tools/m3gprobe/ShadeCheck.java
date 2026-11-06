package javax.microedition.m3g;

import com.sun.m3g.Matrix4;
import java.io.DataOutputStream;
import java.io.FileOutputStream;
import java.lang.reflect.Method;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.util.Random;

/**
 * Bit-exact regression check for Renderer.shade (private, called by reflection): vertex buffers with and without
 * normals / colours, light sets (none, ambient, directional, point, mixed with a null slot), several model matrices,
 * with and without a Material. Prints a hash per case; run against the baseline build and the modified build and diff.
 */
public class ShadeCheck {
    static VertexBuffer buffer(Random r, int n, boolean normals, int colorComps) {
        VertexBuffer vb = new VertexBuffer();
        short[] p = new short[n * 3];
        for (int i = 0; i < p.length; i++) p[i] = (short) (r.nextInt(2000) - 1000);
        VertexArray pa = new VertexArray(n, 3, 2);
        pa.set(0, n, p);
        vb.setPositions(pa, 0.5f, new float[] { 3.0f, -7.0f, 11.0f });
        if (normals) {
            byte[] nb = new byte[n * 3];
            for (int i = 0; i < nb.length; i++) nb[i] = (byte) (r.nextInt(255) - 127);
            VertexArray na = new VertexArray(n, 3, 1);
            na.set(0, n, nb);
            vb.setNormals(na);
        }
        if (colorComps > 0) {
            byte[] cb = new byte[n * colorComps];
            for (int i = 0; i < cb.length; i++) cb[i] = (byte) r.nextInt(256);
            VertexArray ca = new VertexArray(n, colorComps, 1);
            ca.set(0, n, cb);
            vb.setColors(ca);
        }
        return vb;
    }

    /** Normals / colours stored the other ways VertexArray allows: short, float, a wide int, and NaN / Inf values. */
    static VertexBuffer variant(Random r, int n, int kind) {
        VertexBuffer vb = buffer(r, n, false, 0);
        VertexArray na, ca;
        switch (kind) {
        case 0: {                                   /* short normals, float RGBA colours */
            short[] s = new short[n * 3];
            for (int i = 0; i < s.length; i++) s[i] = (short) (r.nextInt(65535) - 32767);
            na = new VertexArray(n, 3, 2);
            na.set(0, n, s);
            float[] f = new float[n * 4];
            for (int i = 0; i < f.length; i++) f[i] = r.nextFloat() * 1.4f - 0.2f;
            ca = new VertexArray(n, 4, 4);
            ca.set(0, n, f);
            break;
        }
        case 1: {                                   /* float normals (not unit length), byte RGB colours */
            float[] f = new float[n * 3];
            for (int i = 0; i < f.length; i++) f[i] = (r.nextFloat() - 0.5f) * 6.0f;
            na = new VertexArray(n, 3, 4);
            na.set(0, n, f);
            byte[] c = new byte[n * 3];
            for (int i = 0; i < c.length; i++) c[i] = (byte) r.nextInt(256);
            ca = new VertexArray(n, 3, 1);
            ca.set(0, n, c);
            break;
        }
        case 2: {                                   /* 4-byte (fixed) storage written straight into data[] */
            na = new VertexArray(n, 3, 4);
            for (int i = 0; i < na.data.length; i++) na.data[i] = r.nextInt(131071) - 65535;
            ca = new VertexArray(n, 4, 4);
            for (int i = 0; i < ca.data.length; i++) ca.data[i] = r.nextInt(65537);
            break;
        }
        default: {                                  /* NaN / Inf in normals and colours */
            float[] f = new float[n * 3];
            for (int i = 0; i < f.length; i++) {
                int k = r.nextInt(8);
                f[i] = k == 0 ? Float.NaN : k == 1 ? Float.POSITIVE_INFINITY : k == 2 ? Float.NEGATIVE_INFINITY
                     : (r.nextFloat() - 0.5f) * 2.0f;
            }
            na = new VertexArray(n, 3, 4);
            na.set(0, n, f);
            float[] g = new float[n * 4];
            for (int i = 0; i < g.length; i++) {
                int k = r.nextInt(8);
                g[i] = k == 0 ? Float.NaN : k == 1 ? 1.0e30f : k == 2 ? -1.0e30f : r.nextFloat() * 2.0f - 0.5f;
            }
            ca = new VertexArray(n, 4, 4);
            ca.set(0, n, g);
            break;
        }
        }
        vb.setNormals(na);
        vb.setColors(ca);
        return vb;
    }

    static Light light(int mode, int color, float inten) {
        Light l = new Light();
        l.setMode(mode);
        l.setColor(color);
        l.setIntensity(inten);
        return l;
    }

    static float[] rotation(Random r, boolean translate) {
        float[] t = translate ? new float[] { rf(r) * 20, rf(r) * 20, rf(r) * 20 } : new float[3];
        float[] o = { rf(r) * 360, rf(r), rf(r), rf(r) };
        float[] s = { 1, 1, 1 };
        float[] m = new float[16];
        Matrix4.compose(m, t, o, s);
        return m;
    }

    static float rf(Random r) {
        return r.nextFloat() * 2 - 1;
    }

    /** Little-endian case file for shade_test.c: header ints, prm, normals, colours, then the Java loop's output. */
    static void dump(String dir, int idx, Object[] a, byte[] expected) throws Exception {
        int n = ((Integer) a[0]).intValue();
        float[] prm = (float[]) a[1];
        int[] nI = (int[]) a[2];
        float[] nF = (float[]) a[3];
        int nC = ((Integer) a[4]).intValue(), nS = ((Integer) a[5]).intValue();
        int[] cI = (int[]) a[6];
        float[] cF = (float[]) a[7];
        int cC = ((Integer) a[8]).intValue(), cS = ((Integer) a[9]).intValue();
        int nl = ((Integer) a[10]).intValue();
        int flags = (nI != null ? 1 : 0) | (nF != null ? 2 : 0) | (cI != null ? 4 : 0) | (cF != null ? 8 : 0);
        int nCount = (nI != null || nF != null) ? n * nC : 0;
        int cCount = (cI != null || cF != null) ? n * cC : 0;
        ByteBuffer b = ByteBuffer.allocate(8 * 4 + prm.length * 4 + nCount * 4 + cCount * 4 + expected.length);
        b.order(ByteOrder.LITTLE_ENDIAN);
        b.putInt(n).putInt(nC).putInt(nS).putInt(cC).putInt(cS).putInt(nl).putInt(prm.length).putInt(flags);
        for (int i = 0; i < prm.length; i++) b.putFloat(prm[i]);
        for (int i = 0; i < nCount; i++) { if (nI != null) b.putInt(nI[i]); else b.putFloat(nF[i]); }
        for (int i = 0; i < cCount; i++) { if (cI != null) b.putInt(cI[i]); else b.putFloat(cF[i]); }
        b.put(expected);
        DataOutputStream o = new DataOutputStream(new FileOutputStream(dir + "/case_" + idx + ".bin"));
        o.write(b.array());
        o.close();
    }

    public static void main(String[] args) throws Exception {
        String dumpDir = args.length > 0 ? args[0] : null;
        int dumped = 0;
        Method sh = Renderer.class.getDeclaredMethod("shade", new Class[] {
            VertexBuffer.class, float[].class, Light[].class, float[][].class, Material.class });
        sh.setAccessible(true);
        Random r = new Random(5);
        VertexBuffer[] vbs = {
            buffer(r, 600, true, 4), buffer(r, 600, true, 3), buffer(r, 600, false, 4), buffer(r, 600, false, 0),
            variant(r, 600, 0), variant(r, 600, 1), variant(r, 600, 2), variant(r, 600, 3)
        };
        Material mat = new Material();
        mat.setColor(Material.DIFFUSE, 0xFF80C0E0);
        mat.setColor(Material.AMBIENT, 0xFF404040);
        mat.setColor(Material.EMISSIVE, 0xFF101010);
        Material[] mats = { null, mat };

        Light amb = light(Light.AMBIENT, 0xFFFFFF, 0.7f);
        Light dir = light(Light.DIRECTIONAL, 0xFFEEDD, 1.3f);
        Light dir0 = light(Light.DIRECTIONAL, 0xFFFFFF, 1.0f);
        Light omni = light(Light.OMNI, 0xCCDDFF, 2.0f);
        omni.setAttenuation(1.0f, 0.01f, 0.0005f);
        Light[][] sets = {
            {}, { amb }, { dir }, { amb, dir }, { omni }, { amb, null, dir, omni }, { dir0, dir }
        };

        long all = 1469598103934665603L;
        int cases = 0;
        for (int vi = 0; vi < vbs.length; vi++) {
            for (int si = 0; si < sets.length; si++) {
                for (int mi = 0; mi < mats.length; mi++) {
                    for (int k = 0; k < 3; k++) {
                        float[] model = rotation(r, true);
                        float[][] lm = new float[sets[si].length][];
                        for (int i = 0; i < lm.length; i++) lm[i] = rotation(r, true);
                        if (si == 6) {              /* a zero-length direction: the light must be skipped */
                            lm[0] = new float[16];
                            Matrix4.identity(lm[0]);
                            lm[0][8] = 0; lm[0][9] = 0; lm[0][10] = 0;
                        }
                        Renderer.useNativeShade = true;   /* the host stub refuses, which would switch it off for good */
                        com.sun.m3g.NativeBridge.lastShadeArgs = null;
                        byte[] out = (byte[]) sh.invoke(null, new Object[] { vbs[vi], model, sets[si], lm, mats[mi] });
                        if (dumpDir != null && com.sun.m3g.NativeBridge.lastShadeArgs != null) {
                            dump(dumpDir, dumped++, com.sun.m3g.NativeBridge.lastShadeArgs, out);
                        }
                        long h = 1469598103934665603L;
                        for (int i = 0; i < out.length; i++) h = (h ^ (out[i] & 0xFF)) * 1099511628211L;
                        all = (all ^ h) * 1099511628211L;
                        cases++;
                        System.out.println("vb" + vi + " lights" + si + " mat" + mi + " m" + k + " " + Long.toHexString(h));
                    }
                }
            }
        }
        System.out.println("CASES " + cases + " ALL " + Long.toHexString(all) + " DUMPED " + dumped);
    }
}
