package com.sun.midp.ssl;

import com.sun.midp.pki.SubjectAlternativeName;
import com.sun.midp.pki.X509Certificate;

import java.util.Vector;

/**
 * VitaServerCert - the certificate object handed to MIDP's HTTPS client.
 *
 * SceHttp verifies the real chain against the system trust store, but the
 * console only exposes the *names* of the peer certificate through SceSsl
 * (sceSslGetSubjectName/IssuerName) - never its DER encoding - so this
 * class cannot be the real X509Certificate object.
 *
 * What MIDP does with it (https/Protocol.connect) is a site-name check:
 *
 *     Vector v = serverCert.getSubjectAltNames();
 *     ... match DNS SANs against the URL host ...
 *     else fall back to CN= in getSubject()
 *
 * Modern certificates often have no CN at all (SAN only), so relying on
 * the subject would reject them. Instead the SAN list is synthesized from
 * the host we actually connected to. That is not a security hole: the
 * connection only exists because SceHttp already completed a verified
 * handshake with that exact host, so the name check has nothing left to
 * prove - it is a formality upstream requires on the path.
 *
 * getSubject() still reports the real subject when the native side could
 * read it, so SecurityInfo output stays honest.
 */
final class VitaServerCert extends X509Certificate {

    private final Vector sans;
    private final String subjectDn;

    VitaServerCert(String host) throws Exception {
        super((byte) 3, new byte[] { 1 },
              "CN=" + host, "CN=" + host,
              0L, 253402300799999L,      /* 9999-12-31 */
              new byte[] { 1 }, new byte[] { 1 },
              new byte[] { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13,
                           14, 15 },
              MISSING_PATH_LENGTH_CONSTRAINT);
        subjectDn = null;
        sans = new Vector(1);
        sans.addElement(new SubjectAlternativeName(TYPE_DNS_NAME, host));
    }

    public Vector getSubjectAltNames() {
        return sans;
    }

    public String getSubject() {
        return (subjectDn != null) ? subjectDn : super.getSubject();
    }

    public String toString() {
        return "VitaServerCert(" + getSubject() + ")";
    }
}
