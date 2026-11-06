package com.sun.midp.ssl;

import java.io.ByteArrayOutputStream;
import java.io.IOException;

/**
 * VitaHttpBackend - the small native surface the HTTPS tunnel needs.
 *
 * One implementation ships (VitaHttpNative, JNI-style CLDC natives in
 * vita-port/src/vita_https.c); the interface exists so the tunnel's
 * protocol logic (request parsing, header filtering, response synthesis)
 * can be exercised on a PC against a fake backend - that is where the bugs
 * are, and it costs one extra class.
 */
interface VitaHttpBackend {

    /** 1 when SceHttp could be initialised (false on Vita3K builds that
     *  lack the HTTP/HTTPS modules). */
    int available();

    /** Open a connection to host:port. Returns true on success. */
    boolean begin(String host, int port, String userAgent);

    /**
     * Send one request.
     * @param method  "GET" / "POST" / ...
     * @param path    request target, e.g. "/api/score?id=1"
     * @param headers pre-filtered "Name: value\r\n" block (may be "")
     * @param body    request body (may be null)
     * @return HTTP status code, or a negative value when nothing was sent
     */
    int exec(String method, String path, String headers, byte[] body);

    /** Raw response header block, status line first. */
    String responseHeaders();

    /** Declared response body length, or -1 when the server did not say. */
    long responseLength();

    /** Sequential body read; 0 = end. */
    int read(byte[] buf, int off, int len);

    /** Server certificate subject DN ("" when unknown). */
    String subject();

    String protocolVersion();

    String cipherSuite();

    void end();
}
