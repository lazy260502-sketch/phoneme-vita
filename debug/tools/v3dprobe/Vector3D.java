package com.capcom.mascotcapsule;

public class Vector3D {
    public int x;
    public int y;
    public int z;

    private static int logged = 0;
    private static int calls = 0;

    public Vector3D() {
    }

    public Vector3D(int x, int y, int z) {
        this.x = x;
        this.y = y;
        this.z = z;
    }

    /* Non-final statics keep javac from folding these, so each one is a real ldc2_w in <clinit>. */
    private static double K4096 = 4096.0;
    private static double K1000 = 1000.0;
    private static double K16 = 16.0;
    private static double KHALF = 0.5;

    private static void controls() {
        double two = (double) 2;
        double v4096 = (double) 4096;
        System.out.println("[V3D] ctl ldc2_w(expect 4096 1000 16 | half*8=4): " + (int) K4096 + " " + (int) K1000
            + " " + (int) K16 + " | " + (int) (KHALF * 8.0 * (double) 1)
            + " ## no-const(expect 4096 8192 4 4096): " + (int) v4096 + " " + (int) (two * v4096)
            + " " + (int) Math.sqrt((double) 16) + " " + (int) ((double) 8192 / two)
            + " ## mixed(expect 4 8192 4): " + (int) Math.sqrt(K16) + " " + (int) (two * K4096)
            + " " + (int) (K16 / two / two));
    }

    public final void unit() {
        if (calls == 0) {
            controls();
        }
        double dx = (double) x;
        double dy = (double) y;
        double dz = (double) z;
        double sum = dx * dx + dy * dy + dz * dz;
        double len = Math.sqrt(sum);
        int ox = x, oy = y, oz = z;
        x = (int) (dx / len * 4096.0);
        y = (int) (dy / len * 4096.0);
        z = (int) (dz / len * 4096.0);
        calls++;
        boolean zeroIn = (ox == 0 && oy == 0 && oz == 0);
        if (!zeroIn && logged < 40) {
            logged++;
            long s2 = (long) ox * ox + (long) oy * oy + (long) oz * oz;
            System.out.println("[V3D] unit in=(" + ox + "," + oy + "," + oz + ") sum2(long)=" + s2
                + " sum(int)=" + (int) (sum / 1000.0 + 0.5) + "k"
                + " len(int)=" + (int) len + " len*8(int)=" + (int) (len * 8.0)
                + " nx(int)=" + (int) (dx / len * 100.0)
                + " out=(" + x + "," + y + "," + z + ") call#" + calls);
        }
    }

    public static final Vector3D outerProduct(Vector3D a, Vector3D b) {
        Vector3D r = new Vector3D();
        r.x = a.y * b.z - a.z * b.y;
        r.y = a.z * b.x - a.x * b.z;
        r.z = a.x * b.y - a.y * b.x;
        return r;
    }

    public static final int innerProduct(Vector3D a, Vector3D b) {
        return a.x * b.x + a.y * b.y + a.z * b.z;
    }
}
