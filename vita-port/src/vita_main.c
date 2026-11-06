/*
 * vita_main.c - PS Vita entry point for phoneME MIDP (J2ME games)
 *
 * Based on midp-vita/src/main.c. Changes:
 *   - display/input are owned by vita_display.c / vita_input.c (no local
 *     framebuffer setup here)
 *   - reads ux0:/data/J2ME00001/launch.cfg to run any game jar:
 *       line 1: absolute path of the game jar (default: Hello.jar)
 *       line 2: MIDlet class name        (default: HelloMIDlet)
 *       line 3: orientation portrait|landscape (default: portrait)
 *       line 4: optional jit=0|1|2 (default 0 = interpreter only; see the
 *               VITA_JIT_DEFAULT comment below)
 *   - matching internal.config (320x240 or 240x320) copied to ux0 at startup
 */

#include <stdio.h>
#include <stdarg.h> /* v02.44: vita_timing_markf */
#include <stdlib.h>
/* v04.03: mallinfo() - the arena figures printed by menu_probe(). newlib
 * declares it in <malloc.h>; the call is cheap (no allocation, just the
 * allocator's own counters). */
#include <malloc.h>
#include <string.h>

#include <unistd.h>
#include <pthread.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/dirent.h> /* v02.x: sceIoDopen/sceIoDclose (VitaSDK 15+) */
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h> /* v01.72: sceKernelGetThreadId */

#include "vita_watchdog.h" /* v01.72: hang forensics */
#include "vita_kernel_forensics.h" /* v02.20: boot-time kubridge precheck */
#include "vita_trace.h"    /* v02.27: syscall deadpoint tracer */
#include "vita_lifecycle.h" /* v04.32: suspend/resume (audio port + clock) */
#include "vita_motion.h"    /* v04.38: six-axis samples for JSR-256 */

/* MIDP headers */
#include <midpAMS.h>
#include <midp_properties_port.h>

/* Heap parameters */
#include <heap.h>

/* v03.68: microedition.locale is a required MIDP 2.0 property, but forcing a
 * default value changed 极品飞车's behaviour for the worse (see
 * system_cfg_override()). It is therefore NOT written unless game.cfg asks for
 * it with a `locale=` line. Kept as a named constant so the build can override
 * it with -DVITA_DEFAULT_LOCALE=... for experiments. */
#ifndef VITA_DEFAULT_LOCALE
#define VITA_DEFAULT_LOCALE "en-US"
#endif

extern int runMidlet(int argc, char **argv);

#include "vita_menu.h"
#include "vita_jit.h"      /* v02.94: per-round JIT policy + probe */
#include "vita_storage.h"
#include "vita_version.h"
#include "vita_crumb.h"
#include "vita_3d.h"
#include "vita_perf.h" /* v02.97: 444 MHz clock bump + frame timing (P1-3) */
#include "vita_jitmem.h" /* v03.71: executable-memory probe for the JIT */
#include "vita_splash.h" /* v02.99: startup loading screen (P1-2) */
#include "vita_diag.h"   /* v03.70: on-screen diagnostics overlay (P1-8) */

/* log37..log42 verdict: pinning NEVER changed the freeze - unpinned
 * (log1..log36), CPU0 (log37) and CPU1 (log38..log42) all froze with
 * the SAME signature, and each pin merely chose WHICH core stalls (the
 * stalled core follows the VM). v02.18 therefore defaults to UNPINNED
 * (mask 0 = kernel default 3-core mask) and adds a runtime switch
 * ux0:/data/J2ME00001/vm_cpu so a pin can still be requested for an
 * A/B round WITHOUT a rebuild:
 *   '0' or file absent = unpinned (0x70000 user cores, default)
 *   '1'                = pin to CPU1 (0x20000, the v02.13..02.17 behaviour)
 *   '2'                = pin to CPU2 (0x40000)
 * The pin still happens BEFORE any VM code runs.
 *   bit masks: 0x10000 = CPU0, 0x20000 = CPU1, 0x40000 = CPU2 */
#ifndef VITA_VM_CPU_AFFINITY_MASK
#define VITA_VM_CPU_AFFINITY_MASK 0
#endif

/* Baked in by build_jar.sh -> cmake (HELLO_JAR_SIZE): size of the
 * Hello.jar bundled in the VPK, used for the runtime copy check. */
#ifndef HELLO_JAR_SIZE
#define HELLO_JAR_SIZE 0
#endif

/* from vita_display.c: must run before the VM starts */
extern void vita_display_set_orientation(int landscape);
extern void vita_display_set_size(int w, int h);
extern void vita_display_set_scale_mode(int mode);
extern int vita_display_get_width(void);
extern int vita_display_get_height(void);

/* v02.85: on-screen virtual keypad. The launcher only decides whether it
 * is on; drawing and hit testing live in vita_vkey.c / vita_input.c. */
extern int vita_vkey_load(const char *path);
extern void vita_vkey_set_enabled(int on);
extern int vita_vkey_enabled(void);
extern int vita_vkey_mode(void);

/* v02.87: Display.vibrate() backend (src/vita_vibrate.c). default on. */
extern void vita_vibrate_set_allowed(int allowed);

/* v02.90: MIDP permission policy (src/vita_permissions.c) */
extern void vita_permissions_set_mode(int mode);
extern int  vita_permissions_mode(void);
#define VITA_PERMS_RELAXED 0
#define VITA_PERMS_STRICT  1

/* Scale modes - keep in sync with vita_display.c */
#define VITA_SCALE_FIT     0
#define VITA_SCALE_STRETCH 1
#define VITA_SCALE_INTEGER 2

/* from vita_net.c: sceNet stack init; must run before the VM starts so
 * the first socket call from a MIDlet does not hit an idle stack */
extern void vita_net_early_init(void);

/* vitaSDK newlib: the default malloc arena is 32MB, which is too small for
 * the Java heap on top of everything else (the VM refused to start with
 * "Could not allocate VM heap"). This overrides the weak default; the app
 * memory partition is 128MB (see CMakeLists MEMSIZE).
 * v01.28: rolled 96MB back to 64MB together with the 32MB Java heap -
 * the 48MB/96MB pair (v01.27) hung the VM during bootstrap on first UC
 * launch. Keep both values in sync when re-raising. */
unsigned int _newlib_heap_size_user = 64 * 1024 * 1024;

#define DATA_DIR "ux0:/data/J2ME00001"
#define CFG_PATH DATA_DIR "/launch.cfg"

/* v01.58: JIT policy handed to the MIDP VM. runMidlet.c reads this through
 * the weak symbol of the same name (see its "-int" block). launch.cfg may
 * set it on line 4 as "jit=0|1|2":
 *   0 = interpreter on every round   (default, the v01.46 behaviour)
 *   1 = JIT on round 1 only, interpreter from round 2 on
 *   2 = JIT on every round           (reproduces the round-2 crash)
 * JIT on round 2+ is exactly what killed UC on device (faulting PC inside
 * the heap compiler area, PROJECT_MEMORY v01.45 follow-up). Until that root
 * cause is closed the default keeps the whole JIT path off; jit=1 recovers
 * the round-1 performance that was the norm before v01.46. */
/* v04.52 (REVERTED reasoning, kept as history so nobody re-derives it):
 * I briefly made launch.cfg "jit=" unreadable and blamed the log191 freeze on
 * the JIT compiling large methods. THAT WAS WRONG, and the log says so:
 *
 *   round 0: jit cfg=2 -> policy=2
 *   jit: round 0 policy 2 -> restored UseCompiler=1 area=20%
 *   vm UseCompiler(pre)=1 (jit policy=2)
 *   [heapmem] chunk0 needs 33554433 bytes ... -> malloc
 *   vm UseCompiler(post)=0 <- actual round mode
 *   jit: round 0 done: UseCompiler=0 compile-requests=0 produced-code=0
 *
 * `restored UseCompiler=1` is only what the policy ASKED for. What actually
 * ran is the `(post)` line: 0, with compile-requests=0 and produced-code=0.
 * The JIT never compiled a single byte in this session (and the same holds for
 * rounds 1 and 2: post=0, 0 requests). So the freeze in log191 was NOT the
 * compiler, and gating launch.cfg would have changed nothing except taking
 * away a working opt-in.
 *
 * The real event on those lines is the HEAP: 32 MiB cannot be satisfied from a
 * VM-domain block (they cap at 16 MiB) and malloc cannot hand out 32 MiB
 * contiguously either, so the round runs with a degraded heap and UseCompiler
 * is forced to 0 by the VM itself. That is the thread to pull, not the JIT
 * policy. */
#define VITA_JIT_DEFAULT 0
int vita_jit_policy = VITA_JIT_DEFAULT;

/* The VM's own flag (Globals.hpp:231, product(bool, UseCompiler, true)).
 * It is a plain C++ global with an unmangled name; declared here as a byte
 * purely so the launcher can log whether the JIT really is enabled this
 * round. Weak, so a build without the compiler subsystem still links. */
extern unsigned char UseCompiler __attribute__((weak));

/* v04.18: set by the VM's OsMemory_allocate_chunk when the Java heap chunk was
 * backed by VM-domain (executable) memory. Weak for the same reason: a VM built
 * without that path must not break the launcher, and an absent symbol resolves
 * to 0 = "not executable", which is the safe answer. */
extern int vita_heap_executable(void) __attribute__((weak));

static int vita_jit_heap_is_executable(void) {
    if (&vita_heap_executable == NULL) {
        return 0;   /* VM built without the VM-domain heap path */
    }
    return vita_heap_executable() != 0;
}

/* v04.33: the jar running this round, so read_jit_policy() can honour a
 * per-game "jit=" in game.cfg. Set right after the menu / launch.cfg decided
 * which jar to run; on the very first round of a session it is still empty,
 * so that round uses launch.cfg (and the game.cfg value takes over from the
 * next round / next launch of the same game). Keeping the decision here
 * instead of moving the whole JIT block after the menu is deliberate: the
 * JIT policy must be fixed before the VM starts, and this way nothing about
 * the JIT ordering changes. */
static char g_jit_jar[256];

/* Returns the game.cfg "jit=" value, or -1 when unset/unreadable. */
static int read_game_jit(void) {
    char path[320];
    char buf[256];
    const char *slash;
    SceUID fd;
    int n;

    if (g_jit_jar[0] == '\0') {
        return -1;
    }
    slash = strrchr(g_jit_jar, '/');
    if (slash == NULL) {
        return -1;
    }
    snprintf(path, sizeof(path), "%.*s/game.cfg",
             (int)(slash - g_jit_jar), g_jit_jar);
    fd = sceIoOpen(path, SCE_O_RDONLY, 0);
    if (fd < 0) {
        return -1;
    }
    memset(buf, 0, sizeof(buf));
    n = sceIoRead(fd, buf, sizeof(buf) - 1);
    sceIoClose(fd);
    if (n <= 0) {
        return -1;
    }
    buf[n] = '\0';
    {
        char *p = buf;
        while ((p = strstr(p, "jit=")) != NULL) {
            if ((p == buf || p[-1] == '\n') && p[4] >= '0' && p[4] <= '2') {
                return p[4] - '0';
            }
            p += 4;
        }
    }
    return -1;
}

