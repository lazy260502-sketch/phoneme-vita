/*
 * Vita stub implementations for missing PCSL, javacall, and JVM native
 * functions. These are minimal stubs that allow libmidp.so to link.
 * Many of these will need proper implementations for full functionality.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <limits.h>

/* PS Vita headers */
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/io/dirent.h>
#include <psp2/appmgr.h>

/* PCSL string headers */
#include <pcsl_string.h>
#include <pcsl_string_status.h>
#include <pcsl_string_md.h>

/* PCSL file/socket/network/datagram headers */
#include <pcsl_file.h>
#include <pcsl_directory.h>   /* JSR 75 directory/attribute service */
#include <pcsl_socket.h>
#include <pcsl_network.h>
#include <pcsl_datagram.h>
#include <pcsl_print.h>

/* JavaCall memory */
#include <javacall_memory.h>

/* v01.57: stdio-free logging channel */
#include "vita_crumb.h"

/* JVM and KNI */
#include <kni.h>
#include <jvm.h>
#include <midp_logging.h>

/* PCSL string constants.
 *
 * v01.55 ROOT CAUSE FIX: upstream (pcsl/string/utf16/pcsl_string.c)
 * defines the invariant "data is always zero-terminated and ->length
 * COUNTS that terminating zero" - PCSL_STRING_EMPTY is {&zero_char, 1}
 * and every literal built by PCSL_DEFINE_*_LITERAL macros carries
 * sizeof(arr)/sizeof(jchar) (NUL included) as its length. Our old stubs
 * treated ->length as "characters without the terminator" and copied
 * ->length jchars verbatim in cat/append, so an embedded NUL traveled
 * INSIDE the concatenated result: root + "FFFFFFFF\0" + "A.db\0"
 * produced a pcsl_string whose C-view stopped at "…/FFFFFFFF". Every
 * consumer that converts to a C string (snprintf/sceIoOpen) truncated
 * there, so ALL record stores of the internal suite resolved to the
 * same physical file "…/rms/appdb_1A35/FFFFFFFF" - store names and
 * the .db extension were silently dropped ("first save says OK,
 * second launch finds nothing"). The constants below and every
 * length-aware function now follow the upstream convention exactly
 * (see each fix point marked v01.55). */
static jchar vita_empty_string_data = 0;

const pcsl_string PCSL_STRING_EMPTY = { &vita_empty_string_data, 1, 0 };
const pcsl_string PCSL_STRING_NULL  = { NULL, 0, 0 };

/* ========================================================================
 * PCSL String stubs
 * ======================================================================== */

jboolean pcsl_string_is_active(void) {
    return KNI_TRUE;
}

pcsl_string_status pcsl_string_initialize(void) {
    return PCSL_STRING_OK;
}

pcsl_string_status pcsl_string_finalize(void) {
    return PCSL_STRING_OK;
}

jsize pcsl_string_length(const pcsl_string *str) {
    if (str == NULL || str->data == NULL) {
        return 0;
    }
    /* v01.55: upstream counts the terminating zero in ->length and
     * subtracts it here ("Do not count terminating '\0'",
     * pcsl_string.c:110). Callers build sizes/paths from this value. */
    if (str->length > 0 && str->data[str->length - 1] == 0) {
        return str->length - 1;
    }
    return str->length;
}

jsize pcsl_string_utf16_length(const pcsl_string *str) {
    /* v01.55: same upstream rule as pcsl_string_length(). */
    return pcsl_string_length(str);
}

jsize pcsl_string_utf8_length(const pcsl_string *str) {
    if (str == NULL || str->data == NULL) {
        return -1;
    }
    /* Real UTF-8 byte length (no NUL). Callers like midpGetJarEntry
     * compare this against on-disk entry name lengths - it must be
     * exact, not an estimate (v01.24 "JAR Corrupt" root cause:
     * "META-INF/MANIFEST.MF" 20 chars returned as 60, name match
     * could never succeed, CD walk ran past the last entry). */
    jsize n = 0;
    jsize i;
    /* v01.55: stop at the terminating zero (upstream length counts
     * it; the UTF-8 view must not). */
    jsize len = pcsl_string_length(str);
    for (i = 0; i < len; i++) {
        jchar c = str->data[i];
        if (c < 0x80) {
            n += 1;
        } else if (c < 0x800) {
            n += 2;
        } else {
            n += 3;
        }
    }
    return n;
}

pcsl_string_status pcsl_string_convert_to_utf8(const pcsl_string *string,
                                               jbyte *buffer,
                                               jsize buffer_length,
                                               jsize *converted_length) {
    if (string == NULL || buffer == NULL) {
        return PCSL_STRING_EINVAL;
    }
    /* Proper UTF-16 -> UTF-8 conversion (multi-byte sequences
     * included, NUL terminated) matching upstream semantics. */
    jsize o = 0;
    jsize i;
    /* v01.55: convert the CONTENT only (upstream "->length" counts
     * the terminating zero; the UTF-8 view stops before it). The old
     * loop walked ->length jchars, so an embedded zero was copied
     * into the middle of the result and every C-string consumer
     * (snprintf %s, sceIoOpen) truncated at it. */
    jsize len = pcsl_string_length(string);
    for (i = 0; i < len; i++) {
        jchar c = string->data[i];
        jsize need;
        if (c < 0x80) {
            need = 1;
        } else if (c < 0x800) {
            need = 2;
        } else {
            need = 3;
        }
        if (o + need >= buffer_length) {
            return PCSL_STRING_BUFFER_OVERFLOW;
        }
        if (c < 0x80) {
            buffer[o++] = (jbyte)c;
        } else if (c < 0x800) {
            buffer[o++] = (jbyte)(0xC0 | (c >> 6));
            buffer[o++] = (jbyte)(0x80 | (c & 0x3F));
        } else {
            buffer[o++] = (jbyte)(0xE0 | (c >> 12));
            buffer[o++] = (jbyte)(0x80 | ((c >> 6) & 0x3F));
            buffer[o++] = (jbyte)(0x80 | (c & 0x3F));
        }
    }
    buffer[o] = 0;
    if (converted_length != NULL) {
        *converted_length = o;
    }
    return PCSL_STRING_OK;
}

pcsl_string_status pcsl_string_convert_to_utf16(const pcsl_string *str,
                                                jchar *buffer,
                                                jsize buffer_length,
                                                jsize *converted_length) {
    if (str == NULL || buffer == NULL) {
        return PCSL_STRING_EINVAL;
    }
    /* v01.55: upstream copies str->length jchars (terminator
     * included) and reports length-1 as converted; the caller-side
     * content is identical to before, only the reported count now
     * excludes the zero. */
    jsize len = str->length;
    if (buffer_length < len) {
        return PCSL_STRING_BUFFER_OVERFLOW;
    }
    memcpy(buffer, str->data, len * sizeof(jchar));
    if (converted_length != NULL) {
        *converted_length = len - 1;
    }
    return PCSL_STRING_OK;
}

pcsl_string_status pcsl_string_convert_from_utf8(const jbyte *buffer,
                                                  jsize buffer_length,
                                                  pcsl_string *string) {
    if (buffer == NULL || string == NULL) {
        return PCSL_STRING_EINVAL;
    }
    /* v01.55: upstream strips trailing zeroes from the source and
     * appends exactly ONE terminating zero; ->length counts it. */
    while (buffer_length > 0 && buffer[buffer_length - 1] == 0) {
        buffer_length--;
    }
    jchar *data = (jchar *)malloc((buffer_length + 1) * sizeof(jchar));
    if (data == NULL) {
        return PCSL_STRING_ENOMEM;
    }
    jsize i;
    for (i = 0; i < buffer_length; i++) {
        data[i] = (jchar)(buffer[i] & 0xFF);
    }
    data[buffer_length] = 0;
    string->data = data;
    string->length = buffer_length + 1;
    string->flags = PCSL_STRING_IN_HEAP;
    return PCSL_STRING_OK;
}

pcsl_string_status pcsl_string_convert_from_utf16(const jchar *buffer,
                                                   jsize buffer_length,
                                                   pcsl_string *string) {
    if (buffer == NULL || string == NULL) {
        return PCSL_STRING_EINVAL;
    }
    /* v01.55: upstream strips ALL trailing zeroes from the source
     * (pcsl_string.c:315 "Strip trailing zero characters") and then
     * appends exactly ONE terminating zero; ->length counts it. */
    while (buffer_length > 0 && buffer[buffer_length - 1] == 0) {
        buffer_length--;
    }
    jchar *data = (jchar *)malloc((buffer_length + 1) * sizeof(jchar));
    if (data == NULL) {
        return PCSL_STRING_ENOMEM;
    }
    memcpy(data, buffer, buffer_length * sizeof(jchar));
    data[buffer_length] = 0;
    string->data = data;
    string->length = buffer_length + 1;
    string->flags = PCSL_STRING_IN_HEAP;
    return PCSL_STRING_OK;
}

jboolean pcsl_string_equals(const pcsl_string *str1, const pcsl_string *str2) {
    if (str1 == str2) {
        return KNI_TRUE;
    }
    if (str1 == NULL || str2 == NULL) {
        return KNI_FALSE;
    }
    /* v01.55: compare the CONTENT (terminator excluded) - a literal
     * "A\0" (length 2) must equal a built "A\0" regardless of how
     * each side was constructed. */
    jsize len1 = pcsl_string_length(str1);
    jsize len2 = pcsl_string_length(str2);
    if (len1 != len2) {
        return KNI_FALSE;
    }
    if (len1 == 0) {
        return KNI_TRUE;
    }
    return memcmp(str1->data, str2->data, len1 * sizeof(jchar)) == 0
        ? KNI_TRUE : KNI_FALSE;
}

pcsl_string_status pcsl_string_compare(const pcsl_string *str1,
                                       const pcsl_string *str2,
                                       jint *comparison) {
    if (str1 == NULL || str2 == NULL || comparison == NULL) {
        return PCSL_STRING_EINVAL;
    }
    /* v01.55: content-only comparison, see pcsl_string_equals. */
    jsize len1 = pcsl_string_length(str1);
    jsize len2 = pcsl_string_length(str2);
    jsize min_len = (len1 < len2) ? len1 : len2;
    int cmp = 0;
    if (min_len > 0) {
        cmp = memcmp(str1->data, str2->data, min_len * sizeof(jchar));
    }
    if (cmp == 0) {
        cmp = (int)len1 - (int)len2;
    }
    *comparison = (cmp < 0) ? -1 : (cmp > 0) ? 1 : 0;
    return PCSL_STRING_OK;
}

