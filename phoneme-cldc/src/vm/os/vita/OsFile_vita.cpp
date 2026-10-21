/*
 * OsFile_vita.cpp - PS Vita file I/O implementation
 *
 * newlib on Vita maps stdio to sceIo* already, so most of the
 * standard file API works. We use jvm_fopen/jvm_fclose wrappers
 * same as Linux.
 */

#include "incls/_precompiled.incl"
#include "incls/_OsFile_vita.cpp.incl"

#if defined(VITA) && !ENABLE_PCSL
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>

/* v02.70: whole-jar-in-memory read path.
 *
 * log113 (boot preload of ~494 classes): io_read n=3892 us=4,492,493, i.e.
 * 1.15 ms per sceIoRead and ~8 reads per class. BufferedFile::BUFFERSIZE is
 * 256 bytes and find_entry (central directory, end of the jar) alternates
 * with open_entry/read (local header + data, start of the jar) on the same
 * FILE, so every refill is a real syscall. The jar is only 1.3 MB: read it
 * once and serve OsFile_read/seek/length from RAM.
 *
 * Scope: only OsFile_open(<name>.jar, "rb"). Everything else keeps the
 * stdio path. A memory handle is the address of a slot in g_jar_h[]; a real
 * FILE* can never point into that array, so the address range tells the two
 * apart. Over the size caps, or if malloc/read fails, OsFile_open falls
 * through to the original fopen path. ux0:/data/J2ME00001/jar_mem.cfg
 * starting with '0'/'n'/'N' turns the feature off (A/B on hardware).
 *
 * Threading: jar OsFile_* calls run on the VM thread only (class loading,
 * ResourceInputStream); the tables are deliberately lock-free. */
extern void vita_boot_log(const char *fmt, ...);   /* JVM_vita.cpp */
extern "C" unsigned long long vita_now_us(void);   /* OS_vita.cpp */

#define VITA_JARMEM_SLOTS      8
#define VITA_JARMEM_PATH       256
#define VITA_JARMEM_MAX_FILE   (4L * 1024L * 1024L)
#define VITA_JARMEM_MAX_TOTAL  (8L * 1024L * 1024L)
#define VITA_JARMEM_CHUNK      (1024 * 1024)

struct VitaJarImage {
  char           path[VITA_JARMEM_PATH];
  unsigned char *data;      /* NULL = slot free */
  long           size;
  int            refs;
};

struct VitaJarHandle {
  VitaJarImage  *img;       /* NULL = slot free */
  long           pos;
};

static VitaJarImage  g_jar_img[VITA_JARMEM_SLOTS];
static VitaJarHandle g_jar_h[VITA_JARMEM_SLOTS];
static long          g_jar_total = 0;
static int           g_jar_enabled = -1;   /* -1 = not decided yet */

static bool vita_jar_is_handle(const void *h) {
  const char *p  = (const char *)h;
  const char *lo = (const char *)&g_jar_h[0];
  const char *hi = (const char *)&g_jar_h[VITA_JARMEM_SLOTS];
  return p >= lo && p < hi;
}

static bool vita_jar_name(const char *name) {
  size_t n = strlen(name);
  if (n < 5 || n >= VITA_JARMEM_PATH) {
    return false;
  }
  const char *e = name + n - 4;
  return e[0] == '.' && (e[1] | 0x20) == 'j' &&
         (e[2] | 0x20) == 'a' && (e[3] | 0x20) == 'r';
}

static bool vita_jarmem_enabled(void) {
  if (g_jar_enabled < 0) {
    g_jar_enabled = 1;
    int fd = open("ux0:/data/J2ME00001/jar_mem.cfg", O_RDONLY);
    if (fd >= 0) {
      char c = 0;
      if (read(fd, &c, 1) == 1 && (c == '0' || c == 'n' || c == 'N')) {
        g_jar_enabled = 0;
      }
      close(fd);
    }
    vita_boot_log("[jarmem] enabled=%d", g_jar_enabled);
  }
  return g_jar_enabled != 0;
}

