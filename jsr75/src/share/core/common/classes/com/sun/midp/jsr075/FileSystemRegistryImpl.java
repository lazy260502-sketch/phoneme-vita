/*
 * JSR 75 registry implementation for the PS Vita port.
 *
 * Vita-local subsystem, original implementation.
 *
 * Root model: exactly one root, named ROOT_NAME, whose URL is
 * "file:///data/" (equivalently "file://localhost/data/", both are
 * accepted by resolvePath()).  It maps onto the application data
 * directory the launcher chdir()s into, i.e. the same directory
 * FileStore.getRootPath() reports.  No other root (ux0: elsewhere,
 * imc0:, uma0:, ...) is exposed.
 */
package com.sun.midp.jsr075;

import java.util.Enumeration;
import java.util.Vector;

import javax.microedition.io.file.FileSystemListener;

/**
 * Single-root implementation of the JSR 75 file system registry.
 */
public class FileSystemRegistryImpl {

    /** Name of the only exposed root, without the trailing '/'. */
    public static final String ROOT_NAME = "data";

    /**
     * The root name as handed out by {@link #listRoots()}.
     *
     * <p>JSR 75 hands out root <em>names</em>, not URLs: the name may be
     * appended to "file:///" to form a URL, so it carries the trailing
     * '/'.  Handing out ROOT_URL here instead makes every caller that
     * does the documented <code>"file:///" + root</code> build
     * "file:///file:///data/" and fail.</p>
     */
    public static final String ROOT_LIST_NAME = ROOT_NAME + "/";

    /** URL of the only exposed root. */
    public static final String ROOT_URL = "file:///" + ROOT_LIST_NAME;

    /** Registered listeners; never fired on this platform. */
    private static final Vector listeners = new Vector();

    /** Not instantiable. */
    private FileSystemRegistryImpl() {
    }

    /**
     * Lists the roots.
     *
     * @return an enumeration holding the single root name
     */
    public static Enumeration listRoots() {
        Vector roots = new Vector(1);
        roots.addElement(ROOT_LIST_NAME);
        return roots.elements();
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
     * Translates a JSR 75 URL into the absolute path handed to the native
     * layer.
     *
     * <p>Accepted form: <code>file://[localhost]/data[/rest]</code>.  The
     * returned path is the root reported by
     * {@link FileStore#getRootPath()} followed by the remainder, so the
     * two never diverge.</p>
     *
     * @param url the connection URL
     * @return the absolute path, no trailing slash except for the root
     * @exception IllegalArgumentException if the URL is not a JSR 75 URL
     *              for the exposed root or tries to escape it
     */
    public static String resolvePath(String url) {
        final String scheme = "file://";

        if (url == null || !url.startsWith(scheme)) {
            throw new IllegalArgumentException("not a file URL: " + url);
        }

        /* JSR 75 URL syntax is file://<host>/<root>/<path>.  The host of the
         * local device is "localhost"; the empty authority ("file:///data")
         * is the accepted abbreviation of the same thing.  Both spellings
         * are in the wild, and a MIDlet that does the documented
         * "file://localhost/" + root build of a name from listRoots() --
         * MiniXplorer does exactly that -- must reach the same root as one
         * that uses "file:///".  Any other host names a device this
         * single-root port does not expose. */
        String rest = url.substring(scheme.length());   // "localhost/data/x"
        int hostEnd = rest.indexOf('/');
        String host = (hostEnd == -1) ? rest : rest.substring(0, hostEnd);
        if (host.length() != 0 && !host.equalsIgnoreCase("localhost")) {
            throw new IllegalArgumentException(
                    "unknown host: " + host + " in " + url);
        }
        rest = (hostEnd == -1) ? "" : rest.substring(hostEnd + 1);
        String root = FileStore.getRootPath();          // "ux0:/data/J2ME00001/"

        if (rest.length() == 0) {
            throw new IllegalArgumentException("missing root: " + url);
        }

        int slash = rest.indexOf('/');
        String rootName = (slash == -1) ? rest : rest.substring(0, slash);
        if (!rootName.equals(ROOT_NAME)) {
            throw new IllegalArgumentException("unknown root: " + rootName);
        }

        String rel = (slash == -1) ? "" : rest.substring(slash + 1);
        if (rel.length() == 0) {
            return root;            /* a URL without a trailing slash */
        }
        if (rel.charAt(rel.length() - 1) == '/') {
            rel = rel.substring(0, rel.length() - 1);
            if (rel.length() == 0) {
                return root;
            }
        }
        checkRelative(rel);

        if (root.length() > 0 && root.charAt(root.length() - 1) == '/') {
            return root + rel;
        }
        return root + "/" + rel;
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
