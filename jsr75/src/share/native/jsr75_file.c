/*
 * JSR 75 (PDA Optional Packages) native layer for the PS Vita port.
 *
 * This is a Vita-local subsystem: no upstream phoneME source was copied.
 * The Java-facing interface signatures follow the JSR 75 specification,
 * the implementation below is written from scratch against
 *   - the PCSL file layer the Vita port already provides for plain byte
 *     I/O (pcsl_file_open/close/read/write/unlink/rename/truncate/seek/
 *     sizeof/exist), and
 *   - the SceIo / SceAppMgr calls that layer cannot express: directory
 *     enumeration that includes sub-directories, directory creation and
 *     removal, stat-derived timestamps, free and total space.
 *
 * Why the second group goes to the OS directly:
 *   - pcsl_file_getnextentry() deliberately skips directories (v01.44) to
 *     keep the AMS/RMS cleanup paths from recursing into them, so JSR 75
 *     must not reuse it; we walk with sceIoDopen()/sceIoDread() instead
 *     and never touch the shared PCSL enumeration code.
 *   - the pcsl_directory.h group (is_directory/mkdir/rmdir/get_time/
 *     getfreesize/gettotalsize) is implemented in the Vita executable
 *     (src/vita_pcsl.c), not in the PCSL set that libmidp.so links
 *     against, so referring to it here would leave undefined symbols at
 *     link time.  See the nm audit in PROJECT_MEMORY.md.
 *   - pcsl_file_exist()'s app0: fallback is wrong for a JSR 75 root.
 *
 * Root policy: the only exposed file system root is the application data
 * directory, i.e. the current working directory the launcher chdir()s to
 * ("ux0:/data/J2ME00001").  This is the exact same root that
 * vita_resolve_path() in vita_pcsl.c resolves relative PCSL paths against,
 * so the two stay consistent by construction instead of by a duplicated
 * #define.  Paths handed down from Java are therefore absolute
 * ("ux0:/data/J2ME00001/...") and pass straight through vita_resolve_path().
 *
 * Root policy: the only exposed file system root is the application data
 * directory, i.e. the current working directory the launcher chdir()s to
 * ("ux0:/data/J2ME00001").  This is the exact same root that
 * vita_resolve_path() in vita_pcsl.c resolves relative PCSL paths against,
 * so the two stay consistent by construction instead of by a duplicated
 * #define.  Paths handed down from Java are therefore absolute
 * ("ux0:/data/J2ME00001/...") and pass straight through vita_resolve_path().
 *
 * KNI note: the ROM generator synthesises "Java_<class>_<method>" for any
 * native method it cannot resolve from its own NativesTable (see
 * SourceObjectWriter::put_c_function), so the names below are resolved at
 * link time by the usual KNI convention and need no table regeneration.
 */

#include <string.h>
#include <unistd.h>     /* getcwd() - same root source as vita_resolve_path */

#include <kni.h>
#include <pcsl_string.h>
#include <pcsl_memory.h>
#include <pcsl_file.h>
#include <pcsl_directory.h>
#include <midpUtilKni.h>
#include <midpError.h>

#include <psp2/io/fcntl.h>
#include <psp2/io/dirent.h>
#include <psp2/io/stat.h>
#include <psp2/appmgr.h>       /* sceAppMgrGetDevInfo() for free/total */

/* ------------------------------------------------------------------ */
/* Small helpers                                                       */
/* ------------------------------------------------------------------ */

#define JSR75_PATH_MAX 512

/* Convert a Java string (already unpacked by the caller) into a
 * NUL-terminated UTF-8 C string.  Returns 0 on success. */
static int jsr75_cpath(const pcsl_string *s, char *out) {
    jsize len = 0;
    if (s == NULL || pcsl_string_is_null(s)) {
        return -1;
    }
    if (pcsl_string_convert_to_utf8(s, (jbyte *)out, JSR75_PATH_MAX - 1,
                                    &len) != PCSL_STRING_OK) {
        return -1;
    }
    out[len] = '\0';
    return (len > 0) ? 0 : -1;
}

/* Physical root, always terminated with a path separator.  Derived from
 * the cwd the launcher set, with the same fallback vita_resolve_path()
 * uses in vita_pcsl.c. */
