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
#include <stdio.h>   /* v03.01: snprintf in vita_jarmem_report */
#include <string.h>
#include <errno.h>   /* v03.01: errno in the jarmem open-failure line */
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
 * ResourceInputStream); the tables are deliberately lock-free.
 *
 * v02.75: images are now kept resident after the last handle closes (refs
 * drops to 0 but the buffer is not freed). JarFileParser::get() only caches
 * min(MAX_CACHED_PARSERS, MaxCachedJarParsers) parsers, so opening a second
 * jar evicts the first and closes its handle; with free-on-last-close that
 * re-read the whole jar on every switch (log115 round 0 loaded each jar
 * twice in preload and once more in resolve). A strcmp hit in acquire() now
 * reuses the resident image instead. Slots are recycled only when all 8 are
 * occupied; the 8 MB total cap and the 8-slot table keep the cost bounded. */
extern void vita_boot_log(const char *fmt, ...);   /* JVM_vita.cpp */
extern "C" unsigned long long vita_now_us(void);   /* OS_vita.cpp */

#define VITA_JARMEM_SLOTS      8   /* resident jar IMAGES (memory) */
/* v03.07: handle slots are separate from image slots, and deliberately
 * cheap. A handle is {image*, long pos} = 8 bytes, so 32 of them cost 256 B.
 *
 * Why the split (log123, the first log that ran the v03.03 counters):
 *   round 0 pre : handles=0/8  total=0
 *   round 0 post: handles=0/8  refs=0/0
 *   round 1 pre : handles=8/8  refs=4/4  no_slot=30   <- already full
 *   round 2 pre : handles=8/8  refs=4/4  no_slot=38
 * i.e. eight handles leaked during round 0 and were NEVER returned, so from
 * round 1 on every OsFile_open("*.jar","rb") fell silently through to stdio.
 * BufferedFile::BUFFERSIZE is 256 B and find_entry (end of jar) alternates
 * with open_entry (start of jar) on the same FILE, so each class costs ~8
 * real sceIoRead syscalls at ~1.2 ms: that is the 4.8-6.4 s io_read that
 * rounds 1-4 spent in preload.
 *
 * The leak itself is upstream VM behaviour we do not want to change blind:
 * JarFileParser holds parsers through ObjectHeap::WEAK global refs, and a
 * weak ref that gets collected is unregistered WITHOUT calling
 * parser().dispose(), so the FileDescriptor (and this handle) survives until
 * the Java finalizer runs - which on a 32 MB heap and a single VM thread may
 * be a long time. With 8 slots that is fatal within one round; with 32 it
 * takes many rounds, and the counters below name the caller so the real leak
 * can be fixed once it is identified instead of guessed at. */
#define VITA_JARMEM_HANDLES    32
#define VITA_JARMEM_PATH       256
#define VITA_JARMEM_MAX_FILE   (4L * 1024L * 1024L)
#define VITA_JARMEM_MAX_TOTAL  (8L * 1024L * 1024L)
#define VITA_JARMEM_CHUNK      (1024 * 1024)
#define VITA_JARMEM_LEAKSITES  4   /* distinct callers reported on no_slot */

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
static VitaJarHandle g_jar_h[VITA_JARMEM_HANDLES];
static long          g_jar_total = 0;
static int           g_jar_enabled = -1;   /* -1 = not decided yet */

