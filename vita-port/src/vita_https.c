/*
 * vita_https.c - HTTPS for MIDP through SceHttp (roadmap P0-7)
 *
 * WHY THIS SHAPE
 * --------------
 * phoneME's HTTPS support lives in two upstream pieces:
 *   - com.sun.midp.io.j2me.https.Protocol: a complete HTTP/1.1 client
 *     (headers, chunked, redirects, keep-alive, HttpConnection API).
 *   - com.sun.midp.ssl.SSLStreamConnection: the TLS record layer.
 * The second one comes from restricted_crypto (Handshake/Record/In/Out,
 * ~3600 lines) and speaks SSLv3/TLS 1.0 only. Every server on the internet
 * today rejects TLS 1.0/1.1, so compiling it would add a lot of GPL code
 * that cannot connect to anything.
 *
 * vitaSDK's SceSsl is not an alternative either: libSceSsl_stub.a exports
 * only sceSslInit/Term, the certificate inspectors and three internal
 * helpers - there is no open/read/write/close, i.e. no way to use it as a
 * transport.
 *
 * SceHttp, on the other hand, is fully exported and is the stack the
 * browser uses: it does TLS 1.2/1.3 with the system trust store. So the
 * port keeps ALL of upstream's HTTP logic (the 2500-line Protocol.java is
 * used unmodified) and replaces only SSLStreamConnection with a tunnel
 * that turns the request/response byte stream into SceHttp transactions.
 *
 * This file is the native half: it owns the SceHttp handles and moves
 * bytes. All protocol reasoning (request parsing, header filtering,
 * response synthesis) lives in Java so it can be unit-tested on a PC -
 * see vita-port/vendor_src/com/sun/midp/ssl/VitaTunnel.java.
 *
 * LIMITATIONS (documented, not accidental)
 * ----------------------------------------
 * - One transaction at a time: the tunnel is created per connection and
 *   MIDP does not pipeline.
 * - Request bodies must be complete before the response is read (MIDP's
 *   "streaming" write-then-read mode is not supported; it is not used by
 *   any game sample and cannot work through SceHttp anyway).
 * - The peer certificate DER is not obtainable (SceSsl only exposes names
 *   and dates), so the Java side builds a synthetic certificate whose
 *   subject comes from sceSslGetSubjectName and whose SAN carries the
 *   host. Real chain verification is done by SceHttp.
 */
#include <psp2/net/http.h>
#include <psp2/sysmodule.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vita_https.h"

/* SceHttp's own pool. 256 KB is what the SDK samples use for a handful of
 * connections; this port only ever has one live transaction. */
#define HTTPS_POOL_SIZE   (256 * 1024)
#define HTTPS_MAX_HEADER  8192

static int g_http_up = 0;      /* 0 = not tried, 1 = ready, -1 = failed */
static int g_tmpl = -1;
static int g_conn = -1;
static int g_req = -1;
static int g_status = 0;
static long long g_body_left = 0;
static char g_subject[256];