/* Read the optional "jit=N" setting from launch.cfg. Missing or malformed
 * values leave the default in place. A per-game game.cfg entry wins (see
 * read_game_jit above).
 *
 * v04.52: restored to its previous behaviour. I had briefly stopped reading
 * launch.cfg while chasing the log191 freeze under the wrong theory that the
 * JIT was compiling; the log proves it compiled nothing (UseCompiler(post)=0,
 * compile-requests=0 on every round). The opt-in works and costs nothing when
 * unused, so it stays. */
static int read_jit_policy(void) {
    int policy = VITA_JIT_DEFAULT;
    int per_game = read_game_jit();
    SceUID fd;

    if (per_game >= 0) {
        return per_game;
    }
    fd = sceIoOpen(CFG_PATH, SCE_O_RDONLY, 0);
    if (fd >= 0) {
        static char buf[512];
        memset(buf, 0, sizeof(buf));
        if (sceIoRead(fd, buf, sizeof(buf) - 1) > 0) {
            char *p = strstr(buf, "jit=");
            if (p != NULL && p[4] >= '0' && p[4] <= '2') {
                policy = p[4] - '0';
            }
        }
        sceIoClose(fd);
    }
    return policy;
}

/* v02.18: runtime pin switch. File absent/unreadable = compiled-in
 * default (0 = unpinned). '0' = unpinned, '1' = CPU1, '2' = CPU2,
 * anything else = leave the compiled default. Runs BEFORE the VM so the
 * pin ordering of the v02.13..02.17 experiments is preserved. */
static unsigned int read_vm_cpu_mask(void) {
    SceUID fd = sceIoOpen(DATA_DIR "/vm_cpu", SCE_O_RDONLY, 0);
    char c = 0;
    if (fd >= 0) {
        if (sceIoRead(fd, &c, 1) == 1) {
            sceIoClose(fd);
            if (c == '0') return 0;            /* explicitly unpinned  */
            if (c == '1') return 0x20000;      /* CPU1 (02.13..02.17)  */
            if (c == '2') return 0x40000;      /* CPU2                 */
            return VITA_VM_CPU_AFFINITY_MASK;  /* malformed: compiled  */
        }
        sceIoClose(fd);
    }
    return VITA_VM_CPU_AFFINITY_MASK;         /* absent: compiled dflt */
}

static FILE *g_log = NULL;

static void dlog(const char *s) {
    fprintf(stderr, "%s", s);
    if (g_log == NULL) {
        g_log = fopen(DATA_DIR "/boot_log.txt", "w");
    }
    if (g_log != NULL) {
        fprintf(g_log, "%s", s);
        fflush(g_log);
    }
}

static void dlog_str(const char *prefix, const char *s) {
    dlog(prefix);
    dlog(s);
    dlog("\n");
}

/* v02.44: launch-timing marks -> ux0:/data/J2ME00001/boot_timing.log
 * (append, one open/write/close per line so a kill loses nothing).
 * Clock = sceKernelGetProcessTimeWide() (us since process start), the same
 * clock as the "[+N us]" prefix in vm_boot.log, so the launcher marks, the
 * VM boot stages and the display marks (vita_display.c) line up on one
 * timeline. Answers "how long is the white screen and where does it go":
 *   round_begin        menu selection handed over to the launcher
 *   runMidlet_enter    just before the VM starts
 *   first_check_events first time the VM comes back to the event pump
 *   first_flip         first MIDlet frame submitted (may itself be blank)
 *   first_content_flip first frame that is NOT all white (vita_display.c)
 *   runMidlet_exit     round over
 * Rare (about 6 lines per round): no measurable cost. */
volatile int vita_timing_round = 0;
volatile int vita_timing_want_first_ce = 0;
extern void vita_display_timing_reset(void);

void vita_timing_markf(const char *fmt, ...) {
    char buf[200];
    int n;
    va_list ap;
    SceUID fd;

    n = snprintf(buf, sizeof(buf), "[+%llu us] round=%d ",
                 (unsigned long long)sceKernelGetProcessTimeWide(),
                 (int)vita_timing_round);
    if (n < 0 || n >= (int)sizeof(buf) - 2) {
        return;
    }
    va_start(ap, fmt);
    n += vsnprintf(buf + n, sizeof(buf) - (size_t)n - 2, fmt, ap);
    va_end(ap);
    if (n < 0) {
        return;
    }
    if (n > (int)sizeof(buf) - 2) {
        n = (int)sizeof(buf) - 2;
    }
    buf[n++] = '\n';
    fd = sceIoOpen(DATA_DIR "/boot_timing.log",
                   SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0777);
    if (fd >= 0) {
        sceIoWrite(fd, buf, (SceSize)n);
        sceIoClose(fd);
    }
}

/* Copy a file via sceIo (used to seed the writable data dir from app0:).
 * IMPORTANT: remove the destination first instead of relying on
 * SCE_O_TRUNC - Vita3K does not honor O_TRUNC, so copying a SHORTER
 * file over a longer one leaves a stale tail. The corrupted length
 * then fails JarFileParser's EOCD length check (endpos+22+com==len)
 * and EVERY class in the jar throws ClassNotFoundException
 * (v01.16-v01.18 "CNFE for all classes" root cause; caught by the
 * "runtime Hello.jar size=X (expected Y)" boot-log check). */
static void copy_file(const char *src, const char *dst) {
    SceUID in = sceIoOpen(src, SCE_O_RDONLY, 0);
    if (in < 0) {
        return;
    }
    sceIoRemove(dst); /* ignore error - may not exist */
    SceUID out = sceIoOpen(dst, SCE_O_WRONLY | SCE_O_CREAT, 0777);
    if (out >= 0) {
        static char buf[4096];
        ssize_t n;
        while ((n = sceIoRead(in, buf, sizeof(buf))) > 0) {
            sceIoWrite(out, buf, n);
        }
        sceIoClose(out);
    }
    sceIoClose(in);
}

/* Read launch.cfg:
 *   line 1: game jar path RELATIVE to the data dir (NOT absolute!)
 *   line 2: MIDlet class name
 *   line 3: optional orientation "portrait" / "landscape" (default portrait)
 *   any line starting with "menu=0" -> *skip_menu = 1 (skip the native menu)
 * Returns 0 when jar+class were read.
 *
 * The jar path MUST be relative: main chdir()s to DATA_DIR and the VM
 * classpath is ':'-separated, so an "ux0:/data/..." entry would be cut
 * at the colon into "ux0" + "/data/..." (see the chdir note in main). */
static int read_launch_cfg(char *jar, size_t jar_sz,
                           char *cls, size_t cls_sz,
                           char *orient, size_t orient_sz,
                           int *skip_menu) {
    /* v02.16: parse into locals and copy out only on SUCCESS. The old
     * code wrote jar/cls as it parsed, so a jit-only launch.cfg
     * ("jit=0", written by the menu's JIT setting) left jar="jit=0"
     * behind even though the function returned -1 - and the caller's
     * "fall back to the bundled defaults" path then tried to load a jar
     * literally named "jit=0". */
    char tjar[256];
    char tcls[128];
    char torient[32];
    SceUID fd;

    if (skip_menu != NULL) {
        *skip_menu = 0;
    }
    tjar[0] = '\0';
    tcls[0] = '\0';
    snprintf(torient, sizeof(torient), "portrait");

    fd = sceIoOpen(CFG_PATH, SCE_O_RDONLY, 0);
    if (fd < 0) {
        return -1;
    }
    static char buf[512];
    memset(buf, 0, sizeof(buf));
    (void)sceIoRead(fd, buf, sizeof(buf) - 1);
    sceIoClose(fd);

    /* v02.16 (log40 A/B infra): optional line-start "menu=0" -> the
     * caller skips the native menu and goes straight to this cfg's
     * jar/class. Scanned on the raw buffer, BEFORE the line parsing
     * below turns newlines into NULs. Keeping the menu out of the run
     * means no 2 MB framebuffer alloc / touch init / filesystem scan
     * can be blamed for a hang. */
    if (skip_menu != NULL) {
        char *p = buf;
        while (p != NULL && *p != '\0') {
            if (strncmp(p, "menu=0", 6) == 0) {
                *skip_menu = 1;
                break;
            }
            p = strpbrk(p, "\r\n");
            if (p != NULL) {
                p += strspn(p, "\r\n");
            }
        }
    }

    /* line 1: jar path */
    char *nl = strpbrk(buf, "\r\n");
    if (nl == NULL) {
        return -1;
    }
    *nl = '\0';
    snprintf(tjar, sizeof(tjar), "%s", buf);

    /* line 2: class name */
    char *p2 = nl + 1 + strspn(nl + 1, "\r\n");
    char *nl2 = strpbrk(p2, "\r\n");
    if (nl2 != NULL) {
        *nl2 = '\0';
    }
    snprintf(tcls, sizeof(tcls), "%s", p2);
    if (tjar[0] == '\0' || tcls[0] == '\0') {
        return -1;
    }

    /* line 3 (optional): orientation */
    if (nl2 != NULL) {
        char *p3 = nl2 + 1 + strspn(nl2 + 1, "\r\n");
        char *nl3 = strpbrk(p3, "\r\n");
        if (nl3 != NULL) {
            *nl3 = '\0';
        }
        if (p3[0] != '\0') {
            snprintf(torient, sizeof(torient), "%s", p3);
        }
    }

    snprintf(jar, jar_sz, "%s", tjar);
    snprintf(cls, cls_sz, "%s", tcls);
    snprintf(orient, orient_sz, "%s", torient);
    return 0;
}

/* ------------------------------------------------------------------
 * v02.82: per-game display settings.
 * v02.84: that struct grew into the whole per-game configuration.
 *
 * game.cfg lives next to game.jar and is written by the menu. Format is
 * key=value so fields can be added without reordering:
 *
 *   class=com.example.Main        ('-' = auto-detect)
 *   orient=portrait|landscape
 *   width=240                     virtual screen size (optional)
 *   height=320
 *   scale=fit|stretch|integer     (optional, default fit)
 *   profile=generic               device identity (see profiles below)
 *   locale=zh-CN                  microedition.locale (optional)
 *   midlet=1                      1..N entry picked from MIDlet-N
 *   jad=auto|off                  use <dir>/game.jad when present
 *
 * The pre-02.82 two-line form (class, orientation) is still accepted:
 * a line without '=' is treated positionally.
 *
 * Why the size is fixed here and not in Java: Canvas.getWidth() comes
 * from lfjport_get_screen_width(), i.e. from this file, and the Java UI
 * reads it once at init. Why the identity is fixed here: MIDP exposes
 * System.getProperty() from system.config (application properties), so
 * the profile has to be written into that file before the VM reads it.
 * ------------------------------------------------------------------ */