pcsl_string_status pcsl_string_cat(const pcsl_string *str1,
                                   const pcsl_string *str2,
                                   pcsl_string *str) {
    if (str1 == NULL || str2 == NULL || str == NULL) {
        return PCSL_STRING_EINVAL;
    }
    /* v01.55 ROOT CAUSE FIX: upstream pcsl_string_cat (pcsl_string.c
     * :490) strips the terminating zero of the FIRST string before
     * concatenating ("Strip the terminating zero at the end of the
     * first string"), producing {content1 + content2 + '\0'} with
     * ->length counting that single final zero. Our old version
     * copied ->length jchars of BOTH inputs verbatim, so a literal
     * like the suite-id path component "FFFFFFFF\0" (length 9)
     * injected its embedded zero into the middle of every built path;
     * all later C-string consumers truncated at it and every record
     * store of the internal suite resolved to the same physical
     * file "…/rms/appdb_1A35/FFFFFFFF" (store name and .db extension
     * silently dropped - "save says OK, next launch finds nothing").
     */
    {
        jsize len1 = str1->data != NULL ? str1->length : 0;
        jsize len2 = str2->data != NULL ? str2->length : 0;
        /* Treat NULL data as the empty string. */
        if (len1 > 0 && str1->data[len1 - 1] == 0) {
            len1--; /* drop the terminator of the first part */
        }
        jsize total = len1 + len2;
        if (total == 0) {
            /* both parts empty: keep the canonical EMPTY constant */
            *str = PCSL_STRING_EMPTY;
            return PCSL_STRING_OK;
        }
        jchar *data = (jchar *)malloc(total * sizeof(jchar));
        if (data == NULL) {
            *str = PCSL_STRING_NULL;
            return PCSL_STRING_ENOMEM;
        }
        if (len1 > 0) {
            memcpy(data, str1->data, len1 * sizeof(jchar));
        }
        if (len2 > 0) {
            memcpy(data + len1, str2->data, len2 * sizeof(jchar));
        }
        /* str2 supplies the single terminating zero (it always ends
         * with one); if a caller passed a zero-less buffer, add it. */
        if (data[total - 1] != 0) {
            /* grow by one to append the terminator */
            jchar *grown = (jchar *)realloc(data, (total + 1) * sizeof(jchar));
            if (grown == NULL) {
                free(data);
                *str = PCSL_STRING_NULL;
                return PCSL_STRING_ENOMEM;
            }
            data = grown;
            data[total] = 0;
            total++;
        }
        str->data = data;
        str->length = total;
        str->flags = PCSL_STRING_IN_HEAP;
        return PCSL_STRING_OK;
    }
}

pcsl_string_status pcsl_string_dup(const pcsl_string *src, pcsl_string *dst) {
    if (src == NULL || dst == NULL) {
        return PCSL_STRING_EINVAL;
    }
    /* v01.55: upstream returns the SAME struct (not a heap copy) for
     * constants (EMPTY/NULL/literals - flags lack IN_HEAP), so
     * pcsl_string_free() on the duplicate never frees static data. */
    if (!(src->flags & PCSL_STRING_IN_HEAP)) {
        *dst = *src;
        return PCSL_STRING_OK;
    }
    return pcsl_string_cat(src, &PCSL_STRING_EMPTY, dst);
}

pcsl_string_status pcsl_string_append(pcsl_string *dst, const pcsl_string *src) {
    if (dst == NULL || src == NULL) {
        return PCSL_STRING_EINVAL;
    }
    /* v01.55: cat() now handles the terminator; the old manual copy
     * moved embedded zeroes into the middle of the result. */
    pcsl_string tmp;
    pcsl_string_status rc = pcsl_string_cat(dst, src, &tmp);
    if (rc != PCSL_STRING_OK) {
        return rc;
    }
    pcsl_string_free(dst);
    *dst = tmp;
    return PCSL_STRING_OK;
}

pcsl_string_status pcsl_string_append_char(pcsl_string *dst, const jchar newchar) {
    if (dst == NULL) {
        return PCSL_STRING_EINVAL;
    }
    /* v01.55: build a proper terminated part and go through cat(). */
    jchar part[2];
    part[0] = newchar;
    part[1] = 0;
    pcsl_string part_str;
    pcsl_string_status rc = pcsl_string_convert_from_utf16(part, 1, &part_str);
    if (rc != PCSL_STRING_OK) {
        return rc;
    }
    rc = pcsl_string_append(dst, &part_str);
    pcsl_string_free(&part_str);
    return rc;
}

pcsl_string_status pcsl_string_append_buf(pcsl_string *dst,
                                          const jchar *newtext,
                                          const jint textsize) {
    if (dst == NULL || newtext == NULL) {
        return PCSL_STRING_EINVAL;
    }
    /* v01.55: same route as upstream - convert_from_utf16 (which
     * strips trailing zeroes and terminates) then append. */
    pcsl_string part;
    pcsl_string_status rc =
        pcsl_string_convert_from_utf16(newtext, textsize, &part);
    if (rc != PCSL_STRING_OK) {
        return rc;
    }
    rc = pcsl_string_append(dst, &part);
    pcsl_string_free(&part);
    return rc;
}

void pcsl_string_predict_size(pcsl_string *str, jint size) {
    (void)str;
    (void)size;
}

pcsl_string_status pcsl_string_substring(const pcsl_string *str,
                                         jint begin_index, jint end_index,
                                         pcsl_string *dst) {
    if (str == NULL || dst == NULL) {
        return PCSL_STRING_EINVAL;
    }
    /* v01.55: end_index bounds against the CONTENT length (without
     * the terminating zero), matching upstream pcsl_string_length()
     * semantics; the produced ->length counts the new terminator. */
    jsize content_len = pcsl_string_length(str);
    if (begin_index < 0 || end_index > (jint)content_len ||
            begin_index > end_index) {
        return PCSL_STRING_EINVAL;
    }
    jsize len = end_index - begin_index;
    if (len == 0) {
        *dst = PCSL_STRING_EMPTY;
        return PCSL_STRING_OK;
    }
    jchar *data = (jchar *)malloc((len + 1) * sizeof(jchar));
    if (data == NULL) {
        return PCSL_STRING_ENOMEM;
    }
    memcpy(data, str->data + begin_index, len * sizeof(jchar));
    data[len] = 0;
    dst->data = data;
    /* v01.55: ->length counts the terminating zero (upstream). */
    dst->length = len + 1;
    dst->flags = PCSL_STRING_IN_HEAP;
    return PCSL_STRING_OK;
}

jboolean pcsl_string_starts_with(const pcsl_string *str, const pcsl_string *prefix) {
    if (str == NULL || prefix == NULL) {
        return KNI_FALSE;
    }
    /* v01.55: content lengths (terminator excluded). */
    jsize plen = pcsl_string_length(prefix);
    if (plen > pcsl_string_length(str)) {
        return KNI_FALSE;
    }
    if (plen == 0) {
        return KNI_TRUE;
    }
    return memcmp(str->data, prefix->data, plen * sizeof(jchar)) == 0
        ? KNI_TRUE : KNI_FALSE;
}

jboolean pcsl_string_ends_with(const pcsl_string *str, const pcsl_string *suffix) {
    if (str == NULL || suffix == NULL) {
        return KNI_FALSE;
    }
    /* v01.55: content lengths (terminator excluded). */
    jsize slen = pcsl_string_length(suffix);
    jsize tlen = pcsl_string_length(str);
    if (slen > tlen) {
        return KNI_FALSE;
    }
    if (slen == 0) {
        return KNI_TRUE;
    }
    return memcmp(str->data + (tlen - slen),
                  suffix->data, slen * sizeof(jchar)) == 0
        ? KNI_TRUE : KNI_FALSE;
}

jint pcsl_string_index_of(const pcsl_string *str, jint c) {
    if (str == NULL || str->data == NULL) {
        return -1;
    }
    /* v01.55: search the content only. */
    jsize i;
    jsize len = pcsl_string_length(str);
    for (i = 0; i < len; i++) {
        if (str->data[i] == (jchar)c) {
            return i;
        }
    }
    return -1;
}

jint pcsl_string_index_of_from(const pcsl_string *str, jint c, jint from_index) {
    if (str == NULL || str->data == NULL) {
        return -1;
    }
    /* v01.55: search the content only. */
    jsize i;
    jsize len = pcsl_string_length(str);
    for (i = from_index; i < len; i++) {
        if (str->data[i] == (jchar)c) {
            return i;
        }
    }
    return -1;
}

jint pcsl_string_last_index_of(const pcsl_string *str, jint c) {
    if (str == NULL || str->data == NULL) {
        return -1;
    }
    /* v01.55: search the content only (terminator excluded). */
    jint i;
    jint len = (jint)pcsl_string_length(str);
    for (i = len - 1; i >= 0; i--) {
        if (str->data[i] == (jchar)c) {
            return i;
        }
    }
    return -1;
}

jint pcsl_string_last_index_of_from(const pcsl_string *str, jint c, jint from_index) {
    if (str == NULL || str->data == NULL) {
        return -1;
    }
    /* v01.55: content length bounds the start. */
    jint len = (jint)pcsl_string_length(str);
    jint start = (from_index < len) ? from_index : len - 1;
    jint i;
    for (i = start; i >= 0; i--) {
        if (str->data[i] == (jchar)c) {
            return i;
        }
    }
    return -1;
}

pcsl_string_status pcsl_string_trim(const pcsl_string *str, pcsl_string *dst) {
    if (str == NULL || dst == NULL) {
        return PCSL_STRING_EINVAL;
    }
    return pcsl_string_dup(str, dst);
}

pcsl_string_status pcsl_string_trim_from_end(const pcsl_string *str, pcsl_string *dst) {
    if (str == NULL || dst == NULL) {
        return PCSL_STRING_EINVAL;
    }
    return pcsl_string_dup(str, dst);
}

