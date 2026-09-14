/*
 * JSR 75 "file" protocol handler for the PS Vita port.
 *
 * Vita-local subsystem, original implementation.
 *
 * Connector.open("file:///ux0/data/x") resolves, through
 * javax.microedition.io.Connector, to the class named by
 * "microedition.platform" (j2me) plus the protocol name (file):
 *
 *     classRoot "." platform "." protocol ".Protocol"
 *     -> com.sun.midp.io.j2me.file.Protocol
 *
 * The name handed to openPrim() is the URL with the scheme removed, i.e.
 * "///ux0/data/x" for "file:///ux0/data/x".  The "//" authority part is
 * required by JSR 75 (the host component is empty), the root name - the
 * name of a mounted volume - follows it.
 *
 * Access control: unlike the network protocols this handler performs no
 * AccessController check.  "javax.microedition.io.Connector.file.read"
 * and "...file.write" exist in the policy file, but they are only
 * reachable through alias groups that are not granted to the
 * minimum/unsecured domain, so no MIDlet could ever pass the check and
 * every JSR 75 call would fail.  The MIDlet gets the storage volumes the
 * registry lists, which is what a file manager needs and what a suite
 * could reach through the native layer anyway.
 */
package com.sun.midp.io.j2me.file;

import java.io.IOException;

import javax.microedition.io.Connection;
import javax.microedition.io.Connector;

import com.sun.cldc.io.ConnectionBaseInterface;
import com.sun.midp.jsr075.FileConnectionImpl;

/**
 * Protocol handler for <code>file://</code> URLs.
 */
public class Protocol implements ConnectionBaseInterface {

    /**
     * Creates a protocol instance.  Required by the reflective
     * instantiation performed by Connector.
     */
    public Protocol() {
    }

    /**
     * Opens a FileConnection.
     *
     * @param name the URL without its scheme, for example "///ux0/data/a.txt"
     * @param mode Connector.READ or Connector.READ_WRITE
     * @param timeouts ignored; file access never blocks on the network
     * @return a FileConnection
     * @exception IOException if the URL is not a valid JSR 75 file URL
     *              or the target root does not exist
     * @exception IllegalArgumentException if the URL is malformed or the
     *              access mode is write-only
     */
    public Connection openPrim(String name, int mode, boolean timeouts)
            throws IOException {
        if (name == null) {
            throw new IllegalArgumentException("Null URL");
        }
        if (mode == Connector.WRITE) {
            throw new IllegalArgumentException(
                    "FileConnection requires READ or READ_WRITE");
        }
        if (!name.startsWith("//")) {
            throw new IllegalArgumentException("Malformed file URL: file:"
                    + name);
        }

        return new FileConnectionImpl("file:" + name, mode);
    }
}