typedef struct {
    int width;
    int height;
    int scale_mode;
    const char *profile;   /* points into the built-in table */
    char locale[16];       /* "" = keep the template value */
    int use_jad;           /* 1 = pass the .jad to runMidlet */
    int vkey;              /* -1 = not stated, 0/1 = force off/on */
    int vibrate;           /* -1 = not stated, 0/1 = force off/on */
    int perms;             /* -1 = not stated, 0 = relaxed, 1 = strict */
} VitaGameConfig;

/* ---- device profiles (v02.84) ----
 * A game decides which code path to take from microedition.platform and
 * device.manufacturer: Nokia games use com.nokia.*, Sony Ericsson games
 * check for "SonyEricsson", and several Chinese games special-case the
 * platform string. The default ("generic") keeps the historical values
 * (platform "j2me") so existing behaviour does not change.
 *
 * The strings mirror what the real devices reported; the exact firmware
 * suffix is what games grep for, so it is kept verbatim.
 * size_w/size_h is only a *default* for games that have no explicit
 * width/height in game.cfg. */
typedef struct {
    const char *name;          /* game.cfg value */
    const char *label;         /* menu label */
    const char *platform;
    const char *manufacturer;
    const char *model;
    const char *version;
    const char *profiles;      /* microedition.profiles */
    int size_w, size_h;
} VitaProfile;

static const VitaProfile vita_profiles[] = {
    { "generic", "generic (PS Vita)",
      "j2me", "Generic", "J2ME", "1.0", "MIDP-2.1", 240, 320 },
    { "nokia6230", "Nokia 6230",
      "Nokia6230/2.0 (03.15) Profile/MIDP-1.0 Configuration/CLDC-1.0",
      "Nokia", "6230", "03.15", "MIDP-1.0", 128, 128 },
    { "nokia_n95", "Nokia N95",
      "NokiaN95/11.0.026; Series60/3.1 Profile/MIDP-2.0 "
      "Configuration/CLDC-1.1",
      "Nokia", "N95", "11.0.026", "MIDP-2.0", 240, 320 },
    { "se_k800", "Sony Ericsson K800i",
      "SonyEricssonK800i/R1CB Browser/NetFront/3.3 Profile/MIDP-2.0 "
      "Configuration/CLDC-1.1",
      "Sony Ericsson", "K800i", "R1CB", "MIDP-2.0", 240, 320 },
    { "samsung_e250", "Samsung SGH-E250",
      "SAMSUNG-SGH-E250/1.0 Profile/MIDP-2.0 Configuration/CLDC-1.1",
      "Samsung", "SGH-E250", "1.0", "MIDP-2.0", 128, 160 },
    { "siemens_c65", "Siemens C65",
      "SIE-C65/00 Profile/MIDP-1.0 Configuration/CLDC-1.0",
      "Siemens", "C65", "00", "MIDP-1.0", 130, 130 }
};
#define VITA_PROFILE_N \
    ((int)(sizeof(vita_profiles) / sizeof(vita_profiles[0])))
#define VITA_PROFILE_DEFAULT "generic"

static const VitaProfile *profile_by_name(const char *name) {
    int i;
    if (name != NULL) {
        for (i = 0; i < VITA_PROFILE_N; i++) {
            if (strcmp(vita_profiles[i].name, name) == 0) {
                return &vita_profiles[i];
            }
        }
    }
    return &vita_profiles[0];
}

static void game_config_defaults(VitaGameConfig *d) {
    d->width = 240;
    d->height = 320;
    d->scale_mode = VITA_SCALE_FIT;
    d->profile = VITA_PROFILE_DEFAULT;
    d->locale[0] = '\0';
    d->use_jad = 1;            /* auto: only used when a valid .jad exists */
    d->vkey = -1;              /* follow vkey.cfg */
    d->vibrate = -1;           /* follow the built-in default (on) */
    d->perms = -1;             /* follow the built-in default (relaxed) */
}

/* ------------------------------------------------------------------
 * v02.86: global defaults (ux0:/data/J2ME00001/defaults.cfg).
 *
 * The menu's 设置 page writes this file; every entry can still be
 * overridden by the per-game game.cfg, which is more specific. Keeping
 * it separate from menu.cfg matters because the LAUNCHER has to read it
 * (menu.cfg is UI state: list/grid, settings cursor).
 *
 * Only keys that make sense for "every game" live here: device profile,
 * scale mode, virtual keypad, vibrate, volume. Screen size deliberately
 * does not - that is per game by nature.
 * ------------------------------------------------------------------ */
#define DEFAULTS_CFG DATA_DIR "/defaults.cfg"

static void defaults_read(VitaGameConfig *d) {
    char buf[512];
    SceUID fd;
    int n;
    const char *p;

    game_config_defaults(d);
    /* defaults are read on top of the built-in values, so width/height
     * stay 0 = "let the profile decide" - see the note above. */
    d->width = 0;
    d->height = 0;

    fd = sceIoOpen(DEFAULTS_CFG, SCE_O_RDONLY, 0);
    if (fd < 0) {
        return;
    }
    memset(buf, 0, sizeof(buf));
    n = sceIoRead(fd, buf, sizeof(buf) - 1);
    sceIoClose(fd);
    if (n <= 0) {
        return;
    }
    buf[n] = '\0';

    for (p = buf; *p != '\0'; ) {
        const char *nl = strpbrk(p, "\r\n");
        const char *end = (nl != NULL) ? nl : p + strlen(p);
        int len = (int)(end - p);
        char line[160];

        if (len > 0 && len < (int)sizeof(line)) {
            memcpy(line, p, (size_t)len);
            line[len] = '\0';
            if (line[0] != '#') {
                char *eq = strchr(line, '=');
                if (eq != NULL) {
                    char *k = line;
                    char *v = eq + 1;
                    char *ke = eq;
                    char *ve;
                    while (*k == ' ' || *k == '\t') k++;
                    while (ke > k && (ke[-1] == ' ' || ke[-1] == '\t')) ke--;
                    *ke = '\0';
                    while (*v == ' ' || *v == '\t') v++;
                    ve = v + strlen(v);
                    while (ve > v && (ve[-1] == ' ' || ve[-1] == '\t')) {
                        *--ve = '\0';
                    }
                    if (strcmp(k, "profile") == 0) {
                        d->profile = profile_by_name(v)->name;
                    } else if (strcmp(k, "scale") == 0) {
                        d->scale_mode = (strcmp(v, "stretch") == 0) ? 1 :
                                        (strcmp(v, "integer") == 0) ? 2 : 0;
                    } else if (strcmp(k, "vkey") == 0) {
                        d->vkey = (strcmp(v, "off") == 0 ||
                                   strcmp(v, "0") == 0) ? 0 : 1;
                    } else if (strcmp(k, "locale") == 0) {
                        snprintf(d->locale, sizeof(d->locale), "%s", v);
                    } else if (strcmp(k, "vibrate") == 0) {
                        d->vibrate = (strcmp(v, "off") == 0 ||
                                      strcmp(v, "0") == 0) ? 0 : 1;
                    } else if (strcmp(k, "perms") == 0) {
                        /* v02.90: MIDP permission policy. "strict" runs
                         * phoneME's own tables (an unsigned jar then gets
                         * ONESHOT/SESSION defaults and can be prompted);
                         * anything else is the permissive default. */
                        d->perms = (strcmp(v, "strict") == 0) ? 1 : 0;
                    }
                }
            }
        }
        if (nl == NULL) {
            break;
        }
        p = nl + 1;
    }
}

static int cfg_int(const char *s, int def) {
    int v = 0, digits = 0;
    while (*s == ' ' || *s == '\t') s++;
    while (*s >= '0' && *s <= '9') {
        v = v * 10 + (*s - '0');
        s++;
        digits++;
        if (v > 4096) return def;   /* nonsense */
    }
    return digits ? v : def;
}

/* Build "<dir of jar>/game.cfg" into out. Returns 0 when the jar path
 * has no directory part (bundled Hello.jar ships no game.cfg). */
static int game_cfg_path_for_jar(const char *jar, char *out, size_t outsz) {
    const char *slash = strrchr(jar, '/');
    size_t n;
    if (slash == NULL) {
        return -1;
    }
    n = (size_t)(slash - jar);
    if (n == 0 || n + 10 >= outsz) {
        return -1;
    }
    memcpy(out, jar, n);
    snprintf(out + n, outsz - n, "/game.cfg");
    return 0;
}

/* Find <dir>/game.jad or <dir>/<jarbase>.jad next to the jar. Fills out
 * and returns 1 when it exists. Only checks existence here - the menu
 * decides whether the content is usable (needs a MIDlet-1 line). */
static int find_jad_for_jar(const char *jar, char *out, size_t outsz) {
    const char *slash = strrchr(jar, '/');
    const char *base;
    const char *dot;
    char dir[300];
    size_t n;
    SceIoStat st;

    if (slash == NULL) {
        return 0;
    }
    n = (size_t)(slash - jar);
    if (n == 0 || n + 10 >= sizeof(dir)) {
        return 0;
    }
    memcpy(dir, jar, n);
    dir[n] = '\0';

    snprintf(out, outsz, "%s/game.jad", dir);
    if (sceIoGetstat(out, &st) >= 0) {
        return 1;
    }
    base = slash + 1;
    dot = strrchr(base, '.');
    if (dot != NULL && dot > base) {
        snprintf(out, outsz, "%s/%.*s.jad", dir, (int)(dot - base), base);
        if (sceIoGetstat(out, &st) >= 0) {
            return 1;
        }
    }
    return 0;
}

