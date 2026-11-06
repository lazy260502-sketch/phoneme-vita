/*
 * VitaSensor.java - the sensor list, the URL matcher and the data source.
 *
 * DATA SOURCE: synthetic. The console does have a six-axis unit (sceMotion),
 * but wiring it needs a native bridge plus a symbol-resolution path for classes
 * that live in the jar rather than in the ROM; this version therefore produces
 * a slow, plausible tilt so that a game reading the accelerometer sees
 * changing, in-range values instead of nothing. PROJECT_MEMORY.md records this
 * as the next step.
 */
package javax.microedition.sensor;

import java.io.DataInputStream;
import java.io.IOException;

import javax.microedition.io.Connector;
import javax.microedition.io.file.FileConnection;

final class VitaSensor {

    static final String MODEL = "vita";
    /** The specification requires at least 256. */
    static final int MAX_BUFFER = 256;

    private static SensorInfo[] sensors;

    private VitaSensor() {
    }

    static synchronized SensorInfo[] allSensors() {
        if (sensors == null) {
            sensors = new SensorInfo[] {
                new VitaSensorInfo("acceleration",
                        SensorInfo.CONTEXT_TYPE_DEVICE,
                        new String[] { "axis_x", "axis_y", "axis_z" },
                        VitaSensorInfo.KIND_ACCEL),
                new VitaSensorInfo("angular_velocity",
                        SensorInfo.CONTEXT_TYPE_DEVICE,
                        new String[] { "rot_x", "rot_y", "rot_z" },
                        VitaSensorInfo.KIND_GYRO)
            };
        }
        return sensors;
    }

    /*
     * REAL SAMPLES. The launcher publishes the console's six-axis data as a
     * 24-byte file (see vita-port/src/vita_motion.c): 6 little-endian float32,
     * ax ay az in g and gx gy gz in rad/s. We read it at most every 20 ms and
     * remember the first URL that works; if none does (Vita3K, or a build
     * without the publisher) value() falls back to the synthetic tilt below.
     */
    private static final String[] MOTION_URLS = {
        /* JSR-75 roots are the device volumes (see jsr75_file.c:26-33), so the
         * first candidate is the fully qualified one. The others cover a
         * future root policy change; the synthetic tilt covers none matching. */
        "file:///ux0/data/J2ME00001/motion.bin",
        "file:///motion.bin",
        "file:///data/J2ME00001/motion.bin",
        "file://motion.bin"
    };
    private static final float[] real = new float[6];
    private static long lastReadMs = -10000L;
    private static int goodUrl = -1;
    private static boolean haveReal = false;

    private static synchronized void refresh() {
        long now = System.currentTimeMillis();
        if (now - lastReadMs < 20) {
            return;
        }
        lastReadMs = now;
        int n = MOTION_URLS.length;
        int start = (goodUrl >= 0) ? goodUrl : 0;
        for (int k = 0; k < n; k++) {
            int i = (start + k) % n;
            FileConnection fc = null;
            DataInputStream in = null;
            try {
                fc = (FileConnection) Connector.open(MOTION_URLS[i],
                                                     Connector.READ);
                if (!fc.exists()) {
                    continue;
                }
                in = fc.openDataInputStream();
                real[0] = in.readFloat();
                real[1] = in.readFloat();
                real[2] = in.readFloat();
                real[3] = in.readFloat();
                real[4] = in.readFloat();
                real[5] = in.readFloat();
                /* A console lying still reads about +-1 g on some axis. All
                 * zeros means sceMotion produced nothing (Vita3K does not
                 * emulate the six-axis unit): treat the file as absent rather
                 * than report free fall, which is worse than fake data. */
                if (Math.abs(real[0]) + Math.abs(real[1]) + Math.abs(real[2])
                        < 0.05) {
                    haveReal = false;
                    continue;
                }
                haveReal = true;
                goodUrl = i;
                return;
            } catch (Throwable t) {
                haveReal = false;       /* try the next URL */
            } finally {
                if (in != null) {
                    try {
                        in.close();
                    } catch (Throwable t) {
                    }
                }
                if (fc != null) {
                    try {
                        fc.close();
                    } catch (Throwable t) {
                    }
                }
            }
        }
    }

    /* ---- data source ------------------------------------------- */

    /*
     * One sample. Returns the console's real value when the publisher file is
     * readable, otherwise the synthetic tilt (a game that only checks "does the
     * API work" still sees sane numbers).
     */
    static double value(int kind, int axis, long t) {
        refresh();
        if (haveReal) {
            if (kind == VitaSensorInfo.KIND_ACCEL) {
                return real[axis] * 9.81;   /* the file carries g, the API m/s^2 */
            }
            return real[3 + axis];
        }
        return synthetic(kind, axis, t);
    }

    private static double synthetic(int kind, int axis, long t) {
        double s = (double) t / 1000.0;
        if (kind == VitaSensorInfo.KIND_ACCEL) {
            if (axis == 0) {
                return 0.90 * Math.sin(s * 0.90);
            }
            if (axis == 1) {
                return 0.90 * Math.cos(s * 0.70);
            }
            return 9.81 + 0.15 * Math.sin(s * 1.30);
        }
        if (axis == 0) {
            return 0.35 * Math.sin(s * 1.10);
        }
        if (axis == 1) {
            return 0.35 * Math.cos(s * 0.80);
        }
        return 0.20 * Math.sin(s * 0.50);
    }

    /* ---- sensor URL handling ------------------------------------- */

    /** Quantity part of a URL, with the scheme and slashes removed. */
    static String quantityOf(String url) {
        String u = url;
        int i = u.indexOf("://");
        if (i >= 0) {
            u = u.substring(i + 3);
        } else if (u.startsWith("//")) {
            u = u.substring(2);
        }
        int end = u.indexOf(';');
        if (end >= 0) {
            u = u.substring(0, end);
        }
        return u.length() == 0 ? null : u;
    }

    static String param(String url, String key) {
        String pat = key + "=";
        int i = url.indexOf(pat);
        if (i < 0) {
            return null;
        }
        int start = i + pat.length();
        int end = url.indexOf(';', start);
        return (end < 0) ? url.substring(start) : url.substring(start, end);
    }

    static boolean urlMatches(SensorInfo s, String url) {
        String q = quantityOf(url);
        if (q == null || !q.equals(s.getQuantity())) {
            return false;
        }
        String ctx = param(url, "contextType");
        if (ctx != null && !ctx.equals(s.getContextType())) {
            return false;
        }
        String model = param(url, "model");
        if (model != null && !model.equals(s.getModel())) {
            return false;
        }
        return true;
    }

    static SensorConnection open(String name) throws IOException {
        if (name == null) {
            throw new IOException("null sensor URL");
        }
        SensorInfo[] all = allSensors();
        for (int i = 0; i < all.length; i++) {
            if (urlMatches(all[i], name)) {
                return new VitaSensorConnection((VitaSensorInfo) all[i]);
            }
        }
        throw new IOException("no sensor matches " + name);
    }
}
