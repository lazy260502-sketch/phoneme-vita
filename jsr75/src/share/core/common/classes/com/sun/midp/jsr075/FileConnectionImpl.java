/*
 * JSR 75 FileConnection implementation for the PS Vita port.
 *
 * Vita-local subsystem, original implementation.
 *
 * Path model: the connection keeps both the URL it was opened with and
 * the absolute path produced by FileSystemRegistryImpl.resolve().  The
 * URL decides the "is a directory" intent (a trailing '/' is a
 * directory, per JSR 75), the absolute path is what the native layer
 * sees.  Nothing outside the root can be reached: resolve() rejects
 * ".." elements and only the roots listed by the registry are accepted,
 * so the root name is fixed for the lifetime of the connection and only
 * setFileConnection() moves the target downwards.
 *
 * Stream model: one native file handle per connection, reopened when the
 * open mode changes.  Each stream keeps its own position, so every
 * read/write pair is preceded by an explicit seek; position tracking in
 * Java is exact even though two streams may share one handle.
 *
 * Hidden-file model: the port has no hidden attribute in the Vita file
 * API, so "hidden" is defined as a leading '.' in the entry name.  This
 * makes isHidden()/setHidden() and list(filter, includeHidden)
 * consistent with each other instead of inventing a second mechanism.
 */
package com.sun.midp.jsr075;

import java.io.DataInputStream;
import java.io.DataOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.Enumeration;
import java.util.Vector;

import javax.microedition.io.Connector;
import javax.microedition.io.file.ConnectionClosedException;
import javax.microedition.io.file.FileConnection;
import javax.microedition.io.file.IllegalModeException;

/**
 * FileConnection backed by {@link FileStore}.
 */
public class FileConnectionImpl implements FileConnection {

    /** Mode passed to {@link FileStore#openFile} for reading. */
    private static final int NATIVE_READ = 0;

    /** Mode passed to {@link FileStore#openFile} for read/write. */
    private static final int NATIVE_WRITE = 1;

    /** Upper bound on the recursion performed by directorySize(). */
    private static final int MAX_DEPTH = 12;

    /** The URL this connection was opened with. */
    private String url;

    /** True when the URL asked for a directory by ending with '/'. */
    private boolean dirUrl;

    /** The absolute path handed to the native layer. */
    private String absPath;

    /** Name of the root the connection lives on, without a trailing '/'. */
    private final String rootName;

    /** The root-relative path; empty for the root itself. */
    private String relPath;

    /** Access mode, either Connector.READ or Connector.READ_WRITE. */
    private final int accessMode;

    /** True while the connection is usable. */
    private boolean open = true;

    /** Native handle, 0 when no handle is held. */
    private int handle;

    /** Mode the current handle was opened with. */
    private int handleMode = -1;

    /** True while an input stream is open. */
    private boolean inputOpen;

    /** True while an output stream is open. */
    private boolean outputOpen;

    /** Per-connection read attribute (see setReadable). */
    private boolean readable = true;

    /** Per-connection write attribute (see setWritable). */
    private boolean writable = true;

    /**
     * Creates a connection for a URL on one of the exposed roots.
     *
     * @param url the connection URL
     * @param accessMode Connector.READ or Connector.READ_WRITE
     * @exception IllegalArgumentException if the URL is not handled or
     *              the mode asks for write-only access
     */
    public FileConnectionImpl(String url, int accessMode) {
        if (accessMode == Connector.WRITE) {
            throw new IllegalArgumentException(
                    "FileConnection requires READ or READ_WRITE");
        }

        this.url = url;
        this.accessMode = accessMode;
        this.dirUrl = url.length() > 0
                && url.charAt(url.length() - 1) == '/';

        /* resolve() does the whole split: absolute device path, name of
         * the root it belongs to and the remainder below that root.  The
         * registry is the only place that knows how a root maps onto a
         * device, so no prefix has to be recovered here. */
        String[] parts = FileSystemRegistryImpl.resolve(url);

        this.absPath = parts[0];
        this.rootName = parts[1];
        this.relPath = parts[2];
    }

