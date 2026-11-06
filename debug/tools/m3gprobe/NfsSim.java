package javax.microedition.m3g;
import java.io.*;
import com.sun.m3g.M3gFile;

/** Replays 极品飞车's two loading sequences exactly as decompiled. */
public class NfsSim {
    static byte[] load(String p) throws Exception {
        InputStream in = NfsSim.class.getResourceAsStream(p);
        if (in == null) throw new IOException("no " + p);
        byte[] buf = new byte[1<<22]; DataInputStream di = new DataInputStream(in);
        int n=0,r; while ((r=di.read(buf,n,buf.length-n))>0) n+=r;
        byte[] e = new byte[n]; System.arraycopy(buf,0,e,0,n); return e;
    }
    static Object3D[] loadM3g(String p) throws Exception {
        byte[] e = load(p);
        M3gFile f = M3gFile.parse(e,0);
        return M3gReader.readAll(e,f);
    }
    static int fails = 0;
    static void check(String what, boolean ok) {
        System.out.println((ok ? "  ok   " : "  FAIL ") + what);
        if (!ok) fails++;
    }
    public static void main(String[] a) throws Exception {
        System.out.println("=== 场景 A: x.q()/ab.a() -- /d, find(12)/(13), addChild ===");
        Object3D d0 = loadM3g("/javax/microedition/m3g/d")[0];
        Node model = (Node) d0.duplicate();
        check("duplicate preserves source userID (" + d0.getUserID() + " -> " + model.getUserID() + ")", model.getUserID() == d0.getUserID());
        Group g12 = (Group) model.find(12);
        Group g13 = (Group) model.find(13);
        check("find(12) != null", g12 != null);
        check("find(13) != null", g13 != null);
        if (g12 != null && g13 != null) {
            try {
                g12.addChild(new Group());
                g13.addChild(new Group());
                check("both addChild accepted (bci 135 + 855)", true);
            } catch (Throwable t) { check("addChild: " + t, false); }
        }

        System.out.println("=== 场景 B: x.a() -- /c into new World, find(10)/(11), getTransformTo ===");
        Object3D c0 = loadM3g("/javax/microedition/m3g/c")[0];
        check("loaded[0] is the tree root (has children)",
              c0 instanceof Group && ((Group)c0).getChildCount() > 0);
        World w = new World();
        w.addChild((Node) c0);
        Object3D o10 = w.find(10);
        Object3D o11 = w.find(11);
        check("world.find(10) != null", o10 != null);
        check("world.find(11) != null", o11 != null);
        if (o10 != null && o11 != null) {
            Transform t = new Transform();
            boolean ok = ((Node)o10).getTransformTo((Node)o11, t);
            check("getTransformTo(o10,o11) == true (not out-of-graph)", ok);
            /* 游戏随后把该矩阵丢给 ab.a(Transform,int[]) 用，验证可读 */
            check("transform is finite", isFinite(t));
        }

        System.out.println("=== 场景 C: 位置寻址未受影响 (/aq 索引 0/2/3 都是 Node) ===");
        Object3D[] aq = loadM3g("/javax/microedition/m3g/aq");
        int[] idx = {0,2,3,6,7,8,9,10,11};
        boolean allNode = true;
        for (int i = 0; i < idx.length; i++)
            if (!(aq[idx[i]] instanceof Node)) { allNode = false; System.out.println("    idx " + idx[i] + " not Node: " + aq[idx[i]].getClass().getSimpleName()); }
        check("all 9 packed indices of /aq are Nodes", allNode);

        System.out.println(fails == 0 ? "\nALL PASS" : "\nFAILURES: " + fails);
    }
    static boolean isFinite(Transform t) {
        for (int i = 0; i < 16; i++) {
            float v = t.m[i];
            if (Float.isNaN(v) || Float.isInfinite(v)) return false;
        }
        return true;
    }
}