static int game_config_read(const char *jar, const char *orient,
                            const VitaGameConfig *defs, VitaGameConfig *d) {
    char path[300];
    char buf[1024];
    SceUID fd;
    int n, positional = 0;
    int have_w = 0, have_h = 0;
    int cfg_landscape = -1;   /* -1 = not stated in game.cfg */
    const char *p;

    /* v02.86: start from the global defaults (defaults.cfg), then let
     * game.cfg override field by field. */
    if (defs != NULL) {
        *d = *defs;
    } else {
        game_config_defaults(d);
    }
    d->width = 0;
    d->height = 0;

    if (game_cfg_path_for_jar(jar, path, sizeof(path)) != 0) {
        path[0] = '\0';
    }
    fd = (path[0] != '\0') ? sceIoOpen(path, SCE_O_RDONLY, 0) : -1;
    if (fd < 0) {
        /* No game.cfg: orientation + profile default size only. */
        const VitaProfile *pr = profile_by_name(d->profile);
        int land = (orient != NULL && strcmp(orient, "landscape") == 0);
        d->width = land ? pr->size_h : pr->size_w;
        d->height = land ? pr->size_w : pr->size_h;
        return 0;
    }
    memset(buf, 0, sizeof(buf));
    n = sceIoRead(fd, buf, sizeof(buf) - 1);
    sceIoClose(fd);
    if (n <= 0) {
        return 0;
    }
    buf[n] = '\0';

    for (p = buf; *p != '\0'; ) {
        const char *nl = strpbrk(p, "\r\n");
        const char *end = (nl != NULL) ? nl : p + strlen(p);
        int len = (int)(end - p);
        char line[200];

        if (len > 0 && len < (int)sizeof(line)) {
            memcpy(line, p, (size_t)len);
            line[len] = '\0';
            if (line[0] != '#' && line[0] != '\0') {
                char *eq = strchr(line, '=');
                if (eq != NULL) {
                    char *k = line;
                    char *v = eq + 1;
                    char *ke = eq;
                    char *ve;
                    while (*k == ' ' || *k == '\t') k++;
                    while (ke > k && (ke[-1] == ' ' || ke[-1] == '\t')) ke--;
                    *ke = '\0';
                    while (*v == ' ' || *v == '\t') v++;
                    /* values may carry trailing blanks ("width = 176 ") */
                    ve = v + strlen(v);
                    while (ve > v && (ve[-1] == ' ' || ve[-1] == '\t')) {
                        *--ve = '\0';
                    }
                    if (strcmp(k, "width") == 0) {
                        int iv = cfg_int(v, -1);
                        if (iv > 0) {
                            d->width = iv;
                            have_w = 1;
                        }
                    } else if (strcmp(k, "height") == 0) {
                        int iv = cfg_int(v, -1);
                        if (iv > 0) {
                            d->height = iv;
                            have_h = 1;
                        }
                    } else if (strcmp(k, "orient") == 0) {
                        /* explicit size wins: only apply the orientation
                         * when no width/height entry was seen yet */
                        if (strcmp(v, "landscape") == 0) {
                            cfg_landscape = 1;
                        } else if (strcmp(v, "portrait") == 0) {
                            cfg_landscape = 0;
                        }
                    } else if (strcmp(k, "scale") == 0) {
                        if (strcmp(v, "stretch") == 0) {
                            d->scale_mode = VITA_SCALE_STRETCH;
                        } else if (strcmp(v, "integer") == 0) {
                            d->scale_mode = VITA_SCALE_INTEGER;
                        } else {
                            d->scale_mode = VITA_SCALE_FIT;
                        }
                    } else if (strcmp(k, "profile") == 0) {
                        d->profile = profile_by_name(v)->name;
                    } else if (strcmp(k, "locale") == 0) {
                        snprintf(d->locale, sizeof(d->locale), "%s", v);
                    } else if (strcmp(k, "jad") == 0) {
                        d->use_jad = !(strcmp(v, "off") == 0 ||
                                       strcmp(v, "0") == 0);
                    } else if (strcmp(k, "vkey") == 0) {
                        d->vkey = (strcmp(v, "off") == 0 ||
                                   strcmp(v, "0") == 0) ? 0 : 1;
                    } else if (strcmp(k, "vibrate") == 0) {
                        d->vibrate = (strcmp(v, "off") == 0 ||
                                      strcmp(v, "0") == 0) ? 0 : 1;
                    } else if (strcmp(k, "perms") == 0) {
                        d->perms = (strcmp(v, "strict") == 0) ? 1 : 0;
                    }
                    /* class=/midlet= are consumed by the menu; ignored
                     * here - the launcher already has the class name. */
                } else {
                    /* legacy positional form: line 1 class, line 2 orient.
                     * Both values override the caller's orientation - the
                     * file is per game and therefore more specific than
                     * launch.cfg. */
                    if (positional == 1) {
                        if (strcmp(line, "landscape") == 0) {
                            cfg_landscape = 1;
                        } else if (strcmp(line, "portrait") == 0) {
                            cfg_landscape = 0;
                        }
                    }
                }
                positional++;
            }
        }
        if (nl == NULL) {
            break;
        }
        p = nl + 1;
    }

    /* Resolve the size. Explicit width/height always win; otherwise the
     * orientation decides, and the *profile* supplies the numbers (a
     * Nokia 6230 profile means 128x128 so a game written for that screen
     * lays itself out correctly without the user knowing the size). */
    if (!have_w || !have_h) {
        const VitaProfile *pr = profile_by_name(d->profile);
        int land;
        if (cfg_landscape >= 0) {
            land = cfg_landscape;
        } else {
            land = (orient != NULL && strcmp(orient, "landscape") == 0);
        }
        if (!have_w) {
            d->width = land ? pr->size_h : pr->size_w;
        }
        if (!have_h) {
            d->height = land ? pr->size_w : pr->size_h;
        }
        /* A half-specified size (only width=) keeps the given number and
         * derives the other from the profile's aspect ratio suffix - just
         * fall back to the profile's other dimension. */
    }
    if (d->width <= 0) d->width = 240;
    if (d->height <= 0) d->height = 320;
    return 1;
}

/* Copy the VPK's internal.config to the writable lib/ dir, rewriting the
 * two display-size properties. The static templates only cover 240x320
 * and 320x240; any other game.cfg size needs this. Written via
 * remove+create because Vita3K ignores SCE_O_TRUNC (v01.16 lesson) and a
 * shorter file with a stale tail breaks the property reader. */
static void seed_internal_config(int w, int h) {
    static char inbuf[8192];
    static char outbuf[8192];
    const char *src = "app0:/data/J2ME00001/lib/internal.config";
    SceUID fd;
    int n, i = 0, o = 0;

    fd = sceIoOpen(src, SCE_O_RDONLY, 0);
    if (fd < 0) {
        copy_file((w > h)
                      ? "app0:/data/J2ME00001/lib/internal.landscape.config"
                      : "app0:/data/J2ME00001/lib/internal.config",
                  DATA_DIR "/lib/internal.config");
        return;
    }
    memset(inbuf, 0, sizeof(inbuf));
    n = sceIoRead(fd, inbuf, sizeof(inbuf) - 1);
    sceIoClose(fd);
    if (n <= 0) {
        copy_file("app0:/data/J2ME00001/lib/internal.config",
                  DATA_DIR "/lib/internal.config");
        return;
    }
    inbuf[n] = '\0';

    while (i < n && o < (int)sizeof(outbuf) - 1) {
        int e = i;
        int len;
        while (e < n && inbuf[e] != '\n') e++;
        len = e - i;
        if (len > 0 &&
            (strncmp(inbuf + i, "microedition.display.width:", 27) == 0 ||
             strncmp(inbuf + i, "microedition.display.height:", 28) == 0)) {
            int is_w = (strncmp(inbuf + i, "microedition.display.width:", 27)
                        == 0);
            int k = snprintf(outbuf + o, sizeof(outbuf) - (size_t)o,
                             is_w ? "microedition.display.width: %d\n"
                                  : "microedition.display.height: %d\n",
                             is_w ? w : h);
            if (k > 0) o += k;
        } else {
            if (len > (int)sizeof(outbuf) - 1 - o) {
                len = (int)sizeof(outbuf) - 1 - o;
            }
            memcpy(outbuf + o, inbuf + i, (size_t)len);
            o += len;
            if (o < (int)sizeof(outbuf) - 1) {
                outbuf[o++] = '\n';
            }
        }
        i = (e < n) ? e + 1 : n;
    }
    outbuf[o] = '\0';

    sceIoRemove(DATA_DIR "/lib/internal.config");
    fd = sceIoOpen(DATA_DIR "/lib/internal.config",
                   SCE_O_WRONLY | SCE_O_CREAT, 0777);
    if (fd >= 0) {
        sceIoWrite(fd, outbuf, (SceSize)o);
        sceIoClose(fd);
    }
}

/* ------------------------------------------------------------------ */
/* v02.84: device identity.
 *
 * IMPORTANT: System.getProperty() in phoneME reads the *application*
 * property set, which comes from lib/system.config - NOT from
 * internal.config (that one only feeds getInternalProperty). The
 * template's "microedition.platform: j2me" is therefore the value every
 * MIDlet sees today; the "PS Vita" line in internal.config is dead for
 * application properties. A profile has to rewrite system.config.
 *
 * The template is read from app0: and copied line by line, replacing
 * the keys a profile owns and appending the ones it did not have. The
 * rest (Connector.protocolpath, ...) is preserved verbatim, so a future
 * template change does not need a code change.
 *
 * v02.90: the template no longer carries security.policyfile /
 * security.messagefile. Those pointed at _policy.txt and
 * _function_groups.txt, which are NOT in the VPK: this build uses the
 * hard-coded policy tables (src/vita_permissions.c) and nothing reads
 * these two properties, so they only sent readers looking for files that
 * do not exist.
 *
 * Replacement happens through remove+create: Vita3K ignores
 * SCE_O_TRUNC and a longer new value over a shorter old one would leave
 * a stale tail (v01.16 lesson). */