/* v03.01: why is this instrumented at all?
 *
 * log120 loaded exactly TWO jars into RAM (midp_system.jar and the round-0
 * game.jar) and then never logged another "[jarmem] loaded" line, while
 * rounds 1-4 each spent 5.9-7.3 s in preload with io_read n=4121..4462
 * us=4.8..6.4 s. Every failure path in this file either logs (skip / short
 * read / malloc) or returns silently, and the log had none of those lines -
 * so either nothing ever reached this code, or a SILENT path did:
 *
 *   - vita_jar_open: no free handle slot          (silent)
 *   - vita_jar_acquire: open() failed             (silent)
 *   - vita_jar_acquire: all 8 images resident and none has refs==0 (silent)
 *   - OsFile_open: mode not exactly "rb"           (silent, by design)
 *   - OsFile_open: name not *.jar                  (silent, by design)
 *   - OsFile_open: jar_mem.cfg disabled it         (silent, by design)
 *
 * Without per-reason counters those are indistinguishable, and guessing
 * wrong here means "optimising" I/O that is already served from RAM. The
 * counters below are monotonic for the whole process; vita_jarmem_report()
 * dumps them at preload begin and end, so a per-round delta is a
 * subtraction - the same trick vita_trace_preload_report() uses. */
struct VitaJarStat {
  unsigned int open_call;   /* OsFile_open(.jar,"rb") with the feature on */
  unsigned int reuse;       /* strcmp hit on a resident image            */
  unsigned int loaded;      /* fresh image read from disk                */
  unsigned int no_slot;     /* all handle slots busy                     */
  unsigned int no_image;    /* all image slots busy, none recyclable     */
  unsigned int open_fail;   /* open(path) failed                         */
  unsigned int skip_size;   /* size <= 0 or over the per-file/total cap  */
  unsigned int malloc_fail; /* image buffer allocation failed            */
  unsigned int short_read;  /* disk read came up short                   */
  unsigned int bypass;      /* *.jar rb but we did NOT take it          */
  /* v03.07: did any handle ever come back? If close stays 0 while open_call
   * climbs, nothing in the process ever closes a jar and the slot table is a
   * one-way counter - which is exactly what log123 showed (handles=8/8 in
   * every round after the first). */
  unsigned int close_call;
};

static VitaJarStat g_jar_stat;


/* v03.07: who asks for a handle when the table is full? Deduplicated by
 * return address, so four distinct callers are enough to identify the leak
 * without flooding the log. Values are offsets into this module, i.e. usable
 * directly with addr2line after subtracting the load base. */
static unsigned int g_jar_leaksite[VITA_JARMEM_LEAKSITES];
static unsigned int g_jar_leaksite_hits[VITA_JARMEM_LEAKSITES];
static int          g_jar_leaksite_n = 0;

static void vita_jar_note_leaksite(unsigned int ra) {
  int i;
  for (i = 0; i < g_jar_leaksite_n; i++) {
    if (g_jar_leaksite[i] == ra) {
      g_jar_leaksite_hits[i]++;
      return;
    }
  }
  if (g_jar_leaksite_n < VITA_JARMEM_LEAKSITES) {
    g_jar_leaksite[g_jar_leaksite_n] = ra;
    g_jar_leaksite_hits[g_jar_leaksite_n] = 1;
    g_jar_leaksite_n++;
  }
}

