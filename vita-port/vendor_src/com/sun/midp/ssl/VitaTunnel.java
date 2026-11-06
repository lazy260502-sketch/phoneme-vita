package com.sun.midp.ssl;

import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;

/**
 * VitaTunnel - turns MIDP's "write a request, read a response" byte stream
 * into whole HTTP transactions on the console's own TLS stack.
 *
 * WHY
 * ---
 * phoneME's HTTPS client (com.sun.midp.io.j2me.https.Protocol, 2500 lines,
 * unchunked/redirect/keep-alive logic all tested by upstream) writes an
 * HTTP/1.1 request into a StreamConnection and reads the response back.
 * Upstream supplies that stream from com.sun.midp.ssl.SSLStreamConnection,
 * which is a TLS record layer built on restricted_crypto - SSLv3/TLS 1.0
 * only, useless against today's servers.
 *
 * The console has no usable TLS socket either (SceSsl exports no
 * record-layer functions), but it DOES have a complete HTTPS client
 * (SceHttp). So this class plays the role of the TLS stream: it buffers
 * what the HTTP layer writes, hands the request to SceHttp, and serves the
 * response back as an ordinary HTTP/1.1 byte stream. Everything above it
 * stays upstream code.
 *
 * WHAT IT DOES WITH THE REQUEST
 * -----------------------------
 * - Parses the head (request line + headers) and the body.
 * - Drops hop-by-hop headers SceHttp owns: Host, Connection, Keep-Alive,
 *   Proxy-Connection, Transfer-Encoding, Content-Length, TE, Upgrade,
 *   Accept-Encoding (the tunnel asks for identity), Proxy-Authorization.
 * - De-chunks a chunked request body so SceHttp sees plain bytes.
 *
 * WHAT IT DOES WITH THE RESPONSE
 * ------------------------------
 * - Replays the status line verbatim.
 * - Replays the headers minus Transfer-Encoding / Content-Encoding (no
 *   decoder here) / Connection / Keep-Alive.
 * - Adds Content-Length when SceHttp reports one, otherwise leaves the body
 *   framed by EOF - which phoneME's HTTP layer explicitly supports
 *   ("Non-chunked unknown length" branch in Protocol.read).
 * - Adds Connection: close: the tunnel is one transaction.
 *
 * LIMITS
 * ------
 * - Upload and download are buffered, not streamed. Request bodies are
 *   capped at 4 MB and the response is read incrementally, so the only
 *   real cost is that MIDP's "write body while reading" mode is refused.
 *   No MIDP game in the test set uses it, and SceHttp cannot express it.
 */
final class VitaTunnel {

    /** Request cap: a J2ME game's POST is a few KB. */
    static final int MAX_REQUEST = 4 * 1024 * 1024;
    /** Read buffer for the body pump. */
    private static final int PUMP = 2048;

    private final String host;
    private final int port;
    private final VitaHttpBackend backend;

    private final ByteArrayOutputStream request = new ByteArrayOutputStream();
    private boolean executed;
    private boolean execFailed;
    private int status;

    /** Synthesized response already produced by the backend. */
    private byte[] responseHeader;
    private int responseHeaderPos;
    private byte[] bodyBuf;
    private int bodyPos;
    private int bodyLen;
    private boolean eof;
    private long declaredLength = -1;

    private boolean closed;

    VitaTunnel(String host, int port, VitaHttpBackend backend) {
        this.host = host;
        this.port = port;
        this.backend = backend;
    }

    /* ------------------------------------------------------------------ */
    /* Byte-stream facade                                                 */
    /* ------------------------------------------------------------------ */

    OutputStream openOutputStream() {
        return new OutputStream() {
            public void write(int b) throws IOException {
                checkCap(1);
                request.write(b);
            }

            public void write(byte[] b, int off, int len)
                    throws IOException {
                checkCap(len);
                request.write(b, off, len);
            }

            public void close() throws IOException {
                /* Nothing to do: the transaction starts on the first read,
                 * which is where MIDP blocks anyway. */
            }
        };
    }

