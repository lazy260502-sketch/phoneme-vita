package javax.microedition.m3g;

import java.io.*;

/**
 * v04.29: Texture2D.imageFilter reaches the draw call. (1) new Texture2D(image) defaults to FILTER_NEAREST as the M3G
 * spec says; (2) a Texture2D read from a file keeps what the file says; (3) the texture argument of NativeBridge.draw
 * carries bit 16 exactly when the Appearance's Texture2D asks for FILTER_NEAREST, and the handle below it is untouched.
 * Usage: FilterCheck some.m3g
 */
public class FilterCheck {
    static int fails = 0;
    static void check(boolean c, String msg) {
        System.out.println((c ? "ok:   " : "FAIL: ") + msg);
        if (!c) fails++;
    }

    public static void main(String[] a) throws Exception {
        Image2D img = new Image2D(Image2D.RGBA, 4, 4, new byte[4 * 4 * 4]);
        Texture2D t = new Texture2D(img);
        check(t.getImageFilter() == Texture2D.FILTER_NEAREST, "new Texture2D(image): image filter NEAREST");
        check(t.getLevelFilter() == Texture2D.FILTER_BASE_LEVEL, "new Texture2D(image): level filter BASE_LEVEL");
        check(t.getWrappingS() == Texture2D.WRAP_REPEAT && t.getWrappingT() == Texture2D.WRAP_REPEAT, "new Texture2D(image): wrapping REPEAT");

        File x = new File(a[0]);
        byte[] data = new byte[(int) x.length()];
        DataInputStream in = new DataInputStream(new FileInputStream(x));
        in.readFully(data);
        in.close();
        Object3D[] objs = M3gReader.readAll(data, com.sun.m3g.M3gFile.parse(data, 0));
        int linear = 0, other = 0;
        for (int i = 0; i < objs.length; i++) {
            if (objs[i] instanceof Texture2D) {
                if (((Texture2D) objs[i]).getImageFilter() == Texture2D.FILTER_LINEAR) linear++; else other++;
            }
        }
        check(linear > 0 && other == 0, "textures read from the file keep their LINEAR (" + linear + " of them)");

        SkinnedMesh sm = null;
        for (int i = 0; i < objs.length && sm == null; i++) if (objs[i] instanceof SkinnedMesh) sm = (SkinnedMesh) objs[i];
        Texture2D tex = sm.appearances[0].getTexture(0);
        check(tex != null, "the model has a texture");
        Image2D target = new Image2D(Image2D.RGBA, 240, 320);
        Graphics3D g = Graphics3D.getInstance();
        Camera c = new Camera();
        c.setPerspective(54.0f, 1.3333334f, 1.0f, 10000.0f);
        int distinct = 0;
        java.util.Vector seen = new java.util.Vector();
        for (int i = 0; i < sm.appearances.length; i++) {
            Texture2D ti = sm.appearances[i] == null ? null : sm.appearances[i].getTexture(0);
            if (ti != null && !seen.contains(ti)) { seen.addElement(ti); distinct++; }
        }
        System.out.println("   the model has " + sm.appearances.length + " submeshes using " + distinct + " distinct Texture2D");
        for (int pass = 0; pass < 2; pass++) {
            /* lastTexInfo is the LAST draw, so every submesh's Texture2D has to be switched, not just the first */
            for (int i = 0; i < seen.size(); i++) {
                ((Texture2D) seen.elementAt(i)).setFiltering(Texture2D.FILTER_BASE_LEVEL,
                    pass == 0 ? Texture2D.FILTER_NEAREST : Texture2D.FILTER_LINEAR);
            }
            g.bindTarget(target, true, 0);
            g.setViewport(0, 0, 240, 320);
            g.setCamera(c, new Transform());
            Transform tr = new Transform();
            tr.postTranslate(-1.92f, 0.83f, -6.07f);
            tr.postScale(0.012f, 0.012f, 0.012f);
            g.render(sm, tr);
            g.releaseTarget();
            String info = com.sun.m3g.NativeBridge.lastTexInfo;          /* "idx=N tex=H" */
            int h = Integer.parseInt(info.substring(info.indexOf("tex=") + 4).trim());
            System.out.println("   pass " + pass + " (" + (pass == 0 ? "NEAREST" : "LINEAR") + "): draw texture argument = 0x" + Integer.toHexString(h));
            check(((h >> 16) & 1) == (pass == 0 ? 1 : 0), "bit 16 is " + (pass == 0 ? "set" : "clear"));
            check((h & 0xFFFF) != 0, "the handle below it is still there");
        }
        System.out.println(fails == 0 ? "PASSED" : "FAILED (" + fails + ")");
    }
}
