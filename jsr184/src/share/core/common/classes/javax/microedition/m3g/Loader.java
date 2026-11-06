package javax.microedition.m3g;

import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.io.InputStream;

import javax.microedition.io.Connector;

import com.sun.m3g.M3gFile;

/**
 * M3G resource loader.
 *
 * Implemented:
 *   - PNG / JPEG resources -> a single Image2D (JSR 184 requires this)
 *   - .m3g container parsing and validation (see com.sun.m3g.M3gFile)
 *
 * NOT implemented yet: object deserialisation. The container layer proves a
 * file is well formed and lists its objects; turning those bytes into live
 * Object3D trees is the remaining piece (see PROJECT_MEMORY). Titles that
 * build their scene in code never call this.
 */
public class Loader {

    private static final byte[] M3G_ID = {
        (byte)0xAB, 'J', 'S', 'R', '1', '8', '4', (byte)0xBB, 13, 10, 26, 10
    };
    private static final byte[] PNG_ID = {
        (byte)0x89, 'P', 'N', 'G', 13, 10, 26, 10
    };
    private static final byte[] JPEG_ID = { (byte)0xFF, (byte)0xD8 };

    private Loader() {
    }

    public static Object3D[] load(String name) throws IOException {
        if (name == null) {
            throw com.sun.m3g.M3gTrace.fail(new NullPointerException());
        }
        /* v03.24: read the resource as a JAR ENTRY, not through Connector.
         *
         * Connector.openInputStream() rejects a bare path: CLDC's
         * Connector.openPrim() does `int colon = name.indexOf(':')` and throws
         * IllegalArgumentException("no ':' in URL") when colon < 1. Titles call
         * Loader.load("/jill_asa.m3g") and Loader.load("/c") - no scheme - so
         * the load ALWAYS failed. Both games wrap the call in
         * `catch (Exception) { return null; }`, so the failure never reached
         * the log; the model array simply stayed null and nothing was drawn.
         * That is log130: 54 image opens, ZERO ".m3g" opens, zero renderer
         * frames, and a blank 3D area (生化危机) / blue screen (极品飞车).
         *
         * Class.getResourceAsStream() takes the same absolute spelling: it
         * strips a leading '/' in Class.resolveName() and looks the remainder
         * up from the jar root, which is exactly what these titles mean.
         * A name that really is a URL ("file:", "http:") still goes through
         * Connector. */
        byte[] data = readResource(name);
        if (data == null) {
            trace("load FAILED '" + name + "' (not found)");
            throw new IOException("cannot open " + name);
        }
        /* External images inside the .m3g are named relative to that file, so
         * resolve them against its directory. Keep the leading '/' on the
         * result (base = "/" for a root-level model) so the lookup is absolute
         * - a relative name would be prefixed with javax/microedition/lcdui/
         * and miss. */
        int slash = name.lastIndexOf('/');
        String base = (slash >= 0) ? name.substring(0, slash + 1) : null;
        try {
            Object3D[] objs = loadM3G(data, 0, base);
            if (dbgLoads < DBG_LOAD_MAX) {
                dbgLoads++;
                trace("load '" + name + "' ok objects="
                      + (objs == null ? 0 : objs.length));
            }
            return objs;
        } catch (IOException e) {
            trace("load FAILED '" + name + "' (" + e + ")");
            throw e;
        } catch (RuntimeException e) {
            trace("load FAILED '" + name + "' (" + e + ")");
            throw new IOException("bad m3g: " + name);
        }
    }

    /* Bounded so a title that probes for optional models cannot flood the log. */
    private static int dbgLoads = 0;
    private static final int DBG_LOAD_MAX = 64;

    /**
     * Reads a MIDlet-suite resource by name. Returns null when nothing matches.
     * <p>
     * A leading '/' means "from the jar root" (Class.resolveName strips it);
     * a name that carries a URL scheme is left to Connector.
     */
    private static byte[] readResource(String name) throws IOException {
        InputStream in = null;
        try {
            in = Loader.class.getResourceAsStream(name);
        } catch (RuntimeException ignored) {
            in = null;
        }
        if (in == null && name.indexOf(':') > 0) {
            try {
                in = Connector.openInputStream(name);
            } catch (RuntimeException ignored) {
                in = null;
            }
        }
        if (in == null) {
            return null;
        }
        try {
            return readAll(in);
        } finally {
            try {
                in.close();
            } catch (IOException ignored) {
            }
        }
    }

