/*
 * vita_https_kni.c - KNI glue between com.sun.midp.ssl.VitaHttpNative and
 *                    the SceHttp bridge in vita_https.c (roadmap P0-7)
 *
 * Kept separate from vita_https.c so that file stays a plain C module with
 * no VM headers: the tunnel logic is unit-tested on a PC and the test must
 * not have to drag in kni.h.
 *
 * The function names below are not free choices - they are the ones the
 * MIDP build generated into nativeFunctionTable.cpp from
 * VitaHttpNative.java. Run `grep VitaHttpNative nativeFunctionTable.cpp`
 * after touching the Java native list.
 *
 * All strings crossing this boundary are ASCII: URLs, methods and header
 * names are, and the Java side builds them from byte values. Anything above
 * 0x7F becomes '?' instead of being silently truncated.
 */
#include <kni.h>
#include <commonKNIMacros.h>
#include <string.h>

#include "vita_https.h"

/* Copy a Java String into a NUL-terminated jchar buffer, mapping non-ASCII
 * to '?'. Returns the length, or -1 when it does not fit. */
static int str_to_ascii(jchar *dst, int cap, jobject handle) {
    jsize len;

    if (dst == NULL || cap <= 0) return -1;
    dst[0] = 0;
    if (KNI_IsNullHandle(handle)) return 0;
    len = KNI_GetStringLength(handle);
    if (len >= cap) return -1;
    KNI_GetStringRegion(handle, 0, len, dst);
    {
        jsize i;
        for (i = 0; i < len; i++) {
            if (dst[i] > 0x7F) dst[i] = '?';
        }
    }
    dst[len] = 0;
    return (int)len;
}

/* jchar -> char for the ASCII-only strings above. */
static void to_bytes(char *dst, const jchar *src, int n) {
    int i;
    for (i = 0; i < n; i++) {
        dst[i] = (char)src[i];
    }
    dst[n] = '\0';
}

KNIEXPORT KNI_RETURNTYPE_INT
KNIDECL(com_sun_midp_ssl_VitaHttpNative_nAvailable) {
    KNI_ReturnInt(vita_https_available());
}

KNIEXPORT KNI_RETURNTYPE_INT
KNIDECL(com_sun_midp_ssl_VitaHttpNative_nBegin) {
    jchar whost[256], wua[128];
    char host[256], ua[128];
    int port, nhost, nua, rc;

    KNI_StartHandles(2);
    KNI_DeclareHandle(h);
    KNI_DeclareHandle(u);
    KNI_GetParameterAsObject(1, h);
    KNI_GetParameterAsObject(3, u);
    port = KNI_GetParameterAsInt(2);

    nhost = str_to_ascii(whost, 256, h);
    nua = str_to_ascii(wua, 128, u);
    KNI_EndHandles();

    /* Outside the handle block: KNI_EndHandles() *is* the closing brace of
     * the block KNI_StartHandles() opens, so nothing may follow it in the
     * same scope except the return. */
    rc = 0;
    if (nhost > 0) {
        if (nua < 0) {
            nua = 0;
        }
        to_bytes(host, whost, nhost);
        to_bytes(ua, wua, nua);
        rc = vita_https_begin(host, port, (nua > 0) ? ua : NULL);
    }
    KNI_ReturnInt(rc);
}