pcsl_string_status pcsl_string_convert_to_jint(const pcsl_string *str, jint *value) {
    if (str == NULL || value == NULL || str->data == NULL) {
        return PCSL_STRING_EINVAL;
    }
    /* Simple ASCII atoi */
    jint result = 0;
    jsize i;
    /* v01.55: parse the content only (terminator excluded). */
    jsize len = pcsl_string_length(str);
    for (i = 0; i < len; i++) {
        if (str->data[i] >= '0' && str->data[i] <= '9') {
            result = result * 10 + (str->data[i] - '0');
        } else {
            return PCSL_STRING_EINVAL;
        }
    }
    *value = result;
    return PCSL_STRING_OK;
}

pcsl_string_status pcsl_string_convert_from_jint(jint value, pcsl_string *str) {
    if (str == NULL) {
        return PCSL_STRING_EINVAL;
    }
    jchar buf[16];
    int len = 0;
    if (value == 0) {
        buf[len++] = '0';
    } else {
        jint v = value;
        if (v < 0) {
            v = -v;
        }
        while (v > 0) {
            buf[len++] = '0' + (v % 10);
            v /= 10;
        }
        if (value < 0) {
            buf[len++] = '-';
        }
        /* Reverse */
        int i;
        for (i = 0; i < len / 2; i++) {
            jchar tmp = buf[i];
            buf[i] = buf[len - 1 - i];
            buf[len - 1 - i] = tmp;
        }
    }
    return pcsl_string_convert_from_utf16(buf, len, str);
}

pcsl_string_status pcsl_string_convert_to_jlong(const pcsl_string *str, jlong *value) {
    if (str == NULL || value == NULL) {
        return PCSL_STRING_EINVAL;
    }
    /* Stub: use jint conversion */
    jint iv;
    pcsl_string_status s = pcsl_string_convert_to_jint(str, &iv);
    if (s == PCSL_STRING_OK) {
        *value = (jlong)iv;
    }
    return s;
}

pcsl_string_status pcsl_string_convert_from_jlong(jlong value, pcsl_string *str) {
    return pcsl_string_convert_from_jint((jint)value, str);
}

pcsl_string_status pcsl_string_free(pcsl_string *str) {
    if (str == NULL) {
        return PCSL_STRING_EINVAL;
    }
    if (str->flags & PCSL_STRING_IN_HEAP) {
        free(str->data);
    }
    str->data = NULL;
    str->length = 0;
    str->flags = 0;
    return PCSL_STRING_OK;
}

const jbyte *pcsl_string_get_utf8_data(const pcsl_string *str) {
    if (str == NULL || str->data == NULL) {
        return NULL;
    }
    /* Upstream contract: fresh malloc'd buffer, released by the
     * paired pcsl_string_release_utf8_data call (which frees it).
     * The old stub returned a static buffer and the release stub
     * leaked - see midpGetJarEntry / InstallerCommandLine usage. */
    jsize len = pcsl_string_utf8_length(str);
    if (len < 0) {
        return NULL;
    }
    jbyte *buf = (jbyte *)malloc((size_t)len + 1);
    if (buf == NULL) {
        return NULL;
    }
    if (pcsl_string_convert_to_utf8(str, buf, len + 1, NULL) != PCSL_STRING_OK) {
        free(buf);
        return NULL;
    }
    return buf;
}

void pcsl_string_release_utf8_data(const jbyte *buf, const pcsl_string *str) {
    (void)str;
    /* Must free the buffer returned by pcsl_string_get_utf8_data
     * (upstream frees the pcsl_mem_malloc'd block here). */
    free((void *)buf);
}

const jchar *pcsl_string_get_utf16_data(const pcsl_string *str) {
    if (str == NULL) {
        return NULL;
    }
    return str->data;
}

void pcsl_string_release_utf16_data(const jchar *buf, const pcsl_string *str) {
    (void)buf;
    (void)str;
}

jboolean pcsl_string_is_null(const pcsl_string *str) {
    if (str == NULL) {
        return KNI_TRUE;
    }
    return (str->data == NULL) ? KNI_TRUE : KNI_FALSE;
}

/* ========================================================================
 * PCSL ESC (escaped strings) stubs
 * ======================================================================== */

pcsl_string_status pcsl_esc_init(void) {
    return PCSL_STRING_OK;
}

pcsl_string_status pcsl_esc_attach_string(const pcsl_string *str,
                                          pcsl_string *escaped) {
    if (str == NULL || escaped == NULL) {
        return PCSL_STRING_EINVAL;
    }
    return pcsl_string_dup(str, escaped);
}

pcsl_string_status pcsl_esc_extract_attached(const pcsl_string *escaped,
                                             pcsl_string *str) {
    if (escaped == NULL || str == NULL) {
        return PCSL_STRING_EINVAL;
    }
    return pcsl_string_dup(escaped, str);
}

/* ========================================================================
 * PCSL File stubs - Vita implementation
 * ======================================================================== */

/**
 * File handle structure for Vita
 */
typedef struct {
    int fd;  /* Vita file descriptor */
    char path[600];  /* Full resolved path, same size as abs_path buffers.
     * Was 256: deep UC cache paths got silently truncated by strncpy,
     * then pcsl_file_truncate's close+reopen opened a NONEXISTENT path,
     * left fd=-1, and every later seek/read on the still-live handle
     * returned EBADFD (0x80010051) - UC could no longer read its local
     * cache and fell back to full network reloads ("too slow"). */
    /* v01.47: lazy logical truncation. Vita has no ftruncate syscall
     * and Vita3K's dropped SCE_O_TRUNC makes the old remove+create
     * swap the ONLY way to physically shrink a file - but that swap
     * is impossible while a TWIN handle holds the name (empty-named
     * record stores share ONE path between db and idx handles), so
     * every RMS compact failed and left stale data past the new
     * logical size ("second launch stuck on init"). Instead of
     * swapping the file we just REMEMBER the new size here and clamp
     * reads/seeks/size reports to it. phoneME's RMS never relies on
     * the physical EOF (all bounds come from the db header and the
     * idx offset table), so a logical truncate is fully transparent.
     * -1 = no truncation pending (or the clamp was lifted by a write
     * that grew the file again). */
    long logical_size;
} VitaFileHandle;

/* Open-handle registry: POSIX allows unlinking an open file (the name
 * disappears, existing fds keep working until closed) and phoneME's
 * suite/RMS cleanup relies on it. Windows - and therefore Vita3K, whose
 * open_file() uses _wfopen without FILE_SHARE_DELETE - fails the delete
 * with ERROR_SHARING_VIOLATION (Error 32) whenever ANY handle in this
 * process still refers to the file, and then misreports it to the app
 * as ENOENT (0x80010002). To compensate, pcsl_file_unlink evicts all
 * same-path handles (sceIoClose + fd = -1, the POSIX "name is gone"
 * state) before retrying the remove. Registry slots are tiny pointers;
 * overflow just degrades eviction, never correctness of open/close. */
#define VITA_MAX_OPEN_FILES 64
static VitaFileHandle *g_open_handles[VITA_MAX_OPEN_FILES];

static void vita_handle_register(VitaFileHandle *vf) {
    int i;
    for (i = 0; i < VITA_MAX_OPEN_FILES; i++) {
        if (g_open_handles[i] == NULL) {
            g_open_handles[i] = vf;
            return;
        }
    }
}

static void vita_handle_unregister(VitaFileHandle *vf) {
    int i;
    for (i = 0; i < VITA_MAX_OPEN_FILES; i++) {
        if (g_open_handles[i] == vf) {
            g_open_handles[i] = NULL;
            return;
        }
    }
}

/* Close every open handle referring to abs_path (see registry comment).
 * Returns how many were evicted. Currently unused: v01.36's eviction
 * broke UC's second launch (see pcsl_file_unlink), but the mechanism is
 * kept for a future targeted fix (e.g. evict only during a real suite
 * uninstall, never while a MIDlet is running). */
__attribute__((unused))
static int vita_handles_evict(const char *abs_path) {
    int i, evicted = 0;
    for (i = 0; i < VITA_MAX_OPEN_FILES; i++) {
        VitaFileHandle *vf = g_open_handles[i];
        if (vf != NULL && vf->fd >= 0 && strcmp(vf->path, abs_path) == 0) {
            sceIoClose(vf->fd);
            vf->fd = -1; /* POSIX unlink semantics: name gone, fd dead */
            evicted++;
        }
    }
    return evicted;
}

/* True when at least one of OUR registered handles still refers to
 * abs_path with a live fd. pcsl_file_unlink uses this to SKIP the
 * sceIoRemove it already knows must fail: Windows/Vita3K cannot delete
 * a file with an in-process FILE* open (no FILE_SHARE_DELETE), and
 * attempting it only feeds the emulator's 3-line "Cannot remove file /
 * Error code: 32 / io_error_impl" complaint into vita3k.log. The
 * caller still gets an honest failure - v01.37 semantics are untouched
 * (the held file is LIVE suite data and must survive); only the
 * emulator-side log noise is eliminated (v01.43). */
static int vita_handles_held(const char *abs_path) {
    int i;
    for (i = 0; i < VITA_MAX_OPEN_FILES; i++) {
        VitaFileHandle *vf = g_open_handles[i];
        if (vf != NULL && vf->fd >= 0 && strcmp(vf->path, abs_path) == 0) {
            return 1;
        }
    }
    return 0;
}

/* v01.53: smallest pending logical clamp our own handles hold for
 * abs_path, or -1 when the path carries no clamp.
 *
 * pcsl_file_getusedspace() sums PHYSICAL st_size, but pcsl_file_truncate
 * only flips a flag in the handle, so a store whose physical file is
 * bloated (the v01.52 salvage wrote its whole stale tail as zeros, up
 * to whatever GB-sized offset a garbage block header claimed) keeps
 * reporting that bloat as USED space for as long as the handle lives.
 * storage_get_free_space() = totalSpace - usedSpace then answers 0,
 * every RecordStore space query returns 0 and the MIDlet reports "not
 * enough RMS space" - while the store itself is tiny.
 * Reading the clamp back here makes the two views agree. */
static long vita_logical_size_for(const char *abs_path) {
    int i;
    long best = -1;
    for (i = 0; i < VITA_MAX_OPEN_FILES; i++) {
        VitaFileHandle *vf = g_open_handles[i];
        if (vf == NULL || vf->fd < 0 || vf->logical_size < 0) {
            continue;
        }
        if (strcmp(vf->path, abs_path) != 0) {
            continue;
        }
        if (best < 0 || vf->logical_size < best) {
            best = vf->logical_size;
        }
    }
    return best;
}