static bool vita_jar_is_handle(const void *h) {
  const char *p  = (const char *)h;
  const char *lo = (const char *)&g_jar_h[0];
  const char *hi = (const char *)&g_jar_h[VITA_JARMEM_HANDLES];
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
      g_jar_stat.reuse++;
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
    /* v02.75: every slot holds a resident image. Recycle one that no
     * handle currently references (refs == 0) so the table stays bounded;
     * if all images are in use, fall through to stdio (OsFile_open). */
    for (i = 0; i < VITA_JARMEM_SLOTS; i++) {
      if (g_jar_img[i].refs == 0) {
        img = &g_jar_img[i];
        break;
      }
    }
    if (img != NULL) {
      free(img->data);
      g_jar_total -= img->size;
      img->data = NULL;
      img->size = 0;
      img->refs = 0;
      img->path[0] = 0;
    }
  }
  if (img == NULL) {
    /* v03.01: silent before - the 8 image slots are all pinned (refs > 0),
     * so every later jar quietly fell back to stdio. */
    g_jar_stat.no_image++;
    return NULL;
  }

  unsigned long long t0 = vita_now_us();
  int fd = open(path, O_RDONLY);
  if (fd < 0) {
    g_jar_stat.open_fail++;
    /* errno matters: EMFILE/ENFILE means the process ran out of fds (the
     * launcher never exits, so a leaked fd in any round stays leaked), a
     * plain ENOENT means the path form changed. */
    vita_boot_log("[jarmem] open failed %s errno=%d", path, errno);
    return NULL;
  }
  long size = (long)lseek(fd, 0, SEEK_END);
  if (size <= 0 || size > VITA_JARMEM_MAX_FILE ||
      g_jar_total + size > VITA_JARMEM_MAX_TOTAL ||
      lseek(fd, 0, SEEK_SET) != 0) {
    g_jar_stat.skip_size++;
    vita_boot_log("[jarmem] skip %s size=%ld total=%ld max=%ld/%ld", path,
                  size, g_jar_total, (long)VITA_JARMEM_MAX_FILE,
                  (long)VITA_JARMEM_MAX_TOTAL);
    close(fd);
    return NULL;
  }
  unsigned char *buf = (unsigned char *)malloc((size_t)size);
  if (buf == NULL) {
    g_jar_stat.malloc_fail++;
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
    g_jar_stat.short_read++;
    vita_boot_log("[jarmem] short read %ld/%ld for %s", got, size, path);
    free(buf);
    return NULL;
  }

  strcpy(img->path, path);
  img->data = buf;
  img->size = size;
  img->refs = 1;
  g_jar_total += size;
  g_jar_stat.loaded++;
  vita_boot_log("[jarmem] loaded %s size=%ld us=%llu total=%ld", path, size,
                vita_now_us() - t0, g_jar_total);
  return img;
}

