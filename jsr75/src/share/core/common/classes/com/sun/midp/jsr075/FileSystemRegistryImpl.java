/*
 * JSR 75 registry implementation for the PS Vita port.
 *
 * Vita-local subsystem, original implementation.
 *
 * Root model: a root is the root directory of one of the Vita's mounted
 * storage volumes, named after the device it lives on - "ux0/", "imc0/",
 * "uma0/", "app0/" - which is the shape JSR 75 expects, since the other
 * platforms that expose file systems hand out names like "C:/" and "E:/".
 * The URL file:///ux0/data therefore maps onto the device absolute path
 * ux0:/data, which is exactly what the native layer wants: PCSL's
 * vita_resolve_path() passes any "<device>:" prefixed path straight
 * through, and sceIo* wants it in that form too.
 *
 * The application data directory the launcher chdir()s into is no longer
 * a root of its own: it is reachable as ux0/data/J2ME00001 like any other
 * directory.  That is the point of this model - a file manager MIDlet
 * lists the whole device instead of being locked into the directory the
 * port happens to keep its jar and config in.
 */
package com.sun.midp.jsr075;

import java.util.Enumeration;
import java.util.Vector;

import javax.microedition.io.file.FileSystemListener;

/**
 * Multi-root implementation of the JSR 75 file system registry.
 */
public class FileSystemRegistryImpl {

    /**
     * Candidate roots, in list order, as device names without the ':'.
     *
     * <p>ux0 (main storage) and app0 (this application's own package) are
     * mounted for the whole lifetime of the process; imc0 and uma0 are
     * optional cards, so they are only listed when the volume answers a
     * directory probe.</p>
     */
    private static final String[] ROOT_DEVS = {"ux0", "imc0", "uma0", "app0"};

    /** True for the entries of ROOT_DEVS that need no existence probe. */
    private static final boolean[] ROOT_ALWAYS = {true, false, false, true};

    /**
     * The roots actually exposed, as device names without ':'.
     *
     * <p>Built on first use rather than in a static initialiser: the
     * probe below is a native call, and class initialisation must not
     * depend on native code (the ROMizer links these classes at build
     * time).</p>
     */
    private static Vector roots = null;

    /** Registered listeners; never fired on this platform. */
    private static final Vector listeners = new Vector();

    /** Not instantiable. */
    private FileSystemRegistryImpl() {
    }

    /**
     * The exposed roots, probing the optional volumes the first time.
     *
     * @return the roots as device names without ':'
     */
    private static synchronized Vector availableRoots() {
        if (roots == null) {
            Vector found = new Vector(ROOT_DEVS.length);
            for (int i = 0; i < ROOT_DEVS.length; i++) {
                if (ROOT_ALWAYS[i]
                        || FileStore.isDirectory(ROOT_DEVS[i] + ":/")) {
                    found.addElement(ROOT_DEVS[i]);
                }
            }
            roots = found;
        }
        return roots;
    }

    /**
     * Lists the roots, i.e. the root directory of every mounted volume.
     *
     * @return an enumeration of root names, each with a trailing '/'
     */
    public static Enumeration listRoots() {
        Vector found = availableRoots();
        Vector names = new Vector(found.size());
        for (int i = 0; i < found.size(); i++) {
            names.addElement(found.elementAt(i) + "/");
        }
        return names.elements();
    }

    /**
     * Registers a listener.
     *
     * @param listener the listener
     * @return false when it was already registered
     * @exception NullPointerException if listener is null
     */
    public static synchronized boolean addListener(FileSystemListener listener)
    {
        if (listener == null) {
            throw new NullPointerException();
        }
        if (listeners.indexOf(listener) != -1) {
            return false;
        }
        listeners.addElement(listener);
        return true;
    }

    /**
     * Removes a listener.
     *
     * @param listener the listener
     * @return true when it had been registered
     * @exception NullPointerException if listener is null
     */
    public static synchronized boolean removeListener(
            FileSystemListener listener) {
        if (listener == null) {
            throw new NullPointerException();
        }
        int index = listeners.indexOf(listener);
        if (index == -1) {
            return false;
        }
        listeners.removeElementAt(index);
        return true;
    }

