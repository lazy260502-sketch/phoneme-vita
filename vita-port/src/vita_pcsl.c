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
#include <pcsl_socket.h>
#include <pcsl_network.h>
#include <pcsl_datagram.h>
#include <pcsl_print.h>

/* JavaCall memory */
#include <javacall_memory.h>

/* JVM and KNI */
#include <kni.h>
#include <jvm.h>
#include <midp_logging.h>

/* PCSL string constants */
const pcsl_string PCSL_STRING_EMPTY = { NULL, 0, 0 };
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
    return str->length;
}

jsize pcsl_string_utf16_length(const pcsl_string *str) {
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
    for (i = 0; i < str->length; i++) {
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
    for (i = 0; i < string->length; i++) {
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
    jsize len = str->length;
    if (buffer_length < len + 1) {
        return PCSL_STRING_BUFFER_OVERFLOW;
    }
    memcpy(buffer, str->data, len * sizeof(jchar));
    buffer[len] = 0;
    if (converted_length != NULL) {
        *converted_length = len;
    }
    return PCSL_STRING_OK;
}

pcsl_string_status pcsl_string_convert_from_utf8(const jbyte *buffer,
                                                  jsize buffer_length,
                                                  pcsl_string *string) {
    if (buffer == NULL || string == NULL) {
        return PCSL_STRING_EINVAL;
    }
    /* Allocate jchar array and copy */
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
    string->length = buffer_length;
    string->flags = PCSL_STRING_IN_HEAP;
    return PCSL_STRING_OK;
}

pcsl_string_status pcsl_string_convert_from_utf16(const jchar *buffer,
                                                   jsize buffer_length,
                                                   pcsl_string *string) {
    if (buffer == NULL || string == NULL) {
        return PCSL_STRING_EINVAL;
    }
    jchar *data = (jchar *)malloc((buffer_length + 1) * sizeof(jchar));
    if (data == NULL) {
        return PCSL_STRING_ENOMEM;
    }
    memcpy(data, buffer, buffer_length * sizeof(jchar));
    data[buffer_length] = 0;
    string->data = data;
    string->length = buffer_length;
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
    if (str1->length != str2->length) {
        return KNI_FALSE;
    }
    return memcmp(str1->data, str2->data, str1->length * sizeof(jchar)) == 0
        ? KNI_TRUE : KNI_FALSE;
}

pcsl_string_status pcsl_string_compare(const pcsl_string *str1,
                                       const pcsl_string *str2,
                                       jint *comparison) {
    if (str1 == NULL || str2 == NULL || comparison == NULL) {
        return PCSL_STRING_EINVAL;
    }
    jsize min_len = (str1->length < str2->length) ? str1->length : str2->length;
    int cmp = memcmp(str1->data, str2->data, min_len * sizeof(jchar));
    if (cmp == 0) {
        cmp = str1->length - str2->length;
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
    jsize total = str1->length + str2->length;
    jchar *data = (jchar *)malloc((total + 1) * sizeof(jchar));
    if (data == NULL) {
        return PCSL_STRING_ENOMEM;
    }
    memcpy(data, str1->data, str1->length * sizeof(jchar));
    memcpy(data + str1->length, str2->data, str2->length * sizeof(jchar));
    data[total] = 0;
    str->data = data;
    str->length = total;
    str->flags = PCSL_STRING_IN_HEAP;
    return PCSL_STRING_OK;
}

pcsl_string_status pcsl_string_dup(const pcsl_string *src, pcsl_string *dst) {
    if (src == NULL || dst == NULL) {
        return PCSL_STRING_EINVAL;
    }
    return pcsl_string_cat(src, &PCSL_STRING_EMPTY, dst);
}

pcsl_string_status pcsl_string_append(pcsl_string *dst, const pcsl_string *src) {
    if (dst == NULL || src == NULL) {
        return PCSL_STRING_EINVAL;
    }
    jsize total = dst->length + src->length;
    jchar *data = (jchar *)malloc((total + 1) * sizeof(jchar));
    if (data == NULL) {
        return PCSL_STRING_ENOMEM;
    }
    if (dst->data != NULL && dst->length > 0) {
        memcpy(data, dst->data, dst->length * sizeof(jchar));
    }
    memcpy(data + dst->length, src->data, src->length * sizeof(jchar));
    data[total] = 0;
    if (dst->flags & PCSL_STRING_IN_HEAP) {
        free(dst->data);
    }
    dst->data = data;
    dst->length = total;
    dst->flags = PCSL_STRING_IN_HEAP;
    return PCSL_STRING_OK;
}

pcsl_string_status pcsl_string_append_char(pcsl_string *dst, const jchar newchar) {
    if (dst == NULL) {
        return PCSL_STRING_EINVAL;
    }
    jsize total = dst->length + 1;
    jchar *data = (jchar *)malloc((total + 1) * sizeof(jchar));
    if (data == NULL) {
        return PCSL_STRING_ENOMEM;
    }
    if (dst->data != NULL && dst->length > 0) {
        memcpy(data, dst->data, dst->length * sizeof(jchar));
    }
    data[dst->length] = newchar;
    data[total] = 0;
    if (dst->flags & PCSL_STRING_IN_HEAP) {
        free(dst->data);
    }
    dst->data = data;
    dst->length = total;
    dst->flags = PCSL_STRING_IN_HEAP;
    return PCSL_STRING_OK;
}

pcsl_string_status pcsl_string_append_buf(pcsl_string *dst,
                                          const jchar *newtext,
                                          const jint textsize) {
    if (dst == NULL || newtext == NULL) {
        return PCSL_STRING_EINVAL;
    }
    jsize total = dst->length + textsize;
    jchar *data = (jchar *)malloc((total + 1) * sizeof(jchar));
    if (data == NULL) {
        return PCSL_STRING_ENOMEM;
    }
    if (dst->data != NULL && dst->length > 0) {
        memcpy(data, dst->data, dst->length * sizeof(jchar));
    }
    memcpy(data + dst->length, newtext, textsize * sizeof(jchar));
    data[total] = 0;
    if (dst->flags & PCSL_STRING_IN_HEAP) {
        free(dst->data);
    }
    dst->data = data;
    dst->length = total;
    dst->flags = PCSL_STRING_IN_HEAP;
    return PCSL_STRING_OK;
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
    if (begin_index < 0 || end_index > str->length || begin_index > end_index) {
        return PCSL_STRING_EINVAL;
    }
    jsize len = end_index - begin_index;
    jchar *data = (jchar *)malloc((len + 1) * sizeof(jchar));
    if (data == NULL) {
        return PCSL_STRING_ENOMEM;
    }
    memcpy(data, str->data + begin_index, len * sizeof(jchar));
    data[len] = 0;
    dst->data = data;
    dst->length = len;
    dst->flags = PCSL_STRING_IN_HEAP;
    return PCSL_STRING_OK;
}

jboolean pcsl_string_starts_with(const pcsl_string *str, const pcsl_string *prefix) {
    if (str == NULL || prefix == NULL) {
        return KNI_FALSE;
    }
    if (prefix->length > str->length) {
        return KNI_FALSE;
    }
    return memcmp(str->data, prefix->data, prefix->length * sizeof(jchar)) == 0
        ? KNI_TRUE : KNI_FALSE;
}

jboolean pcsl_string_ends_with(const pcsl_string *str, const pcsl_string *suffix) {
    if (str == NULL || suffix == NULL) {
        return KNI_FALSE;
    }
    if (suffix->length > str->length) {
        return KNI_FALSE;
    }
    return memcmp(str->data + (str->length - suffix->length),
                  suffix->data, suffix->length * sizeof(jchar)) == 0
        ? KNI_TRUE : KNI_FALSE;
}

jint pcsl_string_index_of(const pcsl_string *str, jint c) {
    if (str == NULL || str->data == NULL) {
        return -1;
    }
    jsize i;
    for (i = 0; i < str->length; i++) {
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
    jsize i;
    for (i = from_index; i < str->length; i++) {
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
    jint i;
    for (i = str->length - 1; i >= 0; i--) {
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
    jint start = (from_index < str->length) ? from_index : str->length - 1;
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
    for (i = 0; i < str->length; i++) {
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
    char path[256];  /* File path for debugging */
} VitaFileHandle;

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

/* Simple sceIo-based debug logging for Vita */
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
    debug_log_init();
    char log_buf[1024];
    snprintf(log_buf, sizeof(log_buf), "[file_open] path='%s' flags=0x%x\n", path_utf8, oflags);
    debug_log(log_buf);

    /* Try to open the file using Vita IO */
    vf->fd = sceIoOpen(abs_path, oflags, 0777);
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
            snprintf(log_buf, sizeof(log_buf), "[file_open] FAILED '%s' (0x%x)\n",
                     abs_path, (int)vf->fd);
            debug_log(log_buf);
            free(vf);
            return -1;
        }
        debug_log("[file_open] ok via app0 prefix\n");
        strncpy(vf->path, app0_path, sizeof(vf->path) - 1);
        vf->path[sizeof(vf->path) - 1] = '\0';
    } else {
        debug_log("[file_open] ok direct\n");
        strncpy(vf->path, abs_path, sizeof(vf->path) - 1);
        vf->path[sizeof(vf->path) - 1] = '\0';
    }

    *handle = vf;
    return 0;
}

int pcsl_file_close(void *handle) {
    if (handle == NULL) {
        return 0;
    }
    
    VitaFileHandle *vf = (VitaFileHandle *)handle;
    if (vf->fd >= 0) {
        sceIoClose(vf->fd);
        vf->fd = -1;
    }
    free(vf);
    return 0;
}

int pcsl_file_read(void *handle, unsigned char *buf, long size) {
    if (handle == NULL || buf == NULL || size <= 0) {
        return -1;
    }
    
    VitaFileHandle *vf = (VitaFileHandle *)handle;
    return sceIoRead(vf->fd, buf, size);
}

int pcsl_file_write(void *handle, unsigned char *buf, long size) {
    if (handle == NULL || buf == NULL || size <= 0) {
        return -1;
    }
    
    VitaFileHandle *vf = (VitaFileHandle *)handle;
    return sceIoWrite(vf->fd, buf, size);
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
    
    /* VPK fallback: swap the ux0:/data prefix for app0: (same layout
     * inside the VPK) - the raw "app0:%s" form was invalid for
     * absolute paths. */
    char app0_path[600];
    if (strncmp(abs_path, "ux0:/data/", 10) == 0) {
        snprintf(app0_path, sizeof(app0_path), "app0:/%s", abs_path + 5);
    } else {
        snprintf(app0_path, sizeof(app0_path), "app0:%s", abs_path);
    }
    
    int result = sceIoRemove(abs_path);
    if (result < 0) {
        if (vita_unlink_enoent_is_ok(abs_path)) {
            /* The target was already gone (Vita3K misreports this as
             * "file in use"). Treat delete-of-a-gone-file as success. */
            return 0;
        }
        result = sceIoRemove(app0_path);
        if (result < 0 && vita_unlink_enoent_is_ok(app0_path)) {
            return 0;
        }
    }
    return result;
}

/* Vita has no sceIoFtruncate, so shrink by read-rewrite: save the
 * first `size` bytes, reopen the same path with O_TRUNC, write them
 * back. The file position is preserved (clamped to the new size) to
 * match POSIX ftruncate semantics callers expect.
 * A previous stub returned success without doing anything, so
 * RecordStoreImpl.compactRecords left stale blocks past the new
 * logical size - the second launch then walked garbage record
 * headers ("first launch OK, every launch after broken"). */
int pcsl_file_truncate(void *handle, long size) {
    if (handle == NULL || size < 0) {
        return -1;
    }

    VitaFileHandle *vf = (VitaFileHandle *)handle;
    if (vf->fd < 0) {
        return -1;
    }

    SceOff cur_pos = sceIoLseek(vf->fd, 0, SCE_SEEK_CUR);
    SceOff file_size = sceIoLseek(vf->fd, 0, SCE_SEEK_END);
    if (cur_pos < 0 || file_size < 0) {
        return -1;
    }

    if (size >= file_size) {
        /* Growing (or unchanged): RMS never does this; just restore
         * the position and report success. */
        sceIoLseek(vf->fd, cur_pos, SCE_SEEK_SET);
        return 0;
    }

    /* Save the bytes that must survive. */
    unsigned char *buf = (unsigned char *)malloc((size_t)size > 0 ? (size_t)size : 1);
    if (buf == NULL) {
        return -1;
    }

    sceIoLseek(vf->fd, 0, SCE_SEEK_SET);
    long got = 0;
    while (got < size) {
        int n = sceIoRead(vf->fd, buf + got, size - got);
        if (n <= 0) {
            free(buf);
            sceIoLseek(vf->fd, cur_pos, SCE_SEEK_SET);
            return -1;
        }
        got += n;
    }

    /* Rewrite the file in place via the path captured at open time
     * (always the resolved absolute path that opened successfully). */
    sceIoClose(vf->fd);
    vf->fd = sceIoOpen(vf->path, SCE_O_RDWR | SCE_O_CREAT | SCE_O_TRUNC, 0777);
    if (vf->fd < 0) {
        free(buf);
        return -1;
    }

    long put = 0;
    while (put < size) {
        int n = sceIoWrite(vf->fd, buf + put, size - put);
        if (n <= 0) {
            free(buf);
            return -1;
        }
        put += n;
    }
    free(buf);

    if (cur_pos > size) {
        cur_pos = size;
    }
    sceIoLseek(vf->fd, cur_pos, SCE_SEEK_SET);
    return 0;
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
    
    return sceIoLseek(vf->fd, offset, whence);
}

long pcsl_file_sizeofopenfile(void *handle) {
    if (handle == NULL) {
        return -1;
    }
    
    VitaFileHandle *vf = (VitaFileHandle *)handle;
    SceOff current = sceIoLseek(vf->fd, 0, SCE_SEEK_CUR);
    SceOff size = sceIoLseek(vf->fd, 0, SCE_SEEK_END);
    sceIoLseek(vf->fd, current, SCE_SEEK_SET);
    return (long)size;
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
        total += (long)entry.d_stat.st_size;
    }
    sceIoDclose(dfd);

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
 * PCSL Print stubs
 * ======================================================================== */

void pcsl_print_chars(const char *s, int length) {
    (void)length;
    if (s != NULL) {
        fprintf(stderr, "%s", s);
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