static OsFile_Handle vita_jar_open(const char *path) {
  int i;
  for (i = 0; i < VITA_JARMEM_HANDLES; i++) {
    if (g_jar_h[i].img == NULL) {
      break;
    }
  }
  if (i == VITA_JARMEM_HANDLES) {
    /* v03.01: the other silent path. A leaked handle (open without close)
     * would exhaust the table permanently and every later jar would
     * quietly fall back to stdio for the rest of the process.
     * v03.07: and now it records WHO asked, so the leak can be fixed
     * instead of just tolerated. */
    g_jar_stat.no_slot++;
    vita_jar_note_leaksite((unsigned int)(unsigned long)
                           __builtin_return_address(0));
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
  g_jar_stat.close_call++;
  h->img = NULL;
  h->pos = 0;
  /* v02.75: keep the image resident instead of freeing it when the last
   * handle closes. JarFileParser::get() evicts the previous jar from its
   * parser cache whenever a second jar is opened; that dispose() closes the
   * OsFile_Handle and used to free the image here, forcing a full ~1 MB
   * re-read on the next open (log115 saw midp_system.jar + game.jar each
   * loaded twice during round-0 preload, then once more during resolve).
   * Now the image stays in g_jar_img[] with refs == 0 and is reused by the
   * strcmp hit in vita_jar_acquire(); it is only recycled when the slot
   * table fills up. Memory stays bounded by VITA_JARMEM_SLOTS and
   * VITA_JARMEM_MAX_TOTAL. */
  if (img != NULL && img->refs > 0) {
    img->refs--;
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

/* v03.01: dump the counters + the live slot table. Called at preload begin
 * and end (JVM.cpp), so a round's activity is the delta between two
 * consecutive "begin"/"done" pairs - see the VitaJarStat comment for why
 * this exists at all.
 *
 * Reading it:
 *   hit>0            jar served from RAM, no disk I/O for it
 *   loaded>0         read once from disk into RAM
 *   bypass/skip/...  fell back to stdio -> every OsFile_read is a syscall
 *                    (BUFFERSIZE is 256 B and find_entry/open_entry alternate
 *                     on the same FILE, so that is ~8 syscalls per class) */
extern "C" void vita_jarmem_report(const char *tag) {
  char buf[420];
  int pos = 0;
  int i, used_h = 0, live = 0;

  /* Resolve the feature flag now instead of printing -1: a reader cannot
   * tell "-1 = not decided yet" from a bug, and this is the first thing
   * they look at. The helper logs its own "[jarmem] enabled=N" line once. */
  (void)vita_jarmem_enabled();

  for (i = 0; i < VITA_JARMEM_HANDLES; i++) {
    if (g_jar_h[i].img != NULL) {
      used_h++;
    }
  }
  for (i = 0; i < VITA_JARMEM_SLOTS; i++) {
    if (g_jar_img[i].data != NULL) {
      live++;
    }
  }
  pos = snprintf(buf, sizeof(buf),
                 "[jarmem] %s enabled=%d hit=%u loaded=%u closed=%u "
                 "bypass=%u no_slot=%u no_img=%u open_fail=%u skip=%u "
                 "malloc=%u short=%u | handles=%d/%d images=%d/%d total=%ld",
                 tag != NULL ? tag : "-", g_jar_enabled,
                 g_jar_stat.reuse, g_jar_stat.loaded, g_jar_stat.close_call,
                 g_jar_stat.bypass,
                 g_jar_stat.no_slot, g_jar_stat.no_image,
                 g_jar_stat.open_fail, g_jar_stat.skip_size,
                 g_jar_stat.malloc_fail, g_jar_stat.short_read,
                 used_h, VITA_JARMEM_HANDLES, live, VITA_JARMEM_SLOTS,
                 g_jar_total);
  vita_boot_log("%s", buf);

  /* v03.07: the leak callers, deduplicated. `open` climbing while `closed`
   * stays at 0 means nothing in the process ever closes a jar handle - the
   * log123 signature. Each address is an offset in this module, so addr2line
   * after subtracting the load base names the function. */
  for (i = 0; i < g_jar_leaksite_n; i++) {
    int n = snprintf(buf, sizeof(buf),
                     "[jarmem]   leak caller[%d] ra=0x%08x hits=%u",
                     i, g_jar_leaksite[i], g_jar_leaksite_hits[i]);
    if (n > 0) {
      vita_boot_log("%s", buf);
    }
  }

  /* One line per live image. A path that never shows up here is a jar that
   * is being read from disk on every class, which is the whole point. */
  for (i = 0; i < VITA_JARMEM_SLOTS; i++) {
    if (g_jar_img[i].data != NULL) {
      int n = 0;
      /* the slot index makes "which slot got recycled" visible */
      n = snprintf(buf, sizeof(buf), "[jarmem]   [%d] refs=%d size=%ld %s",
                   i, g_jar_img[i].refs, g_jar_img[i].size,
                   g_jar_img[i].path);
      if (n > 0) {
        vita_boot_log("%s", buf);
      }
    }
  }
}
#endif /* VITA && !ENABLE_PCSL */

#ifdef __cplusplus
extern "C" {
#endif

#if !ENABLE_PCSL
OsFile_Handle OsFile_open(const JvmPathChar *filename, const char *mode) {
#if defined(VITA)
  /* v03.01: count the paths that never reach vita_jar_open at all. Without
   * this a jar that is opened as "r+b"/"rb " or under a name that is not
   * *.jar (or with the feature switched off) looks exactly like a healthy
   * run: zero [jarmem] lines and no error anywhere. */
  if (mode != NULL && vita_jar_name(filename)) {
    if (mode[0] == 'r' && mode[1] == 'b' && mode[2] == 0) {
      if (vita_jarmem_enabled()) {
        g_jar_stat.open_call++;
        OsFile_Handle jh = vita_jar_open(filename);
        if (jh != NULL) {
          return jh;
        }
      } else {
        g_jar_stat.bypass++;
      }
    } else {
      /* A *.jar opened for write (romizer) or with a weird mode: expected
       * to be rare. Counted so an unexpected mode is visible. */
      g_jar_stat.bypass++;
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
