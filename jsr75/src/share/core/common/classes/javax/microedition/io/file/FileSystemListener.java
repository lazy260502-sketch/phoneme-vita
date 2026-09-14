/*
 * JSR 75 (PDA Optional Packages) public API for the PS Vita port.
 *
 * Vita-local subsystem: the interface signatures follow the JSR 75
 * specification, the documentation and the implementation are original.
 */
package javax.microedition.io.file;

/**
 * Listener notified when a file system root is added to or removed from
 * the device.
 *
 * <p>The Vita port exposes only stable built-in storage, so a registered
 * listener never receives a callback; the registration API is provided
 * for JSR 75 compatibility.</p>
 */
public interface FileSystemListener {

    /** Constant indicating that a root has been added. */
    int ROOT_ADDED = 0;

    /** Constant indicating that a root has been removed. */
    int ROOT_REMOVED = 1;

    /**
     * Called when a file system root changes.
     *
     * @param state either {@link #ROOT_ADDED} or {@link #ROOT_REMOVED}
     * @param rootName the name of the affected root, as returned by
     *                 {@link FileSystemRegistry#listRoots()}
     */
    void rootChanged(int state, String rootName);
}