    /**
     * Device name of a root, when it is one of the exposed roots.
     *
     * @param rootName the root name from the URL, without a trailing '/'
     * @return the device name ("ux0"), or null when unknown
     */
    private static String findRoot(String rootName) {
        Vector found = availableRoots();
        for (int i = 0; i < found.size(); i++) {
            if (found.elementAt(i).equals(rootName)) {
                return rootName;
            }
        }
        return null;
    }

    /**
     * Translates a JSR 75 URL into the paths the connection works with.
     *
     * <p>Accepted form: <code>file://[localhost]/&lt;root&gt;[/rest]</code>,
     * where the root is one of the names
     * {@link #listRoots()} hands out.  "<code>..</code>" elements are
     * rejected outright: the Vita file API resolves them against the
     * volume, so they could be used to climb out of the root.</p>
     *
     * @param url the connection URL
     * @return {absolute device path, root name, root-relative path}
     * @exception IllegalArgumentException if the URL is not a JSR 75 URL
     *              for an exposed root or tries to escape it
     */
    public static String[] resolve(String url) {
        final String scheme = "file://";

        if (url == null || !url.startsWith(scheme)) {
            throw new IllegalArgumentException("not a file URL: " + url);
        }

        /* JSR 75 URL syntax is file://<host>/<root>/<path>.  The host of the
         * local device is "localhost"; the empty authority ("file:///ux0")
         * is the accepted abbreviation of the same thing.  Both spellings
         * are in the wild, and a MIDlet that does the documented
         * "file://localhost/" + root build of a name from listRoots() --
         * MiniXplorer does exactly that -- must reach the same root as one
         * that uses "file:///".  Any other host names a device this port
         * does not expose. */
        String rest = url.substring(scheme.length());   // "localhost/ux0/x"
        int hostEnd = rest.indexOf('/');
        String host = (hostEnd == -1) ? rest : rest.substring(0, hostEnd);
        if (host.length() != 0 && !host.equalsIgnoreCase("localhost")) {
            throw new IllegalArgumentException(
                    "unknown host: " + host + " in " + url);
        }
        rest = (hostEnd == -1) ? "" : rest.substring(hostEnd + 1);

        if (rest.length() == 0) {
            throw new IllegalArgumentException("missing root: " + url);
        }

        int slash = rest.indexOf('/');
        String rootName = (slash == -1) ? rest : rest.substring(0, slash);
        String dev = findRoot(rootName);
        if (dev == null) {
            throw new IllegalArgumentException("unknown root: " + rootName);
        }

        String rel = (slash == -1) ? "" : rest.substring(slash + 1);
        while (rel.length() > 0 && rel.charAt(rel.length() - 1) == '/') {
            rel = rel.substring(0, rel.length() - 1);
        }
        if (rel.length() > 0) {
            checkRelative(rel);
        }

        String abs = dev + ":/";                // "ux0:/"
        if (rel.length() > 0) {
            abs = abs + rel;                    // "ux0:/data/x"
        }
        return new String[] { trimSlash(abs), rootName, rel };
    }

    /**
     * Translates a JSR 75 URL into the absolute path handed to the native
     * layer.
     *
     * @param url the connection URL
     * @return the absolute path, e.g. <code>ux0:/data/x</code> or
     *         <code>ux0:</code> for the root of a volume
     * @exception IllegalArgumentException if the URL is not a JSR 75 URL
     *              for an exposed root or tries to escape it
     */
    public static String resolvePath(String url) {
        return resolve(url)[0];
    }

    /**
     * Removes a trailing '/' from a device path.
     *
     * @param s input path
     * @return the path without a trailing '/'
     */
    private static String trimSlash(String s) {
        if (s.length() > 1 && s.charAt(s.length() - 1) == '/') {
            return s.substring(0, s.length() - 1);
        }
        return s;
    }

    /**
     * Rejects path elements that would leave the root.  The Vita file
     * API resolves ".." against the volume, so browsing up from the root
     * has to be stopped here rather than by sceIo*.
     *
     * @param rel the root-relative path
     * @exception IllegalArgumentException on a ".." element
     */
    private static void checkRelative(String rel) {
        int start = 0;
        while (start <= rel.length()) {
            int end = rel.indexOf('/', start);
            if (end == -1) {
                end = rel.length();
            }
            if (end - start == 2
                    && rel.charAt(start) == '.'
                    && rel.charAt(start + 1) == '.') {
                throw new IllegalArgumentException(
                        "path escapes the root: " + rel);
            }
            if (end == rel.length()) {
                break;
            }
            start = end + 1;
        }
    }
}
