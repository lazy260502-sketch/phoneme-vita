package javax.microedition.m3g;

import java.io.*;
import java.util.*;

/**
 * For every file given: counts Texture2D wrapping / filtering, CompositingMode blending / alpha threshold, and how
 * many Image2D have transparent texels (alpha 0) whose RGB is black - the data behind "dark edges on textures".
 * Usage: TexScan file...   (files that are not M3G are skipped)
 */
public class TexScan {
    static Map wrap = new TreeMap(), filt = new TreeMap(), blend = new TreeMap(), thr = new TreeMap();
    static int images = 0, imagesWithClear = 0, imagesClearBlack = 0, textures = 0;
    static Map usage = new TreeMap();

    static boolean hasClear(Image2D im) {
        if (im == null || im.pixels == null || im.mutable) return false;
        for (int k = 0; k < im.pixels.length; k++) if ((im.pixels[k] >>> 24) == 0) return true;
        return false;
    }
    static long clearTexels = 0, clearBlackTexels = 0, totalTexels = 0;

    static void inc(Map m, String k) {
        Integer v = (Integer) m.get(k);
        m.put(k, new Integer(v == null ? 1 : v.intValue() + 1));
    }

    public static void main(String[] args) throws Exception {
        int files = 0;
        for (int a = 0; a < args.length; a++) {
            File x = new File(args[a]);
            byte[] data = new byte[(int) x.length()];
            DataInputStream in = new DataInputStream(new FileInputStream(x));
            in.readFully(data);
            in.close();
            Object3D[] objs;
            try {
                objs = M3gReader.readAll(data, com.sun.m3g.M3gFile.parse(data, 0));
            } catch (Throwable t) {
                continue;
            }
            files++;
            for (int i = 0; i < objs.length; i++) {
                Object3D o = objs[i];
                if (o instanceof Texture2D) {
                    Texture2D t = (Texture2D) o;
                    textures++;
                    inc(wrap, "S=" + (t.wrapS == 240 ? "CLAMP" : t.wrapS == 241 ? "REPEAT" : "?" + t.wrapS)
                        + " T=" + (t.wrapT == 240 ? "CLAMP" : t.wrapT == 241 ? "REPEAT" : "?" + t.wrapT));
                    inc(filt, "level=" + t.levelFilter + " image=" + (t.imageFilter == 209 ? "NEAREST"
                        : t.imageFilter == 210 ? "LINEAR" : "?" + t.imageFilter));
                } else if (o instanceof Appearance) {
                    Appearance ap = (Appearance) o;
                    Texture2D t0 = ap.getTexture(0);
                    if (t0 != null && hasClear(t0.getImage())) {
                        CompositingMode cm = ap.getCompositingMode();
                        inc(usage, "blending=" + (cm == null ? "none(REPLACE)" : String.valueOf(cm.getBlending())));
                    }
                } else if (o instanceof CompositingMode) {
                    CompositingMode c = (CompositingMode) o;
                    inc(blend, "blending=" + c.getBlending());
                    inc(thr, "alphaThreshold=" + (int) (c.getAlphaThreshold() * 255.0f + 0.5f) + "/255");
                } else if (o instanceof Image2D) {
                    Image2D im = (Image2D) o;
                    if (im.pixels == null || im.mutable) continue;
                    images++;
                    int clear = 0, clearBlack = 0;
                    for (int k = 0; k < im.pixels.length; k++) {
                        int p = im.pixels[k];
                        if ((p >>> 24) == 0) {
                            clear++;
                            if ((p & 0xFFFFFF) == 0) clearBlack++;
                        }
                    }
                    totalTexels += im.pixels.length;
                    clearTexels += clear;
                    clearBlackTexels += clearBlack;
                    if (clear > 0) imagesWithClear++;
                    if (clear > 0 && clearBlack * 10 >= clear * 9) imagesClearBlack++;
                }
            }
        }
        System.out.println("files=" + files + " textures=" + textures + " images=" + images);
        System.out.println("alpha-0 images by the blending of the Appearances that use them:");
        System.out.println("  " + usage);
        System.out.println("wrap:   " + wrap);
        System.out.println("filter: " + filt);
        System.out.println("blend:  " + blend);
        System.out.println("thresh: " + thr);
        System.out.println("images with alpha-0 texels: " + imagesWithClear + " of " + images
            + "; of those, mostly (>=90%) black RGB: " + imagesClearBlack);
        System.out.println("texels: " + totalTexels + " total, " + clearTexels + " alpha-0, " + clearBlackTexels
            + " of those with RGB 0");
    }
}
