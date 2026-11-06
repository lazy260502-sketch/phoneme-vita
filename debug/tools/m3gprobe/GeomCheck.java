package javax.microedition.m3g;

import java.io.DataOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.lang.reflect.Field;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.util.Random;

/**
 * Dumps what Renderer.decodeAttr and SkinnedMesh.applySkin really hand to the native decode()/bones() (the host stub
 * refuses, so the Java loops run) together with the Java result, for geom_test.c to replay through m3g_geom.h.
 * Usage: GeomCheck dumpDir file.m3g
 */
public class GeomCheck {
    static int nd = 0, nb = 0;

    static void dumpDecode(String dir, Object[] a, float[] expected) throws Exception {
        int n = ((Integer) a[0]).intValue(), stride = ((Integer) a[1]).intValue();
        int[] di = (int[]) a[2];
        float[] df = (float[]) a[3];
        int comps = ((Integer) a[4]).intValue();
        float[] sb = (float[]) a[5];
        int cnt = n * comps;
        ByteBuffer b = ByteBuffer.allocate(8 * 4 + 16 + cnt * 4 + n * stride * 4).order(ByteOrder.LITTLE_ENDIAN);
        b.putInt(n).putInt(stride).putInt(comps).putInt(df != null ? 1 : 0).putInt(0).putInt(0).putInt(0).putInt(0);
        for (int i = 0; i < 4; i++) b.putFloat(sb[i]);
        for (int i = 0; i < cnt; i++) { if (df != null) b.putFloat(df[i]); else b.putInt(di[i]); }
        for (int i = 0; i < n * stride; i++) b.putFloat(expected[i]);
        DataOutputStream o = new DataOutputStream(new FileOutputStream(dir + "/d_" + (nd++) + ".bin"));
        o.write(b.array());
        o.close();
    }

    static void dumpBones(String dir, Object[] a, float[] packOut, byte[] state) throws Exception {
        float[] local = (float[]) a[0];
        int[] parent = (int[]) a[1];
        float[] inv = (float[]) a[2];
        byte[] used = (byte[]) a[3];
        int nk = ((Integer) a[4]).intValue();
        int pad = (nk + 3) & ~3;
        ByteBuffer b = ByteBuffer.allocate(4 + nk * 16 * 4 * 2 + nk * 4 + pad + pad + nk * 16 * 4).order(ByteOrder.LITTLE_ENDIAN);
        b.putInt(nk);
        for (int i = 0; i < nk * 16; i++) b.putFloat(local[i]);
        for (int i = 0; i < nk; i++) b.putInt(parent[i]);
        for (int i = 0; i < nk * 16; i++) b.putFloat(inv[i]);
        for (int i = 0; i < pad; i++) b.put(i < nk ? used[i] : 0);
        for (int i = 0; i < pad; i++) b.put(i < nk ? state[i] : 0);
        for (int k = 0; k < nk; k++) for (int q = 0; q < 16; q++) b.putFloat(state[k] == 1 ? packOut[k * 16 + q] : 0.0f);
        DataOutputStream o = new DataOutputStream(new FileOutputStream(dir + "/b_" + (nb++) + ".bin"));
        o.write(b.array());
        o.close();
    }

    static float[] fld(Object o, String name) throws Exception {
        Field f = o.getClass().getDeclaredField(name);
        f.setAccessible(true);
        return (float[]) f.get(o);
    }

