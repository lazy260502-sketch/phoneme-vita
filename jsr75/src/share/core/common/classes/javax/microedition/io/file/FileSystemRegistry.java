/*
 * JSR 75 (PDA Optional Packages) public API for the PS Vita port.
 *
 * Vita-local subsystem: the interface signatures follow the JSR 75
 * specification, the documentation and the implementation are original.
 */
package javax.microedition.io.file;

import java.util.Enumeration;

/**
 * Registry of the file system roots available to the application.
 *
 * <p>The Vita port registers a single root, the application data
 * directory.  See <code>com.sun.midp.jsr075.FileSystemRegistryImpl</code>
 * for the mapping to the physical path.</p>
 */
public class FileSystemRegistry {

    /** This class is not instantiable. */
    private FileSystemRegistry() {
    }

    /**
     * Registers a listener for root change events.
     *
     * @param listener the listener, must not be null
     * @return false when the listener was already registered
     * @exception NullPointerException if listener is null
     */
    public static boolean addFileSystemListener(FileSystemListener listener) {
        return com.sun.midp.jsr075.FileSystemRegistryImpl.addListener(listener);
    }

    /**
     * Removes a previously registered listener.
     *
     * @param listener the listener, must not be null
     * @return true when the listener was registered
     * @exception NullPointerException if listener is null
     */
    public static boolean removeFileSystemListener(FileSystemListener listener) {
        return com.sun.midp.jsr075.FileSystemRegistryImpl.removeListener(
                listener);
    }

    /**
     * Lists the currently mounted roots.
     *
     * @return an enumeration of root URL strings, each ending with '/'
     */
    public static Enumeration listRoots() {
        return com.sun.midp.jsr075.FileSystemRegistryImpl.listRoots();
    }
}