/* Find or load the image for `path`; bumps refs. NULL = use stdio. */
static VitaJarImage *vita_jar_acquire(const char *path) {
  int i;
  for (i = 0; i < VITA_JARMEM_SLOTS; i++) {
    if (g_jar_img[i].data != NULL && strcmp(g_jar_img[i].path, path) == 0) {
      g_jar_img[i].refs++;
      return &g_jar_img[i];
    }
  }
  VitaJarImage *img = NULL;
  for (i = 0; i < VITA_JARMEM_SLOTS; i++) {
    if (g_jar_img[i].data == NULL) {
      img = &g_jar_img[i];
      break;
    }
  }
  if (img == NULL) {
    return NULL;
  }

  unsigned long long t0 = vita_now_us();
  int fd = open(path, O_RDONLY);
  if (fd < 0) {
    return NULL;
  }
  long size = (long)lseek(fd, 0, SEEK_END);
  if (size <= 0 || size > VITA_JARMEM_MAX_FILE ||
      g_jar_total + size > VITA_JARMEM_MAX_TOTAL ||
      lseek(fd, 0, SEEK_SET) != 0) {
    vita_boot_log("[jarmem] skip %s size=%ld total=%ld", path, size,
                  g_jar_total);
    close(fd);
    return NULL;
  }
  unsigned char *buf = (unsigned char *)malloc((size_t)size);
  if (buf == NULL) {
    vita_boot_log("[jarmem] malloc(%ld) failed for %s", size, path);
    close(fd);
    return NULL;
  }
  long got = 0;
  while (got < size) {
    long want = size - got;
    if (want > VITA_JARMEM_CHUNK) {
      want = VITA_JARMEM_CHUNK;
    }
    int r = (int)read(fd, buf + got, (size_t)want);
    if (r <= 0) {
      break;
    }
    got += r;
  }
  close(fd);
  if (got != size) {
    vita_boot_log("[jarmem] short read %ld/%ld for %s", got, size, path);
    free(buf);
    return NULL;
  }

  strcpy(img->path, path);
  img->data = buf;
  img->size = size;
  img->refs = 1;
  g_jar_total += size;
  vita_boot_log("[jarmem] loaded %s size=%ld us=%llu total=%ld", path, size,
                vita_now_us() - t0, g_jar_total);
  return img;
}

static OsFile_Handle vita_jar_open(const char *path) {
  int i;
  for (i = 0; i < VITA_JARMEM_SLOTS; i++) {
    if (g_jar_h[i].img == NULL) {
      break;
    }
  }
  if (i == VITA_JARMEM_SLOTS) {
    return NULL;
  }
  VitaJarImage *img = vita_jar_acquire(path);
  if (img == NULL) {
    return NULL;
  }
  g_jar_h[i].img = img;
  g_jar_h[i].pos = 0;
  return (OsFile_Handle)&g_jar_h[i];
}

static int vita_jar_close(OsFile_Handle handle) {
  VitaJarHandle *h = (VitaJarHandle *)handle;
  VitaJarImage *img = h->img;
  h->img = NULL;
  h->pos = 0;
  if (img != NULL && --img->refs <= 0) {
    free(img->data);
    g_jar_total -= img->size;
    img->data = NULL;
    img->size = 0;
    img->refs = 0;
    img->path[0] = 0;
  }
  return 0;
}

static long vita_jar_seek(OsFile_Handle handle, long offset, int origin) {
  VitaJarHandle *h = (VitaJarHandle *)handle;
  long size = h->img != NULL ? h->img->size : 0;
  long np;
  if (origin == SEEK_SET) {
    np = offset;
  } else if (origin == SEEK_CUR) {
    np = h->pos + offset;
  } else if (origin == SEEK_END) {
    np = size + offset;
  } else {
    return -1;
  }
  if (np < 0) {
    return -1;   /* fseek fails on a negative result; position unchanged */
  }
  h->pos = np;   /* past EOF is allowed, reads then return 0 (as fseek) */
  return 0;
}
#endif /* VITA && !ENABLE_PCSL */