KNIEXPORT KNI_RETURNTYPE_INT
KNIDECL(com_sun_midp_ssl_VitaHttpNative_nExec) {
    jchar wmethod[32], wpath[1024], whdrs[2048];
    char method[32], path[1024], hdrs[2048];
    const unsigned char *bodyPtr;
    int nm, np, nh, bodyLen = 0, rc;
    jsize blen = 0;

    KNI_StartHandles(4);
    KNI_DeclareHandle(m);
    KNI_DeclareHandle(p);
    KNI_DeclareHandle(hd);
    KNI_DeclareHandle(bd);
    KNI_GetParameterAsObject(1, m);
    KNI_GetParameterAsObject(2, p);
    KNI_GetParameterAsObject(3, hd);
    KNI_GetParameterAsObject(4, bd);

    nm = str_to_ascii(wmethod, 32, m);
    np = str_to_ascii(wpath, 1024, p);
    nh = str_to_ascii(whdrs, 2048, hd);

    bodyPtr = NULL;
    if (!KNI_IsNullHandle(bd)) {
        blen = KNI_GetArrayLength(bd);
        if (blen > 0) {
            /* JavaByteArray() returns a pinned pointer into the Java array,
             * so a POST body costs no copy at all - the largest request the
             * tunnel allows (4 MB) is fine. It must stay pinned until
             * vita_https_exec returns, hence the KNI handles stay open. */
            bodyPtr = (const unsigned char *)JavaByteArray(bd);
            bodyLen = (int)blen;
        }
    }

    if (nm <= 0 || np <= 0 || nh < 0) {
        rc = -1;                       /* single exit: KNI_EndHandles cannot
                                        * appear inside a bare if-block */
    } else {
        to_bytes(method, wmethod, nm);
        to_bytes(path, wpath, np);
        to_bytes(hdrs, whdrs, nh);
        rc = vita_https_exec(method, path, hdrs, nh,
                             (const char *)bodyPtr, bodyLen);
    }
    KNI_EndHandles();
    KNI_ReturnInt(rc);
}

KNIEXPORT KNI_RETURNTYPE_INT
KNIDECL(com_sun_midp_ssl_VitaHttpNative_nHeaders) {
    int cap, n;

    KNI_StartHandles(1);
    KNI_DeclareHandle(a);
    KNI_GetParameterAsObject(1, a);
    cap = KNI_GetParameterAsInt(2);
    n = 0;
    if (!KNI_IsNullHandle(a) && cap > 0) {
        n = vita_https_resp_headers((char *)JavaByteArray(a), cap);
    }
    KNI_EndHandles();
    KNI_ReturnInt(n);
}

KNIEXPORT KNI_RETURNTYPE_LONG
KNIDECL(com_sun_midp_ssl_VitaHttpNative_nLength) {
    KNI_ReturnLong((jlong)vita_https_resp_length());
}

KNIEXPORT KNI_RETURNTYPE_INT
KNIDECL(com_sun_midp_ssl_VitaHttpNative_nRead) {
    int cap, n;

    KNI_StartHandles(1);
    KNI_DeclareHandle(a);
    KNI_GetParameterAsObject(1, a);
    cap = KNI_GetParameterAsInt(2);
    n = 0;
    if (!KNI_IsNullHandle(a) && cap > 0) {
        n = vita_https_read((char *)JavaByteArray(a), cap);
    }
    KNI_EndHandles();
    KNI_ReturnInt(n);
}

KNIEXPORT KNI_RETURNTYPE_INT
KNIDECL(com_sun_midp_ssl_VitaHttpNative_nSubject) {
    int cap, n;

    KNI_StartHandles(1);
    KNI_DeclareHandle(a);
    KNI_GetParameterAsObject(1, a);
    cap = KNI_GetParameterAsInt(2);
    n = 0;
    if (!KNI_IsNullHandle(a) && cap > 0) {
        n = vita_https_subject((char *)JavaByteArray(a), cap);
    }
    KNI_EndHandles();
    KNI_ReturnInt(n);
}

KNIEXPORT KNI_RETURNTYPE_OBJECT
KNIDECL(com_sun_midp_ssl_VitaHttpNative_nProtoVersion) {
    KNI_StartHandles(1);
    KNI_DeclareHandle(r);
    KNI_NewStringUTF(vita_https_proto_version(), r);
    KNI_EndHandlesAndReturnObject(r);
}

KNIEXPORT KNI_RETURNTYPE_OBJECT
KNIDECL(com_sun_midp_ssl_VitaHttpNative_nCipher) {
    KNI_StartHandles(1);
    KNI_DeclareHandle(r);
    KNI_NewStringUTF(vita_https_cipher(), r);
    KNI_EndHandlesAndReturnObject(r);
}

KNIEXPORT KNI_RETURNTYPE_VOID
KNIDECL(com_sun_midp_ssl_VitaHttpNative_nEnd) {
    vita_https_end();
    KNI_ReturnVoid();
}
