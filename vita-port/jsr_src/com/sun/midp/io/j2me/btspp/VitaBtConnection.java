/*
 * VitaBtConnection.java - StreamConnection / StreamConnectionNotifier for the
 * btspp: scheme (see VitaBtBus.java for the loopback model).
 */
package com.sun.midp.io.j2me.btspp;

import java.io.DataInputStream;
import java.io.DataOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;

import javax.microedition.io.StreamConnection;
import javax.microedition.io.StreamConnectionNotifier;

final class VitaBtConnection implements StreamConnection {

    private final VitaBtInput in = new VitaBtInput();
    private final VitaBtOutput out = new VitaBtOutput();
    private boolean closed;

    public InputStream openInputStream() throws IOException {
        check();
        return in;
    }

    public DataInputStream openDataInputStream() throws IOException {
        check();
        return new DataInputStream(in);
    }

    public OutputStream openOutputStream() throws IOException {
        check();
        return out;
    }

    public DataOutputStream openDataOutputStream() throws IOException {
        check();
        return new DataOutputStream(out);
    }

    public void close() {
        closed = true;
    }

    private void check() throws IOException {
        if (closed) {
            throw new IOException("connection closed");
        }
    }
}

final class VitaBtNotifier implements StreamConnectionNotifier {

    private boolean closed;

    /**
     * Loopback: a peer connection is available immediately. Blocking here (as a
     * real notifier would) would hang a single-player MIDlet forever.
     */
    public StreamConnection acceptAndOpen() throws IOException {
        if (closed) {
            throw new IOException("notifier closed");
        }
        return new VitaBtConnection();
    }

    public void close() {
        closed = true;
    }
}