    /* ------------------------------------------------------------------ */
    /* Helpers                                                             */
    /* ------------------------------------------------------------------ */

    /**
     * Removes a single trailing '/' (the root keeps its form when it is
     * the whole path).
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
     * Appends a name to a path, inserting a '/' only when needed.
     *
     * @param base base path
     * @param name element to append
     * @return the joined path
     */
    private static String join(String base, String name) {
        if (base.length() > 0 && base.charAt(base.length() - 1) == '/') {
            return base + name;
        }
        return base + "/" + name;
    }

    /**
     * Path of the containing directory.
     *
     * @return the parent path, or null for the root
     */
    private String parentPath() {
        int slash = absPath.lastIndexOf('/');
        if (slash <= 0) {
            return null;
        }
        return absPath.substring(0, slash);
    }

    /**
     * Throws when the connection has been closed.
     *
     * @exception ConnectionClosedException always when closed
     */
    private void checkOpen() throws ConnectionClosedException {
        if (!open) {
            throw new ConnectionClosedException();
        }
    }

    /**
     * Throws when this connection has no write access.
     *
     * @exception IllegalModeException when opened for reading only
     */
    private void checkWrite() {
        if (accessMode == Connector.READ) {
            throw new IllegalModeException("Connection is read only");
        }
    }

    /**
     * (Re)opens the native handle in the requested mode.
     *
     * @param mode NATIVE_READ or NATIVE_WRITE
     * @exception IOException when the file cannot be opened
     */
    private void ensureHandle(int mode) throws IOException {
        if (handle != 0 && handleMode != mode) {
            FileStore.closeFile(handle);
            handle = 0;
        }
        if (handle == 0) {
            int h = FileStore.openFile(absPath, mode);
            if (h == 0) {
                throw new IOException("Cannot open " + url);
            }
            handle = h;
            handleMode = mode;
        }
    }

    /**
     * Releases the native handle if one is held.
     */
    private void releaseHandle() {
        if (handle != 0) {
            FileStore.closeFile(handle);
            handle = 0;
            handleMode = -1;
        }
    }

    /**
     * Name of the last element of a because path.
     *
     * @param p a path
     * @return the last element
     */
    private static String lastElement(String p) {
        String s = trimSlash(p);
        int slash = s.lastIndexOf('/');
        return (slash == -1) ? s : s.substring(slash + 1);
    }

    /**
     * Wildcard matcher for list(filter, includeHidden): '*' matches any
     * run of characters, '?' exactly one.  Implemented here because CLDC
     * has no regular expressions.
     *
     * @param pattern the filter
     * @param name the entry name
     * @return true when the name matches
     */
    private static boolean matchFilter(String pattern, String name) {
        return matchAt(pattern, 0, name, 0);
    }

    /**
     * Recursive step of {@link #matchFilter}.
     *
     * @param p pattern
     * @param pi position in the pattern
     * @param n name
     * @param ni position in the name
     * @return true when the remainder matches
     */
    private static boolean matchAt(String p, int pi, String n, int ni) {
        while (pi < p.length()) {
            char c = p.charAt(pi);
            if (c == '*') {
                pi++;
                if (pi == p.length()) {
                    return true;
                }
                for (int i = ni; i <= n.length(); i++) {
                    if (matchAt(p, pi, n, i)) {
                        return true;
                    }
                }
                return false;
            }
            if (c == '?') {
                if (ni >= n.length()) {
                    return false;
                }
                pi++;
                ni++;
                continue;
            }
            if (ni >= n.length() || n.charAt(ni) != c) {
                return false;
            }
            pi++;
            ni++;
        }
        return ni == n.length();
    }

