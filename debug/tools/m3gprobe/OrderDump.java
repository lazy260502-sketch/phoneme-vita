package javax.microedition.m3g;
import java.io.*;
import java.util.*;
import java.util.zip.*;
import com.sun.m3g.M3gFile;
public class OrderDump {
    public static void main(String[] a) throws Exception {
        ZipFile zf = new ZipFile(a[0]);
        List<String> names = new ArrayList<String>();
        for (Enumeration en = zf.entries(); en.hasMoreElements();) {
            ZipEntry ze = (ZipEntry)en.nextElement();
            String nm = ze.getName();
            if (ze.isDirectory() || nm.indexOf('.') >= 0) continue;
            ByteArrayOutputStream bo = new ByteArrayOutputStream();
            InputStream in = zf.getInputStream(ze);
            int r; byte[] t = new byte[8192];
            while ((r = in.read(t)) > 0) bo.write(t,0,r);
            if (M3gFile.hasIdentifier(bo.toByteArray(),0)) names.add(nm);
        }
        Collections.sort(names);
        for (String nm : names) {
            byte[] e;
            InputStream in = zf.getInputStream(zf.getEntry(nm));
            ByteArrayOutputStream bo = new ByteArrayOutputStream();
            int r; byte[] t = new byte[8192];
            while ((r = in.read(t)) > 0) bo.write(t,0,r);
            e = bo.toByteArray();
            M3gFile f = M3gFile.parse(e,0);
            Object3D[] objs = M3gReader.readAll(e,f);
            StringBuilder sb = new StringBuilder();
            for (int i = 0; i < objs.length; i++) {
                if (objs[i] == null) { sb.append("- "); continue; }
                sb.append(objs[i].getClass().getSimpleName().charAt(0))
                  .append(objs[i].getUserID()).append(' ');
            }
            System.out.println(nm + "\t" + objs.length + "\t" + sb);
        }
        zf.close();
    }
}
