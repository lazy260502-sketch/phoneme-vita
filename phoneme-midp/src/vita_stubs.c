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

/* PS Vita headers */
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/io/dirent.h>

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
        return 0;
    }
    /* Rough estimate: each UTF-16 char may take 1-3 UTF-8 bytes */
    return str->length * 3;
}

pcsl_string_status pcsl_string_convert_to_utf8(const pcsl_string *string,
                                               jbyte *buffer,
                                               jsize buffer_length,
                                               jsize *converted_length) {
    if (string == NULL || buffer == NULL) {
        return PCSL_STRING_EINVAL;
    }
    jsize len = string->length;
    if (buffer_length < len + 1) {
        return PCSL_STRING_BUFFER_OVERFLOW;
    }
    /* Simple: just copy jchar to jbyte (ASCII subset) */
    jsize i;
    for (i = 0; i < len; i++) {
        buffer[i] = (jbyte)(string->data[i] & 0xFF);
    }
    buffer[len] = 0;
    if (converted_length != NULL) {
        *converted_length = len;
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
    /* Convert to static buffer - not thread safe, but works for stubs */
    static jbyte buf[4096];
    jsize i;
    for (i = 0; i < str->length && i < (jsize)sizeof(buf) - 1; i++) {
        buf[i] = (jbyte)(str->data[i] & 0xFF);
    }
    buf[str->length] = 0;
    return buf;
}

void pcsl_string_release_utf8_data(const jbyte *buf, const pcsl_string *str) {
    (void)buf;
    (void)str;
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

    /* Convert jchar path to char for Vita IO */
    char path_utf8[512];
    jsize i;
    for (i = 0; i < fileName->length && i < (jsize)sizeof(path_utf8) - 1; i++) {
        path_utf8[i] = (char)(fileName->data[i] & 0xFF);
    }
    path_utf8[i] = '\0';

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
    vf->fd = sceIoOpen(path_utf8, oflags, 0777);
    if (vf->fd < 0) {
        /* Try with app0: prefix for files in the VPK */
        char app0_path[512];
        snprintf(app0_path, sizeof(app0_path), "app0:%s", path_utf8);
        vf->fd = sceIoOpen(app0_path, oflags, 0777);
        if (vf->fd < 0) {
            snprintf(log_buf, sizeof(log_buf), "[file_open] FAILED '%s' (0x%x)\n", path_utf8, (int)vf->fd);
            debug_log(log_buf);
            free(vf);
            return -1;
        }
        debug_log("[file_open] ok via app0 prefix\n");
        strncpy(vf->path, app0_path, sizeof(vf->path) - 1);
    } else {
        debug_log("[file_open] ok direct\n");
        strncpy(vf->path, path_utf8, sizeof(vf->path) - 1);
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

int pcsl_file_unlink(const pcsl_string *fileName) {
    if (fileName == NULL || fileName->data == NULL) {
        return -1;
    }
    
    /* Convert jchar path to char */
    char path_utf8[512];
    jsize i;
    for (i = 0; i < fileName->length && i < (jsize)sizeof(path_utf8) - 1; i++) {
        path_utf8[i] = (char)(fileName->data[i] & 0xFF);
    }
    path_utf8[i] = '\0';
    
    /* Try with app0: prefix */
    char app0_path[512];
    snprintf(app0_path, sizeof(app0_path), "app0:%s", path_utf8);
    
    int result = sceIoRemove(app0_path);
    if (result < 0) {
        result = sceIoRemove(path_utf8);
    }
    return result;
}

int pcsl_file_truncate(void *handle, long size) {
    (void)handle;
    (void)size;
    return 0;
}

int pcsl_file_exist(const pcsl_string *fileName) {
    if (fileName == NULL || fileName->data == NULL) {
        return 0;
    }
    
    /* Convert jchar path to char */
    char path_utf8[512];
    jsize i;
    for (i = 0; i < fileName->length && i < (jsize)sizeof(path_utf8) - 1; i++) {
        path_utf8[i] = (char)(fileName->data[i] & 0xFF);
    }
    path_utf8[i] = '\0';
    
    /* Try with app0: prefix */
    char app0_path[512];
    snprintf(app0_path, sizeof(app0_path), "app0:%s", path_utf8);
    
    SceIoStat stat;
    if (sceIoGetstat(app0_path, &stat) >= 0) {
        return 1;
    }
    
    if (sceIoGetstat(path_utf8, &stat) >= 0) {
        return 1;
    }
    
    return 0;
}

int pcsl_file_commitwrite(void *handle) {
    (void)handle;
    return 0;
}

int pcsl_file_rename(const pcsl_string *oldName, const pcsl_string *newName) {
    (void)oldName;
    (void)newName;
    return -1;
}

void *pcsl_file_openfilelist(const pcsl_string *string) {
    (void)string;
    return NULL;
}

int pcsl_file_closefilelist(void *handle) {
    (void)handle;
    return 0;
}

int pcsl_file_getnextentry(void *handle, const pcsl_string *string,
                           pcsl_string *result) {
    (void)handle;
    (void)string;
    (void)result;
    return -1;
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
    
    /* Convert jchar path to char */
    char path_utf8[512];
    jsize i;
    for (i = 0; i < fileName->length && i < (jsize)sizeof(path_utf8) - 1; i++) {
        path_utf8[i] = (char)(fileName->data[i] & 0xFF);
    }
    path_utf8[i] = '\0';
    
    /* Try with app0: prefix */
    char app0_path[512];
    snprintf(app0_path, sizeof(app0_path), "app0:%s", path_utf8);
    
    SceIoStat stat;
    if (sceIoGetstat(app0_path, &stat) >= 0) {
        return (long)stat.st_size;
    }
    
    if (sceIoGetstat(path_utf8, &stat) >= 0) {
        return (long)stat.st_size;
    }
    
    return -1;
}

long pcsl_file_getusedspace(const pcsl_string *dirName) {
    (void)dirName;
    return 0;
}

jchar pcsl_file_getfileseparator(void) {
    return '/';
}

jchar pcsl_file_getpathseparator(void) {
    return ':';
}

/* ========================================================================
 * PCSL Socket stubs
 * ======================================================================== */

int pcsl_socket_open_start(unsigned char *ipBytes, int port,
                           void **pHandle, void **pContext) {
    (void)ipBytes;
    (void)port;
    if (pHandle != NULL) *pHandle = NULL;
    if (pContext != NULL) *pContext = NULL;
    return -1; /* PCSL_NET_IOERROR */
}

int pcsl_socket_open_finish(void *handle, void *context) {
    (void)handle;
    (void)context;
    return -1;
}

int pcsl_socket_read_start(void *handle, unsigned char *pData, int len,
                           int *pBytesRead, void **pContext) {
    (void)handle;
    (void)pData;
    (void)len;
    if (pBytesRead != NULL) *pBytesRead = 0;
    if (pContext != NULL) *pContext = NULL;
    return -1;
}

int pcsl_socket_read_finish(void *handle, unsigned char *pData, int len,
                            int *pBytesRead, void *context) {
    (void)handle;
    (void)pData;
    (void)len;
    (void)context;
    if (pBytesRead != NULL) *pBytesRead = 0;
    return -1;
}

int pcsl_socket_write_start(void *handle, char *pData, int len,
                            int *pBytesWritten, void **pContext) {
    (void)handle;
    (void)pData;
    (void)len;
    if (pBytesWritten != NULL) *pBytesWritten = 0;
    if (pContext != NULL) *pContext = NULL;
    return -1;
}

int pcsl_socket_write_finish(void *handle, char *pData, int len,
                             int *pBytesWritten, void *context) {
    (void)handle;
    (void)pData;
    (void)len;
    (void)context;
    if (pBytesWritten != NULL) *pBytesWritten = 0;
    return -1;
}

int pcsl_socket_close_start(void *handle, void **pContext) {
    (void)handle;
    if (pContext != NULL) *pContext = NULL;
    return 0;
}

int pcsl_socket_close_finish(void *handle, void *context) {
    (void)handle;
    (void)context;
    return 0;
}

int pcsl_socket_available(void *handle, int *pBytesAvailable) {
    (void)handle;
    if (pBytesAvailable != NULL) *pBytesAvailable = 0;
    return 0;
}

int pcsl_socket_shutdown_output(void *handle) {
    (void)handle;
    return 0;
}

int pcsl_socket_getlocaladdr(void *handle, char *pAddress) {
    (void)handle;
    if (pAddress != NULL) pAddress[0] = 0;
    return -1;
}

int pcsl_socket_getremoteaddr(void *handle, char *pAddress) {
    (void)handle;
    if (pAddress != NULL) pAddress[0] = 0;
    return -1;
}

/* ========================================================================
 * PCSL Network stubs
 * ======================================================================== */

int pcsl_network_init(void) {
    return 0;
}

int pcsl_network_init_start(PCSL_NET_CALLBACK pcsl_network_callback) {
    (void)pcsl_network_callback;
    return 0;
}

int pcsl_network_init_finish(void) {
    return 0;
}

int pcsl_network_finalize_start(PCSL_NET_CALLBACK pcsl_network_callback) {
    (void)pcsl_network_callback;
    return 0;
}

int pcsl_network_finalize_finish(void) {
    return 0;
}

int pcsl_network_gethostbyname_start(char *hostname, unsigned char *pAddress,
                                      int maxLen, int *pLen,
                                      void **pHandle, void **pContext) {
    (void)hostname;
    (void)pAddress;
    (void)maxLen;
    if (pLen != NULL) *pLen = 0;
    if (pHandle != NULL) *pHandle = NULL;
    if (pContext != NULL) *pContext = NULL;
    return -1;
}

int pcsl_network_gethostbyname_finish(unsigned char *pAddress, int maxLen,
                                       int *pLen, void *handle, void *context) {
    (void)pAddress;
    (void)maxLen;
    (void)handle;
    (void)context;
    if (pLen != NULL) *pLen = 0;
    return -1;
}

int pcsl_network_error(void *handle) {
    (void)handle;
    return 0;
}

int pcsl_network_getLocalHostName(char *pLocalHost) {
    if (pLocalHost != NULL) {
        strcpy(pLocalHost, "vita");
    }
    return 0;
}

int pcsl_network_getLocalIPAddressAsString(char *pLocalIPAddress) {
    if (pLocalIPAddress != NULL) {
        strcpy(pLocalIPAddress, "127.0.0.1");
    }
    return 0;
}

int pcsl_network_getHostByAddr_start(int ipn, char *hostname,
                                      void **pHandle, void **pContext) {
    (void)ipn;
    if (hostname != NULL) hostname[0] = 0;
    if (pHandle != NULL) *pHandle = NULL;
    if (pContext != NULL) *pContext = NULL;
    return -1;
}

int pcsl_network_getHostByAddr_finish(int ipn, char *hostname,
                                       void **pHandle, void *context) {
    (void)ipn;
    (void)context;
    if (hostname != NULL) hostname[0] = 0;
    if (pHandle != NULL) *pHandle = NULL;
    return -1;
}

int pcsl_network_addrToString(unsigned char *ipBytes,
                              unsigned short **pResult, int *pResultLen) {
    (void)ipBytes;
    if (pResult != NULL) *pResult = NULL;
    if (pResultLen != NULL) *pResultLen = 0;
    return -1;
}

int pcsl_network_getlocalport(void *handle, int *pPortNumber) {
    (void)handle;
    if (pPortNumber != NULL) *pPortNumber = 0;
    return 0;
}

int pcsl_network_getremoteport(void *handle, int *pPortNumber) {
    (void)handle;
    if (pPortNumber != NULL) *pPortNumber = 0;
    return 0;
}

int pcsl_network_getsockopt(void *handle, int flag, int *pOptval) {
    (void)handle;
    (void)flag;
    if (pOptval != NULL) *pOptval = 0;
    return 0;
}

int pcsl_network_setsockopt(void *handle, int flag, int optval) {
    (void)handle;
    (void)flag;
    (void)optval;
    return 0;
}

unsigned int pcsl_network_htonl(unsigned int value) {
    return ((value & 0xFF) << 24) | ((value & 0xFF00) << 8) |
           ((value >> 8) & 0xFF00) | ((value >> 24) & 0xFF);
}

unsigned int pcsl_network_ntohl(unsigned int value) {
    return pcsl_network_htonl(value);
}

unsigned short pcsl_network_htons(unsigned short value) {
    return (unsigned short)(((value & 0xFF) << 8) | ((value >> 8) & 0xFF));
}

unsigned short pcsl_network_ntohs(unsigned short value) {
    return pcsl_network_htons(value);
}

/* ========================================================================
 * PCSL Datagram stubs
 * ======================================================================== */

int pcsl_datagram_open_start(int port, void **pHandle, void **pContext) {
    (void)port;
    if (pHandle != NULL) *pHandle = NULL;
    if (pContext != NULL) *pContext = NULL;
    return -1;
}

int pcsl_datagram_open_finish(void *handle, void *context) {
    (void)handle;
    (void)context;
    return -1;
}

int pcsl_datagram_read_start(void *handle, unsigned char *pAddress, int *port,
                             char *buffer, int length, int *pBytesRead,
                             void **pContext) {
    (void)handle;
    (void)pAddress;
    (void)port;
    (void)buffer;
    (void)length;
    if (pBytesRead != NULL) *pBytesRead = 0;
    if (pContext != NULL) *pContext = NULL;
    return -1;
}

int pcsl_datagram_read_finish(void *handle, unsigned char *pAddress, int *port,
                              char *buffer, int length, int *pBytesRead,
                              void *context) {
    (void)handle;
    (void)pAddress;
    (void)port;
    (void)buffer;
    (void)length;
    (void)context;
    if (pBytesRead != NULL) *pBytesRead = 0;
    return -1;
}

int pcsl_datagram_write_start(void *handle, unsigned char *pAddress, int port,
                              char *buffer, int length, int *pBytesWritten,
                              void **pContext) {
    (void)handle;
    (void)pAddress;
    (void)port;
    (void)buffer;
    (void)length;
    if (pBytesWritten != NULL) *pBytesWritten = 0;
    if (pContext != NULL) *pContext = NULL;
    return -1;
}

int pcsl_datagram_write_finish(void *handle, unsigned char *pAddress, int port,
                               char *buffer, int length, int *pBytesWritten,
                               void *context) {
    (void)handle;
    (void)pAddress;
    (void)port;
    (void)buffer;
    (void)length;
    (void)context;
    if (pBytesWritten != NULL) *pBytesWritten = 0;
    return -1;
}

int pcsl_datagram_close_start(void *handle, void **pContext) {
    (void)handle;
    if (pContext != NULL) *pContext = NULL;
    return 0;
}

int pcsl_datagram_close_finish(void *handle, void *context) {
    (void)handle;
    (void)context;
    return 0;
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

/* getInternalPropertyInt - from properties_port */
int getInternalPropertyInt(const char *key) {
    (void)key;
    return 0;
}

/* platformRequest - from platform_request */
int platformRequest(char *pszUrl) {
    (void)pszUrl;
    return 0;
}