    /**
     * Collects the entries of this directory.
     *
     * @param filter wildcard name filter, or null
     * @param includeHidden when true leading-dot entries are kept
     * @return the matching entries, directories with a trailing '/'
     * @exception IOException when the target is not a readable directory
     */
    private Vector listEntries(String filter, boolean includeHidden)
            throws IOException {
        checkOpen();
        if (!isDirectory()) {
            throw new IOException("Not a directory: " + url);
        }

        Vector result = new Vector();
        int listing = FileStore.listOpen(absPath);
        if (listing == 0) {
            throw new IOException("Cannot list " + url);
        }
        try {
            String name;
            while ((name = FileStore.listNext(listing)) != null) {
                boolean directory =
                        name.length() > 1 && name.endsWith("/");
                String plain = directory
                        ? name.substring(0, name.length() - 1) : name;

                if (!includeHidden && plain.startsWith(".")) {
                    continue;
                }
                if (filter != null && !matchFilter(filter, plain)) {
                    continue;
                }
                result.addElement(name);
            }
        } finally {
            FileStore.listClose(listing);
        }
        return result;
    }

    /**
     * Recursively adds up the size of a directory tree.
     *
     * @param path absolute directory path
     * @param depth remaining recursion budget
     * @return the total size in bytes
     * @exception IOException on a listing failure
     */
    private long treeSize(String path, int depth) throws IOException {
        if (depth <= 0) {
            return 0;
        }

        long total = 0;
        int listing = FileStore.listOpen(path);
        if (listing == 0) {
            return 0;
        }
        try {
            String name;
            while ((name = FileStore.listNext(listing)) != null) {
                if (name.length() > 1 && name.endsWith("/")) {
                    total += treeSize(join(path, name.substring(0,
                            name.length() - 1)), depth - 1);
                } else {
                    long size = FileStore.fileSize(join(path, name));
                    if (size > 0) {
                        total += size;
                    }
                }
            }
        } finally {
            FileStore.listClose(listing);
        }
        return total;
    }

    /* ------------------------------------------------------------------ */
    /* FileConnection: streams                                             */
    /* ------------------------------------------------------------------ */

    /**
     * Opens an input stream at position 0.
     *
     * @return the stream
     * @exception IOException when the target is not a readable file
     */
    public synchronized InputStream openInputStream() throws IOException {
        checkOpen();
        if (inputOpen) {
            throw new IOException("An input stream is already open");
        }
        if (isDirectory() || !exists()) {
            throw new IOException("Not a file: " + url);
        }
        ensureHandle(NATIVE_READ);
        inputOpen = true;
        return new InnerInputStream();
    }

    /**
     * Opens a data input stream at position 0.
     *
     * @return the stream
     * @exception IOException when the target is not a readable file
     */
    public DataInputStream openDataInputStream() throws IOException {
        return new DataInputStream(openInputStream());
    }

    /**
     * Opens an output stream, truncating the file to zero length.
     *
     * @return the stream
     * @exception IOException when the target cannot be written
     */
    public synchronized OutputStream openOutputStream() throws IOException {
        checkWrite();
        if (isDirectory()) {
            throw new IOException("Not a file: " + url);
        }
        if (!FileStore.truncateFile(absPath, 0)) {
            throw new IOException("Cannot truncate " + url);
        }
        return openOutputStream(0);
    }

    /**
     * Opens an output stream at the given offset, preserving content.
     *
     * @param byteOffset position of the first written byte
     * @return the stream
     * @exception IOException when the target cannot be written
     */
    public synchronized OutputStream openOutputStream(long byteOffset)
            throws IOException {
        checkOpen();
        checkWrite();
        if (outputOpen) {
            throw new IOException("An output stream is already open");
        }
        if (isDirectory()) {
            throw new IOException("Not a file: " + url);
        }
        if (byteOffset < 0) {
            throw new IOException("Negative offset");
        }
        ensureHandle(NATIVE_WRITE);
        outputOpen = true;

        InnerOutputStream out = new InnerOutputStream();
        out.position = byteOffset;
        return out;
    }