/* v01.53: best-effort PHYSICAL shrink of abs_path to `size`.
 *
 * Vita has no sceIoTruncate of its own; both routes below end up in
 * sceIoChstat with the size field, so at least one of them works on a
 * platform that implements the file-size metadata operation at all.
 * The path may live in the read-only app0: VPK, which must never be
 * opened for write.
 * Every failure is silently tolerated: the caller's logical clamp
 * already keeps reads/size reports honest, so this only decides whether
 * the space also comes back to pcsl_file_getusedspace(). */
static int vita_physical_truncate(const char *abs_path, long size) {
    int fd;
    int rv;

    if (size < 0 || strncmp(abs_path, "app0:", 5) == 0) {
        return -1;
    }

    /* Path-based first: newlib's truncate() is a thin sceIoChstat
     * wrapper, so it needs no descriptor - which matters because our
     * handles come from raw sceIoOpen and ftruncate() only resolves
     * descriptors that went through newlib's own fd table. It also
     * avoids a second handle on a file that is already open. */
    if (truncate(abs_path, (off_t)size) == 0) {
        return 0;
    }

    /* Fallback: a newlib descriptor on the same path is one that
     * ftruncate() accepts. */
    fd = open(abs_path, O_WRONLY);
    if (fd < 0) {
        return -1;
    }
    rv = ftruncate(fd, (off_t)size);
    close(fd);
    return rv;
}

/* v01.45: held-by-ANOTHER-handle variant for the truncate/O_TRUNC
 * swap paths. pcsl_file_truncate legitimately holds its own handle
 * on the path it is about to swap - the question there is whether a
 * TWIN handle (e.g. the idx handle of an empty-named record store,
 * which shares the db's suffix-less path) would anchor the name
 * through the remove. Same registry walk, one exclusion.
 * v01.47: the swap itself is gone (lazy logical truncation), but the
 * helper is kept for possible future swap-style operations.
 * v01.53: pcsl_file_truncate uses it again, to decide whether a
 * physical shrink is safe (a twin handle sharing the path must not have
 * the file clipped underneath it). */
static int vita_handles_held_ex(const char *abs_path, VitaFileHandle *self) {
    int i;
    for (i = 0; i < VITA_MAX_OPEN_FILES; i++) {
        VitaFileHandle *vf = g_open_handles[i];
        if (vf != NULL && vf != self && vf->fd >= 0 &&
            strcmp(vf->path, abs_path) == 0) {
            return 1;
        }
    }
    return 0;
}

/* Relative-path resolution for raw sceIo* calls.
 * The VM class loader opens classpath jars via newlib stdio
 * (OsFile_vita.cpp -> jvm_fopen), which resolves relative paths
 * against the process cwd (= DATA_DIR after the chdir in
 * vita_main.c). Raw sceIo* calls do NOT honor the cwd - they need
 * an absolute device-prefixed path. JarReader (MIDlet MANIFEST
 * loading) reaches pcsl_file_open with the same relative jar path
 * used on the classpath; without the prefix sceIoOpen fails and
 * readJarEntry throws "JAR not found" (v01.23 regression hunt).
 * The root is queried at runtime with getcwd() so nothing is
 * hardcoded - it follows whatever vita_main.c chdir'd to. */
/* Forward declaration: used by pcsl_file_open's O_TRUNC emulation
 * (above) but defined next to the other unlink helpers (below). */
static void vita_report_held(const char *op, const char *path);

static void vita_resolve_path(const char *in, char *out, size_t outsz) {
    if (strchr(in, ':') != NULL || in[0] == '/') {
        /* already device-prefixed (ux0:, app0:...) or rooted */
        snprintf(out, outsz, "%s", in);
    } else {
        char root[256];
        if (getcwd(root, sizeof(root)) == NULL || root[0] == '\0') {
            /* fall back to the canonical data dir if cwd unavailable */
            snprintf(root, sizeof(root), "ux0:/data/J2ME00001");
        }
        snprintf(out, outsz, "%s/%s", root, in);
    }
}

int pcsl_file_init(void) {
    return 0;
}

int pcsl_file_finalize(void) {
    return 0;
}

/* Simple sceIo-based debug logging for Vita. DISABLED by default:
 * writing one line per pcsl_file_open (open+write+close on the host
 * side per call, plus a trace line per syscall in Vita3K) doubled
 * the syscall count of UC's init (hundreds of opens) and was a major
 * contributor to the "stuck on data init" slowness. Flip to 1 only
 * while actively debugging file-layer issues. */
#define VITA_FILE_DEBUG 0

#if VITA_FILE_DEBUG
static void debug_log_init(void) {}
static void debug_log(const char *msg) {
    static SceUID log_fd = -1;
    if (log_fd < 0) {
        log_fd = sceIoOpen("ux0:/data/debug_log.txt", 
            SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0777);
    }
    if (log_fd >= 0) {
        sceIoWrite(log_fd, msg, strlen(msg));
        sceIoClose(log_fd);
        log_fd = -1;  // Reset so next call reopens
    }
}
#endif

int pcsl_file_open(const pcsl_string *fileName, int flags, void **handle) {
    if (fileName == NULL || fileName->data == NULL || handle == NULL) {
        return -1;
    }

    VitaFileHandle *vf = (VitaFileHandle *)malloc(sizeof(VitaFileHandle));
    if (vf == NULL) {
        return -1;
    }

    /* Convert jchar path to UTF-8 for Vita IO */
    char path_utf8[512];
    jsize converted = 0;
    if (pcsl_string_convert_to_utf8(fileName, (jbyte *)path_utf8,
                                    sizeof(path_utf8), &converted)
        != PCSL_STRING_OK) {
        free(vf);
        return -1;
    }
    /* Resolve relative paths against the data root (see
     * vita_resolve_path comment): raw sceIo* ignores the cwd. */
    char abs_path[600];
    vita_resolve_path(path_utf8, abs_path, sizeof(abs_path));

    /* v01.55 diagnostic: one line per DISTINCT path+flags reaching the
     * file layer. The v01.54 log proved every named record store
     * (A/B/G2/P/CK/R/coreA) opened THE SAME 104-byte header and ran
     * the same salvage - either they all resolve to one physical file
     * (name lost in the path build) or every per-store file carries
     * identical damage. This breadcrumb settles which one it is, and
     * shows the exact file name each store maps to. rms/ paths only;
     * 8 dedup slots keep the log bounded. */
    if (strstr(abs_path, "/rms/") != NULL) {
        static char seen[8][600];
        static int seen_n = 0;
        int i;
        int dup = 0;
        for (i = 0; i < seen_n; i++) {
            if (strncmp(seen[i], abs_path, sizeof(seen[0]) - 1) == 0) {
                dup = 1;
                break;
            }
        }
        if (!dup) {
            SceIoStat probe;
            long sz = -1;
            if (sceIoGetstat(abs_path, &probe) >= 0) {
                sz = (long)probe.st_size;
            }
            if (seen_n < 8) {
                snprintf(seen[seen_n], sizeof(seen[0]), "%s", abs_path);
                seen_n++;
            }
            fprintf(stderr, "[pcsl] open path=%s flags=0x%x size=%ld\n",
                    abs_path, flags, sz);
        }
    }

    /* Convert PCSL flags to Vita/Unix flags - explicit mapping (bit values differ!) */
    int oflags = 0;
    switch (flags & 0x03) {  /* mask out access mode bits */
        case PCSL_FILE_O_RDONLY: oflags = SCE_O_RDONLY; break;
        case PCSL_FILE_O_WRONLY: oflags = SCE_O_WRONLY; break;
        case PCSL_FILE_O_RDWR:   oflags = SCE_O_RDWR;   break;
        default:                 oflags = SCE_O_RDONLY; break;
    }
    if (flags & PCSL_FILE_O_CREAT)  oflags |= SCE_O_CREAT;
    if (flags & PCSL_FILE_O_TRUNC)  oflags |= SCE_O_TRUNC;
    if (flags & PCSL_FILE_O_APPEND) oflags |= SCE_O_APPEND;

    /* DEBUG: log the open request */
#if VITA_FILE_DEBUG
    debug_log_init();
    char log_buf[1024];
    snprintf(log_buf, sizeof(log_buf), "[file_open] path='%s' flags=0x%x\n", path_utf8, oflags);
    debug_log(log_buf);
#endif

    /* Try to open the file using Vita IO */
    vf->fd = sceIoOpen(abs_path, oflags, 0777);
    vf->logical_size = -1; /* v01.47: no truncation pending */
    if (vf->fd >= 0 && (oflags & SCE_O_TRUNC)) {
        /* Vita3K's translate_open_mode() has no SCE_O_TRUNC branch -
         * every open maps to "rb+" and the flag is silently dropped,
         * so a pre-existing file keeps its old length (see
         * pcsl_file_truncate for the full story). v01.47: emulate the
         * truncation LOGICALLY instead of via the remove+create swap:
         * the swap cannot work while a twin handle holds the name
         * (empty-named record stores share ONE path between db and
         * idx handles), and a failed swap used to hand out a handle
         * whose contents contradict the requested O_TRUNC. A pending
         * logical_size == 0 gives the caller an empty file from the
         * first read/size query on, which is all O_TRUNC promises. */
        SceOff sz = sceIoLseek(vf->fd, 0, SCE_SEEK_END);
        if (sz > 0) {
            vf->logical_size = 0;
            sceIoLseek(vf->fd, 0, SCE_SEEK_SET);
        }
    }
    if (vf->fd < 0) {
        /* Not in the writable data dir - retry inside the VPK. The VPK
         * mirrors the data layout (data/J2ME00001/lib/*.png), so swap
         * the ux0:/data device prefix for app0:. Building the fallback
         * from the RAW path here produced "app0:ux0:/..." for absolute
         * inputs (always ENOENT) and a WRONG root for relative ones
         * once cwd != DATA_DIR. */
        char app0_path[600];
        if (strncmp(abs_path, "ux0:/data/", 10) == 0) {
            snprintf(app0_path, sizeof(app0_path), "app0:/%s",
                     abs_path + 5);
        } else {
            snprintf(app0_path, sizeof(app0_path), "app0:%s", abs_path);
        }
        vf->fd = sceIoOpen(app0_path, oflags, 0777);
        if (vf->fd < 0) {
#if VITA_FILE_DEBUG
            snprintf(log_buf, sizeof(log_buf), "[file_open] FAILED '%s' (0x%x)\n",
                     abs_path, (int)vf->fd);
            debug_log(log_buf);
#endif
            free(vf);
            return -1;
        }
#if VITA_FILE_DEBUG
        debug_log("[file_open] ok via app0 prefix\n");
#endif
        strncpy(vf->path, app0_path, sizeof(vf->path) - 1);
        vf->path[sizeof(vf->path) - 1] = '\0';
    } else {
#if VITA_FILE_DEBUG
        debug_log("[file_open] ok direct\n");
#endif
        strncpy(vf->path, abs_path, sizeof(vf->path) - 1);
        vf->path[sizeof(vf->path) - 1] = '\0';
    }

    vita_handle_register(vf);
    *handle = vf;
    return 0;
}

