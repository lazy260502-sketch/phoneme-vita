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

/* MIDP headers */
#include <midpAMS.h>
#include <midp_properties_port.h>

/* Heap parameters */
#include <heap.h>

extern int runMidlet(int argc, char **argv);

#include "vita_menu.h"
#include "vita_storage.h"
#include "vita_version.h"
#include "vita_crumb.h"

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
#define VITA_JIT_DEFAULT 0
int vita_jit_policy = VITA_JIT_DEFAULT;

/* The VM's own flag (Globals.hpp:231, product(bool, UseCompiler, true)).
 * It is a plain C++ global with an unmangled name; declared here as a byte
 * purely so the launcher can log whether the JIT really is enabled this
 * round. Weak, so a build without the compiler subsystem still links. */
extern unsigned char UseCompiler __attribute__((weak));

/* Read the optional "jit=N" setting from launch.cfg. Missing or malformed
 * values leave the default in place. */
static int read_jit_policy(void) {
    int policy = VITA_JIT_DEFAULT;
    SceUID fd = sceIoOpen(CFG_PATH, SCE_O_RDONLY, 0);
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

    freopen(DATA_DIR "/midp_stdout.log", "w", stdout);
    /* stderr: TEMPORARILY restored to a file to capture the suite
     * crash-restart loop (music toggle kills GameMidlet; the JVM fatal
     * message lands on stderr). The per-class CP dumps still make this
     * file large - look at the TAIL. Once the crash is fixed, point
     * stderr back at /dev/null. */
    freopen(DATA_DIR "/midp_stderr.log", "w", stderr);
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

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
                vita_jit_policy = (cfg_jit == 1 && round > 0) ? 0 : cfg_jit;
                crumb_printf("round %d: jit cfg=%d -> policy=%d",
                             round, cfg_jit, vita_jit_policy);
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
                    if (setjmp(vita_menu_escape) == 0) {
                        vita_menu_run(&sel);
                        /* not reached: the menu always longjmps */
                    }
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
                int landscape = (strcmp(orient, "portrait") != 0);
                vita_display_set_orientation(landscape);
                /* Seed config from the read-only VPK copy; the display
                 * properties in internal.config must match the chosen
                 * orientation. Re-copied EVERY round: midpFinalize tears
                 * down the property store. */
                if (landscape) {
                    copy_file("app0:/data/J2ME00001/lib/internal.landscape.config",
                              DATA_DIR "/lib/internal.config");
                } else {
                    copy_file("app0:/data/J2ME00001/lib/internal.config",
                              DATA_DIR "/lib/internal.config");
                }
                copy_file("app0:/data/J2ME00001/lib/system.config",
                          DATA_DIR "/lib/system.config");
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
                jar_path,
            };
            int run_argc = 6;

            dlog_str("classpath: ", classpath);
            dlog_str("starting MIDlet: ", class_name);

            /* v01.57 crash-hunt anchors: section bars around each VM round
             * so a truncated crumb.log shows exactly which phase died. */
            crumb_marker("round begin");
            crumb_printf("launch: %s / %s", jar_path, class_name);
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
                crumb_printf("runMidlet returned %d", status);
                crumb_marker("runMidlet exit");
                crumb_flush();
                /* v02.43: normal-exit counters (OS_vita.cpp) -> exit_probe.log */
                {
                    extern void vita_exit_probe(int round, int status);
                    vita_exit_probe(round, status);
                }

                vita_wd_round_active = 0; /* v01.72: round over, stand by */

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
