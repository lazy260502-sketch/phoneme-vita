package com.sun.midp.ssl;

/**
 * VitaHttpNative - the shipping VitaHttpBackend, backed by the native
 * SceHttp bridge in vita-port/src/vita_https.c.
 *
 * Every method is a thin wrapper; no protocol logic lives here so the
 * native side stays a byte mover (see the comment at the top of
 * vita_https.c for why).
 */
final class VitaHttpNative implements VitaHttpBackend {

    /** Header block cap passed to the native side; SceHttp is configured
     *  with the same limit. A real response head is ~1 KB. */
    private static final int HDR_CAP = 8192;

    private static native int nAvailable();
    private static native int nBegin(String host, int port, String ua);
    private static native int nExec(String method, String path,
                                    String headers, byte[] body);
    private static native int nHeaders(byte[] buf, int cap);
    private static native long nLength();
    private static native int nRead(byte[] buf, int cap);
    private static native int nSubject(byte[] buf, int cap);
    private static native String nProtoVersion();
    private static native String nCipher();
    private static native void nEnd();

    private byte[] hdrBuf;
    private byte[] subjBuf;

    public int available() {
        return nAvailable();
    }

    public boolean begin(String host, int port, String userAgent) {
        return nBegin(host, port, userAgent) > 0;
    }

    public int exec(String method, String path, String headers, byte[] body) {
        return nExec(method, path, headers, body);
    }

    public String responseHeaders() {
        int n;
        if (hdrBuf == null) {
            hdrBuf = new byte[HDR_CAP];
        }
        n = nHeaders(hdrBuf, hdrBuf.length);
        if (n <= 0) {
            return "";
        }
        if (n > hdrBuf.length) {
            n = hdrBuf.length;
        }
        return new String(hdrBuf, 0, n);
    }

    public long responseLength() {
        return nLength();
    }

    public int read(byte[] buf, int off, int len) {
        /* The native side always fills from the start of the array; the
         * tunnel never uses a non-zero offset, but honour the contract. */
        if (off == 0) {
            return nRead(buf, len);
        }
        {
            int n;
            byte[] tmp = new byte[len];
            n = nRead(tmp, len);
            if (n > 0) {
                System.arraycopy(tmp, 0, buf, off, n);
            }
            return n;
        }
    }

    public String subject() {
        int n;
        if (subjBuf == null) {
            subjBuf = new byte[256];
        }
        n = nSubject(subjBuf, subjBuf.length);
        if (n <= 0) {
            return "";
        }
        if (n > subjBuf.length) {
            n = subjBuf.length;
        }
        return new String(subjBuf, 0, n);
    }

    public String protocolVersion() {
        String s = nProtoVersion();
        return (s != null) ? s : "TLSv1.2";
    }

    public String cipherSuite() {
        String s = nCipher();
        return (s != null) ? s : "UNKNOWN";
    }

    public void end() {
        nEnd();
    }
}