    /**
     * Opens a data output stream, truncating the file to zero length.
     *
     * @return the stream
     * @exception IOException when the target cannot be written
     */
    public DataOutputStream openDataOutputStream() throws IOException {
        return new DataOutputStream(openOutputStream());
    }

    /**
     * Closes the connection and any stream and handle it owns.
     *
     * @exception IOException never
     */
    public synchronized void close() throws IOException {
        releaseHandle();
        inputOpen = false;
        outputOpen = false;
        open = false;
    }

    /* ------------------------------------------------------------------ */
    /* FileConnection: sizes                                               */
    /* ------------------------------------------------------------------ */

    /**
     * Capacity of the volume behind the root.
     *
     * @return the total size in bytes
     */
    public long totalSize() {
        return FileStore.totalSize(absPath);
    }

    /**
     * Free space of the volume behind the root.
     *
     * @return the available size in bytes
     */
    public long availableSize() {
        return FileStore.availableSize(absPath);
    }

    /**
     * Space occupied by this target.
     *
     * @return the size in bytes
     */
    public long usedSize() {
        try {
            if (isDirectory()) {
                return treeSize(absPath, MAX_DEPTH);
            }
            long size = FileStore.fileSize(absPath);
            return (size > 0) ? size : 0;
        } catch (IOException x) {
            return 0;
        }
    }

    /**
     * Size of this directory tree.
     *
     * @param includeSubDirs when true sub directories are walked
     * @return the size in bytes
     * @exception IOException when the target is not a directory
     */
    public long directorySize(boolean includeSubDirs) throws IOException {
        checkOpen();
        if (!isDirectory()) {
            throw new IOException("Not a directory: " + url);
        }
        return treeSize(absPath, includeSubDirs ? MAX_DEPTH : 1);
    }

    /**
     * Size of this file.
     *
     * @return the size in bytes
     * @exception IOException when the target is not an existing file
     */
    public long fileSize() throws IOException {
        checkOpen();
        if (isDirectory() || !exists()) {
            throw new IOException("Not a file: " + url);
        }
        long size = FileStore.fileSize(absPath);
        if (size < 0) {
            throw new IOException("Cannot stat " + url);
        }
        return size;
    }

    /**
     * Last modification time.
     *
     * @return seconds since the epoch, 0 when unknown
     */
    public long lastModified() {
        return FileStore.lastModified(absPath);
    }

    /* ------------------------------------------------------------------ */
    /* FileConnection: attributes                                          */
    /* ------------------------------------------------------------------ */

    /**
     * Read access of the connection.
     *
     * @return true when the connection may read
     */
    public boolean canRead() {
        return open && readable;
    }

    /**
     * Write access of the connection.
     *
     * @return true when the connection may write
     */
    public boolean canWrite() {
        return open && writable && accessMode != Connector.READ;
    }

    /**
     * Hidden state, modelled as a leading '.' in the name.
     *
     * @return true when the name starts with '.'
     */
    public boolean isHidden() {
        return relPath.length() > 0 && lastElement(absPath).startsWith(".");
    }

    /**
     * Sets the read attribute of this connection.
     *
     * @param value the new value
     * @exception IOException never
     */
    public void setReadable(boolean value) throws IOException {
        checkOpen();
        if (!writable || accessMode == Connector.READ) {
            throw new SecurityException("No write access");
        }
        readable = value;
    }

    /**
     * Sets the write attribute of this connection.
     *
     * @param value the new value
     * @exception IOException never
     */
    public void setWritable(boolean value) throws IOException {
        checkOpen();
        if (!writable || accessMode == Connector.READ) {
            throw new SecurityException("No write access");
        }
        writable = value;
    }