    public static Object3D[] load(byte[] data, int offset) throws IOException {
        if (data == null) {
            throw com.sun.m3g.M3gTrace.fail(new NullPointerException());
        }
        if (offset < 0 || offset >= data.length) {
            throw com.sun.m3g.M3gTrace.fail(new IndexOutOfBoundsException());
        }
        if (matches(data, offset, M3G_ID)) {
            return loadM3G(data, offset);
        }
        if (matches(data, offset, PNG_ID)) {
            return single(imageFromBytes(data, offset, pngFormat(data, offset)));
        }
        if (matches(data, offset, JPEG_ID)) {
            return single(imageFromBytes(data, offset, jpegFormat(data, offset)));
        }
        throw new IOException("unrecognised resource type");
    }

    private static Object3D[] loadM3G(byte[] data, int offset) throws IOException {
        return loadM3G(data, offset, null);
    }

    private static Object3D[] loadM3G(byte[] data, int offset, String base)
            throws IOException {
        M3gFile f = M3gFile.parse(data, offset);
        StringBuffer log = new StringBuffer(128);
        int before = M3gReader.missedImages;
        Object3D[] objs = M3gReader.readAll(data, f, base, log);
        if (M3gReader.missedImages != before) {
            /* Resource resolution failures are the usual reason a loaded
             * model shows up untextured; log which ones and how many. */
            javax.microedition.m3g.Loader.trace(log.toString());
        }
        return objs;
    }

    private static void trace(String msg) {
        try {
            com.sun.m3g.M3gTrace.log(msg);
        } catch (Throwable ignored) {
        }
    }

    private static Object3D[] single(Image2D img) {
        Object3D[] r = new Object3D[1];
        r[0] = img;
        return r;
    }

    private static Image2D imageFromBytes(byte[] data, int offset, int format)
            throws IOException {
        javax.microedition.lcdui.Image img =
            javax.microedition.lcdui.Image.createImage(data, offset,
                                                       data.length - offset);
        int f = (format == 0) ? Image2D.RGB : format;
        return new Image2D(f, img);
    }

    /** PNG colour type -> Image2D format (0 when unknown). */
    private static int pngFormat(byte[] d, int off) {
        int p = off + PNG_ID.length;
        while (p + 8 < d.length) {
            int len = be32(d, p);
            int type = be32(d, p + 4);
            if (type == 0x49484452) {                 /* IHDR */
                if (p + 8 + 10 < d.length) {
                    switch (d[p + 8 + 9] & 0xFF) {    /* colour type */
                    case 0: return Image2D.LUMINANCE;
                    case 4: return Image2D.LUMINANCE_ALPHA;
                    case 6: return Image2D.RGBA;
                    default: return Image2D.RGB;      /* 2, 3 */
                    }
                }
                return 0;
            }
            if (len < 0 || p + 12 + len > d.length) {
                return 0;
            }
            p += 12 + len;
        }
        return 0;
    }

    /** JPEG SOFn component count -> Image2D format (0 when unknown). */
    private static int jpegFormat(byte[] d, int off) {
        int p = off + 2;
        while (p + 3 < d.length) {
            if ((d[p] & 0xFF) != 0xFF) {
                p++;
                continue;
            }
            int m = d[p + 1] & 0xFF;
            p += 2;
            if (m == 0xD8 || m == 0xD9 || (m >= 0xD0 && m <= 0xD7)) {
                continue;                            /* no length field */
            }
            if (p + 2 > d.length) {
                return 0;
            }
            int len = ((d[p] & 0xFF) << 8) | (d[p + 1] & 0xFF);
            boolean sof = (m >= 0xC0 && m <= 0xC3) || (m >= 0xC5 && m <= 0xC7)
                       || (m >= 0xC9 && m <= 0xCB) || (m >= 0xCD && m <= 0xCF);
            if (sof) {
                int q = p + 2 + 5;                   /* len(2) prec(1) h(2) w(2) */
                if (q >= d.length) {
                    return 0;
                }
                return ((d[q] & 0xFF) == 1) ? Image2D.LUMINANCE : Image2D.RGB;
            }
            if (len < 2) {
                return 0;
            }
            p += len;
        }
        return 0;
    }

    private static int be32(byte[] d, int p) {
        return ((d[p] & 0xFF) << 24) | ((d[p + 1] & 0xFF) << 16)
             | ((d[p + 2] & 0xFF) << 8) | (d[p + 3] & 0xFF);
    }

    private static boolean matches(byte[] d, int off, byte[] id) {
        if (d.length - off < id.length) {
            return false;
        }
        for (int i = 0; i < id.length; i++) {
            if (d[off + i] != id[i]) {
                return false;
            }
        }
        return true;
    }

    private static byte[] readAll(InputStream in) throws IOException {
        ByteArrayOutputStream out = new ByteArrayOutputStream(8192);
        byte[] buf = new byte[4096];
        int n;
        while ((n = in.read(buf)) > 0) {
            out.write(buf, 0, n);
        }
        return out.toByteArray();
    }
}
