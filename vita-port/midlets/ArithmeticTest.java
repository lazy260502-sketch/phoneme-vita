/*
 * ArithmeticTest - a numeric fingerprint of the VM, built to settle ONE question:
 * does the device compute the same integers as a host JVM for the operations
 * behind 极品飞车's glyph coordinates?
 *
 * Why: every log so far shows the IDIOMA glyphs at x=209..366 while the panel
 * they belong to sits at x=64..224 on a 240-wide canvas, and y is always
 * correct. The title computes those numbers itself, in this form (verified by
 * `javap -p -c` on z.a(Graphics,int,int,int,int,int,int)):
 *
 *     packed  = d[i++];  type = (packed >> 14) & 3;  key = packed & 0x3FFF;
 *     targetPos = d[i++];  targetVal = d[i++];
 *     cur = c[base + key];
 *     t  = progress - curPos;
 *     c[base + key] = (short)( ((targetPos - t) * cur + t * targetVal)
 *                              / targetPos );
 *     x = x0 + (width * spread) / 255;              // z.a(int,int,int)
 *
 * Everything there is integer arithmetic: idiv, imul, isub, ishl, ishr, iand,
 * i2s (narrow to short) and saload/sastore. If any of those is wrong in this
 * port's interpreter, the coordinates come out wrong while the code still runs
 * to completion - exactly what is observed. The host has the same numbers, so
 * a seed + a printed hash is enough to compare the two.
 *
 * Output goes to System.out (vm_output.log). Run it and diff against the
 * expectations printed by `java ArithTest` on the host.
 */
import javax.microedition.midlet.MIDlet;
import javax.microedition.lcdui.*;

public class ArithmeticTest extends MIDlet implements Runnable {

    private static final int SEED = 0x2545F491;

    private volatile boolean running = true;

    public void startApp() {
        Thread t = new Thread(this);
        t.start();
    }

    public void pauseApp() { }
    public void destroyApp(boolean unconditional) { running = false; }

    public void run() {
        try {
            p("[ARITH] begin");

            /* --- 1. idiv: sign, exact division, truncation --- */
            int[] a = { 7, -7, 7, -7, 100, -100, 65536, 65535 };
            int[] b = { 2, 2, -2, -2, 7, 7, 255, 255 };
            for (int i = 0; i < a.length; i++) {
                p("[ARITH] div " + a[i] + "/" + b[i] + "=" + (a[i] / b[i])
                  + " rem=" + (a[i] % b[i]));
            }

            /* --- 2. imul / overflow wrap --- */
            p("[ARITH] mul " + (65535 * 65535)
              + " " + (100000 * 30000)
              + " " + (-2147483648 / -1 == -2147483648 ? "MIN/-1 wraps" : "?"));

            /* --- 3. shifts and masks (packed-key decode) --- */
            int[] packed = { 0x7FFF, 0x4001, 0x8003, 0xC005, 0x0010 };
            for (int i = 0; i < packed.length; i++) {
                int v = packed[i];
                p("[ARITH] pack " + v + " type=" + ((v >> 14) & 3)
                  + " key=" + (v & 0x3FFF)
                  + " shl4=" + ((short) (v << 4)));
            }

            /* --- 4. i2s narrowing (sastore into short[]) --- */
            short[] s = new short[8];
            int[] vals = { 0, 1, -1, 32767, 32768, -32768, -32769, 70000 };
            for (int i = 0; i < vals.length; i++) {
                s[i] = (short) vals[i];
            }
            StringBuffer sb = new StringBuffer();
            for (int i = 0; i < s.length; i++) {
                sb.append(s[i]);
                sb.append(' ');
            }
            p("[ARITH] i2s " + sb.toString().trim());

            /* --- 5. arraycopy of a short[] then saload back --- */
            short[] src = new short[16];
            for (int i = 0; i < src.length; i++) {
                src[i] = (short) (i * 37 - 200);
            }
            short[] dst = new short[16];
            System.arraycopy(src, 4, dst, 4, 6);
            sb = new StringBuffer();
            for (int i = 0; i < dst.length; i++) {
                sb.append(dst[i]);
                sb.append(' ');
            }
            p("[ARITH] arraycopy " + sb.toString().trim());

            /* --- 6. the interpolation, on the real recipe --- */
            /* (targetPos - t) * cur + t * targetVal) / targetPos */
            int[][] cases = {
                /* targetPos, t, cur, targetVal */
                { 255, 0, 64, 240 },
                { 255, 255, 64, 240 },
                { 255, 128, 64, 240 },
                { 176, 0, 0, 100 },
                { 176, 88, 0, 100 },
                { 4096, 1024, -30, 130 },
                { 1000, 1, 0, 255 },
            };
            for (int i = 0; i < cases.length; i++) {
                int tp = cases[i][0], t = cases[i][1];
                int cur = cases[i][2], tv = cases[i][3];
                int r = ((tp - t) * cur + t * tv) / tp;
                p("[ARITH] interp tp=" + tp + " t=" + t + " cur=" + cur
                  + " tv=" + tv + " -> " + r + " (short " + ((short) r) + ")");
            }

            /* --- 7. the scale-up used for x: x0 + (W * spread) / 255 --- */
            int W = 240, H = 320;
            int[] spread = { 0, 1, 64, 100, 128, 187, 200, 222, 255 };
            for (int i = 0; i < spread.length; i++) {
                p("[ARITH] scale spread=" + spread[i]
                  + " x=" + (W * spread[i]) / 255
                  + " y=" + (H * spread[i]) / 255);
            }

            /* --- 8. a deterministic pseudo-random walk, as a fingerprint --- */
            int x = SEED;
            int sum = 0;
            for (int i = 0; i < 4096; i++) {
                x = x * 1103515245 + 12345;
                int key = (x >> 16) & 0x3FFF;
                int pos = (x >> 8) & 0xFF;
                int cur = (x & 0xFFFF) - 32768;
                int val = ((x >> 20) & 0x3FF) - 512;
                int tp = 255;
                int t = pos;
                int r = ((tp - t) * cur + t * val) / tp;
                sum = (sum + ((short) r) + key) & 0x7FFFFFFF;
            }
            p("[ARITH] fingerprint sum=" + sum + " x=" + x);

            p("[ARITH] done");
        } catch (Throwable t) {
            p("[ARITH] EXCEPTION " + t);
            t.printStackTrace();
        }
        notifyDestroyed();
    }

    private static void p(String s) {
        System.out.println(s);
    }
}
