/*
 * VitaBtBus.java - the shared byte bus behind every btspp connection.
 *
 * A Vita has no Bluetooth radio, so connections are loopback: everything
 * written lands in this 4 KB ring, and every btspp connection on the console
 * reads from it. That makes "write then read" work, and lets two connections in
 * one MIDlet talk to each other.
 *
 * Reading an empty bus returns 0 bytes rather than -1: -1 means EOF, and a game
 * that sees EOF stops its receive loop for good.
 */
package com.sun.midp.io.j2me.btspp;

final class VitaBtBus {

    private static final int CAP = 4096;
    private static final byte[] buf = new byte[CAP];
    private static int head = 0;    /* index of the oldest byte */
    private static int len = 0;     /* bytes available */

    private VitaBtBus() {
    }

    static synchronized void write(byte[] b, int off, int n) {
        for (int i = 0; i < n; i++) {
            if (len == CAP) {           /* drop the oldest byte */
                head = (head + 1) % CAP;
                len--;
            }
            buf[(head + len) % CAP] = b[off + i];
            len++;
        }
    }

    static synchronized int read(byte[] b, int off, int n) {
        int k = (n < len) ? n : len;
        for (int i = 0; i < k; i++) {
            b[off + i] = buf[(head + i) % CAP];
        }
        head = (head + k) % CAP;
        len -= k;
        return k;
    }

    static synchronized int available() {
        return len;
    }
}

final class VitaBtInput extends java.io.InputStream {

    public int read() {
        byte[] one = new byte[1];
        int n = VitaBtBus.read(one, 0, 1);
        return (n == 1) ? (one[0] & 0xFF) : 0;
    }

    public int read(byte[] b, int off, int length) {
        if (b == null) {
            throw new NullPointerException();
        }
        if (off < 0 || length < 0 || off + length > b.length) {
            throw new IndexOutOfBoundsException();
        }
        return VitaBtBus.read(b, off, length);
    }

    public int available() {
        return VitaBtBus.available();
    }
}

final class VitaBtOutput extends java.io.OutputStream {

    public void write(int b) {
        byte[] one = new byte[] { (byte) b };
        VitaBtBus.write(one, 0, 1);
    }

    public void write(byte[] b, int off, int length) {
        if (b == null) {
            throw new NullPointerException();
        }
        if (off < 0 || length < 0 || off + length > b.length) {
            throw new IndexOutOfBoundsException();
        }
        VitaBtBus.write(b, off, length);
    }
}