    private void checkCap(int n) throws IOException {
        if (request.size() + n > MAX_REQUEST) {
            throw new IOException("HTTPS request too large");
        }
        if (executed) {
            throw new IOException("HTTPS request already sent");
        }
    }

    InputStream openInputStream() {
        return new InputStream() {
            public int read() throws IOException {
                byte[] one = new byte[1];
                int n = read(one, 0, 1);
                return (n <= 0) ? -1 : (one[0] & 0xff);
            }

            public int read(byte[] b, int off, int len) throws IOException {
                return tunnelRead(b, off, len);
            }

            public int available() throws IOException {
                if (!executed) {
                    return 0;
                }
                return (bodyLen - bodyPos) + (responseHeader.length
                        - responseHeaderPos);
            }

            public void close() throws IOException {
                /* Closing the response does not abort the transaction; the
                 * connection is torn down by SSLStreamConnection.close(). */
            }
        };
    }

    /* ------------------------------------------------------------------ */
    /* The mechanism                                                      */
    /* ------------------------------------------------------------------ */

    private int tunnelRead(byte[] b, int off, int len) throws IOException {
        if (len == 0) {
            return 0;
        }
        if (!executed) {
            execute();
        }
        if (execFailed) {
            throw new IOException("HTTPS request failed");
        }

        /* Header bytes first, straight from the synthesized block. */
        if (responseHeaderPos < responseHeader.length) {
            int n = responseHeader.length - responseHeaderPos;
            if (n > len) {
                n = len;
            }
            System.arraycopy(responseHeader, responseHeaderPos, b, off, n);
            responseHeaderPos += n;
            return n;
        }

        /* Then the body, pumped from the backend. */
        if (bodyPos >= bodyLen) {
            if (eof) {
                return -1;
            }
            pump();
            if (bodyPos >= bodyLen) {
                return -1;
            }
        }
        {
            int n = bodyLen - bodyPos;
            if (n > len) {
                n = len;
            }
            System.arraycopy(bodyBuf, bodyPos, b, off, n);
            bodyPos += n;
            return n;
        }
    }

    private void pump() throws IOException {
        if (bodyBuf == null) {
            bodyBuf = new byte[PUMP];
        }
        {
            int n = backend.read(bodyBuf, 0, bodyBuf.length);
            if (n <= 0) {
                eof = true;
                bodyPos = bodyLen = 0;
                return;
            }
            bodyPos = 0;
            bodyLen = n;
        }
    }

    /**
     * Run the buffered request through the backend and build the response
     * stream. Visible for the host unit test.
     */
    void execute() throws IOException {
        byte[] raw = request.toByteArray();
        int headEnd = findHeadEnd(raw);
        String head;
        String body = "";
        String method;
        String path;
        String filtered;

        if (headEnd < 0) {
            executed = true;
            execFailed = true;
            throw new IOException("HTTPS: malformed request (no header end)");
        }
        head = latin1(raw, 0, headEnd);
        if (headEnd + 4 < raw.length) {
            body = latin1(raw, headEnd + 4, raw.length - (headEnd + 4));
        }

        {
            int sp1 = head.indexOf(' ');
            int sp2 = head.indexOf(' ', sp1 + 1);
            if (sp1 <= 0 || sp2 <= sp1) {
                executed = true;
                execFailed = true;
                throw new IOException("HTTPS: malformed request line");
            }
            method = head.substring(0, sp1);
            path = head.substring(sp1 + 1, sp2);
        }

        filtered = filterRequestHeaders(head);

        /* A chunked upload has to be de-chunked before SceHttp sees it. */
        if (isChunked(head)) {
            body = decodeChunked(body);
        }

        executed = true;
        status = backend.exec(method, path, filtered,
                              body.length() > 0
                                      ? toLatin1(body)
                                      : null);
        if (status < 0) {
            execFailed = true;
            return;
        }

        declaredLength = backend.responseLength();
        responseHeader = buildResponseHeader(backend.responseHeaders(),
                                             declaredLength,
                                             body.length());
        if (responseHeader == null) {
            execFailed = true;
            return;
        }
        responseHeaderPos = 0;
        bodyPos = bodyLen = 0;
        eof = (declaredLength == 0);
    }