int pcsl_file_close(void *handle) {
    if (handle == NULL) {
        return 0;
    }
    
    VitaFileHandle *vf = (VitaFileHandle *)handle;
    long pending = vf->logical_size;
    vita_handle_unregister(vf);
    if (vf->fd >= 0) {
        sceIoClose(vf->fd);
        vf->fd = -1;
    }
    /* v01.53: make a pending logical truncation durable. Once this
     * handle is gone the clamp is gone with it (no one remembers the
     * logical size any more), so a physical tail would start counting
     * as used space again on the next open. Best effort: the truncate
     * may fail on a platform that cannot chstat-by-fd, and a twin
     * handle still using the path must not have the file clipped
     * underneath it. */
    if (pending >= 0 && !vita_handles_held(vf->path)) {
        vita_physical_truncate(vf->path, pending);
    }
    free(vf);
    return 0;
}

int pcsl_file_read(void *handle, unsigned char *buf, long size) {
    if (handle == NULL || buf == NULL || size <= 0) {
        return -1;
    }
    
    VitaFileHandle *vf = (VitaFileHandle *)handle;
    if (vf->fd < 0) {
        return -1; /* dead handle - fail fast (see pcsl_file_seek) */
    }
    {
        /* v01.47: honor a pending logical truncation. The caller sees
         * EOF at the logical size even though the physical file still
         * carries the stale tail. */
        long pos = (long)sceIoLseek(vf->fd, 0, SCE_SEEK_CUR);
        if (vf->logical_size >= 0 && pos >= vf->logical_size) {
            return 0; /* EOF at the logical size */
        }
    }
    return sceIoRead(vf->fd, buf, size);
}

int pcsl_file_write(void *handle, unsigned char *buf, long size) {
    if (handle == NULL || buf == NULL || size <= 0) {
        return -1;
    }
    
    VitaFileHandle *vf = (VitaFileHandle *)handle;
    if (vf->fd < 0) {
        return -1; /* dead handle - fail fast (see pcsl_file_seek) */
    }
    {
        int n = (int)sceIoWrite(vf->fd, buf, size);
        /* v01.47: a write past the logical size means the file is
         * growing again (RMS record append after a compact); the
         * clamp no longer matches reality, lift it. -1 = no clamp. */
        if (n > 0 && vf->logical_size >= 0) {
            long end = (long)sceIoLseek(vf->fd, 0, SCE_SEEK_CUR);
            if (end > vf->logical_size) {
                vf->logical_size = -1;
            }
        }
        return n;
    }
}

/* Degraded-noise helper for unlink: Vita3K maps SCE errno values onto
 * the wrong Windows errno table when it logs io_error_impl, so a plain
 * ENOENT (0x80010002 = 0x80010000 | 2, "file does not exist") shows up
 * as "Error code: 32 (another program is using this file)". That is a
 * misreport, not a real sharing violation. midp_remove_suite's cleanup
 * loop unlinks every enumerated entry unconditionally, so a target that
 * vanished (or that is only a filename prefix of a store) legitimately
 * returns ENOENT; deleting an already-gone file has achieved its goal.
 * Treat that as success so storage_delete_file does not print noise, but
 * without hardcoding the code: probe with sceIoGetstat - if the path no
 * longer exists the unlink is considered done. */
static void vita_report_held(const char *op, const char *path) {
    /* v01.45: dedup key is op+path, not path alone. Path-only dedup
     * swallowed the 23:55 "truncate blocked" for a path already
     * reported as "unlink blocked" at startup, and the stderr trail
     * could no longer be matched against Vita3K's remove_file lines. */
    static char seen[8][176];
    static int seen_n = 0;
    int i;
    for (i = 0; i < seen_n; i++) {
        if (strncmp(seen[i], op, 15) == 0 &&
            strncmp(seen[i] + 16, path, sizeof(seen[0]) - 17) == 0) {
            return;
        }
    }
    if (seen_n < 8) {
        snprintf(seen[seen_n], sizeof(seen[0]), "%-15s %s", op, path);
        seen_n++;
    }
    fprintf(stderr, "[pcsl] %s blocked, file open elsewhere: %s\n",
            op, path);
}

static int vita_unlink_enoent_is_ok(const char *abs_path) {
    SceIoStat st;
    if (sceIoGetstat(abs_path, &st) >= 0) {
        return 0; /* still there: the earlier remove really failed */
    }
    /* Getstat also failed with ENOENT - nothing to delete. */
    return 1;
}

int pcsl_file_unlink(const pcsl_string *fileName) {
    if (fileName == NULL || fileName->data == NULL) {
        return -1;
    }
    
    /* Convert jchar path to UTF-8 */
    char path_utf8[512];
    if (pcsl_string_convert_to_utf8(fileName, (jbyte *)path_utf8,
                                    sizeof(path_utf8), NULL) != PCSL_STRING_OK) {
        return -1;
    }
    char abs_path[600];
    vita_resolve_path(path_utf8, abs_path, sizeof(abs_path));

    /* v01.43: registry pre-check BEFORE the syscall. A live same-path
     * handle means the file is open right here in this process - under
     * Windows/Vita3K semantics the remove below can only fail with a
     * sharing violation (Vita3K logs it as the scary "Error code: 32"
     * triplet). That delete is never legitimate here: the v01.36 lesson
     * proved the held FFFFFFFF files are the RUNNING suite's live RMS
     * data and must survive. So skip the doomed syscall entirely and
     * report the failure once via the breadcrumb - same return value
     * the caller always saw, but Vita3K's log stays clean. */
    if (vita_handles_held(abs_path)) {
        vita_report_held("unlink", abs_path);
        return -1;
    }

    int result = sceIoRemove(abs_path);
    if (result < 0) {
        if (vita_unlink_enoent_is_ok(abs_path)) {
            /* The target was already gone. Treat delete-of-a-gone-file
             * as success. */
            return 0;
        }
        vita_report_held("unlink", abs_path);
        /* The file still exists and the remove failed: ERROR_SHARING_VIOLATION
         * under Vita3K (it returns 0x80010002 for this too). v01.36 used to
         * evict same-path handles and retry here, which made the remove
         * succeed - but the eviction also killed fds still held by Java
         * code, so UC's RMS data came back half-written and the second
         * launch broke (NPE + 1 fps). The sharing violation is actually
         * PROTECTING live suite data from midp_remove_suite's cleanup
         * loop: report failure and let the file survive. The log lines
         * Vita3K prints for the failed syscall are cosmetic.
         * Also: do NOT fall back to the app0 copy here - the ux0 file
         * exists (stat succeeded above), so an app0 attempt can only
         * fail again and double the log noise. */
        return result;
    }
    return result;
}

/* Vita has no sceIoFtruncate syscall, so shrink by read-rewrite.
 *
 * v01.41 root-cause rewrite. The v01.40 close->reopen(O_TRUNC)
 * emulation NEVER actually truncated under Vita3K: the emulator's
 * translate_open_mode() (io/src/filesystem.cpp) has NO SCE_O_TRUNC
 * branch - every open lands in "rb+". So the reopen kept the old
 * length, writing the saved prefix back just overwrote the first
 * `size` bytes, and the stale tail stayed forever. RMS compact
 * then reported success while garbage record headers survived
 * past the new logical size ("second launch stuck on init"),
 * and pcsl_file_sizeofopenfile kept reporting the OLD size.
 * On real hardware O_TRUNC works, so this is an emulator-only
 * silent data corruption the v01.40 log analysis could not see.
 *
 * The ONLY sequence that truncates on BOTH Vita3K and a real
 * device is remove + create + write-back:
 *   - remove frees the name (works even while our fd is open on
 *     Vita3K only if the FILE* is closed first - so we close),
 *   - create gives a guaranteed 0-byte file,
 *   - write-back restores the surviving prefix.
 *
 * Failure safety: the prefix bytes live in memory during the
 * swap, so a failure at any step can still restore them (best
 * effort) - the same window the old implementation had.
 *
 * A previous stub returned success without doing anything, so
 * RecordStoreImpl.compactRecords left stale blocks past the new
 * logical size - the second launch then walked garbage record
 * headers ("first launch OK, every launch after broken"). */
/* v01.47: LAZY LOGICAL TRUNCATION. Vita has no ftruncate syscall, and
 * Vita3K silently drops SCE_O_TRUNC (translate_open_mode has no branch
 * for it), so the only physical way to shrink a file was the
 * remove+create swap. That swap CANNOT work while a twin handle holds
 * the name: an empty-named record store gets no .db/.idx suffix, so
 * its db AND idx handles share ONE path and the surviving handle keeps
 * the name anchored - the remove fails (Vita3K Error 32) and, worse,
 * the create-open then lands on the OLD non-empty file ("rb+" never
 * truncates) leaving stale record blocks past the new logical size.
 * The next launch walks that garbage ("first launch OK, every launch
 * after broken/stuck on init").
 *
 * phoneME's RMS never reads past what the db header (RS6_DATA_SIZE)
 * and the idx offset table authorize, so the physical tail is dead
 * weight only: we just remember the new size in the handle and clamp
 * reads/seeks/size reports to it (see VitaFileHandle.logical_size).
 * No syscall can fail, compact succeeds, no stale tail is ever
 * exposed.
 * v01.53: the clamp alone turned out to be too weak - the physical
 * bytes are what pcsl_file_getusedspace() counts, and v01.52's salvage
 * could leave a GB-sized zero tail behind, so "used space" stayed
 * above the suite budget forever and the MIDlet reported "not enough
 * RMS space". The clamp is now only the FLOOR of the fix: after it is
 * recorded we also try a real shrink (vita_physical_truncate), and if
 * that is impossible (twin handle, no platform support, app0:) the
 * stale tail is at least excluded from the used-space sum by
 * vita_logical_size_for(). */
