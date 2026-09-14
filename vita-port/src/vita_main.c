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
#include <stdlib.h>
#include <string.h>

#include <unistd.h>
#include <pthread.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>

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
 *   line 1: game jar absolute path
 *   line 2: MIDlet class name
 *   line 3: optional orientation "portrait" / "landscape" (default portrait)
 * Returns 0 when jar+class were read. */
static int read_launch_cfg(char *jar, size_t jar_sz,
                           char *cls, size_t cls_sz,
                           char *orient, size_t orient_sz) {
    SceUID fd = sceIoOpen(CFG_PATH, SCE_O_RDONLY, 0);
    if (fd < 0) {
        return -1;
    }
    static char buf[512];
    memset(buf, 0, sizeof(buf));
    (void)sceIoRead(fd, buf, sizeof(buf) - 1);
    sceIoClose(fd);

    /* line 1: jar path */
    char *nl = strpbrk(buf, "\r\n");
    if (nl == NULL) {
        return -1;
    }
    *nl = '\0';
    snprintf(jar, jar_sz, "%s", buf);

    /* line 2: class name */
    char *p2 = nl + 1 + strspn(nl + 1, "\r\n");
    char *nl2 = strpbrk(p2, "\r\n");
    if (nl2 != NULL) {
        *nl2 = '\0';
    }
    snprintf(cls, cls_sz, "%s", p2);
    if (jar[0] == '\0' || cls[0] == '\0') {
        return -1;
    }

    /* line 3 (optional): orientation */
    snprintf(orient, orient_sz, "portrait");
    if (nl2 != NULL) {
        char *p3 = nl2 + 1 + strspn(nl2 + 1, "\r\n");
        char *nl3 = strpbrk(p3, "\r\n");
        if (nl3 != NULL) {
            *nl3 = '\0';
        }
        if (p3[0] != '\0') {
            snprintf(orient, orient_sz, "%s", p3);
        }
    }
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
    crumb_printf("version: %s", VITA_PORT_VERSION_STRING);

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
        VitaGameSel sel;
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

            /* Native game menu: pick an installed game (see vita_menu.c
             * for the games/ + inbox/ layout). Falls through to
             * launch.cfg / Hello when the user quits without a selection. */
            if (vita_menu_run(&sel)) {
                snprintf(jar_path, sizeof(jar_path), "%s", sel.jar);
                snprintf(class_name, sizeof(class_name), "%s", sel.cls);
                snprintf(orient, sizeof(orient), "%s", sel.orient);
                /* VM classpath splits on ':' - strip "ux0:/data/J2ME00001/"
                 * so the entry is RELATIVE (see chdir above) */
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
            } else if (read_launch_cfg(jar_path, sizeof(jar_path),
                                       class_name, sizeof(class_name),
                                       orient, sizeof(orient)) == 0) {
                dlog_str("launch.cfg jar: ", jar_path);
                dlog_str("launch.cfg class: ", class_name);
                dlog_str("launch.cfg orientation: ", orient);
            } else {
                dlog("launch.cfg not found, defaults in use");
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
                /* Read the VM flag AFTER the round: runMidlet.c has parsed
                 * "-int" by then, so this is the mode the round actually
                 * ran in (1 = JIT enabled, 0 = pure interpreter). */
                crumb_printf("vm UseCompiler=%d (jit policy=%d)",
                             (&UseCompiler == NULL) ? -1 : (int)UseCompiler,
                             vita_jit_policy);
                status = runMidlet(run_argc, run_argv);
                crumb_printf("runMidlet returned %d", status);
                crumb_marker("runMidlet exit");
                crumb_flush();

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