static const char *jsr75_root(void) {
    static char root[JSR75_PATH_MAX];
    static int ready = 0;

    if (!ready) {
        size_t n;
        if (getcwd(root, sizeof(root) - 2) == NULL || root[0] == '\0') {
            strcpy(root, "ux0:/data/J2ME00001");
        }
        n = strlen(root);
        if (n == 0 || root[n - 1] != '/') {
            root[n] = '/';
            root[n + 1] = '\0';
        }
        ready = 1;
    }
    return root;
}

/* The root itself is reported by sceIoGetstat with a stat mode that has
 * no IFDIR bit in some firmware revisions; treat the root string as a
 * directory unconditionally. */
static int jsr75_is_root_path(const char *path) {
    size_t n = strlen(path);
    const char *r = jsr75_root();
    if (n + 1 == strlen(r) && strncmp(path, r, n) == 0) {
        return 1;
    }
    if (strcmp(path, r) == 0) {
        return 1;
    }
    return 0;
}

/* Device prefix of an absolute path, colon included ("ux0:").  Returns 0
 * on success, -1 when the path carries no device part. */
static int jsr75_dev_of(const char *path, char *dev, size_t devsz) {
    const char *colon = strchr(path, ':');
    size_t n;

    if (colon == NULL) {
        return -1;
    }
    n = (size_t)(colon - path) + 1;
    if (n + 1 > devsz) {
        return -1;
    }
    memcpy(dev, path, n);
    dev[n] = '\0';
    return 0;
}

/* 1 when the path exists and is a directory, 0 otherwise. */
static int jsr75_stat_is_dir(const char *path) {
    SceIoStat st;

    if (sceIoGetstat(path, &st) < 0) {
        return 0;
    }
    return SCE_S_ISDIR(st.st_mode) ? 1 : 0;
}

/* SceIoStat carries SceDateTime (broken down); JSR 75 wants seconds since
 * 1970-01-01.  days_from_civil (Hinnant) keeps this libc- and
 * timezone-free, exactly as vita_pcsl.c does it for
 * pcsl_file_get_time(). */