int pcsl_file_truncate(void *handle, long size) {
    if (handle == NULL || size < 0) {
        return -1;
    }

    VitaFileHandle *vf = (VitaFileHandle *)handle;
    if (vf->fd < 0) {
        return -1;
    }

    {
        /* NOTE: capture the caller's position BEFORE probing the size.
         * (Seeking to SEEK_END first made the old "restore cur_pos"
         * a seek back to EOF, i.e. no restore at all.) */
        SceOff cur_pos = sceIoLseek(vf->fd, 0, SCE_SEEK_CUR);
        SceOff file_size = sceIoLseek(vf->fd, 0, SCE_SEEK_END);
        if (file_size < 0) {
            return -1;
        }

        if ((long)file_size <= size && vf->logical_size < 0) {
            /* Growing (or unchanged) physical file and no pending
             * clamp: RMS never does this; nothing to do. */
            if (cur_pos >= 0) {
                sceIoLseek(vf->fd, cur_pos, SCE_SEEK_SET);
            }
            return 0;
        }

        /* Record the clamp. If a previous clamp exists, the smaller
         * value wins (two shrinks in a row must not un-shrink). The
         * effective target is that same minimum, so the physical
         * shrink below can never land ABOVE what callers are shown. */
        if (vf->logical_size < 0 || size < vf->logical_size) {
            vf->logical_size = size;
        } else {
            size = vf->logical_size;
        }

        /* v01.53: now that the caller's view is consistent, try to make
         * the FILE consistent too. Only a physical shrink returns the
         * space: pcsl_file_getusedspace() sums real st_size, and
         * storage_get_free_space() = totalSpace - usedSpace, so a store
         * whose physical file stayed bloated (the v01.52 salvage wrote
         * the whole stale tail as zeros before truncating) keeps the
         * MIDlet's free-space figure at 0 and every RecordStore then
         * reports "not enough RMS space" although the store itself is
         * tiny. Skipped when a TWIN handle shares this exact path
         * (empty-named record stores put db and idx on one path),
         * because shrinking under its feet could clip data it still
         * believes in - the logical clamp covers that case. */
        if (!vita_handles_held_ex(vf->path, vf) &&
            vita_physical_truncate(vf->path, size) == 0) {
            SceOff after = sceIoLseek(vf->fd, 0, SCE_SEEK_END);
            if (after >= 0 && (long)after <= size) {
                vf->logical_size = -1; /* file and clamp agree again */
            }
        }

        if (cur_pos >= 0) {
            sceIoLseek(vf->fd, cur_pos, SCE_SEEK_SET);
        }
        return 0;
    }
}

int pcsl_file_exist(const pcsl_string *fileName) {
    if (fileName == NULL || fileName->data == NULL) {
        return 0;
    }
    
    /* Convert jchar path to UTF-8 */
    char path_utf8[512];
    if (pcsl_string_convert_to_utf8(fileName, (jbyte *)path_utf8,
                                    sizeof(path_utf8), NULL) != PCSL_STRING_OK) {
        return 0;
    }
    char abs_path[600];
    vita_resolve_path(path_utf8, abs_path, sizeof(abs_path));
    
    /* VPK fallback: swap the ux0:/data prefix for app0: */
    char app0_path[600];
    if (strncmp(abs_path, "ux0:/data/", 10) == 0) {
        snprintf(app0_path, sizeof(app0_path), "app0:/%s", abs_path + 5);
    } else {
        snprintf(app0_path, sizeof(app0_path), "app0:%s", abs_path);
    }
    
    SceIoStat stat;
    if (sceIoGetstat(abs_path, &stat) >= 0) {
        return 1;
    }
    
    if (sceIoGetstat(app0_path, &stat) >= 0) {
        return 1;
    }
    
    return 0;
}

int pcsl_file_commitwrite(void *handle) {
    (void)handle;
    return 0;
}

/* Real rename via sceIoRename. The old stub always failed, breaking
 * every write_file() commit in the suite store (temp file + rename
 * pattern, suitestore_intern.c) - data stayed in the .tmp file and
 * the real file was never created/updated. */
int pcsl_file_rename(const pcsl_string *oldName, const pcsl_string *newName) {
    if (oldName == NULL || oldName->data == NULL ||
        newName == NULL || newName->data == NULL) {
        return -1;
    }

    char old_utf8[512], new_utf8[512];
    if (pcsl_string_convert_to_utf8(oldName, (jbyte *)old_utf8,
                                    sizeof(old_utf8), NULL) != PCSL_STRING_OK ||
        pcsl_string_convert_to_utf8(newName, (jbyte *)new_utf8,
                                    sizeof(new_utf8), NULL) != PCSL_STRING_OK) {
        return -1;
    }

    /* Raw sceIo* needs absolute device-prefixed paths (see
     * vita_resolve_path): both names arrive as storage-root
     * relative paths from midpStorage. */
    char old_abs[600], new_abs[600];
    vita_resolve_path(old_utf8, old_abs, sizeof(old_abs));
    vita_resolve_path(new_utf8, new_abs, sizeof(new_abs));

    int rv = sceIoRename(old_abs, new_abs);
    return (rv < 0) ? -1 : 0;
}

/* ======================================================================
 * Directory enumeration (pcsl_file_openfilelist/getnextentry/closefilelist)
 *
 * v01.34: these were NULL/-1 stubs since the first port. Every consumer
 * that walks a directory silently saw "no entries":
 *   - RecordStore.listRecordStores() -> getNumberOfStores -> NULL handle
 *     -> OUT_OF_MEM_LEN -> OutOfMemoryError thrown FROM NATIVE, aborting
 *     UC's startup state restore (ao.a() NPE one frame up). First launch
 *     worked because creates go through pcsl_file_open/exist (real), but
 *     any second launch that first LISTS the stores died - "delete the
 *     appdb dir and it works again" symptom.
 *   - midp_remove_suite's file cleanup loop also iterates this way, so
 *     suite removal left files behind.
 * Contract copied from the reference POSIX port (pcsl/file/posix): the
 * input string is "rootdir + match-prefix". We split at the last file
 * separator, opendir the root, and on each getnextentry() return the
 * next readdir() entry whose name starts with the match prefix, as a
 * FULL path (root + entry name) in *result (caller frees).
 * ====================================================================== */

typedef struct VitaDirIter {
    int   rootLength;   /* in jchars, includes the trailing '/'  */
    int   matchLength;  /* in jchars, prefix after the separator */
    SceUID dfd;         /* sceIoDopen handle, < 0 when exhausted */
} VitaDirIter;

void* pcsl_file_openfilelist(const pcsl_string *string) {
    VitaDirIter *it;
    char path_utf8[600];
    SceUID dfd;
    int filelistLen, rootLength;
    jchar sep;

    if (string == NULL || string->data == NULL) {
        return NULL;
    }

    /* Resolve the directory part against the data root, the same way
     * pcsl_file_open does (relative storage paths like "appdb_1A35/"
     * arrive here from midpStorage). */
    if (pcsl_string_convert_to_utf8(string, (jbyte *)path_utf8,
                                    sizeof(path_utf8), NULL)
        != PCSL_STRING_OK) {
        return NULL;
    }

    filelistLen = pcsl_string_length(string);
    rootLength = (int)pcsl_string_last_index_of(
        string, (jint)pcsl_file_getfileseparator());
    if (rootLength < 0) {
        rootLength = 0; /* no separator: everything is a match prefix */
    } else {
        rootLength++;   /* include the separator in the root */
    }

    /* Trim the match prefix off the directory we open. The separator
     * scan above is in jchars; do the same on the UTF-8 copy. */
    {
        char *slash = strrchr(path_utf8, '/');
        if (rootLength > 0 && slash != NULL) {
            /* keep rootLength-1 jchars == bytes up to the slash */
            *(slash + 1) = '\0';
        }
    }

    sep = pcsl_file_getfileseparator();
    (void)sep;

    {
        char abs_dir[600];
        vita_resolve_path(path_utf8, abs_dir, sizeof(abs_dir));
        dfd = sceIoDopen(abs_dir);
    }
    if (dfd < 0) {
        return NULL;
    }

    it = (VitaDirIter *)malloc(sizeof(VitaDirIter));
    if (it == NULL) {
        sceIoDclose(dfd);
        return NULL;
    }
    it->rootLength = rootLength;
    it->matchLength = filelistLen - rootLength;
    it->dfd = dfd;
    return it;
}

int pcsl_file_closefilelist(void *handle) {
    VitaDirIter *it = (VitaDirIter *)handle;
    if (it == NULL) {
        return -1;
    }
    if (it->dfd >= 0) {
        sceIoDclose(it->dfd);
    }
    free(it);
    return 0;
}

int pcsl_file_getnextentry(void *handle, const pcsl_string *string,
                           pcsl_string *result) {
    VitaDirIter *it = (VitaDirIter *)handle;
    SceIoDirent entry;
    pcsl_string matchName = PCSL_STRING_NULL;
    pcsl_string rootpath = PCSL_STRING_NULL;
    pcsl_string returnVal = PCSL_STRING_NULL;
    char match_utf8[256];
    int matchLen = 0;
    int rv = -1;

    if (it == NULL || it->dfd < 0) {
        return -1;
    }

    /* Match prefix = string[rootLength .. rootLength+matchLength) */
    if (it->matchLength > 0) {
        if (pcsl_string_substring(string, it->rootLength,
                                  it->rootLength + it->matchLength,
                                  &matchName) != PCSL_STRING_OK) {
            return -1;
        }
        if (pcsl_string_convert_to_utf8(&matchName, (jbyte *)match_utf8,
                                        sizeof(match_utf8), NULL)
            == PCSL_STRING_OK) {
            matchLen = strlen(match_utf8);
        }
        pcsl_string_free(&matchName);
    }

    while (sceIoDread(it->dfd, &entry) > 0) {
        const char *name = entry.d_name;

        if (name[0] == '\0' || strcmp(name, ".") == 0 ||
            strcmp(name, "..") == 0) {
            continue;
        }
        if (matchLen > 0 && strncmp(name, match_utf8, matchLen) != 0) {
            continue;
        }
        /* v01.44: never return subdirectories. The upstream POSIX port
         * relies on readdir()+stat; FFFFFFFF (suite id) directories
         * under the appdb root matched the caller's match prefix, got
         * handed back as "files", and midp_remove_suite's cleanup loop
         * then passed them to pcsl_file_unlink. Removing a directory
         * that an open RMS handle anchors is impossible on Windows
         * (Vita3K logs it as the "Error code: 32" triplet) and pointless
         * on a real device: the per-file iteration in
         * rmsdb_remove_record_stores_for_suite + the storage iterator
         * already delete every file inside first. Skip dirs so the
         * cleanup loop only ever sees real files. */
        if (entry.d_stat.st_attr & SCE_SO_IFDIR) {
            continue;
        }

        /* Found one: result = string[0 .. rootLength) + name */
        if (pcsl_string_substring(string, 0, it->rootLength, &rootpath)
                != PCSL_STRING_OK ||
            pcsl_string_convert_from_utf8((const jbyte *)name,
                                          (jsize)strlen(name),
                                          &returnVal) != PCSL_STRING_OK ||
            pcsl_string_cat(&rootpath, &returnVal, result)
                != PCSL_STRING_OK) {
            break;
        }
        rv = 0;
        break;
    }

    pcsl_string_free(&returnVal);
    pcsl_string_free(&rootpath);
    return rv;
}