    /* ------------------------------------------------------------------ */
    /* Request side                                                       */
    /* ------------------------------------------------------------------ */

    /** Index of the "\r\n\r\n" that ends the head, or -1. */
    static int findHeadEnd(byte[] b) {
        for (int i = 0; i + 3 < b.length; i++) {
            if (b[i] == '\r' && b[i + 1] == '\n' &&
                b[i + 2] == '\r' && b[i + 3] == '\n') {
                return i;
            }
        }
        return -1;
    }

    /** True when the request declares Transfer-Encoding: chunked. */
    static boolean isChunked(String head) {
        return headerValue(head, "Transfer-Encoding") != null;
    }

    /** Case-insensitive header lookup in a raw head block. */
    static String headerValue(String head, String name) {
        int i = 0;
        int n = head.length();
        while (i < n) {
            int eol = head.indexOf('\n', i);
            if (eol < 0) {
                eol = n;
            }
            {
                String line = head.substring(i, eol);
                int colon = line.indexOf(':');
                if (colon > 0 &&
                    line.substring(0, colon).trim().equalsIgnoreCase(name)) {
                    return line.substring(colon + 1).trim();
                }
            }
            i = eol + 1;
        }
        return null;
    }

    /**
     * Rebuild the header block for SceHttp: keep end-to-end headers, drop
     * the ones that describe a connection we are not using.
     */
    static String filterRequestHeaders(String head) {
        StringBuffer out = new StringBuffer();
        int i = 0;
        int n = head.length();
        boolean first = true;

        while (i < n) {
            int eol = head.indexOf('\n', i);
            if (eol < 0) {
                eol = n;
            }
            {
                String line = head.substring(i, eol);
                if (line.endsWith("\r")) {
                    line = line.substring(0, line.length() - 1);
                }
                if (first) {
                    first = false;      /* request line: not a header */
                } else if (line.length() > 0) {
                    int colon = line.indexOf(':');
                    if (colon > 0) {
                        String name = line.substring(0, colon).trim();
                        if (!isHopByHopRequest(name)) {
                            out.append(line).append("\r\n");
                        }
                    }
                }
            }
            i = eol + 1;
        }
        return out.toString();
    }

    private static boolean isHopByHopRequest(String name) {
        return name.equalsIgnoreCase("Host")
            || name.equalsIgnoreCase("Connection")
            || name.equalsIgnoreCase("Keep-Alive")
            || name.equalsIgnoreCase("Proxy-Connection")
            || name.equalsIgnoreCase("Proxy-Authorization")
            || name.equalsIgnoreCase("Transfer-Encoding")
            || name.equalsIgnoreCase("Content-Length")
            || name.equalsIgnoreCase("Accept-Encoding")
            || name.equalsIgnoreCase("TE")
            || name.equalsIgnoreCase("Upgrade");
    }

    /** Turn "5\r\nhello\r\n0\r\n\r\n" into "hello". */
    static String decodeChunked(String body) throws IOException {
        StringBuffer out = new StringBuffer();
        int i = 0;
        int n = body.length();

        while (i < n) {
            int eol = body.indexOf("\r\n", i);
            if (eol < 0) {
                break;
            }
            {
                String sz = body.substring(i, eol).trim();
                int semi = sz.indexOf(';');
                if (semi >= 0) {
                    sz = sz.substring(0, semi);
                }
                int len;
                try {
                    len = Integer.parseInt(sz, 16);
                } catch (NumberFormatException e) {
                    throw new IOException("HTTPS: bad chunk size");
                }
                i = eol + 2;
                if (len == 0) {
                    break;
                }
                if (i + len > n) {
                    throw new IOException("HTTPS: truncated chunk");
                }
                out.append(body.substring(i, i + len));
                i += len;
                if (i + 1 < n && body.charAt(i) == '\r') {
                    i += 2;                     /* CRLF after the data */
                }
            }
        }
        return out.toString();
    }

