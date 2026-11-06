package com.nokia.mid.sound;

/** Writes RingTone.toMidi(blob) of each file argument to <file>.mid (host helper for render_smf.c). */
public class DumpMidi {
    public static void main(String[] a) throws Exception {
        for (int i = 0; i < a.length; i++) {
            java.io.File f = new java.io.File(a[i]);
            byte[] d = new byte[(int) f.length()];
            java.io.FileInputStream in = new java.io.FileInputStream(f);
            int n = 0;
            while (n < d.length) { int r = in.read(d, n, d.length - n); if (r < 0) break; n += r; }
            in.close();
            byte[] m = RingTone.toMidi(d);
            System.out.println(a[i] + " (" + d.length + " bytes) -> " + (m == null ? "null" : m.length + " bytes SMF"));
            if (m != null) {
                java.io.FileOutputStream o = new java.io.FileOutputStream(a[i] + ".mid");
                o.write(m);
                o.close();
            }
        }
    }
}