    /**
     * Marks the target hidden or visible by renaming it.
     *
     * @param hidden the new value
     * @exception IOException when the rename fails
     */
    public void setHidden(boolean hidden) throws IOException {
        checkOpen();
        checkWrite();
        if (relPath.length() == 0) {
            throw new IOException("Cannot rename the root");
        }

        String name = lastElement(absPath);
        if (hidden == name.startsWith(".")) {
            return;
        }

        String target = hidden ? ("." + name) : name.substring(1);
        if (target.length() == 0 || target.startsWith("..")) {
            throw new IOException("Cannot derive a hidden name");
        }
        String parent = parentPath();
        if (parent == null) {
            throw new IOException("Cannot rename the root");
        }
        if (!FileStore.renameTo(absPath, join(parent, target))) {
            throw new IOException("Cannot rename " + url);
        }
        absPath = join(parent, target);
    }

    /* ------------------------------------------------------------------ */
    /* FileConnection: listing                                             */
    /* ------------------------------------------------------------------ */

    /**
     * Lists the contents, hidden entries included.
     *
     * @return the entry names
     * @exception IOException when the target is not a directory
     */
    public Enumeration list() throws IOException {
        return listEntries(null, true).elements();
    }

    /**
     * Lists the contents.
     *
     * @param filter wildcard name filter, or null
     * @param includeHidden when true leading-dot entries are kept
     * @return the entry names, directories with a trailing '/'
     * @exception IOException when the target is not a directory
     */
    public Enumeration list(String filter, boolean includeHidden)
            throws IOException {
        return listEntries(filter, includeHidden).elements();
    }

    /* ------------------------------------------------------------------ */
    /* FileConnection: operations                                          */
    /* ------------------------------------------------------------------ */

    /**
     * Creates the file, or the directory when the URL ends with '/'.
     *
     * @exception IOException when the target already exists or cannot be
     *              created
     */
    public void create() throws IOException {
        checkOpen();
        checkWrite();

        if (dirUrl) {
            makeDirectory();
            return;
        }

        if (relPath.length() == 0) {
            throw new IOException("Cannot create the root");
        }
        if (FileStore.exists(absPath)) {
            throw new IOException("Already exists: " + url);
        }
        if (!FileStore.createFile(absPath)) {
            throw new IOException("Cannot create " + url);
        }
    }

    /**
     * Creates the directory.
     *
     * @exception IOException when the directory already exists or cannot
     *              be created
     */
    public void mkdir() throws IOException {
        checkOpen();
        checkWrite();
        makeDirectory();
    }

    /**
     * Shared body of create() and mkdir() for the directory case.
     *
     * @exception IOException on failure
     */
    private void makeDirectory() throws IOException {
        if (relPath.length() == 0) {
            throw new IOException("Cannot create the root");
        }
        if (FileStore.exists(absPath)) {
            throw new IOException("Already exists: " + url);
        }
        if (!FileStore.mkdirFile(absPath)) {
            throw new IOException("Cannot create directory " + url);
        }
    }

    /**
     * Tests whether the target exists.
     *
     * @return true when it exists
     */
    public boolean exists() {
        if (relPath.length() == 0) {
            return open;
        }
        return FileStore.exists(absPath);
    }

    /**
     * Tests whether the target is a directory.
     *
     * @return true when it is a directory
     */
    public boolean isDirectory() {
        if (relPath.length() == 0) {
            return open;
        }
        return FileStore.isDirectory(absPath);
    }

    /**
     * Deletes the target.  A directory has to be empty.
     *
     * @exception IOException when the target is missing, is a non-empty
     *              directory, or cannot be removed
     */
    public void delete() throws IOException {
        checkOpen();
        checkWrite();
        if (relPath.length() == 0) {
            throw new IOException("Cannot delete the root");
        }
        if (!FileStore.exists(absPath)) {
            throw new IOException("Does not exist: " + url);
        }

        if (FileStore.isDirectory(absPath)) {
            releaseHandle();
            if (!listEntries(null, true).isEmpty()) {
                throw new IOException("Directory is not empty: " + url);
            }
            if (!FileStore.deleteDir(absPath)) {
                throw new IOException("Cannot delete " + url);
            }
        } else {
            releaseHandle();
            if (!FileStore.deleteFile(absPath)) {
                throw new IOException("Cannot delete " + url);
            }
        }
    }

