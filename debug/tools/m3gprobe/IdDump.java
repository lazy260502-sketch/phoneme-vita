package javax.microedition.m3g;

import java.io.DataInputStream;
import java.io.InputStream;
import com.sun.m3g.M3gFile;
import java.util.Vector;

/** Dump every object's userID + type for one .m3g, and locate specific IDs. */
public class IdDump {

    static void walk(String label, Object3D o, int depth) {
        if (o == null || depth > 6) return;
        System.out.println(label + " " + o.getClass().getName() + " userID=" + o.getUserID());
        Vector refs = new Vector();
        o.collectReferences(refs);
        for (int i = 0; i < refs.size(); i++) {
            Object3D r = (Object3D) refs.elementAt(i);
            if (r instanceof Node) {
                walk(label + "  ", r, depth + 1);
            }
        }
    }

    public static void main(String[] a) throws Exception {
        String path = a[0];
        int idA = a.length > 1 ? Integer.parseInt(a[1]) : 12;
        int idB = a.length > 2 ? Integer.parseInt(a[2]) : 13;

        InputStream in = IdDump.class.getResourceAsStream(path);
        if (in == null) { System.out.println("no resource " + path); return; }
        byte[] buf = new byte[1 << 20];
        int n = 0, r;
        DataInputStream d = new DataInputStream(in);
        while (n < buf.length && (r = d.read(buf, n, buf.length - n)) > 0) n += r;
        byte[] exact = new byte[n]; System.arraycopy(buf, 0, exact, 0, n);
        M3gFile f = M3gFile.parse(exact, 0);
        System.out.println("objects=" + f.objects.length);
        /* every slot id */
        int nid = 0;
        for (int i = 0; i < f.objects.length; i++) {
            if (f.objects[i] != null && f.objects[i].id != 0) nid++;
        }
        System.out.println("slots with non-zero id = " + nid);

        Object3D[] objs = M3gReader.readAll(exact, f);
        System.out.println("loaded = " + objs.length);

        int found = 0;
        for (int i = 0; i < objs.length; i++) {
            Object3D o = objs[i];
            if (o != null && o.getUserID() != 0) {
                found++;
                if (found <= 60)
                    System.out.println("  [" + i + "] " + o.getClass().getName()
                                       + " userID=" + o.getUserID());
            }
        }
        System.out.println("objects with non-zero userID = " + found);

        for (int i = 0; i < objs.length; i++) {
            Object3D o = objs[i];
            if (o == null) continue;
            if (o.getUserID() == idA || o.getUserID() == idB) {
                System.out.println("*** slot " + i + " " + o.getClass().getName()
                                   + " userID=" + o.getUserID());
            }
        }

        Object3D root = objs[0];
        System.out.println("--- root=" + (root == null ? "null" : root.getClass().getName()));
        for (int t : new int[]{idA, idB, 3, 12, 13}) {
            Object3D hit = root == null ? null : root.find(t);
            System.out.println("root.find(" + t + ") = "
                               + (hit == null ? "NULL" : hit.getClass().getName()));
        }
    }
}