static int system_cfg_override(const VitaProfile *pr, const char *locale,
                               char *out, size_t outsz,
                               const char *in, int n) {
    static const char *keys[] = {
        "microedition.platform", "microedition.profiles",
        "microedition.locale", "device.manufacturer", "device.model",
        "device.version"
    };
    const char *vals[6];
    int used[6];
    int drop_locale = 0;
    const char *p;
    int i, o = 0;

    /* v03.91: `locale=-` REMOVES the microedition.locale line.
     *
     * Before this, "locale absent" could not be reproduced once the template
     * carried the key: system_cfg_override writes an override when the value is
     * non-NULL and copies the template line when it is NULL, so there was no
     * way to make the property genuinely missing. That matters because the
     * v03.68 note below records that the presence of this one property changes
     * which of the title's two paint paths runs, and every observation of the
     * 极品飞车 IDIOMA screen being correct (x=64, fully on canvas) was made on a
     * run where the property was missing.
     *
     * With this sentinel the A/B needs no rebuild:
     *     locale=-        in game.cfg / defaults.cfg  -> property MISSING
     *     locale=en-US    (or any value)              -> property set
     * A single '-' cannot be a real locale, so it cannot collide with a
     * legitimate value. */
    if (locale != NULL && locale[0] == '-' && locale[1] == '\0') {
        drop_locale = 1;
        locale = NULL;
    }
    (void)n;   /* the buffer is NUL-terminated by the caller */
    vals[0] = pr->platform;
    vals[1] = pr->profiles;
    /* v03.68: REVERTED the v03.66 forced default. microedition.locale is a
     * required MIDP 2.0 property, so defaulting it looked like a pure spec fix,
     * but it is NOT free:
     *
     *   log154 (v03.63, locale absent)  极品飞车 painted a complete frame and
     *                                   blitted it: "drawImage 240x320 d=0,0"
     *                                   appears twice, 6 text rows drawn.
     *   log155 (v03.67, locale="en-US") the same title draws its panel and only
     *                                   3 of its 6 text rows, then stops; the
     *                                   full-surface blit NEVER happens, so the
     *                                   screen keeps showing the splash logo
     *                                   that was blitted last. The user's report
     *                                   "stuck at the splash logo, the blue
     *                                   screen has not even appeared" matches.
     *   The layout really did change: the same glyphs moved from x=310 to
     *   x=314. And only 54 drawRegion calls were logged in that run against a
     *   200 budget, so this is not my instrumentation cutting the frame short.
     *
     * ab.paint() has two paths, and the value of the property decides which one
     * runs: with `d` true it draws straight to the screen and returns, with `d`
     * false it draws into an off-screen Image and then blits it. The title
     * reaches that state differently once it believes it knows the language.
     *
     * The user's own comparison settles the intent question: J2ME Loader ALSO
     * shows this IDIOMA chooser, so the chooser is a normal first-run screen and
     * the locale is not needed to get past it. Therefore the property is left
     * ABSENT by default again (previous behaviour), and game.cfg's `locale=`
     * still sets it for anyone who wants the other path. */
    vals[2] = (locale != NULL && locale[0] != '\0') ? locale : NULL;
    vals[3] = pr->manufacturer;
    vals[4] = pr->model;
    vals[5] = pr->version;
    for (i = 0; i < 6; i++) {
        used[i] = 0;
    }

    for (p = in; p != NULL && *p != '\0' && o < (int)outsz - 1; ) {
        const char *nl = strchr(p, '\n');
        const char *end = (nl != NULL) ? nl : p + strlen(p);
        int len = (int)(end - p);
        int matched = -1;
        char line[256];

        if (len > 0 && len < (int)sizeof(line)) {
            memcpy(line, p, (size_t)len);
            line[len] = '\0';
            if (line[0] != '#') {
                for (i = 0; i < 6; i++) {
                    size_t kl = strlen(keys[i]);
                    if (strncmp(line, keys[i], kl) == 0 && line[kl] == ':') {
                        matched = i;
                        used[i] = 1;
                        break;
                    }
                }
            }
        }
        if (drop_locale && matched == 2) {
            /* v03.91: `locale=-` asked for the property to be MISSING, so the
             * template line is dropped instead of copied. `used[2]` is already
             * 1, so the "keys the template did not carry" loop below will not
             * re-add it either. */
        } else if (matched >= 0 && vals[matched] != NULL) {
            int k = snprintf(out + o, outsz - (size_t)o, "%s: %s\n",
                             keys[matched], vals[matched]);
            if (k > 0) o += k;
        } else if (len > 0 && len < (int)sizeof(line)) {
            if (len > (int)outsz - 1 - o) {
                len = (int)outsz - 1 - o;
            }
            memcpy(out + o, p, (size_t)len);
            o += len;
            if (o < (int)outsz - 1) out[o++] = '\n';
        }
        if (nl == NULL) break;
        p = nl + 1;
    }

    /* keys the template did not carry */
    for (i = 0; i < 6; i++) {
        if (!used[i] && vals[i] != NULL) {
            int k = snprintf(out + o, outsz - (size_t)o, "%s: %s\n",
                             keys[i], vals[i]);
            if (k > 0) o += k;
        }
    }
    if (o < (int)outsz) out[o] = '\0';
    return o;
}

/* v03.86: optional runtime property additions, read from ux0 .../gfx.cfg.
 *
 * Why this file has to exist: seed_system_config() rebuilds
 * DATA_DIR/lib/system.config from the app0 template on EVERY round, so a hand
 * edit to that file is silently thrown away - and app0 is inside the VPK, i.e.
 * read-only. Without a separate file there is NO way to switch a diagnostic on
 * without rebuilding, and an instruction that cannot be followed on the device
 * is worse than no instruction. Contents are appended VERBATIM (they are
 * ordinary "key: value" lines; the MIDP parser ignores '#' comments), so adding
 * a property needs no C change. Absent file = byte-for-byte previous behaviour.
 *
 * Current users: j2me.gfxdebug / j2me.gfxbudget (see Graphics.java, v03.86). */
static int append_gfx_cfg(char *out, size_t outsz, int o) {
    static char buf[1024];
    SceUID fd;
    int n, room;

    fd = sceIoOpen(DATA_DIR "/gfx.cfg", SCE_O_RDONLY, 0);
    if (fd < 0) {
        return o;
    }
    n = sceIoRead(fd, buf, sizeof(buf) - 1);
    sceIoClose(fd);
    if (n <= 0) {
        return o;
    }
    buf[n] = '\0';
    /* terminate the template's last line before appending */
    if (o > 0 && o < (int)outsz - 1 && out[o - 1] != '\n') {
        out[o++] = '\n';
    }
    room = (int)outsz - 1 - o;
    if (room <= 0) {
        return o;
    }
    if (n > room) {
        n = room;
    }
    memcpy(out + o, buf, (size_t)n);
    o += n;
    if (o > 0 && out[o - 1] != '\n' && o < (int)outsz - 1) {
        out[o++] = '\n';
    }
    out[o] = '\0';
    return o;
}

static void seed_system_config(const VitaProfile *pr, const char *locale) {
    static char inbuf[8192];
    static char outbuf[8192];
    SceUID fd;
    int n = -1, o, o2, gfx_added, wr = -1;

    memset(inbuf, 0, sizeof(inbuf));
    fd = sceIoOpen("app0:/data/J2ME00001/lib/system.config",
                   SCE_O_RDONLY, 0);
    if (fd >= 0) {
        n = sceIoRead(fd, inbuf, sizeof(inbuf) - 1);
        sceIoClose(fd);
    }
    if (n > 0) {
        inbuf[n] = '\0';
        o = system_cfg_override(pr, locale, outbuf, sizeof(outbuf), inbuf, n);
    } else {
        /* v03.87: the template could not be read. The v01.x fallback copied
         * the VPK file and RETURNED, which silently dropped gfx.cfg - i.e. the
         * one diagnostics switch a tester can set became a no-op exactly when
         * the config pipeline was already in question. Start from an empty
         * base instead so gfx.cfg still reaches System.getProperty(); the
         * fixed overrides (locale/platform) are lost in this branch, but they
         * were lost before too - copy_file() could never succeed either,
         * because it reads the same unreadable path. */
        outbuf[0] = '\0';
        o = 0;
    }
    /* v03.86: per-user diagnostics switches (ux0:.../gfx.cfg), appended
     * after the fixed overrides so a cfg line always wins. */
    o2 = append_gfx_cfg(outbuf, sizeof(outbuf), o);
    gfx_added = o2 - o;
    o = o2;

    if (o <= 0) {
        /* v03.87: log the outcome. log162 (Vita3K) asked for j2me.gfxdebug and
         * got null, and the log could not say whether the template was
         * unreadable, gfx.cfg was missing, or the write failed - three very
         * different bugs with the same symptom. One line settles it. */
        crumb_printf("seed: system.config tmpl=%d gfx=%d -> nothing to write",
                     n, gfx_added);
        return;
    }
    sceIoRemove(DATA_DIR "/lib/system.config");
    fd = sceIoOpen(DATA_DIR "/lib/system.config",
                   SCE_O_WRONLY | SCE_O_CREAT, 0777);
    if (fd >= 0) {
        wr = (int)sceIoWrite(fd, outbuf, (SceSize)o);
        sceIoClose(fd);
    }
    /* tmpl = template bytes read (-1 = open failed), gfx = bytes appended from
     * gfx.cfg, out = size of the file we tried to write, wr = bytes the kernel
     * reported writing (-1 = the open itself failed). wr < out means a short or
     * failed write, which leaves a TRUNCATED property file and therefore a
     * half-empty property set. */
    crumb_printf("seed: system.config tmpl=%d gfx=%d out=%d wr=%d",
                 n, gfx_added, o, wr);
}

/* v02.99: the splash title. jar_path is "games/<name>/game.jar" (relative
 * to DATA_DIR), and <name> is what the user called the game - usually CJK,
 * and already the label the menu showed. Take the last two path segments:
 * everything after the second-to-last '/' and before the last one. Falls
 * back to the jar's own file name, then to "J2ME", so the splash always
 * has something to draw. */
static void splash_name_from_jar(const char *jar, char *out, size_t outsz) {
    const char *last;
    const char *prev;
    size_t n;

    if (outsz == 0) {
        return;
    }
    out[0] = '\0';
    if (jar == NULL || jar[0] == '\0') {
        snprintf(out, outsz, "J2ME");
        return;
    }
    last = strrchr(jar, '/');
    if (last == NULL) {
        snprintf(out, outsz, "%s", jar);
        return;
    }
    /* walk back from last-1 to the previous '/' */
    prev = last;
    while (prev > jar && *(prev - 1) != '/') {
        prev--;
    }
    n = (size_t)(last - prev);
    if (n == 0 || n >= outsz) {
        /* "game.jar" with no directory, or a pathologically long name */
        const char *slash = last + 1;
        if (*slash != '\0') {
            snprintf(out, outsz, "%s", slash);
        } else {
            snprintf(out, outsz, "J2ME");
        }
        return;
    }
    memcpy(out, prev, n);
    out[n] = '\0';
}

/* v04.03: round-boundary arena/stack probe (see the call site for why).
 *
 * Prints ONE line to crumb.log. Deliberately allocation-free apart from the
 * crumb write itself, and it never calls into the VM or the menu, so it cannot
 * perturb what it measures.
 *
 * Fields:
 *   arena  = bytes newlib has obtained from the system (grows, never shrinks)
 *   free   = bytes currently free inside the arena (mallinfo().fordblks)
 *   maxblk = largest single releasable block (mallinfo().keepcost)
 *   sp     = current stack pointer
 *   guard  = canary written to a local; a change between two probes means the
 *            frame was overwritten (the v01.78h failure mode)
 *
 * Reading it:
 *   A ~32 MB drop in `free` between "round end" and the next "menu enter" that
 *   never comes back means the VM heap is not released on the round-return path
 *   (JVM_CleanUp has no caller in the ELF - see the note at the menu call site).
 *   A changed `guard`, or an `sp` that moves between rounds, means stack
 *   corruption instead. */