    /**
     * Renames the target inside its directory.
     *
     * @param newName the new name, without any '/' element
     * @exception IOException when the rename fails
     */
    public void rename(String newName) throws IOException {
        checkOpen();
        checkWrite();
        if (relPath.length() == 0) {
            throw new IOException("Cannot rename the root");
        }
        if (newName == null || newName.length() == 0
                || newName.indexOf('/') != -1) {
            throw new IllegalArgumentException("Invalid name: " + newName);
        }
        if (!FileStore.exists(absPath)) {
            throw new IOException("Does not exist: " + url);
        }
        releaseHandle();

        String parent = parentPath();
        if (parent == null) {
            throw new IOException("Cannot rename the root");
        }
        String target = join(parent, newName);
        if (!FileStore.renameTo(absPath, target)) {
            throw new IOException("Cannot rename " + url + " to " + newName);
        }

        absPath = target;
        relPath = lastElement(target);
        int lastSlash = url.lastIndexOf('/');
        url = (lastSlash == -1 ? "file:///" : url.substring(0, lastSlash + 1))
                + newName;
        dirUrl = false;
    }

    /**
     * Truncates the file.
     *
     * @param byteOffset the new length in bytes
     * @exception IOException when the target is not an existing file
     */
    public void truncate(long byteOffset) throws IOException {
        checkOpen();
        checkWrite();
        if (byteOffset < 0) {
            throw new IOException("Negative length");
        }
        if (isDirectory() || !exists()) {
            throw new IOException("Not a file: " + url);
        }
        releaseHandle();
        if (!FileStore.truncateFile(absPath, byteOffset)) {
            throw new IOException("Cannot truncate " + url);
        }
    }

    /**
     * Moves this connection onto a sibling entry.
     *
     * @param fileName name of an existing entry of this directory
     * @exception IOException when this connection is not a directory or
     *              the entry does not exist
     */
    public void setFileConnection(String fileName) throws IOException {
        checkOpen();
        if (fileName == null || fileName.indexOf('/') != -1) {
            throw new IllegalArgumentException("Invalid name: " + fileName);
        }
        if (!isDirectory()) {
            throw new IOException("Not a directory: " + url);
        }

        String target = join(absPath, fileName);
        if (!FileStore.exists(target)) {
            throw new IOException("Does not exist: " + fileName);
        }
        releaseHandle();
        inputOpen = false;
        outputOpen = false;

        absPath = target;
        relPath = (relPath.length() == 0)
                ? fileName : (relPath + "/" + fileName);
        url = (dirUrl ? url : (url + "/")) + fileName;
        dirUrl = false;
    }

    /* ------------------------------------------------------------------ */
    /* FileConnection: naming                                              */
    /* ------------------------------------------------------------------ */

    /**
     * Name of the target.
     *
     * @return the last path element, or "" for the root
     */
    public String getName() {
        if (relPath.length() == 0) {
            return "";
        }
        return lastElement(absPath);
    }

    /**
     * Path of the target relative to the root name.
     *
     * @return the path, starting with '/'
     */
    public String getPath() {
        return "/" + rootName
                + (relPath.length() == 0 ? "/" : ("/" + relPath));
    }

    /**
     * The URL this connection was opened with; kept in sync by
     * rename() and setFileConnection().
     *
     * @return the URL
     */
    public String getURL() {
        return url;
    }

    /* ------------------------------------------------------------------ */
    /* Streams                                                             */
    /* ------------------------------------------------------------------ */

    /**
     * Input stream over the shared handle.
     */
    private class InnerInputStream extends InputStream {

        /** Absolute read position. */
        private long position;

        /** True once the stream has been closed. */
        private boolean closed;