long pcsl_file_seek(void *handle, long offset, long position) {
    if (handle == NULL) {
        return -1;
    }
    
    VitaFileHandle *vf = (VitaFileHandle *)handle;
    int whence;
    
    switch (position) {
        case PCSL_FILE_SEEK_SET: whence = SCE_SEEK_SET; break;
        case PCSL_FILE_SEEK_CUR: whence = SCE_SEEK_CUR; break;
        case PCSL_FILE_SEEK_END: whence = SCE_SEEK_END; break;
        default: return -1;
    }

    /* A dead handle (fd closed by a failed truncate/open recovery)
     * would turn every seek into sceIoLseek(-1) = EBADFD
     * (0x80010051) noise. Fail fast and leave one breadcrumb in the
     * boot log so a residual seek-after-death is locatable without
     * re-enabling the (very slow) full file trace. */
    if (vf->fd < 0) {
        static int reported = 0;
        if (!reported) {
            reported = 1;
            fprintf(stderr, "[pcsl] seek on dead handle '%s'\n", vf->path);
        }
        return -1;
    }

    /* VITA FIX (v01.49): a negative offset reaching sceIoLseek is
     * always garbage (corrupt RMS block arithmetic upstream); Vita3K
     * on the Windows host reports it as 0x80010051 for EVERY such
     * seek, and real hardware may do worse. Reject it here with the
     * same -1 contract storagePosition already handles, leaving one
     * breadcrumb with the offending offset so the caller is
     * identifiable from the logs. */
    if (offset < 0) {
        static long reported_offset = -1;
        if (reported_offset != offset) {
            reported_offset = offset;
            fprintf(stderr,
                    "[pcsl] seek with negative offset %ld on '%s'\n",
                    offset, vf->path);
        }
        return -1;
    }

    {
        /* v01.51: do NOT clamp the reported position here. The pending
         * logical_size is a truncation request, and the fd really is at
         * the requested offset (sceIoLseek already moved it), so
         * reporting the clamped value made storagePosition() / the file
         * cache believe the file ended at logical_size while the next
         * write went to the un-clamped offset - a silent split between
         * the position the caller thinks it has and the position the
         * handle is at. Hiding a stale tail is the job of the read clamp
         * and pcsl_file_sizeofopenfile(), which still do it. */
        return (long)sceIoLseek(vf->fd, offset, whence);
    }
}

long pcsl_file_sizeofopenfile(void *handle) {
    if (handle == NULL) {
        return -1;
    }
    
    VitaFileHandle *vf = (VitaFileHandle *)handle;
    if (vf->fd < 0) {
        return -1; /* dead handle - fail fast (see pcsl_file_seek) */
    }
    {
        SceOff current = sceIoLseek(vf->fd, 0, SCE_SEEK_CUR);
        SceOff size = sceIoLseek(vf->fd, 0, SCE_SEEK_END);
        sceIoLseek(vf->fd, current, SCE_SEEK_SET);
        /* v01.47: report the logical size while a clamp is pending. */
        if (vf->logical_size >= 0 && (long)size > vf->logical_size) {
            return vf->logical_size;
        }
        return (long)size;
    }
}

long pcsl_file_sizeof(const pcsl_string *fileName) {
    if (fileName == NULL || fileName->data == NULL) {
        return -1;
    }
    
    /* Convert jchar path to UTF-8 */
    char path_utf8[512];
    if (pcsl_string_convert_to_utf8(fileName, (jbyte *)path_utf8,
                                    sizeof(path_utf8), NULL) != PCSL_STRING_OK) {
        return -1;
    }
    char abs_path[600];
    vita_resolve_path(path_utf8, abs_path, sizeof(abs_path));
    
    /* VPK fallback: swap the ux0:/data prefix for app0: */
    char app0_path[600];
    if (strncmp(abs_path, "ux0:/data/", 10) == 0) {
        snprintf(app0_path, sizeof(app0_path), "app0:/%s", abs_path + 5);
    } else {
        snprintf(app0_path, sizeof(app0_path), "app0:%s", abs_path);
    }
    
    SceIoStat stat;
    if (sceIoGetstat(abs_path, &stat) >= 0) {
        return (long)stat.st_size;
    }
    
    if (sceIoGetstat(app0_path, &stat) >= 0) {
        return (long)stat.st_size;
    }
    
    return -1;
}

long pcsl_file_getusedspace(const pcsl_string *dirName) {
    /* v01.34: was "return 0". The real consumer chain is
     * storage_get_free_space() = totalSpace - usedSpace, where
     * totalSpace comes from the system.jam_space property
     * (config/internal.config, bytes). Used to matter only for the
     * RecordStore space checks; with used=0 and the default 4MB
     * total they "worked" by accident. Now honestly sums the regular
     * files directly inside the given storage root (one level, like
     * the POSIX reference: "does not consider files in
     * subdirectories"). */
    char path_utf8[600];
    char abs_dir[620];
    SceUID dfd;
    SceIoDirent entry;
    long total = 0;

    if (dirName == NULL || dirName->data == NULL) {
        return -1;
    }
    if (pcsl_string_convert_to_utf8(dirName, (jbyte *)path_utf8,
                                    sizeof(path_utf8), NULL)
        != PCSL_STRING_OK) {
        return -1;
    }
    vita_resolve_path(path_utf8, abs_dir, sizeof(abs_dir));

    dfd = sceIoDopen(abs_dir);
    if (dfd < 0) {
        return -1;
    }

    while (sceIoDread(dfd, &entry) > 0) {
        if (entry.d_stat.st_attr & SCE_SO_IFDIR) {
            continue; /* directories do not count */
        }
        /* v01.53: prefer the LOGICAL size when one of our handles holds
         * a pending truncation for this exact entry. A file whose
         * physical tail is stale (see pcsl_file_truncate) is logically
         * smaller than st_size, and reporting the physical number makes
         * storage_get_free_space() answer 0 for the whole suite - which
         * is exactly the "not enough RMS space" the MIDlet shows. */
        {
            char entry_path[900];
            long physical = (long)entry.d_stat.st_size;
            long logical;
            snprintf(entry_path, sizeof(entry_path), "%s/%s",
                     abs_dir, entry.d_name);
            logical = vita_logical_size_for(entry_path);
            total += (logical >= 0 && logical < physical) ? logical
                                                          : physical;
        }
    }
    sceIoDclose(dfd);

    /* v01.53: one-shot breadcrumb when the used-space figure is big
     * enough to zero out a suite's free space on its own. The MIDlet's
     * "not enough space" only ever comes from this number, and after
     * the v01.52 salvage incident a single leftover store could carry
     * a multi-MB/Gb physical tail - so name the culprit directory and
     * its (possibly inflated) total instead of leaving the next
     * investigation to guess again. */
    if (total > 1024 * 1024) {
        static char reported[4][620];
        static int reported_n = 0;
        int i, seen = 0;
        for (i = 0; i < reported_n; i++) {
            if (strcmp(reported[i], abs_dir) == 0) {
                seen = 1;
                break;
            }
        }
        if (!seen && reported_n < 4) {
            snprintf(reported[reported_n], sizeof(reported[0]), "%s",
                     abs_dir);
            reported_n++;
            fprintf(stderr, "[pcsl] getusedspace %s = %ld bytes\n",
                    abs_dir, total);
        }
    }

    return total;
}

/* Checks the size of free space on the storage device. The pcsl_file.h
 * declaration takes no parameters (upstream marks it for removal); the
 * storage root is fixed on this port, so query ux0: directly.
 * sceAppMgrGetDevInfo is the only documented way to get partition
 * sizes from user mode (SceAppMgr_stub is already linked). */
long pcsl_file_getfreespace(void) {
    uint64_t max_size = 0;
    uint64_t free_size = 0;

    if (sceAppMgrGetDevInfo("ux0:", &max_size, &free_size) < 0) {
        return 0;
    }
    /* long is 32-bit on ARM; a multi-GB free byte count overflows, but
     * every caller (RMS space checks) only compares against small
     * budgets, so saturate at LONG_MAX instead of wrapping negative. */
    if (free_size > (uint64_t)LONG_MAX) {
        return LONG_MAX;
    }
    return (long)free_size;
}

jchar pcsl_file_getfileseparator(void) {
    return '/';
}

jchar pcsl_file_getpathseparator(void) {
    return ':';
}

/* ========================================================================
 * PCSL directory / attribute service (pcsl_directory.h)
 *
 * These are the natives JSR 75 (javax.microedition.io.file) is built on:
 * "is this a directory", "create/remove a directory", "how much space is
 * left", "read/write attributes", "last modified". Until now NOTHING
 * defined them on Vita (pcsl_file.h's 21 entry points were complete, this
 * header's 8 were not) - which is exactly why an "enable USE_JSR_75"
 * build would fail to link, and why directory listing from Java was
 * impossible (MIDP itself never enumerates).
 *
 * Everything below works on the same conventions as the pcsl_file_*
 * family above: pcsl_string -> UTF-8 -> vita_resolve_path() (relative
 * names land under the data root, device-prefixed names pass through)
 * -> sceIo*, with the app0: (VPK) fallback for read paths.
 * ======================================================================== */