#ifdef __cplusplus
extern "C" {
#endif

#if !ENABLE_PCSL
OsFile_Handle OsFile_open(const JvmPathChar *filename, const char *mode) {
#if defined(VITA)
  if (mode != NULL && mode[0] == 'r' && mode[1] == 'b' && mode[2] == 0 &&
      vita_jar_name(filename) && vita_jarmem_enabled()) {
    OsFile_Handle jh = vita_jar_open(filename);
    if (jh != NULL) {
      return jh;
    }
  }
#endif
  return (OsFile_Handle)jvm_fopen(filename, mode);
}

int OsFile_close(OsFile_Handle handle) {
#if defined(VITA)
  if (vita_jar_is_handle(handle)) {
    return vita_jar_close(handle);
  }
#endif
  return jvm_fclose(handle);
}

int OsFile_flush(OsFile_Handle handle) {
#if defined(VITA)
  if (vita_jar_is_handle(handle)) {
    return 0;
  }
#endif
  return jvm_fflush(handle);
}

size_t OsFile_read(OsFile_Handle handle,
                   void *buffer, size_t size, size_t count) {
#if defined(VITA)
  if (vita_jar_is_handle(handle)) {
    VitaJarHandle *h = (VitaJarHandle *)handle;
    if (size == 0 || count == 0 || h->img == NULL ||
        h->pos >= h->img->size) {
      return 0;
    }
    size_t avail = (size_t)(h->img->size - h->pos);
    size_t items = avail / size;
    if (items > count) {
      items = count;
    }
    memcpy(buffer, h->img->data + h->pos, items * size);
    h->pos += (long)(items * size);
    return items;
  }
#endif
  return jvm_fread(buffer, size, count, handle);
}

size_t OsFile_write(OsFile_Handle handle,
                    const void *buffer, size_t size, size_t count) {
#if defined(VITA)
  if (vita_jar_is_handle(handle)) {
    return 0;   /* memory jars are read-only ("rb" only) */
  }
#endif
  return jvm_fwrite(buffer, size, count, handle);
}

long OsFile_length(OsFile_Handle handle) {
#if defined(VITA)
  if (vita_jar_is_handle(handle)) {
    VitaJarHandle *h = (VitaJarHandle *)handle;
    return h->img != NULL ? h->img->size : 0;
  }
#endif
  jvm_fseek(handle, 0, SEEK_END);
  long res = jvm_ftell(handle);
  jvm_fseek(handle, 0, SEEK_SET);
  return res;
}

bool OsFile_exists(const JvmPathChar *name) {
  struct stat buf;
  if (jvm_stat(name, &buf) == 0) {
    if (S_ISREG(buf.st_mode)) {
       return true;
    }
  }
  return false;
}

long OsFile_seek(OsFile_Handle handle, long offset, int origin) {
#if defined(VITA)
  if (vita_jar_is_handle(handle)) {
    return vita_jar_seek(handle, offset, origin);
  }
#endif
  return jvm_fseek(handle, offset, origin);
}

int OsFile_eof(OsFile_Handle handle) {
#if defined(VITA)
  if (vita_jar_is_handle(handle)) {
    VitaJarHandle *h = (VitaJarHandle *)handle;
    return h->img == NULL || h->pos >= h->img->size;
  }
#endif
  return jvm_feof(handle);
}

bool OsFile_rename(const JvmPathChar *from, const JvmPathChar *to) {
  jvm_rename(from, to);
  return true;
}

int OsFile_remove(const JvmPathChar *filename) {
  return jvm_remove(filename);
}
#endif // !ENABLE_PCSL

#ifdef __cplusplus
}
#endif
