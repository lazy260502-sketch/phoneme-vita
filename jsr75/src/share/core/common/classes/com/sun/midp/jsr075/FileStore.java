/*
 * JSR 75 native binding for the PS Vita port.
 *
 * Vita-local subsystem, written from scratch against the PCSL file layer
 * of the port (src/vita_pcsl.c) and the sceIo* directory calls PCSL does
 * not expose.  The C side lives in
 * jsr75/src/share/native/jsr75_file.c and is compiled by the vita-port
 * CMake build, exactly like vita_pcsl.c.  The names below must stay in
 * sync with the KNIDECL() names in that file: the ROM generator resolves
 * them through the standard "Java_<class>_<method>" convention, so no
 * NativesTable regeneration is involved.
 *
 * Root policy: every path handled here is absolute and starts with a Vita
 * device prefix ("ux0:/", "imc0:/", ...), which is the form PCSL and the
 * sceIo* calls want.  The Java layer above is the only producer of those
 * paths: FileSystemRegistryImpl.resolve() turns a JSR 75 URL into
 * "<device>:/<rest>".  getRootPath() is unrelated to that mapping - it
 * reports the launcher's data directory, which is where the MIDlet store
 * lives, not a JSR 75 root.
 */
package com.sun.midp.jsr075;

/**
 * Native file store used by the JSR 75 implementation.
 */
public class FileStore {

    /** Not instantiable. */
    private FileStore() {
    }

    /**
     * Absolute path of the launcher's data directory, ending with '/'.
     *
     * <p>Not a JSR 75 root: the roots are the volumes the registry lists.
     * This is where the port keeps its configuration and the MIDlet
     * store, and it is what the process chdir()ed into.</p>
     *
     * @return the data directory, for example
     *         <code>ux0:/data/J2ME00001/</code>
     */
    public static native String getRootPath();

    /**
     * Tests whether a path exists.
     *
     * @param path absolute path
     * @return true when it exists
     */
    public static native boolean exists(String path);

    /**
     * Tests whether a path is a directory.
     *
     * @param path absolute path
     * @return true when it is a directory
     */
    public static native boolean isDirectory(String path);

    /**
     * Creates an empty file; fails when it already exists.
     *
     * @param path absolute path
     * @return true on success
     */
    public static native boolean createFile(String path);

    /**
     * Creates a single directory level.
     *
     * @param path absolute path
     * @return true on success
     */
    public static native boolean mkdirFile(String path);

    /**
     * Unlinks a file.
     *
     * @param path absolute path
     * @return true on success
     */
    public static native boolean deleteFile(String path);

    /**
     * Removes an empty directory.
     *
     * @param path absolute path
     * @return true on success
     */
    public static native boolean deleteDir(String path);

    /**
     * Renames a file or directory inside the root.
     *
     * @param oldPath current absolute path
     * @param newPath new absolute path
     * @return true on success
     */
    public static native boolean renameTo(String oldPath, String newPath);

    /**
     * Size of a file.
     *
     * @param path absolute path
     * @return the size in bytes, or -1 when unknown
     */
    public static native long fileSize(String path);

    /**
     * Last modification time.
     *
     * @param path absolute path
     * @return seconds since the epoch, 0 when not available
     */
    public static native long lastModified(String path);

    /**
     * Free space of the volume a path lives on.
     *
     * @param path absolute path, used for its device prefix only
     * @return the available size in bytes, or -1 when unknown
     */
    public static native long availableSize(String path);

    /**
     * Capacity of the volume a path lives on.
     *
     * @param path absolute path, used for its device prefix only
     * @return the total size in bytes, or -1 when unknown
     */
    public static native long totalSize(String path);

    /**
     * Opens a file, creating it when mode asks for write access.
     *
     * @param path absolute path
     * @param mode 0 = read only, 1 = read/write (create if missing)
     * @return a handle greater than zero, or 0 on failure
     */
    public static native int openFile(String path, int mode);

    /**
     * Commits and closes a handle from {@link #openFile}.
     *
     * @param handle the handle
     * @return 0 on success, -1 on failure
     */
    public static native int closeFile(int handle);

    /**
     * Reads bytes at the handle's current position.
     *
     * @param handle the handle
     * @param buf destination array
     * @param off offset in buf
     * @param len maximum number of bytes
     * @return the number of bytes read (0 at end of file), or -1 on error
     */
    public static native int readFile(int handle, byte[] buf, int off,
                                      int len);

    /**
     * Writes bytes at the handle's current position.
     *
     * @param handle the handle
     * @param buf source array
     * @param off offset in buf
     * @param len number of bytes
     * @return the number of bytes written, or -1 on error
     */
    public static native int writeFile(int handle, byte[] buf, int off,
                                       int len);

    /**
     * Moves the handle's position.
     *
     * @param handle the handle
     * @param pos absolute position from the start of the file
     * @return the new position, or -1 on error
     */
    public static native long seekFile(int handle, long pos);

    /**
     * Truncates a file to the given length.
     *
     * @param path absolute path
     * @param size the new length in bytes
     * @return true on success
     */
    public static native boolean truncateFile(String path, long size);

    /**
     * Starts a directory listing.
     *
     * @param path absolute directory path
     * @return a handle greater than zero, or 0 on failure
     */
    public static native int listOpen(String path);

    /**
     * Returns the next entry of a listing.
     *
     * @param handle the handle from {@link #listOpen}
     * @return the entry name, directories with a trailing '/', or null at
     *         the end of the directory
     */
    public static native String listNext(int handle);

    /**
     * Releases a listing handle.
     *
     * @param handle the handle from {@link #listOpen}
     * @return 0 on success, -1 on failure
     */
    public static native int listClose(int handle);
}
