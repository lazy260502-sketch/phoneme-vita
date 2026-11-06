package javax.microedition.m3g;
import java.io.*;
import java.util.zip.*;
import com.sun.m3g.M3gFile;
public class Pos2 {
    public static void main(String[] a) throws Exception {
        int[] grp = {0,1,2,3,4,5,6,7,8,9,10,11};
        int[] res = {15,16,24,31,48,64,63,42,13,12,11,14};
        String[] oa = {"/a","/b","/c","/d","/e","/f","/g","/h","/i","/j","/k","/l","/m","/n","/o","/p","/q",
                       "/r","/s","/t","/u","/v","/w","/x","/y","/z","/aa","/ab","/ac","/ad","/ae","/af","/ag",
                       "/ah","/ai","/aj","/ak","/al","/am","/an","/ao","/ap","/aq","/ar","/as","/at","/au","/av",
                       "/aw","/ax","/ay","/az","/ba","/bb","/bc","/bd","/be","/bf","/bg","/bh","/bi","/bj","/bk",
                       "/bl","/bm","/bn","/bo","/bp","/bq","/br","/bs","/bt","/bu","/bv","/bw","/bx","/by","/bz","/ca"};
        int[] grpToRes = new int[12];
        for (int k = 0; k < 12; k++) grpToRes[grp[k]] = res[k];
        Object3D[][] cache = new Object3D[80][];
        ZipFile zf = new ZipFile(a[0]);
        int[] raws = {1536,1538,1539,1542,1543,1544,1545,1546,1547,1548,1549,1550,1551,1552,1554,1556,1557,1558,1559};
        int bad = 0;
        System.out.printf("%-6s %-5s %-5s %-4s %-16s %s%n","raw","group","res","idx","class","userID");
        for (int i = 0; i < raws.length; i++) {
            int raw = raws[i];
            int g = (raw >> 8) & 0xFF, idx = raw & 0xFFFF00FF;
            int r = grpToRes[g];
            if (cache[r] == null) {
                byte[] e;
                ZipEntry ze = zf.getEntry(oa[r].substring(1));
                InputStream in = zf.getInputStream(ze);
                ByteArrayOutputStream bo = new ByteArrayOutputStream();
                int n; byte[] t = new byte[8192];
                while ((n = in.read(t)) > 0) bo.write(t,0,n);
                e = bo.toByteArray();
                cache[r] = M3gReader.readAll(e, M3gFile.parse(e,0));
            }
            Object3D[] arr = cache[r];
            Object3D o = idx < arr.length ? arr[idx] : null;
            boolean ok = o instanceof Node;
            if (!ok) bad++;
            System.out.printf("%-6d %-5d %-5s %-4d %-16s %s%s%n", raw, g, oa[r], idx,
                o == null ? "NULL" : o.getClass().getSimpleName(),
                o == null ? "-" : String.valueOf(o.getUserID()),
                ok ? "" : "   *** FAIL ***");
        }
        System.out.println("\n非 Node: " + bad + " / " + raws.length);
        zf.close();
    }
}
