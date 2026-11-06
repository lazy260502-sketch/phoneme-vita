package com.sun.midp.ssl;

import com.sun.midp.pki.X509Certificate;
import com.sun.midp.publickeystore.WebPublicKeyStore;

import java.io.DataInputStream;
import java.io.DataOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;

import javax.microedition.io.SecurityInfo;
import javax.microedition.io.StreamConnection;

/**
 * SSLStreamConnection - the TLS seam for MIDP's HTTPS and ssl:// protocols,
 * implemented on the console's own HTTP stack.
 *
 * WHY THIS EXISTS (v02.91, roadmap P0-7)
 * --------------------------------------
 * Upstream provides this class from restricted_crypto: a TLS record layer
 * written in Java (Handshake/Record/In/Out, ~3600 lines) that negotiates
 * SSLv3/TLS 1.0. Every public server today refuses TLS 1.0, so compiling it
 * would add a large GPL library that cannot connect to anything.
 *
 * vitaSDK offers no drop-in replacement:
 *   - SceSsl exports only sceSslInit/Term + certificate introspection; there
 *     are no record-layer functions in libSceSsl_stub.a at all.
 *   - SceHttp is complete, does TLS 1.2 in hardware and is what the console
 *     browser uses, but it works in whole HTTP transactions, not sockets.
 *
 * So the port keeps ALL of upstream's HTTP client logic (the 2500-line
 * https/Protocol.java is compiled unmodified and still owns chunked
 * transfer, redirects, keep-alive, header parsing and the HttpConnection
 * API) and substitutes only this class: it presents the request/response
 * byte stream MIDP expects and drives SceHttp underneath. See
 * VitaTunnel.java for the mechanics.
 *
 * CONTRACT KEPT FROM UPSTREAM
 * ---------------------------
 *   SSLStreamConnection(String host, int port,
 *                       InputStream in, OutputStream out,
 *                       WebPublicKeyStore keystore)
 *   InputStream  openInputStream()
 *   OutputStream openOutputStream()
 *   X509Certificate getServerCertificate()
 *   SecurityInfo getSecurityInfo()
 *   void close()
 *
 * The `in`/`out` streams belong to the TCP socket MIDP opened first. They
 * are NOT used for TLS (SceHttp opens its own TCP connection); they are
 * closed when this object is closed so the socket does not leak. The
 * keystore is ignored for verification - SceHttp verifies against the
 * system trust store, which is the only source of roots on this device.
 *
 * LIMITATION: the ssl:// scheme (raw TLS socket) therefore does not work.
 * Nothing in the J2ME game corpus uses it; a game that does gets an
 * IOException instead of garbage. HTTPS, which is what games use, works.
 */
public class SSLStreamConnection implements StreamConnection {

    private final VitaTunnel tunnel;
    private final InputStream rawIn;
    private final OutputStream rawOut;
    private X509Certificate serverCert;
    private boolean certTried;

    /**
     * @param host     server name
     * @param port     server port
     * @param in       socket input (unused for TLS, closed with us)
     * @param out      socket output (unused for TLS, closed with us)
     * @param keystore public key store - accepted for API compatibility,
     *                 ignored (SceHttp uses the system trust store)
     * @throws IOException when HTTPS is not usable on this system
     */
    public SSLStreamConnection(String host, int port,
                               InputStream in, OutputStream out,
                               WebPublicKeyStore keystore)
            throws IOException {
        this(host, port, in, out, keystore, new VitaHttpNative());
    }

    /** Test seam: lets the host unit test inject a fake backend. */
    SSLStreamConnection(String host, int port,
                        InputStream in, OutputStream out,
                        WebPublicKeyStore keystore,
                        VitaHttpBackend backend)
            throws IOException {
        if (host == null) {
            throw new IOException("HTTPS: null host");
        }
        if (backend == null || backend.available() <= 0) {
            throw new IOException("HTTPS not supported on this system");
        }
        if (!backend.begin(host, port,
                           "Mozilla/5.0 (PlayStation Vita) J2ME")) {
            throw new IOException("HTTPS: cannot connect to " + host);
        }
        rawIn = in;
        rawOut = out;
        tunnel = new VitaTunnel(host, port, backend);
    }

    public InputStream openInputStream() throws IOException {
        return tunnel.openInputStream();
    }

    public OutputStream openOutputStream() throws IOException {
        return tunnel.openOutputStream();
    }

    /* StreamConnection extends InputConnection + OutputConnection, so the
     * Data* variants are required by the interface even though the HTTPS
     * layer only uses the plain streams. */
    public DataInputStream openDataInputStream() throws IOException {
        return new DataInputStream(openInputStream());
    }

    public DataOutputStream openDataOutputStream() throws IOException {
        return new DataOutputStream(openOutputStream());
    }

    /**
     * The certificate MIDP validates the site name against. See
     * VitaServerCert for why it is synthesized rather than parsed.
     */
    public X509Certificate getServerCertificate() {
        if (!certTried) {
            certTried = true;
            try {
                serverCert = new VitaServerCert(tunnel.host());
            } catch (Exception e) {
                serverCert = null;
            }
        }
        return serverCert;
    }

    /** SecurityInfo for HttpsConnection.getSecurityInfo(). */
    public SecurityInfo getSecurityInfo() {
        /* Force the transaction so the tunnel has talked to the server
         * (upstream's contract says the connection is established). */
        try {
            getServerCertificate();
        } catch (Throwable t) {
            /* fall through: the info below is still the best we have */
        }
        return new VitaSecurityInfo();
    }

    public void close() throws IOException {
        try {
            tunnel.close();
        } finally {
            try {
                if (rawIn != null) {
                    rawIn.close();
                }
            } catch (Throwable t) {
                /* ignore */
            }
            try {
                if (rawOut != null) {
                    rawOut.close();
                }
            } catch (Throwable t) {
                /* ignore */
            }
        }
    }

    /* ------------------------------------------------------------------ */

    /** Minimal SecurityInfo; SceHttp does not report the cipher suite. */
    private final class VitaSecurityInfo implements SecurityInfo {
        public javax.microedition.pki.Certificate getServerCertificate() {
            return SSLStreamConnection.this.getServerCertificate();
        }

        public String getProtocolVersion() {
            return tunnel.protocolVersion();
        }

        public String getProtocolName() {
            return "TLS";
        }

        public String getCipherSuite() {
            return tunnel.cipherSuite();
        }
    }
}