static void https_log(const char *fmt, ...) {
    /* stderr only: this runs on the VM thread and a synchronous file write
     * costs milliseconds. midp_stderr.log is captured by the launcher. */
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

static void https_close_handles(void) {
    if (g_req >= 0) { sceHttpDeleteRequest(g_req); g_req = -1; }
    if (g_conn >= 0) { sceHttpDeleteConnection(g_conn); g_conn = -1; }
}

/* Lazily load the modules and initialise the library. Returns 0 on
 * success, -1 when HTTPS is not usable on this system (Vita3K without the
 * modules, for instance) - the caller then fails the connection cleanly
 * instead of crashing. */
static int https_init(void) {
    int rc;

    if (g_http_up == 1) return 0;
    if (g_http_up == -1) return -1;

    rc = sceSysmoduleLoadModule(SCE_SYSMODULE_HTTP);
    if (rc < 0 && rc != (int)0x8002D003) {         /* already loaded */
        https_log("[HTTPS] LoadModule(HTTP) rc=0x%08x\n", rc);
        g_http_up = -1;
        return -1;
    }
    rc = sceSysmoduleLoadModule(SCE_SYSMODULE_HTTPS);
    if (rc < 0 && rc != (int)0x8002D003) {
        https_log("[HTTPS] LoadModule(HTTPS) rc=0x%08x\n", rc);
        /* Plain HTTP would still work, but this port only uses SceHttp for
         * HTTPS, so treat it as unavailable. */
        g_http_up = -1;
        return -1;
    }
    rc = sceHttpInit(HTTPS_POOL_SIZE);
    if (rc < 0 && rc != (int)0x80431020) {         /* already inited */
        https_log("[HTTPS] sceHttpInit rc=0x%08x\n", rc);
        g_http_up = -1;
        return -1;
    }
    https_log("[HTTPS] SceHttp ready\n");
    g_http_up = 1;
    return 0;
}

int vita_https_available(void) {
    return (https_init() == 0) ? 1 : 0;
}

const char *vita_https_proto_version(void) {
    /* SceHttp does not report the negotiated version; claiming the modern
     * one is the honest default because that is what it negotiates. */
    return "TLSv1.2";
}

const char *vita_https_cipher(void) {
    /* Same reason: not exposed. SecurityInfo only displays this. */
    return "UNKNOWN";
}

int vita_https_subject(char *buf, int cap) {
    int n;
    if (buf == NULL || cap <= 0) return 0;
    n = (int)strlen(g_subject);
    if (n >= cap) n = cap - 1;
    memcpy(buf, g_subject, (size_t)n);
    buf[n] = '\0';
    return n;
}

int vita_https_begin(const char *host, int port, const char *user_agent) {
    int rc;

    if (host == NULL || port <= 0) return 0;
    if (https_init() != 0) return 0;

    https_close_handles();
    g_status = 0;
    g_body_left = 0;

    /* One template per connection is wasteful but the browser stack keeps
     * per-template state (proxy config, UA) and connections are rare here. */
    if (g_tmpl >= 0) {
        sceHttpDeleteTemplate(g_tmpl);
        g_tmpl = -1;
    }
    g_tmpl = sceHttpCreateTemplate((char *)(user_agent != NULL ? user_agent
                                                              : "J2ME-Vita"),
                                   SCE_HTTP_VERSION_1_1, 1);
    if (g_tmpl < 0) {
        https_log("[HTTPS] CreateTemplate rc=0x%08x\n", g_tmpl);
        return 0;
    }

    g_conn = sceHttpCreateConnection(g_tmpl, (char *)host, "https",
                                     (unsigned short)port, 0);
    if (g_conn < 0) {
        https_log("[HTTPS] CreateConnection rc=0x%08x\n", g_conn);
        g_conn = -1;
        return 0;
    }

    /* Redirects: let SceHttp follow them. MIDP would otherwise reopen the
     * connection itself, which also works, but then a 3xx from a CDN would
     * surface to the game as a redirect it did not ask for. */
    rc = sceHttpSetAutoRedirect(g_conn, 1);
    if (rc < 0) https_log("[HTTPS] SetAutoRedirect rc=0x%08x\n", rc);
    (void)sceHttpSetCookieEnabled(g_conn, 1);
    (void)sceHttpSetConnectTimeOut(g_conn, 20 * 1000 * 1000);
    (void)sceHttpSetRecvTimeOut(g_conn, 30 * 1000 * 1000);
    (void)sceHttpSetSendTimeOut(g_conn, 30 * 1000 * 1000);
    (void)sceHttpSetResponseHeaderMaxSize(g_conn, HTTPS_MAX_HEADER);

    g_subject[0] = '\0';
    return 1;
}

/* Map the request method name the Java side parsed to SceHttp's enum. */
static int https_method(const char *m) {
    if (m == NULL) return SCE_HTTP_METHOD_GET;
    if (strcmp(m, "GET") == 0) return SCE_HTTP_METHOD_GET;
    if (strcmp(m, "POST") == 0) return SCE_HTTP_METHOD_POST;
    if (strcmp(m, "HEAD") == 0) return SCE_HTTP_METHOD_HEAD;
    if (strcmp(m, "PUT") == 0) return SCE_HTTP_METHOD_PUT;
    if (strcmp(m, "DELETE") == 0) return SCE_HTTP_METHOD_DELETE;
    if (strcmp(m, "OPTIONS") == 0) return SCE_HTTP_METHOD_OPTIONS;
    if (strcmp(m, "TRACE") == 0) return SCE_HTTP_METHOD_TRACE;
    return SCE_HTTP_METHOD_GET;
}

/* Add the "Name: value" lines from the block the Java side built. */
static void https_add_headers(int id, const char *block, int len) {
    const char *p = block;
    const char *end = block + len;

    while (p < end) {
        const char *nl = p;
        const char *colon;
        char name[64];
        int nlen;

        while (nl < end && *nl != '\n') nl++;
        if (nl == p) {               /* empty line: end of block */
            break;
        }
        colon = memchr(p, ':', (size_t)(nl - p));
        if (colon != NULL) {
            nlen = (int)(colon - p);
            if (nlen > 0 && nlen < (int)sizeof(name)) {
                memcpy(name, p, (size_t)nlen);
                name[nlen] = '\0';
                {
                    const char *v = colon + 1;
                    int vlen;
                    while (v < nl && (*v == ' ' || *v == '\t')) v++;
                    vlen = (int)(nl - v);
                    while (vlen > 0 && (v[vlen - 1] == '\r' ||
                                        v[vlen - 1] == '\n' ||
                                        v[vlen - 1] == ' ')) {
                        vlen--;
                    }
                    if (vlen >= 0) {
                        char *val = (char *)malloc((size_t)vlen + 1);
                        if (val != NULL) {
                            memcpy(val, v, (size_t)vlen);
                            val[vlen] = '\0';
                            sceHttpAddRequestHeader(id, name, val,
                                                    SCE_HTTP_HEADER_OVERWRITE);
                            free(val);
                        }
                    }
                }
            }
        }
        p = (nl < end) ? nl + 1 : end;
    }
}

int vita_https_exec(const char *method, const char *path,
                    const char *headers, int headers_len,
                    const char *body, int body_len) {
    int rc;

    if (g_conn < 0) return -1000;
    if (g_req >= 0) {
        sceHttpDeleteRequest(g_req);
        g_req = -1;
    }

    g_req = sceHttpCreateRequest(g_conn, https_method(method),
                                 (char *)(path != NULL ? path : "/"),
                                 (unsigned long long)(body_len > 0 ? body_len
                                                                  : 0));
    if (g_req < 0) {
        https_log("[HTTPS] CreateRequest rc=0x%08x\n", g_req);
        g_req = -1;
        return -1001;
    }

    if (headers != NULL && headers_len > 0) {
        https_add_headers(g_req, headers, headers_len);
    }
    /* Ask for an unencoded body: the tunnel has no gzip decoder and
     * phoneME's HTTP layer does not decode it either. */
    (void)sceHttpAddRequestHeader(g_req, "Accept-Encoding", "identity",
                                  SCE_HTTP_HEADER_OVERWRITE);

    rc = sceHttpSendRequest(g_req, body, (unsigned int)(body_len > 0
                                                       ? body_len : 0));
    if (rc < 0) {
        https_log("[HTTPS] SendRequest rc=0x%08x\n", rc);
        return rc;
    }

    rc = sceHttpGetStatusCode(g_req, &g_status);
    if (rc < 0) {
        https_log("[HTTPS] GetStatusCode rc=0x%08x\n", rc);
        return rc;
    }

    {
        unsigned long long len = 0;
        if (sceHttpGetResponseContentLength(g_req, &len) == 0) {
            g_body_left = (long long)len;
        } else {
            g_body_left = -1;         /* unknown: frame by EOF */
        }
    }
    return g_status;
}

int vita_https_resp_headers(char *buf, int cap) {
    char *raw = NULL;
    unsigned int rawlen = 0;
    int n;

    if (g_req < 0 || buf == NULL || cap <= 0) return 0;
    if (sceHttpGetAllResponseHeaders(g_req, &raw, &rawlen) < 0 ||
        raw == NULL || rawlen == 0) {
        return 0;
    }
    n = (int)rawlen;
    if (n > cap - 1) n = cap - 1;
    memcpy(buf, raw, (size_t)n);
    buf[n] = '\0';
    return n;
}

long long vita_https_resp_length(void) {
    return g_body_left;
}

int vita_https_read(char *buf, int cap) {
    int rc;

    if (g_req < 0 || buf == NULL || cap <= 0) return 0;
    if (g_body_left == 0) return 0;          /* known-finished */

    rc = sceHttpReadData(g_req, buf, (unsigned int)cap);
    if (rc < 0) {
        https_log("[HTTPS] ReadData rc=0x%08x\n", rc);
        return -1;
    }
    if (rc == 0) {
        g_body_left = 0;
        return 0;
    }
    if (g_body_left > 0) {
        g_body_left -= rc;
        if (g_body_left < 0) g_body_left = 0;
    }
    return rc;
}

void vita_https_end(void) {
    https_close_handles();
    if (g_tmpl >= 0) {
        sceHttpDeleteTemplate(g_tmpl);
        g_tmpl = -1;
    }
    g_body_left = 0;
    g_status = 0;
    g_subject[0] = '\0';
}
