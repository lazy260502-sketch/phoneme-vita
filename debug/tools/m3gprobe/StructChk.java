package javax.microedition.m3g;
import java.io.*;
import java.util.*;
import java.util.zip.*;
import com.sun.m3g.M3gFile;
public class StructChk {
    public static void main(String[] a) throws Exception {
        ZipFile zf = new ZipFile(a[0]);
        List<String> names = new ArrayList<String>();
        for (Enumeration en = zf.entries(); en.hasMoreElements();) {
            ZipEntry ze = (ZipEntry)en.nextElement();
            String nm = ze.getName();
            if (ze.isDirectory() || nm.indexOf('.') >= 0) continue;
            InputStream in = zf.getInputStream(ze);
            ByteArrayOutputStream bo = new ByteArrayOutputStream();
            int r; byte[] t = new byte[8192];
            while ((r = in.read(t)) > 0) bo.write(t,0,r);
            byte[] e = bo.toByteArray();
            if (!M3gFile.hasIdentifier(e,0)) continue;
            names.add(nm);
        }
        Collections.sort(names);
        System.out.printf("%-16s %6s %6s %6s %7s %s%n","file","objs","nodes","roots","objs0=","note");
        for (String nm : names) {
            ZipEntry ze = zf.getEntry(nm);
            InputStream in = zf.getInputStream(ze);
            ByteArrayOutputStream bo = new ByteArrayOutputStream();
            int r; byte[] t = new byte[8192];
            while ((r = in.read(t)) > 0) bo.write(t,0,r);
            byte[] e = bo.toByteArray();
            M3gFile f = M3gFile.parse(e,0);
            Object3D[] objs = M3gReader.readAll(e,f);
            int nodes=0, roots=0;
            for (int i=0;i<objs.length;i++) if (objs[i] instanceof Node) {
                nodes++;
                if (((Node)objs[i]).parent == null) roots++;
            }
            String o0 = objs.length==0 ? "-" : objs[0].getClass().getSimpleName();
            boolean o0root = objs.length>0 && objs[0] instanceof Node && ((Node)objs[0]).parent==null;
            String note = roots==1 ? (o0root?"single-root, objs0 IS root":"single-root, objs0 NOT root ***") : (roots==0?"no nodes":"multi-root");
            System.out.printf("%-16s %6d %6d %6d %7s %s%n", nm, objs.length, nodes, roots, o0, note);
        }
        zf.close();
    }
}
