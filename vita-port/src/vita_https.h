/*
 * vita_https.h - HTTPS support for MIDP via the console's own HTTP stack
 *                (see vita_https.c)
 *
 * Why SceHttp and not SceSsl: vitaSDK's SceSsl exposes only
 * sceSslInit/Term plus certificate introspection - there is NO record-layer
 * API (no open/read/write/close) in libSceSsl_stub.a, so it cannot be used
 * as a TLS transport. SceHttp is complete and performs TLS internally (it
 * is the browser's stack), including certificate verification against the
 * system trust store.
 *
 * The consequence: we cannot get at the decrypted *socket*, only at whole
 * HTTP transactions. vita_https therefore exposes a request/response API
 * and the Java side presents it as a StreamConnection - see
 * vita-port/vendor_src/com/sun/midp/ssl/VitaTunnel.java.
 */
#ifndef VITA_HTTPS_H
#define VITA_HTTPS_H

/* One function at a time; the MIDP HTTP layer is single-threaded per
 * connection and the tunnel keeps the state in C. Returns 1 on success. */
int vita_https_begin(const char *host, int port, const char *user_agent);

/* Execute one request. `headers` is the raw "Name: value\r\n" block built
 * by the Java side (already filtered); `body` may be NULL/0.
 * Returns the HTTP status code (>=100), or a negative SceHttp/SceNet error
 * when the request could not be sent at all. */
int vita_https_exec(const char *method, const char *path,
                    const char *headers, int headers_len,
                    const char *body, int body_len);

/* Raw response header block (starts with the status line and includes the
 * terminating empty line as returned by sceHttpGetAllResponseHeaders).
 * Returns the number of bytes copied, or <=0 when there is nothing. */
int vita_https_resp_headers(char *buf, int cap);

/* Declared body length, or -1 when the server did not say (the caller then
 * frames the body by EOF). */
long long vita_https_resp_length(void);

/* Sequential body read: >0 bytes, 0 = end of body, <0 = error. */
int vita_https_read(char *buf, int cap);

/* Negotiated TLS info for SecurityInfo: "TLSv1.2" / "TLSv1.3" style string
 * (best effort - SceHttp does not report it, so this is a constant) and the
 * server certificate's subject DN as "CN=..., O=..." ("" when unavailable). */
const char *vita_https_proto_version(void);
const char *vita_https_cipher(void);
int vita_https_subject(char *buf, int cap);

void vita_https_end(void);

/* Set once when SceHttp could not be initialised, so the Java side can
 * report "unsupported" instead of failing per request. */
int vita_https_available(void);

#endif /* VITA_HTTPS_H */