#define MENU_PROBE_GUARD 0x5A5AA5A5u
static unsigned int menu_probe_guard_slot = MENU_PROBE_GUARD;

static void menu_probe(const char *what, int roundno) {
    unsigned int guard = MENU_PROBE_GUARD;
    unsigned int sp = (unsigned int)(unsigned long)&guard;
    unsigned int freeb = 0, arena = 0, maxblk = 0;

    freeb = (unsigned int)mallinfo().fordblks;
    arena = (unsigned int)mallinfo().arena;
    maxblk = (unsigned int)mallinfo().keepcost;

    crumb_printf("probe %s round=%d sp=%08x guard=%s arena=%uK free=%uK max=%uK",
                 what, roundno, sp,
                 (guard == MENU_PROBE_GUARD
                  && menu_probe_guard_slot == MENU_PROBE_GUARD) ? "ok" : "BAD",
                 arena / 1024u, freeb / 1024u, maxblk / 1024u);
    crumb_flush();
    menu_probe_guard_slot = guard;
}

int main(int argc, char *argv[]) {
    (void)argc; (void)argv;
    /* Initialize pte_os (the pthread emulation layer used by newlib's
     * FILE locks and libpthread). Without this, the first
     * pthread_mutex_unlock on a statically-initialized mutex crashes
     * at pte_osAtomicExchange(NULL+4). Must be the very first call. */
    {
        extern void pte_osInit(void);
        pte_osInit();
    }

    char midp_home[256];
    char jar_path[256];
    char jad_path[300];
    char class_name[128];
    char orient[32];
    char classpath[512];

    /* Writable runtime dirs - MUST exist before the freopen() calls below.
     * Otherwise fopen fails silently and stderr stays bound to the tty
     * device (Vita3K "*** TTY:" per-char stream) so midp_stderr.log stays
     * empty on FIRST launch (8-31 log-loss incident).
     * v01.32: all suite storage moved into the rms/ subdir. Migrate the
     * pre-01.32 shared appdb so existing saves survive the update. */
    sceIoMkdir(DATA_DIR, 0777);
    sceIoMkdir(DATA_DIR "/rms", 0777);
    sceIoMkdir(DATA_DIR "/lib", 0777);
    {
        SceUID dd = sceIoDopen(DATA_DIR "/appdb");
        if (dd >= 0) {
            sceIoDclose(dd);
            sceIoRename(DATA_DIR "/appdb", DATA_DIR "/rms/appdb");
        }
    }

    /* v02.97 (P1-3): ask the kernel for the high-performance clock before
     * anything expensive runs - jar load, VM start and the first frames all
     * benefit. It also opens perf.log's session with the boot clock, so a
     * later frame-time line can always be read against the clock it ran at.
     * Opt out / re-tune via ux0:/data/J2ME00001/clock.cfg (see vita_perf.h);
     * a refused request is logged, never fatal. Placed after the stderr
     * redirect so the clock lines land in midp_stderr.log as well. */
    freopen(DATA_DIR "/midp_stdout.log", "w", stdout);
    /* stderr: TEMPORARILY restored to a file to capture the suite
     * crash-restart loop (music toggle kills GameMidlet; the JVM fatal
     * message lands on stderr). The per-class CP dumps still make this
     * file large - look at the TAIL. Once the crash is fixed, point
     * stderr back at /dev/null. */
    freopen(DATA_DIR "/midp_stderr.log", "w", stderr);
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    vita_perf_init();
    /* v04.32: listen for SCE_APPMGR_SYSTEMEVENT_ON_RESUME. The console
     * suspends the process when the user presses the PS button; without
     * this the audio port opened before the suspend stays dead and the
     * boot-time 444/222 MHz request is lost. No VM interaction. */
    vita_lifecycle_init();
    /* v04.38: JSR-256 needs real six-axis data. The sensor classes live in
     * midp_system.jar and this VM has dynamic native methods off, so a jar
     * class cannot have a native method - the launcher samples sceMotion and
     * publishes it as a 24-byte file the Java side reads back. */
    vita_motion_start();
    /* v03.70: the overlay reads diag.cfg once and then only samples
     * counters. Init after the perf/membw modules so a first frame already
     * has real numbers instead of zeros. */
    vita_diag_init();

    /* v03.71/v03.75: the JIT executable-memory probe is NOT run here any more.
     * It is called from the round loop the first time a non-zero "jit=" policy
     * is requested, because its verdict is what decides whether that policy can
     * be honoured at all (log157: jit=2 crashed in the first frame with an
     * instruction-fetch permission fault inside the Java heap). Running it at
     * boot made every launch pay for a probe that only matters to jit!=0, and
     * its last step executes freshly written machine code, so it stays opt-in
     * via ux0:/data/J2ME00001/jitprobe.cfg as well. See vita_jitmem.h. */

    dlog("vita-port J2ME launcher\n");

    /* v01.57 crash-hunt: boot-phase anchors. Everything below runs before
     * the VM starts; a crash in this window leaves crumb.log truncated at
     * the last bar, which pins the failing subsystem without stdio. */
    crumb_marker("launcher start");
/* v02.27: trace the boot path itself from the first syscall;
         * every thread lazily self-registers on its first traced call,
         * so this mainly anchors the main thread's tid early. */
        vita_trace_register();

        crumb_printf("version: %s", VITA_PORT_VERSION_STRING);

    {
        /* Opt-in 3D self-test on the configured backend. The hardware path
         * takes GXM, so the IME is unavailable for this run. */
        SceIoStat gl3d_st;
        if (sceIoGetstat(DATA_DIR "/gl3d_test", &gl3d_st) >= 0) {
            crumb_printf("3d selftest rc=%d", v3d_selftest(120));
        }
    }

    {
        /* v02.18: pin request now comes from ux0:/data/J2ME00001/vm_cpu
         * (default 0 = unpinned, see read_vm_cpu_mask). Log BOTH the
         * requested mask and the pin result so a round's pin state is
         * always reconstructible from boot_log.txt alone. */
        unsigned int pin_mask = read_vm_cpu_mask();
        if (pin_mask != 0) {
            int pin_rc = sceKernelChangeThreadCpuAffinityMask(
                sceKernelGetThreadId(), pin_mask);
            char pin_buf[64];
            snprintf(pin_buf, sizeof(pin_buf),
                     "pin vm cpu mask=0x%x rc=0x%x\n", pin_mask, pin_rc);
            dlog(pin_buf);
            crumb_printf("pin vm cpu mask=0x%x rc=0x%x", pin_mask, pin_rc);
        } else {
            dlog("vm cpu: unpinned (mask=0, kernel default 3-core)\n");
            crumb_printf("vm cpu: unpinned (mask=0)");
        }
    }

    /* v02.20: kubridge visibility precheck. kubridge.skprx may live on
     * ux0:tai/ OR ur0:tai/ - TaiHEN loads both identically, so the
     * INSTALL PATH does not matter; what matters is whether the
     * module-list probe can see it. Recording the tri-state here makes
     * a round's kernel-visibility decidable from boot_log.txt alone,
     * instead of only after a HANG fires kernel_forensics.log. */
    {
        const char *kc = vita_kf_precheck();
        char kc_buf[64];
        snprintf(kc_buf, sizeof(kc_buf), "kubridge precheck: %s\n", kc);
        dlog(kc_buf);
        crumb_printf("kubridge %s", kc);
        /* v02.22: full module table (flag domain x names) into
         * kernel_forensics.log at boot - one log answers visibility. */
        vita_kf_list_modules("ux0:/data/J2ME00001/kernel_forensics.log");
        /* v04.12: the module table alone cannot explain "installed but not
         * callable". This block adds the clock return value, the config.txt
         * scan (with section headers) and both tai directory listings. */
        vita_kf_kubridge_report("ux0:/data/J2ME00001/kernel_forensics.log");
        /* v02.26: resolve SceKernelThreadmgr's kernel data address
         * NOW, on the main thread, via the USERSPACE enum + a ku
         * memcpy read-back self-check. log49 proved the taiHEN
         * cross-domain query never returns on this console - even
         * at boot it killed the main thread (game crashed at
         * startup). The HANG path only reads the cached address. */
        vita_kf_boot_resolve();
    }

    vita_net_early_init();
    crumb_marker("net early init done");
    dlog("version: " VITA_PORT_VERSION_STRING "\n");

    /* CRITICAL: run from the data dir and keep ALL VM classpath entries
     * RELATIVE. The CLDC classpath splits on ':' - an entry like
     * "ux0:/data/..." gets cut at the colon into "ux0" + "/data/..."
     * (system classes only loaded by luck: the second fragment happened
     * to resolve inside app0:). With chdir, "midp_system.jar",
     * "games/x/game.jar" etc. contain no colon at all. */
    if (chdir(DATA_DIR) < 0) {
        dlog("WARNING: sceIoChdir to data dir failed\n");
    }

    /* Default game = bundled Hello.jar; launch.cfg overrides.
     * launch.cfg lines: <jar path> / <class> / [portrait|landscape] */
    copy_file("app0:/data/J2ME00001/Hello.jar", DATA_DIR "/Hello.jar");
    {
        /* v01.78h: static (not stack) + never written between setjmp and
         * longjmp EXCEPT via the menu's out-pointer - both keep the
         * longjmp read below on the defined side of C99 7.13.2.1p3 and
         * put the struct out of reach of the stack zero-writer. */
        static VitaGameSel sel;
        sceIoMkdir(DATA_DIR "/games", 0777);
        sceIoMkdir(DATA_DIR "/inbox", 0777);

        /* One-time subsystem init (NOT inside the round loop):
         * ANI pool events + the tone player thread are global/static and
         * survive VM rounds; re-creating the tone thread every round would
         * leak one thread per game launch (javacall_media_initialize has
         * NO idempotence guard).
         *
         * ANI thread pool init: the ANI blocking framework (used by async
         * media/network paths, e.g. a game calling Manager.createPlayer on
         * an http resource) signals statically-allocated pool events, but
         * nothing in the CLDC-HI startup ever calls ANI_Initialize - the
         * events stay NULL and the first use crashes in
         * pthread_mutex_unlock(NULL->mutex).
         *
         * Re-enabled: the "unresolvable link" diagnosis (commit 2f578a6)
         * was WRONG - plain `-lcldc_vm_ani` resolves ANI_Initialize fine
         * (verified in the current ELF at 0x810af224). Without this call
         * the pool's static events are never initialized and games
         * entering an ANI path crash. */
        {
            extern void ANI_Initialize(void);
            crumb_marker("ANI_Initialize enter");
            ANI_Initialize();
            crumb_marker("ANI_Initialize done");
            dlog("[ANI] thread pool initialized\n");
        }
        /* Media subsystem init: creates the dedicated tone player thread
         * (j2me_tone). Upstream javacall platforms call this from their
         * platform lifecycle; the vita port never did, so tone playback
         * was dead (games calling playTone/Player.start got silence and
         * no END_OF_MEDIA). */
        {
            extern int javacall_media_initialize(void);
            crumb_marker("media init enter");
            javacall_media_initialize();
            crumb_marker("media init done");
            dlog("[media] tone player thread created\n");
        }

        /* v01.72: hang-forensics watchdog - snapshots VM/tone thread
         * wait states when the event pump stalls >3 s in a MIDlet
         * round (see vita_watchdog.c). Started once, runs forever. */
        vita_watchdog_start();

        /* v02.47: pad_nodivert.cfg = run the REAL trampoline, never divert
         * to the shadow (see vita_v247_nodivert in InterpreterRuntime_arm
         * .cpp). Lets the original FileManager exit loop be reproduced on
         * purpose; pair it with wd_early_dump.cfg so the watchdog takes a
         * coredump ~2 s into the loop. Absent file = shipping behaviour. */
        {
            extern volatile int vita_v247_nodivert;
            SceIoStat nst;
            memset(&nst, 0, sizeof(nst));
            if (sceIoGetstat(DATA_DIR "/pad_nodivert.cfg", &nst) >= 0) {
                vita_v247_nodivert = 1;
                crumb_printf("pad_nodivert.cfg present: loop detector OFF");
            }
        }

        /* Launcher main loop: menu -> run MIDlet -> back to menu.
         * The MIDlet exit only ends the VM round (runMidlet returns);
         * the PROCESS never exits. This is deliberate:
         *  1) phoneME supports in-process VM restart (JVM_Initialize is
         *     idempotent, midpInitialize/midpFinalize fully pair up,
         *     Universe::apocalypse frees the Java heap and resets all
         *     bootstrap state - upstream runs MIDP in a loop this way).
         *  2) NEVER call sceKernelExitProcess: Vita3K's relaunch path
         *     (request_process_exit -> on_game_closed) races its GUI and
         *     crashes the emulator. Staying alive avoids it entirely.
         * Each round re-seeds config/appdb/heap parameters because
         * midpFinalize tears them down. */
        int round = 0;
        for (;;) {
            /* v01.58: pick the JIT policy for THIS round. "jit=1" means
             * first round only, so from round 2 on it degrades to 0 and
             * runMidlet.c passes "-int" again. */
            {
                int cfg_jit = read_jit_policy();
                /* v03.75: log157 - "jit=2" in launch.cfg crashed the process
                 * on the first frame the game drew (coredump: fault status
                 * 0x0d = permission, FAR == pc inside the Java heap, pc == lr
                 * => instruction fetch from execute-never memory, i.e. the
                 * compiler area still lives in a USER_RW heap block). That is
                 * only knowable AFTER the executable-memory probe has run, so
                 * run it here - the first time a non-zero policy is asked for -
                 * and downgrade instead of dying. Default jit=0 never enters
                 * this path, so nobody who does not want the JIT pays for it. */
                static int jit_probe_done = 0;
                static int jit_probe_ok = 0;
                if (cfg_jit != 0) {
                    if (!jit_probe_done) {
                        jit_probe_done = 1;
                        jit_probe_ok = vita_jit_probe();
                        crumb_printf("jit: executable-memory probe %s",
                                     jit_probe_ok ? "PASSED"
                                                  : "FAILED (see [JITPROBE])");
                    }
                    if (!jit_probe_ok) {
                        crumb_printf("jit: cfg=%d -> policy 0 (interpreter); "
                                     "no executable memory, the JIT would "
                                     "prefetch-abort on its first call",
                                     cfg_jit);
                        cfg_jit = 0;
                    } else if (!vita_jit_heap_is_executable()) {
                        /* v04.24: DO NOT downgrade here any more - this check
                         * cannot work at this point.
                         *
                         * log183 proved it: the Java heap is allocated inside
                         * JVM_Initialize(), which runs AFTER this decision, so
                         * every round printed "policy 0 ... the Java heap is NOT
                         * in a VM block" and then, one line later,
                         * "[heapmem] chunk0 BACKED BY VM-DOMAIN MEMORY". The
                         * answer always arrived after the question.
                         *
                         * The real gate now lives in runMidlet.c, immediately
                         * after JVM_Initialize(): it injects "-int" only when the
                         * heap really did not become executable. Keeping the
                         * policy at the requested value here lets that code take
                         * the decision with real information.
                         * This line stays as a boot-order record only. */
                        crumb_printf("jit: cfg=%d kept at %d here; the heap is "
                                     "not allocated yet at this point, so the "
                                     "deciding check is in runMidlet.c after "
                                     "JVM_Initialize (v04.24). Last known "
                                     "heap_executable=%d",
                                     cfg_jit, cfg_jit,
                                     vita_jit_heap_is_executable());
                    } else {
                        crumb_printf("jit: cfg=%d left ENABLED - probe passed "
                                     "and the Java heap is VM-domain "
                                     "(executable), so generated code can run",
                                     cfg_jit);
                    }
                }
                vita_jit_policy = (cfg_jit == 1 && round > 0) ? 0 : cfg_jit;
                crumb_printf("round %d: jit cfg=%d -> policy=%d",
                             round, cfg_jit, vita_jit_policy);
                /* v02.94: a previous "-int" round leaves UseCompiler and
                 * CompilerAreaPercentage at 0 for the rest of the process,
                 * so jit=2 never actually enabled the JIT (log119). */
                vita_jit_round_begin(round, vita_jit_policy);
            }

            snprintf(jar_path, sizeof(jar_path), "Hello.jar");
            /* AUDIO DEBUG: ToneTest (self-driving MMAPI smoke test) is the
             * default while the audio investigation is active. Switch back
             * to "HelloMIDlet" when it concludes. */
            snprintf(class_name, sizeof(class_name), "ToneTest");
            snprintf(orient, sizeof(orient), "portrait");

            /* v02.16 (log40 A/B infra): read launch.cfg ONCE here, so
             * its optional "menu=0" line can skip the native menu and
             * launch the cfg's jar/class directly. Pinning the target
             * this way makes an A/B run reproducible: no menu
             * framebuffer/touch/fs-scan state participates, so a hang
             * cannot be blamed on menu leftovers. When the line is
             * absent the menu keeps deciding exactly as in 02.15. */
            {
                int cfg_skip_menu = 0;
                int cfg_ok = (read_launch_cfg(jar_path, sizeof(jar_path),
                                              class_name, sizeof(class_name),
                                              orient, sizeof(orient),
                                              &cfg_skip_menu) == 0);
                if (cfg_ok && cfg_skip_menu) {
                    dlog_str("launch.cfg jar (menu=0): ", jar_path);
                    dlog_str("launch.cfg class (menu=0): ", class_name);
                    dlog_str("launch.cfg orientation: ", orient);
                } else {
                    /* Native game menu: pick an installed game (see
                     * vita_menu.c for the games/ + inbox/ layout). Falls
                     * through to launch.cfg / Hello when the user quits
                     * without a selection.
                     * v01.78h: the menu exits via longjmp (see
                     * vita_menu.h) - never through its epilogue pop,
                     * which read a zeroed saved-register block on quit in
                     * v01.78g. main never returns either (for(;;)), so
                     * the jmp lands in a frame that stays valid for the
                     * whole process. */
                    vita_menu_result = 0;
                    /* v04.03: log134-family investigation. log170 and log173
                     * both show the SAME failure: after a round returns to the
                     * menu, Vita3K reports
                     *     EXCEPTION_ACCESS_VIOLATION / Read violation at
                     *     address 0x460800000
                     * while the menu rescans games/, ~2 s after the round ended
                     * (the heartbeat goes 60 flips/s -> 8 flips/s -> silence).
                     * It reproduces after UC browser, after the file manager
                     * and after a 3D game, so it is neither 3D- nor
                     * title-specific.
                     *
                     * Two candidate mechanisms survive the log, and they need
                     * different fixes, so this prints the discriminator instead
                     * of guessing:
                     *   (a) the VM heap is never released on the round-return
                     *       path - `JVM_CleanUp()` has NO caller in the ELF
                     *       (nm/objdump), so Universe::apocalypse() ->
                     *       ObjectHeap::dispose() never run and the whole
                     *       32 MB chunk (plus the bitvector chunk) leaks per
                     *       round. mallinfo().fordblks dropping by ~32 MB
                     *       between round 1 and round 2 proves it.
                     *   (b) stack corruption: v01.78h already documented the
                     *       launcher's saved-register block being zeroed during
                     *       a long session. A fixed local (mn_guard) plus SP
                     *       logged at the same points shows the frame moving or
                     *       its canary changing between rounds.
                     * One line per round; costs a single mallinfo() call. */
                    menu_probe("menu enter", round);
                    if (setjmp(vita_menu_escape) == 0) {
                        vita_menu_run(&sel);
                        /* not reached: the menu always longjmps */
                    }
                    menu_probe("menu leave", round);
                    if (vita_menu_result) {
                        snprintf(jar_path, sizeof(jar_path), "%s", sel.jar);
                        snprintf(class_name, sizeof(class_name), "%s",
                                 sel.cls);
                        snprintf(orient, sizeof(orient), "%s", sel.orient);
                        /* VM classpath splits on ':' - strip the
                         * "ux0:/data/J2ME00001/" prefix so the entry is
                         * RELATIVE (see chdir above) */
                        {
                            size_t plen = strlen(DATA_DIR "/");
                            if (strncmp(jar_path, DATA_DIR "/", plen) == 0) {
                                memmove(jar_path, jar_path + plen,
                                        strlen(jar_path) - plen + 1);
                            }
                        }
                        dlog_str("menu jar: ", jar_path);
                        dlog_str("menu class: ", class_name);
                        dlog_str("menu orientation: ", orient);
                    } else if (cfg_ok) {
                        dlog_str("launch.cfg jar: ", jar_path);
                        dlog_str("launch.cfg class: ", class_name);
                        dlog_str("launch.cfg orientation: ", orient);
                    } else {
                        dlog("launch.cfg not found, defaults in use");
                    }
                }
            }

            {
                /* v02.82/02.84: per-game configuration. game.cfg next to
                 * the jar decides the virtual screen, the device profile
                 * and the locale; the size is fixed before the VM starts
                 * because the Java UI reads it once at init, and the
                 * identity is written to system.config before the VM
                 * reads the property store. */
                /* v04.33: remember which jar this round runs. read_jit_policy()
                 * runs at the TOP of the next round (before the menu picks a
                 * jar again), and that is where a per-game game.cfg "jit=" is
                 * looked up. Empty on the very first round of a session. */
                snprintf(g_jit_jar, sizeof(g_jit_jar), "%s/%s",
                         DATA_DIR, jar_path);
                VitaGameConfig gc;
                VitaGameConfig defs;
                const VitaProfile *pr;
                /* v02.86: defaults.cfg first (global), then game.cfg. */
                defaults_read(&defs);
                game_config_read(jar_path, orient, &defs, &gc);
                pr = profile_by_name(gc.profile);
                vita_display_set_size(gc.width, gc.height);
                vita_display_set_scale_mode(gc.scale_mode);
                crumb_printf("display: %dx%d scale=%d (orient=%s)",
                             vita_display_get_width(),
                             vita_display_get_height(),
                             gc.scale_mode, orient);
                crumb_printf("profile: %s locale=%s jad=%d",
                             pr->name,
                             (gc.locale[0] != '\0') ? gc.locale : "(unset)",
                             gc.use_jad);
                /* v02.85/02.86: virtual keypad. vkey.cfg stays the
                 * switch the menu toggles; defaults.cfg / game.cfg may
                 * override it (vkey=on|off). Precedence: game.cfg >
                 * defaults.cfg > vkey.cfg. */
                vita_vkey_load(NULL);
                if (gc.vkey >= 0) {
                    vita_vkey_set_enabled(gc.vkey);
                }
                crumb_printf("vkey: %d mode=%d", vita_vkey_enabled(),
                             vita_vkey_mode());
                /* v02.87: rumble. defaults.cfg / game.cfg vibrate=. */
                if (gc.vibrate >= 0) {
                    vita_vibrate_set_allowed(gc.vibrate);
                }
                crumb_printf("vibrate: %d", gc.vibrate);
                /* v02.90: MIDP permission policy. Setting it HERE (before
                 * the VM starts) means the policy tables are already
                 * decided when Permissions.java initialises, so a game's
                 * first network/file call never blocks on a prompt. The
                 * loader also reads defaults.cfg itself as a fallback, so
                 * a missing per-game setting behaves the same. */
                vita_permissions_set_mode(gc.perms == 1 ? VITA_PERMS_STRICT
                                                        : VITA_PERMS_RELAXED);
                crumb_printf("perms: %s",
                             vita_permissions_mode() == VITA_PERMS_STRICT
                                 ? "strict" : "relaxed");
                /* Seed config from the read-only VPK copy; the display
                 * size and the device identity must match the chosen
                 * settings. Re-done EVERY round: midpFinalize tears down
                 * the property store. */
                seed_internal_config(vita_display_get_width(),
                                     vita_display_get_height());
                seed_system_config(pr, gc.locale);

                /* v02.84: game.jad / <base>.jad next to the jar. Passing
                 * the descriptor as runMidlet's arg0 makes
                 * CldcMIDletSuiteLoader read the JAD properties instead
                 * of the manifest ones, which is what games that ship
                 * both expect (their getAppProperty values come from the
                 * JAD). A missing or disabled JAD keeps the .jar path. */
                jad_path[0] = '\0';
                if (gc.use_jad && find_jad_for_jar(jar_path, jad_path,
                                                   sizeof(jad_path))) {
                    dlog_str("jad: ", jad_path);
                } else {
                    jad_path[0] = '\0';
                }

                copy_file("app0:/data/J2ME00001/midp_system.jar",
                          DATA_DIR "/midp_system.jar");
            }

            snprintf(midp_home, sizeof(midp_home), "%s", DATA_DIR);
            setenv("MIDP_HOME", midp_home, 1);

            midpSetConfigDir(DATA_DIR "/lib");

            /* Per-game RMS isolation: derive appdb path from the jar so each
             * game gets its own suite/RMS namespace (prevents save-file
             * collision). The bundled Hello.jar uses the shared
             * "rms/appdb" path. Re-set EVERY round (midpFinalize resets
             * initLevel). The buffer is static: midpSetAppDir() only
             * stores the pointer, and a stack buffer here would dangle
             * once this block exits (runMidlet reads it later). */
            {
                static char appdb_path[96];
                get_per_game_appdb(jar_path, appdb_path, sizeof(appdb_path));
                /* Ensure appdb directory exists */
                SceUID d = sceIoDopen(appdb_path);
                if (d < 0) {
                    sceIoMkdir(appdb_path, 0777);
                    dlog_str("[RMS] created appdb: ", appdb_path);
                } else {
                    sceIoDclose(d);
                }
                midpSetAppDir(appdb_path);
                dlog_str("[RMS] appdb: ", appdb_path);
            }

            /* Java heap before the VM starts (re-set EVERY round:
             * Arguments::finalize clears the config after cleanup) */
            setHeapParameters();

            /* runMidlet arguments:
             *   -classpathext + <jar list> -> additional classpath (getClassPathPlus)
             *   "internal"                 -> INTERNAL_SUITE_ID (no AMS install)
             *   <classname>                -> MIDlet to launch
             *   <jar path>                 -> arg0 for the MIDlet. CRITICAL: the
             *       internal suite only loads MANIFEST/JAD properties when
             *       args[0] ends in .jar/.jad
             *       (CldcMIDletSuiteLoader.createMIDletSuite). Without it
             *       getAppProperty() returns null for EVERYTHING and
             *       framework apps (UC etc.) crash with NullPointerException
             *       in startApp. The path must be RELATIVE (cwd = DATA_DIR)
             *       so the same entry also resolves via pcsl_file for
             *       JarReader. */
            snprintf(classpath, sizeof(classpath),
                     "midp_system.jar:%s", jar_path);

            char *run_argv[] = {
                "runMidlet",
                "-classpathext",
                classpath,
                "internal",
                class_name,
                /* v02.84: arg0 must end in .jar or .jad - that is the only
                 * thing CldcMIDletSuiteLoader uses to decide whether to
                 * read the suite properties at all (see the comment
                 * above). A JAD, when present, is the more specific
                 * source: it is what the game was shipped with. */
                (jad_path[0] != '\0') ? jad_path : jar_path,
            };
            int run_argc = 6;

            dlog_str("classpath: ", classpath);
            dlog_str("starting MIDlet: ", class_name);

            /* v01.57 crash-hunt anchors: section bars around each VM round
             * so a truncated crumb.log shows exactly which phase died. */
            crumb_marker("round begin");
            crumb_printf("launch: %s / %s", jar_path, class_name);
            /* v02.99: arm the splash BEFORE runMidlet. The display layer
             * allocates its framebuffer deep inside runMidlet
             * (lfjport_ui_init) and paints its first frame there, so the
             * overlay has to be active by then or the user gets the white
             * screen it exists to hide. */
            {
                char splash_title[96];
                splash_name_from_jar(jar_path, splash_title,
                                     sizeof(splash_title));
                vita_splash_begin(splash_title);
            }
            /* v02.44: start of the launch-timing timeline (see above) */
            vita_timing_round = round;
            vita_display_timing_reset();
            vita_timing_want_first_ce = 1;
            vita_timing_markf("round_begin %s", class_name);

            /* v01.72: mark the round active for the hang watchdog and
             * record the VM thread id (this thread becomes the VM's main
             * interpreter thread inside runMidlet). */
            vita_wd_round_active = 1;
            vita_wd_vm_tid = sceKernelGetThreadId();

            /* Copy-integrity check: log the runtime jar size so a truncated
             * copy (shorter than the VPK original) is visible in the boot
             * log. The expected size is baked in at build time. */
            {
                SceUID fd = sceIoOpen(DATA_DIR "/Hello.jar", SCE_O_RDONLY, 0);
                if (fd >= 0) {
                    SceOff sz = sceIoLseek(fd, 0, SCE_SEEK_END);
                    sceIoClose(fd);
                    char m[96];
                    snprintf(m, sizeof(m),
                             "runtime Hello.jar size=%lld (expected %lld)\n",
                             (long long)sz, (long long)HELLO_JAR_SIZE);
                    dlog(m);
                } else {
                    dlog("runtime Hello.jar MISSING\n");
                }
            }

            {
                int status;
                crumb_marker("runMidlet enter");
                /* log28 lesson: printing UseCompiler HERE (before the
                 * runMidlet call) shows the Globals.hpp default (1),
                 * because "-int" is parsed INSIDE runMidlet ->
                 * JVM_ParseOneArg. The old single print misled log28
                 * analysis into a JIT hypothesis for an hour. Keep this
                 * pre-round line (shows the default) and add a post-round
                 * line that shows the mode the round actually ran in. */
                crumb_printf("vm UseCompiler(pre)=%d (jit policy=%d)",
                             (&UseCompiler == NULL) ? -1 : (int)UseCompiler,
                             vita_jit_policy);
                vita_timing_markf("runMidlet_enter");
                status = runMidlet(run_argc, run_argv);
                vita_timing_markf("runMidlet_exit status=%d", status);
                {
                    /* v02.64: the "a text component has focus" flag and a
                     * still-open system keyboard are process-wide statics;
                     * a round that ends while a TextField is focused must
                     * not leave SELECT acting as "open keyboard" in the next
                     * application. */
                    extern void vita_ime_set_text_wanted(int on);
                    extern void vita_ime_abort(void);
                    vita_ime_set_text_wanted(0);
                    vita_ime_abort();
                }
                {
                    /* v02.48: JVMSPI_PrintRaw is line-buffered now; drain a
                     * trailing partial line before the round is torn down. */
                    extern void vita_printraw_flush(void);
                    vita_printraw_flush();
                }
                crumb_printf("vm UseCompiler(post)=%d <- actual round mode",
                             (&UseCompiler == NULL) ? -1 : (int)UseCompiler);
                vita_jit_round_end(round);   /* v02.94 compile counters */
                crumb_printf("runMidlet returned %d", status);
                crumb_marker("runMidlet exit");
                /* v02.99: the round is over - never leave the loading
                 * screen armed for the menu's first frame. */
                vita_splash_end();
                crumb_flush();
                /* v02.43: normal-exit counters (OS_vita.cpp) -> exit_probe.log */
                {
                    extern void vita_exit_probe(int round, int status);
                    vita_exit_probe(round, status);
                }

                vita_wd_round_active = 0; /* v01.72: round over, stand by */

                /* v04.03: the other end of the window - see menu_probe(). */
                menu_probe("round end", round);

                {
                    char msg[64];
                    snprintf(msg, sizeof(msg),
                             "runMidlet returned %d - back to menu\n", status);
                    dlog(msg);
                }
            }
            round++;
        } /* for(;;) - menu round loop: NEVER exits */
    }

    /* NOT REACHED: the launcher loop runs until the process is killed.
     * Deliberately no sceKernelExitProcess here (see note above). */
    if (g_log != NULL) {
        fclose(g_log);
    }
    return 0;
}