    public static void main(String[] args) throws Exception {
        String dir = args[0];
        File x = new File(args[1]);
        byte[] data = new byte[(int) x.length()];
        FileInputStream in = new FileInputStream(x);
        in.read(data);
        in.close();
        Object3D[] objs = M3gReader.readAll(data, com.sun.m3g.M3gFile.parse(data, 0));
        SkinnedMesh sm = null;
        for (int i = 0; i < objs.length && sm == null; i++) if (objs[i] instanceof SkinnedMesh) sm = (SkinnedMesh) objs[i];
        VertexBuffer vb = sm.vertexBuffer;
        int n = vb.numVertices;

        /* decode: the model's own positions and texture coordinates, then synthetic float / 4-component / NaN data */
        float[] out = new float[n * 3];
        com.sun.m3g.NativeBridge.lastDecodeArgs = null;
        Renderer.useNativeDecode = true;
        Renderer.decodeAttr(out, n, 3, vb.positions, vb.positionScale, vb.positionBias);
        dumpDecode(dir, com.sun.m3g.NativeBridge.lastDecodeArgs, out);
        if (vb.texCoords != null) {
            float[] uv = new float[n * 2];
            Renderer.useNativeDecode = true;
            Renderer.decodeAttr(uv, n, 2, vb.texCoords, vb.texScale, vb.texBias);
            dumpDecode(dir, com.sun.m3g.NativeBridge.lastDecodeArgs, uv);
        }
        Random r = new Random(3);
        for (int variant = 0; variant < 6; variant++) {
            int comps = (variant % 3 == 2) ? 4 : 3;
            VertexArray va = new VertexArray(200, comps, variant < 3 ? 2 : 4);
            if (variant < 3) {
                for (int i = 0; i < va.data.length; i++) va.data[i] = r.nextInt(65535) - 32768;
            } else {
                float[] f = new float[200 * comps];
                for (int i = 0; i < f.length; i++) {
                    int k = r.nextInt(10);
                    f[i] = (variant == 5 && k == 0) ? Float.NaN : (variant == 5 && k == 1) ? Float.POSITIVE_INFINITY
                         : (r.nextFloat() - 0.5f) * 1000.0f;
                }
                va.set(0, 200, f);
            }
            float[] o3 = new float[600];
            Renderer.useNativeDecode = true;
            Renderer.decodeAttr(o3, 200, 3, va, 0.0276f * (variant + 1), new float[] { 0.0f, 628.1f, -95.7f });
            dumpDecode(dir, com.sun.m3g.NativeBridge.lastDecodeArgs, o3);
            float[] o2 = new float[400];
            Renderer.useNativeDecode = true;
            Renderer.decodeAttr(o2, 200, 2, va, 1.0f / 256.0f, new float[] { 0.5f, -0.25f, 0.0f });
            dumpDecode(dir, com.sun.m3g.NativeBridge.lastDecodeArgs, o2);
        }

        /* bones: one case per animation time, Java matrices as the expectation */
        int[] times = { 0, 400, 1500, 3000, 4300, 5733, 100, 2500 };
        for (int s = 0; s < times.length; s++) {
            sm.animate(times[s]);
            float[] pos = new float[n * 3];
            Renderer.decodeAttr(pos, n, 3, vb.positions, vb.positionScale, vb.positionBias);
            com.sun.m3g.NativeBridge.lastBonesArgs = null;
            SkinnedMesh.useNative = true;
            SkinnedMesh.useNativeBones = true;           /* the stub refuses, which switches it off */
            sm.applySkin(pos, n);
            Object[] a = com.sun.m3g.NativeBridge.lastBonesArgs;
            if (a == null) { System.out.println("no bones call at step " + s); continue; }
            Field fb = SkinnedMesh.class.getDeclaredField("bmBuf");
            fb.setAccessible(true);
            float[][] bm = (float[][]) fb.get(sm);
            Field fs = SkinnedMesh.class.getDeclaredField("stateBuf");
            fs.setAccessible(true);
            byte[] state = (byte[]) fs.get(sm);
            int nk = ((Integer) a[4]).intValue();
            float[] flat = new float[nk * 16];
            for (int k = 0; k < nk; k++) if (state[k] == 1) System.arraycopy(bm[k], 0, flat, k * 16, 16);
            dumpBones(dir, a, flat, state);
        }
        /* state 2 (non-finite or beyond +-1e20): damage one bind-inverse in place so the Java loop reports it */
        Field fi = SkinnedMesh.class.getDeclaredField("bindInv");
        fi.setAccessible(true);
        float[][] inv = (float[][]) fi.get(sm);
        Field fr = SkinnedMesh.class.getDeclaredField("bonesReady");
        fr.setAccessible(true);
        int target = -1;
        for (int k = 0; k < inv.length && target < 0; k++) if (inv[k] != null) target = k;
        float[] save = inv[target].clone();
        for (int mode = 0; mode < 3; mode++) {
            for (int q = 0; q < 16; q++) {
                inv[target][q] = mode == 0 ? save[q] * 1.0e30f : mode == 1 ? save[q] * Float.NaN : save[q] * -1.0e25f;
            }
            fr.setBoolean(sm, false);
            sm.animate(1500);
            float[] pos = new float[n * 3];
            Renderer.decodeAttr(pos, n, 3, vb.positions, vb.positionScale, vb.positionBias);
            com.sun.m3g.NativeBridge.lastBonesArgs = null;
            SkinnedMesh.useNative = true;
            SkinnedMesh.useNativeBones = true;
            sm.applySkin(pos, n);
            Object[] a = com.sun.m3g.NativeBridge.lastBonesArgs;
            Field fb = SkinnedMesh.class.getDeclaredField("bmBuf");
            fb.setAccessible(true);
            float[][] bm = (float[][]) fb.get(sm);
            Field fs = SkinnedMesh.class.getDeclaredField("stateBuf");
            fs.setAccessible(true);
            byte[] state = (byte[]) fs.get(sm);
            int nk = ((Integer) a[4]).intValue();
            float[] flat = new float[nk * 16];
            int s2 = 0;
            for (int k = 0; k < nk; k++) { if (state[k] == 1) System.arraycopy(bm[k], 0, flat, k * 16, 16); if (state[k] == 2) s2++; }
            System.out.println("damaged mode " + mode + ": " + s2 + " bone(s) in state 2");
            dumpBones(dir, a, flat, state);
        }
        System.arraycopy(save, 0, inv[target], 0, 16);
        System.out.println("dumped decode=" + nd + " bones=" + nb);
    }
}