    /* ------------------------------------------------------------------ */
    /* Response side                                                      */
    /* ------------------------------------------------------------------ */

    /**
     * Rebuild a response head MIDP can parse.
     *
     * @param raw          what SceHttp returned (status line first)
     * @param length       declared body length, or -1
     * @param requestBody  body length we uploaded, used only for the 100
     *                     -continue case
     * @return the block, or null when `raw` had no usable status line
     */
    static byte[] buildResponseHeader(String raw, long length,
                                      int requestBody) {
        StringBuffer out = new StringBuffer();
        int i = 0;
        int n = raw.length();
        boolean first = true;
        boolean sawLength = false;

        if (raw == null || raw.length() == 0) {
            return null;
        }

        while (i < n) {
            int eol = raw.indexOf('\n', i);
            if (eol < 0) {
                eol = n;
            }
            {
                String line = raw.substring(i, eol);
                if (line.endsWith("\r")) {
                    line = line.substring(0, line.length() - 1);
                }
                if (first) {
                    first = false;
                    if (!line.startsWith("HTTP/")) {
                        /* SceHttp someetimes returns only headers; build a
                         * status line from the code we already have. */
                        if (line.length() == 0) {
                            i = eol + 1;
                            continue;
                        }
                        out.append("HTTP/1.1 200 OK\r\n");
                        first = true;    /* re-consume this line as header */
                    } else {
                        out.append(line).append("\r\n");
                        i = eol + 1;
                        continue;
                    }
                }
                if (line.length() > 0) {
                    int colon = line.indexOf(':');
                    if (colon > 0) {
                        String name = line.substring(0, colon).trim();
                        if (name.equalsIgnoreCase("Content-Length")) {
                            sawLength = true;
                            out.append(line).append("\r\n");
                        } else if (!isHopByHopResponse(name)) {
                            out.append(line).append("\r\n");
                        }
                    }
                }
            }
            i = eol + 1;
        }

        if (!sawLength && length >= 0) {
            out.append("Content-Length: ").append(length).append("\r\n");
        }
        out.append("Connection: close\r\n");
        out.append("\r\n");
        /* NOT String.getBytes(): its encoding is whatever the platform
         * default is (utf-8 on this build), which would turn a header byte
         * of 0xE9 into two bytes. The head is ISO-8859-1 by definition. */
        return toLatin1(out.toString());
    }

    private static boolean isHopByHopResponse(String name) {
        return name.equalsIgnoreCase("Transfer-Encoding")
            || name.equalsIgnoreCase("Connection")
            || name.equalsIgnoreCase("Keep-Alive")
            || name.equalsIgnoreCase("Content-Encoding")
            || name.equalsIgnoreCase("Proxy-Connection");
    }

    /* ------------------------------------------------------------------ */
    /* Misc                                                               */
    /* ------------------------------------------------------------------ */

    /** Decode bytes as ISO-8859-1 so no character is ever lost. */
    static String latin1(byte[] b, int off, int len) {
        char[] c = new char[len];
        for (int i = 0; i < len; i++) {
            c[i] = (char) (b[off + i] & 0xff);
        }
        return new String(c);
    }

    /** Same for the other direction (String.getBytes() is platform
     *  dependent in CLDC, so do not rely on it for wire bytes). */
    static byte[] toLatin1(String s) {
        byte[] b = new byte[s.length()];
        for (int i = 0; i < s.length(); i++) {
            b[i] = (byte) s.charAt(i);
        }
        return b;
    }

    int status() {
        return status;
    }

    String host() {
        return host;
    }

    int port() {
        return port;
    }

    String subject() {
        return backend.subject();
    }

    String protocolVersion() {
        return backend.protocolVersion();
    }

    String cipherSuite() {
        return backend.cipherSuite();
    }

    void close() {
        if (!closed) {
            closed = true;
            backend.end();
        }
    }
}