        /**
         * Reads one byte.
         *
         * @return the byte, or -1 at end of file
         * @exception IOException when the handle is gone
         */
        public int read() throws IOException {
            byte[] one = new byte[1];
            int n = read(one, 0, 1);
            return (n <= 0) ? -1 : (one[0] & 0xff);
        }

        /**
         * Reads a block.
         *
         * @param buf destination array
         * @param off offset in buf
         * @param len maximum number of bytes
         * @return the number of bytes read, or -1 at end of file
         * @exception IOException when the handle is gone
         */
        public int read(byte[] buf, int off, int len) throws IOException {
            if (closed) {
                throw new IOException("Stream is closed");
            }
            if (buf == null) {
                throw new NullPointerException();
            }
            if (off < 0 || len < 0 || off + len > buf.length) {
                throw new IndexOutOfBoundsException();
            }
            if (len == 0) {
                return 0;
            }
            synchronized (FileConnectionImpl.this) {
                checkOpen();
                ensureHandle(NATIVE_READ);
                if (FileStore.seekFile(handle, position) < 0) {
                    throw new IOException("Seek failed");
                }
                int n = FileStore.readFile(handle, buf, off, len);
                if (n < 0) {
                    throw new IOException("Read failed");
                }
                position += n;
                return (n == 0) ? -1 : n;
            }
        }

        /**
         * Number of bytes that can be read without blocking.
         *
         * @return 0; the native layer cannot report a pending count
         */
        public int available() {
            return 0;
        }

        /**
         * Closing the stream releases the shared handle.
         *
         * @exception IOException never
         */
        public void close() throws IOException {
            if (closed) {
                return;
            }
            closed = true;
            synchronized (FileConnectionImpl.this) {
                inputOpen = false;
                if (!outputOpen) {
                    releaseHandle();
                }
            }
        }

        /**
         * Always false.
         *
         * @return false
         */
        public boolean markSupported() {
            return false;
        }
    }

    /**
     * Output stream over the shared handle.
     */
    private class InnerOutputStream extends OutputStream {

        /** Absolute write position. */
        private long position;

        /** True once the stream has been closed. */
        private boolean closed;

        /**
         * Writes one byte.
         *
         * @param b the byte
         * @exception IOException when the write fails
         */
        public void write(int b) throws IOException {
            byte[] one = new byte[1];
            one[0] = (byte) b;
            write(one, 0, 1);
        }

        /**
         * Writes a block.
         *
         * @param buf source array
         * @param off offset in buf
         * @param len number of bytes
         * @exception IOException when the write fails
         */
        public void write(byte[] buf, int off, int len) throws IOException {
            if (closed) {
                throw new IOException("Stream is closed");
            }
            if (buf == null) {
                throw new NullPointerException();
            }
            if (off < 0 || len < 0 || off + len > buf.length) {
                throw new IndexOutOfBoundsException();
            }
            if (len == 0) {
                return;
            }
            synchronized (FileConnectionImpl.this) {
                checkOpen();
                ensureHandle(NATIVE_WRITE);
                if (FileStore.seekFile(handle, position) < 0) {
                    throw new IOException("Seek failed");
                }
                int n = FileStore.writeFile(handle, buf, off, len);
                if (n < 0) {
                    throw new IOException("Write failed");
                }
                position += n;
                if (n != len) {
                    throw new IOException("Short write");
                }
            }
        }

        /**
         * Flushes by design: writes go through pcsl_file_write() straight
         * to the file, the commit happens when the stream is closed.
         *
         * @exception IOException never
         */
        public void flush() throws IOException {
        }

        /**
         * Closing the stream commits and releases the shared handle.
         *
         * @exception IOException never
         */
        public void close() throws IOException {
            if (closed) {
                return;
            }
            closed = true;
            synchronized (FileConnectionImpl.this) {
                outputOpen = false;
                if (!inputOpen) {
                    releaseHandle();
                }
            }
        }
    }
}
