/*
 * JSR 75 (PDA Optional Packages) public API for the PS Vita port.
 *
 * Vita-local subsystem: the interface signatures follow the JSR 75
 * specification, the documentation and the implementation are original.
 */
package javax.microedition.io.file;

import java.io.DataInputStream;
import java.io.DataOutputStream;
import java.io.IOException;
import java.io.OutputStream;
import java.util.Enumeration;

import javax.microedition.io.StreamConnection;

/**
 * Connection to a file or directory.
 *
 * <p>A FileConnection is created through
 * {@link javax.microedition.io.Connector#open(String, int)} with a URL of
 * the form <code>file:///&lt;root&gt;/&lt;path&gt;</code>, where
 * <code>&lt;root&gt;</code> is one of the roots returned by
 * {@link FileSystemRegistry#listRoots()}.  A name that ends with '/'
 * always designates a directory, otherwise the name may designate either
 * a file or a directory.</p>
 *
 * <p>This port exposes exactly one root; see
 * <code>com.sun.midp.jsr075.FileSystemRegistryImpl</code>.</p>
 */
public interface FileConnection extends StreamConnection {

    /**
     * Opens an input stream on the file, positioned at the beginning.
     * Only one input stream may be open at a time.
     *
     * @return an input stream reading from this connection
     * @exception IOException if the connection is closed, the target is
     *              not a readable file, or an input stream is already open
     */
    java.io.InputStream openInputStream() throws IOException;

    /**
     * Opens a data input stream on the file, positioned at the beginning.
     *
     * @return a data input stream reading from this connection
     * @exception IOException if the stream cannot be opened
     */
    DataInputStream openDataInputStream() throws IOException;

    /**
     * Opens an output stream on the file, truncating the file to zero
     * length.  Only one output stream may be open at a time.
     *
     * @return an output stream writing to this connection
     * @exception IOException if the connection is closed, the target is
     *              not a writable file, or an output stream is already open
     */
    OutputStream openOutputStream() throws IOException;

    /**
     * Opens an output stream on the file at the given byte offset.  The
     * existing content is preserved.
     *
     * @param byteOffset position at which writing starts
     * @return an output stream writing to this connection
     * @exception IOException if the stream cannot be opened
     */
    OutputStream openOutputStream(long byteOffset) throws IOException;

    /**
     * Opens a data output stream on the file, truncating the file to
     * zero length.
     *
     * @return a data output stream writing to this connection
     * @exception IOException if the stream cannot be opened
     */
    DataOutputStream openDataOutputStream() throws IOException;

    /**
     * Size of the file system this connection lives on.
     *
     * @return the total size in bytes
     */
    long totalSize();

    /**
     * Free space in the file system this connection lives on.
     *
     * @return the available size in bytes
     */
    long availableSize();

    /**
     * Space occupied by the file or directory tree of this connection.
     *
     * @return the size in bytes
     */
    long usedSize();

    /**
     * Size of the directory tree of this connection.
     *
     * @param includeSubDirs when true sub directories are included
     * @return the size in bytes
     * @exception IOException if the target is not a directory
     */
    long directorySize(boolean includeSubDirs) throws IOException;

    /**
     * Size of the file behind this connection.
     *
     * @return the file size in bytes
     * @exception IOException if the target is not a file
     */
    long fileSize() throws IOException;

    /**
     * Whether the application may read the target.
     *
     * @return true when the target is readable
     */
    boolean canRead();

    /**
     * Whether the application may write the target.
     *
     * @return true when the target is writable
     */
    boolean canWrite();

    /**
     * Whether the target is marked as hidden.
     *
     * @return true when the target is hidden
     */
    boolean isHidden();

    /**
     * Sets the read attribute.  The Vita file systems carry no POSIX
     * permission bits, so the port accepts the call and keeps the flag
     * per-connection.
     *
     * @param readable the new value
     * @exception IOException if the connection is closed
     * @exception SecurityException if the caller has no write access
     */
    void setReadable(boolean readable) throws IOException, SecurityException;

    /**
     * Sets the write attribute.
     *
     * @param writable the new value
     * @exception IOException if the connection is closed
     * @exception SecurityException if the caller has no write access
     */
    void setWritable(boolean writable) throws IOException, SecurityException;

    /**
     * Marks the target as hidden or visible.  The port models the hidden
     * attribute with a leading '.' in the name (the convention used by
     * {@link #list(String, boolean)}), so this operation renames the
     * target.
     *
     * @param hidden the new value
     * @exception IOException if the target cannot be renamed
     * @exception SecurityException if the caller has no write access
     */
    void setHidden(boolean hidden) throws IOException, SecurityException;

    /**
     * Lists the contents of this directory, including hidden entries.
     *
     * @return an enumeration of the entry names; directories carry a
     *         trailing '/'
     * @exception IOException if the target is not a directory
     */
    Enumeration list() throws IOException;

    /**
     * Lists the contents of this directory.
     *
     * @param filter a wildcard name filter ('*' and '?'), or null
     * @param includeHidden when true entries whose name starts with '.'
     *              are included
     * @return an enumeration of the entry names; directories carry a
     *         trailing '/'
     * @exception IOException if the target is not a directory
     */
    Enumeration list(String filter, boolean includeHidden) throws IOException;

    /**
     * Creates the file, or the directory when the name ends with '/'.
     *
     * @exception IOException if the target already exists or cannot be
     *              created
     * @exception SecurityException if the caller has no write access
     */
    void create() throws IOException, SecurityException;

    /**
     * Creates the directory.
     *
     * @exception IOException if the directory already exists or cannot
     *              be created
     * @exception SecurityException if the caller has no write access
     */
    void mkdir() throws IOException, SecurityException;

    /**
     * Tests whether the target exists.
     *
     * @return true when the target exists
     */
    boolean exists();

    /**
     * Tests whether the target is a directory.
     *
     * @return true when the target is a directory
     */
    boolean isDirectory();

    /**
     * Deletes the target.  A directory must be empty.
     *
     * @exception IOException if the target cannot be deleted
     * @exception SecurityException if the caller has no write access
     */
    void delete() throws IOException, SecurityException;

    /**
     * Renames the target within its directory.
     *
     * @param newName the new name, without any path element
     * @exception IOException if the target does not exist or the rename
     *              fails
     * @exception SecurityException if the caller has no write access
     */
    void rename(String newName) throws IOException, SecurityException;

    /**
     * Truncates the file to the given length.
     *
     * @param byteOffset the new length in bytes
     * @exception IOException if the target is not a file
     */
    void truncate(long byteOffset) throws IOException;

    /**
     * Repositions this connection onto another file of the same
     * directory.
     *
     * @param fileName the name of an existing sibling entry
     * @exception IOException if this connection is not a directory or the
     *              sibling does not exist
     */
    void setFileConnection(String fileName) throws IOException;

    /**
     * The name of the target.
     *
     * @return the last path element
     */
    String getName();

    /**
     * The path of the target, without the root prefix.
     *
     * @return the path starting with '/'
     */
    String getPath();

    /**
     * The URL this connection was opened with.
     *
     * @return the URL
     */
    String getURL();

    /**
     * Last modification time of the target.
     *
     * @return the time in seconds since the epoch, or 0 when unknown
     */
    long lastModified();
}