static long jsr75_datetime_to_epoch(const SceDateTime *dt) {
    long y = (long)dt->year;
    long m = (long)dt->month;
    long d = (long)dt->day;
    long era;
    unsigned yoe, doy, doe;
    long days;

    if (y <= 0 || m <= 0) {
        return 0;
    }
    y -= (m <= 2);
    era = (y >= 0 ? y : y - 399) / 400;
    yoe = (unsigned)(y - era * 400);
    doy = (unsigned)((153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1);
    doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    days = era * 146097L + (long)doe - 719468L;

    return days * 86400L + (long)dt->hour * 3600L + (long)dt->minute * 60L
           + (long)dt->second;
}

/* ------------------------------------------------------------------ */
/* Open file handle table                                              */
/* ------------------------------------------------------------------ */

#define JSR75_MAX_FILES 16

static void *jsr75_files[JSR75_MAX_FILES];

/* flags: 0 = read only, 1 = write (create if missing, do not truncate) */
#define JSR75_MODE_READ  0
#define JSR75_MODE_WRITE 1

static int jsr75_alloc_file_slot(void) {
    int i;
    for (i = 0; i < JSR75_MAX_FILES; i++) {
        if (jsr75_files[i] == NULL) {
            return i;
        }
    }
    return -1;
}

/* ------------------------------------------------------------------ */
/* Directory iterator table                                            */
/* ------------------------------------------------------------------ */

#define JSR75_MAX_ITERS 8

typedef struct {
    int used;
    SceUID fd;
    char prefix[JSR75_PATH_MAX];
} Jsr75Iter;

/* Static storage is zero filled, so "used" (not fd) decides whether a
 * slot is free: SceUID 0 must never be mistaken for a valid descriptor. */
static Jsr75Iter jsr75_iters[JSR75_MAX_ITERS];

static void jsr75_iter_reset(Jsr75Iter *it) {
    it->used = 0;
    it->fd = -1;
    it->prefix[0] = '\0';
}

/* ------------------------------------------------------------------ */
/* Native methods                                                      */
/* ------------------------------------------------------------------ */

/*
 * Returns the exposed root as an absolute Vita path with a trailing '/'.
 * Java uses it to translate file:///data/<x> into ux0:/data/J2ME00001/<x>.
 */
KNIEXPORT KNI_RETURNTYPE_OBJECT
KNIDECL(com_sun_midp_jsr075_FileStore_getRootPath) {
    const char *root = jsr75_root();
    pcsl_string s;

    KNI_StartHandles(1);
    KNI_DeclareHandle(result);
    KNI_ReleaseHandle(result);

    if (pcsl_string_convert_from_utf8((jbyte *)root, (jsize)strlen(root),
                                      &s) == PCSL_STRING_OK) {
        midp_jstring_from_pcsl_string(KNIPASSARGS &s, result);
        pcsl_string_free(&s);
    }

    KNI_EndHandlesAndReturnObject(result);
}

/* body is a statement without the trailing ';' - the macro supplies it, so
 * the call sites read like the assignment they are. */
#define JSR75_HANDLE_METHOD(name, body)                        \
    KNIEXPORT KNI_RETURNTYPE_BOOLEAN                           \
    KNIDECL(name) {                                            \
        jboolean rc = KNI_FALSE;                               \
        KNI_StartHandles(1);                                   \
        GET_PARAMETER_AS_PCSL_STRING(1, path)                  \
            char cpath[JSR75_PATH_MAX];                        \
            if (jsr75_cpath(&path, cpath) == 0) {              \
                body;                                          \
            }                                                  \
        RELEASE_PCSL_STRING_PARAMETER                          \
        KNI_EndHandles();                                      \
        KNI_ReturnBoolean(rc);                                 \
    }

JSR75_HANDLE_METHOD(com_sun_midp_jsr075_FileStore_exists,
    rc = (pcsl_file_exist(&path) == 1) ? KNI_TRUE : KNI_FALSE)

JSR75_HANDLE_METHOD(com_sun_midp_jsr075_FileStore_isDirectory,
    rc = (jsr75_is_root_path(cpath) || jsr75_stat_is_dir(cpath))
         ? KNI_TRUE : KNI_FALSE)

/* create(): fail when the file already exists, otherwise create a zero
 * length file.  PCSL has no dedicated creat(), so open with
 * O_CREAT|O_EXCL-like semantics emulated by the existence check. */
KNIEXPORT KNI_RETURNTYPE_BOOLEAN
KNIDECL(com_sun_midp_jsr075_FileStore_createFile) {
    jboolean rc = KNI_FALSE;

    KNI_StartHandles(1);
    GET_PARAMETER_AS_PCSL_STRING(1, path)
        if (pcsl_file_exist(&path) != 1) {
            void *h = NULL;
            if (pcsl_file_open(&path,
                    PCSL_FILE_O_RDWR | PCSL_FILE_O_CREAT,
                    &h) == 0 && h != NULL) {
                pcsl_file_close(h);
                rc = KNI_TRUE;
            }
        }
    RELEASE_PCSL_STRING_PARAMETER
    KNI_EndHandles();
    KNI_ReturnBoolean(rc);
}

JSR75_HANDLE_METHOD(com_sun_midp_jsr075_FileStore_mkdirFile,
    rc = (sceIoMkdir(cpath, 0777) >= 0) ? KNI_TRUE : KNI_FALSE)

JSR75_HANDLE_METHOD(com_sun_midp_jsr075_FileStore_deleteFile,
    rc = (pcsl_file_unlink(&path) == 0) ? KNI_TRUE : KNI_FALSE)

JSR75_HANDLE_METHOD(com_sun_midp_jsr075_FileStore_deleteDir,
    rc = (sceIoRmdir(cpath) >= 0) ? KNI_TRUE : KNI_FALSE)

/* Rename within the same root; works for files and directories. */
KNIEXPORT KNI_RETURNTYPE_BOOLEAN
KNIDECL(com_sun_midp_jsr075_FileStore_renameTo) {
    jboolean rc = KNI_FALSE;

    KNI_StartHandles(2);
    GET_PARAMETER_AS_PCSL_STRING(1, oldPath)
        GET_PARAMETER_AS_PCSL_STRING(2, newPath)
            rc = (pcsl_file_rename(&oldPath, &newPath) == 0)
                 ? KNI_TRUE : KNI_FALSE;
        RELEASE_PCSL_STRING_PARAMETER
    RELEASE_PCSL_STRING_PARAMETER
    KNI_EndHandles();
    KNI_ReturnBoolean(rc);
}

KNIEXPORT KNI_RETURNTYPE_LONG
KNIDECL(com_sun_midp_jsr075_FileStore_fileSize) {
    jlong rc = -1;

    KNI_StartHandles(1);
    GET_PARAMETER_AS_PCSL_STRING(1, path)
        long sz = pcsl_file_sizeof(&path);
        if (sz >= 0) {
            rc = (jlong)sz;
        }
    RELEASE_PCSL_STRING_PARAMETER
    KNI_EndHandles();
    KNI_ReturnLong(rc);
}

/* Last modification time in seconds since the epoch, 0 when unknown
 * (JSR 75 mandates 0 for "not available"). */
KNIEXPORT KNI_RETURNTYPE_LONG
KNIDECL(com_sun_midp_jsr075_FileStore_lastModified) {
    jlong rc = 0;

    KNI_StartHandles(1);
    GET_PARAMETER_AS_PCSL_STRING(1, path)
        char cpath[JSR75_PATH_MAX];
        SceIoStat st;
        if (jsr75_cpath(&path, cpath) == 0
                && sceIoGetstat(cpath, &st) >= 0) {
            rc = (jlong)jsr75_datetime_to_epoch(&st.st_mtime);
        }
    RELEASE_PCSL_STRING_PARAMETER
    KNI_EndHandles();
    KNI_ReturnLong(rc);
}

/* Free space of the volume the root lives on. -1 stands for "unknown". */
KNIEXPORT KNI_RETURNTYPE_LONG
KNIDECL(com_sun_midp_jsr075_FileStore_availableSize) {
    char dev[32];
    uint64_t max_size = 0;
    uint64_t free_size = 0;

    if (jsr75_dev_of(jsr75_root(), dev, sizeof(dev)) != 0
            || sceAppMgrGetDevInfo(dev, &max_size, &free_size) < 0) {
        KNI_ReturnLong((jlong)-1);
    }
    KNI_ReturnLong((jlong)free_size);
}

KNIEXPORT KNI_RETURNTYPE_LONG
KNIDECL(com_sun_midp_jsr075_FileStore_totalSize) {
    char dev[32];
    uint64_t max_size = 0;
    uint64_t free_size = 0;

    if (jsr75_dev_of(jsr75_root(), dev, sizeof(dev)) != 0
            || sceAppMgrGetDevInfo(dev, &max_size, &free_size) < 0) {
        KNI_ReturnLong((jlong)-1);
    }
    KNI_ReturnLong((jlong)max_size);
}

/*
 * openFile(path, mode) -> handle (>0) or 0.
 * mode 0: read only, mode 1: read/write, created when missing.  No
 * truncation: JSR 75's openOutputStream() keeps the existing content and
 * create() is the only operation that forces a zero length file.
 */
KNIEXPORT KNI_RETURNTYPE_INT
KNIDECL(com_sun_midp_jsr075_FileStore_openFile) {
    jint mode = KNI_GetParameterAsInt(2);
    jint rc = 0;

    KNI_StartHandles(1);
    GET_PARAMETER_AS_PCSL_STRING(1, path)
        void *h = NULL;
        int flags = (mode == JSR75_MODE_WRITE)
                    ? (PCSL_FILE_O_RDWR | PCSL_FILE_O_CREAT)
                    : PCSL_FILE_O_RDONLY;
        int slot = jsr75_alloc_file_slot();
        if (slot >= 0 && pcsl_file_open(&path, flags, &h) == 0 && h != NULL) {
            jsr75_files[slot] = h;
            rc = (jint)(slot + 1);
        }
    RELEASE_PCSL_STRING_PARAMETER
    KNI_EndHandles();
    KNI_ReturnInt(rc);
}

KNIEXPORT KNI_RETURNTYPE_INT
KNIDECL(com_sun_midp_jsr075_FileStore_closeFile) {
    jint h = KNI_GetParameterAsInt(1);
    jint slot = h - 1;
    jint rc = -1;

    if (slot >= 0 && slot < JSR75_MAX_FILES && jsr75_files[slot] != NULL) {
        pcsl_file_commitwrite(jsr75_files[slot]);
        rc = (pcsl_file_close(jsr75_files[slot]) == 0) ? 0 : -1;
        jsr75_files[slot] = NULL;
    }
    KNI_ReturnInt(rc);
}

/* readFile(handle, byte[] buf, int off, int len) -> bytes read, -1 error */
KNIEXPORT KNI_RETURNTYPE_INT
KNIDECL(com_sun_midp_jsr075_FileStore_readFile) {
    jint h = KNI_GetParameterAsInt(1);
    jint off = KNI_GetParameterAsInt(3);
    jint len = KNI_GetParameterAsInt(4);
    jint slot = h - 1;
    jint rc = -1;

    KNI_StartHandles(1);
    KNI_DeclareHandle(buf);
    KNI_GetParameterAsObject(2, buf);

    if (slot >= 0 && slot < JSR75_MAX_FILES && jsr75_files[slot] != NULL
            && buf != NULL && len > 0) {
        if (len > (jint)KNI_GetArrayLength(buf) - off) {
            len = (jint)KNI_GetArrayLength(buf) - off;
        }
        if (len > 0) {
            static unsigned char tmp[2048];
            jint done = 0;
            while (done < len) {
                jint chunk = len - done;
                int n;
                if (chunk > (jint)sizeof(tmp)) {
                    chunk = (jint)sizeof(tmp);
                }
                n = pcsl_file_read(jsr75_files[slot], tmp, (long)chunk);
                if (n <= 0) {
                    break;
                }
                KNI_SetRawArrayRegion(buf, off + done, n, (jbyte *)tmp);
                done += n;
                if (n < chunk) {
                    break;
                }
            }
            rc = done;
        }
    }

    KNI_EndHandles();
    KNI_ReturnInt(rc);
}

/* writeFile(handle, byte[] buf, int off, int len) -> bytes written, -1 error */
KNIEXPORT KNI_RETURNTYPE_INT
KNIDECL(com_sun_midp_jsr075_FileStore_writeFile) {
    jint h = KNI_GetParameterAsInt(1);
    jint off = KNI_GetParameterAsInt(3);
    jint len = KNI_GetParameterAsInt(4);
    jint slot = h - 1;
    jint rc = -1;

    KNI_StartHandles(1);
    KNI_DeclareHandle(buf);
    KNI_GetParameterAsObject(2, buf);

    if (slot >= 0 && slot < JSR75_MAX_FILES && jsr75_files[slot] != NULL
            && buf != NULL && len > 0) {
        if (len > (jint)KNI_GetArrayLength(buf) - off) {
            len = (jint)KNI_GetArrayLength(buf) - off;
        }
        if (len > 0) {
            static unsigned char tmp[2048];
            jint done = 0;
            while (done < len) {
                jint chunk = len - done;
                int n;
                if (chunk > (jint)sizeof(tmp)) {
                    chunk = (jint)sizeof(tmp);
                }
                KNI_GetRawArrayRegion(buf, off + done, chunk, (jbyte *)tmp);
                n = pcsl_file_write(jsr75_files[slot], tmp, (long)chunk);
                if (n <= 0) {
                    break;
                }
                done += n;
                if (n < chunk) {
                    break;
                }
            }
            rc = done;
        }
    }

    KNI_EndHandles();
    KNI_ReturnInt(rc);
}

/* seekFile(handle, long pos) -> new position, -1 on error */
KNIEXPORT KNI_RETURNTYPE_LONG
KNIDECL(com_sun_midp_jsr075_FileStore_seekFile) {
    jint h = KNI_GetParameterAsInt(1);
    jlong pos = KNI_GetParameterAsLong(2);
    jint slot = h - 1;
    jlong rc = -1;

    if (slot >= 0 && slot < JSR75_MAX_FILES && jsr75_files[slot] != NULL) {
        long p = pcsl_file_seek(jsr75_files[slot], (long)pos,
                                PCSL_FILE_SEEK_SET);
        if (p >= 0) {
            rc = (jlong)p;
        }
    }
    KNI_ReturnLong(rc);
}

/* Truncate a file to newSize bytes (create() semantics on an existing
 * file: the offset is written and the file is cut there). */
KNIEXPORT KNI_RETURNTYPE_BOOLEAN
KNIDECL(com_sun_midp_jsr075_FileStore_truncateFile) {
    jlong size = KNI_GetParameterAsLong(2);
    jboolean rc = KNI_FALSE;

    KNI_StartHandles(1);
    GET_PARAMETER_AS_PCSL_STRING(1, path)
        void *h = NULL;
        if (size >= 0
                && pcsl_file_open(&path, PCSL_FILE_O_RDWR, &h) == 0
                && h != NULL) {
            if (pcsl_file_truncate(h, (long)size) == 0) {
                rc = KNI_TRUE;
            }
            pcsl_file_close(h);
        }
    RELEASE_PCSL_STRING_PARAMETER
    KNI_EndHandles();
    KNI_ReturnBoolean(rc);
}

/*
 * listOpen(path) -> iterator handle (>0) or 0.
 * The path is normalised to "dir/" so the entries can be reported back
 * with their full name when needed.
 */
KNIEXPORT KNI_RETURNTYPE_INT
KNIDECL(com_sun_midp_jsr075_FileStore_listOpen) {
    jint rc = 0;

    KNI_StartHandles(1);
    GET_PARAMETER_AS_PCSL_STRING(1, path)
        char cpath[JSR75_PATH_MAX];
        if (jsr75_cpath(&path, cpath) == 0) {
            int i;
            size_t n = strlen(cpath);
            if (n > 0 && cpath[n - 1] != '/' && n + 1 < JSR75_PATH_MAX) {
                cpath[n] = '/';
                cpath[n + 1] = '\0';
            }
            for (i = 0; i < JSR75_MAX_ITERS; i++) {
                if (!jsr75_iters[i].used) {
                    SceUID fd = sceIoDopen(cpath);
                    if (fd >= 0) {
                        size_t m = strlen(cpath);
                        if (m >= JSR75_PATH_MAX) {
                            m = JSR75_PATH_MAX - 1;
                        }
                        jsr75_iters[i].used = 1;
                        jsr75_iters[i].fd = fd;
                        memcpy(jsr75_iters[i].prefix, cpath, m);
                        jsr75_iters[i].prefix[m] = '\0';
                        rc = (jint)(i + 1);
                    }
                    break;
                }
            }
        }
    RELEASE_PCSL_STRING_PARAMETER
    KNI_EndHandles();
    KNI_ReturnInt(rc);
}

/*
 * listNext(handle) -> String, or null at the end of the directory.
 * Directories are reported with a trailing '/' (JSR 75 requirement),
 * "." and ".." are never reported.
 */
KNIEXPORT KNI_RETURNTYPE_OBJECT
KNIDECL(com_sun_midp_jsr075_FileStore_listNext) {
    jint h = KNI_GetParameterAsInt(1);
    jint slot = h - 1;

    KNI_StartHandles(1);
    KNI_DeclareHandle(result);
    KNI_ReleaseHandle(result);

    if (slot >= 0 && slot < JSR75_MAX_ITERS && jsr75_iters[slot].used) {
        SceIoDirent entry;
        int ok = 0;
        for (;;) {
            memset(&entry, 0, sizeof(entry));
            if (sceIoDread(jsr75_iters[slot].fd, &entry) <= 0) {
                break;              /* end of directory or error */
            }
            if (entry.d_name[0] == '\0'
                    || strcmp(entry.d_name, ".") == 0
                    || strcmp(entry.d_name, "..") == 0) {
                continue;
            }
            ok = 1;
            break;
        }
        if (ok) {
            char name[300];
            size_t n;
            strncpy(name, entry.d_name, sizeof(name) - 2);
            name[sizeof(name) - 2] = '\0';
            if (SCE_S_ISDIR(entry.d_stat.st_mode)) {
                n = strlen(name);
                name[n] = '/';
                name[n + 1] = '\0';
            }
            {
                pcsl_string s;
                if (pcsl_string_convert_from_utf8((jbyte *)name,
                            (jsize)strlen(name), &s) == PCSL_STRING_OK) {
                    midp_jstring_from_pcsl_string(KNIPASSARGS &s, result);
                    pcsl_string_free(&s);
                }
            }
        }
    }

    KNI_EndHandlesAndReturnObject(result);
}

KNIEXPORT KNI_RETURNTYPE_INT
KNIDECL(com_sun_midp_jsr075_FileStore_listClose) {
    jint h = KNI_GetParameterAsInt(1);
    jint slot = h - 1;
    jint rc = -1;

    if (slot >= 0 && slot < JSR75_MAX_ITERS && jsr75_iters[slot].used) {
        rc = (sceIoDclose(jsr75_iters[slot].fd) >= 0) ? 0 : -1;
        jsr75_iter_reset(&jsr75_iters[slot]);
    }
    KNI_ReturnInt(rc);
}