/* pcsl_string path -> absolute C path, trailing separators stripped.
 * sceIoGetstat("ux0:/dir/") fails on a trailing slash, but JSR 75
 * FileConnection URIs keep it (it is how a directory is addressed), so
 * normalize here instead of at every call site. */
static int vita_dir_path(const pcsl_string *s, char *out, size_t outsz) {
    if (s == NULL || s->data == NULL) {
        return -1;
    }

    char utf8[512];
    if (pcsl_string_convert_to_utf8(s, (jbyte *)utf8, sizeof(utf8), NULL)
            != PCSL_STRING_OK) {
        return -1;
    }

    size_t len = strlen(utf8);
    while (len > 1 && (utf8[len - 1] == '/' || utf8[len - 1] == '\\')) {
        utf8[--len] = '\0';
    }
    if (len == 0) {
        return -1;  /* the bare current directory is not addressable */
    }

    vita_resolve_path(utf8, out, outsz);
    return 0;
}

/* sceIoGetstat with the same app0: (VPK) fallback pcsl_file_exist uses:
 * system files may live inside the VPK while user files live on ux0:. */
static int vita_stat_any(const char *abs_path, SceIoStat *st) {
    if (sceIoGetstat(abs_path, st) >= 0) {
        return 0;
    }

    char app0_path[600];
    if (strncmp(abs_path, "ux0:/data/", 10) == 0) {
        snprintf(app0_path, sizeof(app0_path), "app0:/%s", abs_path + 5);
    } else {
        snprintf(app0_path, sizeof(app0_path), "app0:%s", abs_path);
    }
    return sceIoGetstat(app0_path, st);
}

/* Extract "ux0:" style device prefix for the partition queries. */
static int vita_device_of(const char *abs_path, char *dev, size_t devsz) {
    const char *colon = strchr(abs_path, ':');
    if (colon == NULL || (size_t)(colon - abs_path) + 2 > devsz) {
        return -1;
    }
    size_t n = (size_t)(colon - abs_path) + 1;  /* keep the colon */
    memcpy(dev, abs_path, n);
    dev[n] = '\0';
    return 0;
}

int pcsl_file_is_directory(const pcsl_string *path) {
    char abs_path[600];
    if (vita_dir_path(path, abs_path, sizeof(abs_path)) != 0) {
        return -1;
    }

    SceIoStat st;
    if (vita_stat_any(abs_path, &st) < 0) {
        return 0;  /* does not exist (or not readable) - not a directory */
    }
    return SCE_S_ISDIR(st.st_mode) ? 1 : 0;
}

int pcsl_file_mkdir(const pcsl_string *dirName) {
    char abs_path[600];
    if (vita_dir_path(dirName, abs_path, sizeof(abs_path)) != 0) {
        return -1;
    }

    /* Single level, like the upstream implementations: JSR 75's
     * create() only ever asks for one new directory. */
    if (sceIoMkdir(abs_path, 0777) < 0) {
        return -1;
    }
    return 0;
}

int pcsl_file_rmdir(const pcsl_string *dirName) {
    char abs_path[600];
    if (vita_dir_path(dirName, abs_path, sizeof(abs_path)) != 0) {
        return -1;
    }

    if (sceIoRmdir(abs_path) < 0) {
        return -1;
    }
    return 0;
}

jlong pcsl_file_getfreesize(const pcsl_string *path) {
    char abs_path[600];
    char dev[32];
    uint64_t max_size = 0;
    uint64_t free_size = 0;

    if (vita_dir_path(path, abs_path, sizeof(abs_path)) != 0
            || vita_device_of(abs_path, dev, sizeof(dev)) != 0) {
        return -1;
    }
    if (sceAppMgrGetDevInfo(dev, &max_size, &free_size) < 0) {
        return -1;
    }
    return (jlong)free_size;  /* jlong is 64-bit: no saturation needed */
}

jlong pcsl_file_gettotalsize(const pcsl_string *path) {
    char abs_path[600];
    char dev[32];
    uint64_t max_size = 0;
    uint64_t free_size = 0;

    if (vita_dir_path(path, abs_path, sizeof(abs_path)) != 0
            || vita_device_of(abs_path, dev, sizeof(dev)) != 0) {
        return -1;
    }
    if (sceAppMgrGetDevInfo(dev, &max_size, &free_size) < 0) {
        return -1;
    }
    return (jlong)max_size;
}

int pcsl_file_get_attribute(const pcsl_string *fileName, int type, int *result) {
    char abs_path[600];
    SceIoStat st;

    if (result == NULL
            || vita_dir_path(fileName, abs_path, sizeof(abs_path)) != 0) {
        return -1;
    }
    if (vita_stat_any(abs_path, &st) < 0) {
        return -1;
    }

    switch (type) {
    case PCSL_FILE_ATTR_READ:
    case PCSL_FILE_ATTR_WRITE:
        /* ux0:/app0: have no per-file permission bits; a file that is
         * there can be read and written (write failures surface as
         * EROFS from sceIoOpen for VPK-backed paths). */
        *result = 1;
        return 0;

    case PCSL_FILE_ATTR_EXECUTE:
    case PCSL_FILE_ATTR_HIDDEN:
        /* Not representable in the Vita filesystem. */
        *result = 0;
        return 0;

    default:
        return -1;
    }
}

int pcsl_file_set_attribute(const pcsl_string *fileName, int type, int value) {
    char abs_path[600];
    SceIoStat st;

    (void)value;
    if (vita_dir_path(fileName, abs_path, sizeof(abs_path)) != 0) {
        return -1;
    }
    if (vita_stat_any(abs_path, &st) < 0) {
        return -1;
    }

    switch (type) {
    case PCSL_FILE_ATTR_READ:
    case PCSL_FILE_ATTR_WRITE:
        /* Accepted as a no-op: nothing to change, nothing to fail. */
        return 0;

    default:
        /* EXECUTE/HIDDEN cannot be set on ux0: - tell JSR 75 the truth
         * so setHidden()/setReadable() throw instead of lying. */
        return -1;
    }
}

/* SceIoStat carries SceDateTime (broken down), JSR 75 wants seconds
 * since 1970-01-01. days_from_civil (Hinnant) keeps it libc- and
 * timezone-free, so Vita3K and real hardware agree. The console RTC is
 * read as-is: on a device whose clock is local time the epoch shifts
 * by the UTC offset, which MIDlets only ever compare with each other. */
static long vita_sce_datetime_to_epoch(const SceDateTime *dt) {
    long y = (long)dt->year;
    long m = (long)dt->month;
    long d = (long)dt->day;

    y -= (m <= 2);
    long era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (unsigned)((153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1);
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    long days = era * 146097L + (long)doe - 719468L;

    return days * 86400L + (long)dt->hour * 3600L + (long)dt->minute * 60L
           + (long)dt->second;
}

int pcsl_file_get_time(const pcsl_string *fileName, int type, long *result) {
    char abs_path[600];
    SceIoStat st;

    if (result == NULL
            || vita_dir_path(fileName, abs_path, sizeof(abs_path)) != 0) {
        return -1;
    }
    if (type != PCSL_FILE_TIME_LAST_MODIFIED) {
        return -1;
    }
    if (vita_stat_any(abs_path, &st) < 0) {
        return -1;
    }
    *result = vita_sce_datetime_to_epoch(&st.st_mtime);
    return 0;
}

/* ========================================================================
 * PCSL Print stubs
 * ======================================================================== */

void pcsl_print_chars(const char *s, int length) {
    /* v01.57: fprintf(stderr) is OFF LIMITS here. This runs per character
     * from JVMSPI_PrintRaw on VM worker threads, and the FIRST stdio
     * write from such a thread triggers newlib's lazy FILE-lock init -
     * the pte_osSemaphoreCreate NULL-store crash site from the v01.56
     * coredump. sceIo channel instead (vita_crumb.c). */
    (void)length;
    if (s != NULL) {
        crumb_append("ux0:/data/vm_stderr.log", s, (int)strlen(s));
    }
}

/* ========================================================================
 * JavaCall memory stubs
 * ======================================================================== */

void *javacall_malloc(unsigned int size) {
    return malloc(size);
}

void javacall_free(void *ptr) {
    free(ptr);
}

/* ========================================================================
 * JVM native function stubs
 * ======================================================================== */

/* Java_com_sun_cldchi_jvm_JVM_flushJarCaches */
void Java_com_sun_cldchi_jvm_JVM_flushJarCaches(void) {
    /* No-op stub */
}

/* Java_com_sun_cldchi_jvm_JVM_monotonicTimeMillis */
jlong Java_com_sun_cldchi_jvm_JVM_monotonicTimeMillis(void) {
    /* Return 0 as stub - should use sceKernelGetSystemTimeWide() */
    return 0;
}

/* ========================================================================
 * Misc stubs
 * ======================================================================== */

/* getInternalPropertyInt - from properties_port.
 * JAVA_HEAP_SIZE: the default (1280KB) cannot hold the chameleon skin
 * images plus a game; give the VM a real heap. 48MB: UC browser
 * (a heavyweight MIDlet) hit OutOfMemoryError during startApp on the
 * SECOND launch while 32MB worked on the first - the round-loop
 * launcher needs headroom for the VM restart path too. Heap chunks
 * come from malloc (OsMemory_vita uses jvm_malloc) and the newlib
 * arena is 96MB, so 48MB Java + native usage still fits.
 * MAX_ISOLATES=1 (single isolate build) keeps the AMS reservation
 * math at its floor.
 *
 * v01.28 REGRESSION: with 48MB the VM now hangs (no display, no tty
 * output) during bootstrap on the FIRST UC launch under Vita3K, while
 * 32MB started fine in v01.26. Rolled back to 32MB until the bootstrap
 * memory math is re-validated. */
int getInternalPropertyInt(const char *key) {
    if (key != NULL) {
        if (strcmp(key, "JAVA_HEAP_SIZE") == 0) {
            return 32 * 1024 * 1024; /* 32MB (48MB regressed first launch) */
        }
        if (strcmp(key, "MAX_ISOLATES") == 0) {
            return 1;
        }
    }
    return 0;
}

/* platformRequest - from platform_request */
int platformRequest(char *pszUrl) {
    (void)pszUrl;
    return 0;
}