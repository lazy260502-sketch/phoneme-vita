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
 *   - matching internal.config (320x240 or 240x320) copied to ux0 at startup
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <psp2/kernel/processmgr.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>

/* MIDP headers */
#include <midpAMS.h>
#include <midp_properties_port.h>

/* Heap parameters */
#include <heap.h>

extern int runMidlet(int argc, char **argv);

/* from vita_display.c: must run before the VM starts */
extern void vita_display_set_orientation(int landscape);

/* vitaSDK newlib: the default malloc arena is 32MB, which is too small for
 * a 32MB Java heap on top of everything else (the VM refused to start with
 * "Could not allocate VM heap"). This overrides the weak default; the app
 * memory partition is 128MB (see CMakeLists MEMSIZE). */
unsigned int _newlib_heap_size_user = 64 * 1024 * 1024;

#define DATA_DIR "ux0:/data/J2ME00001"
#define CFG_PATH DATA_DIR "/launch.cfg"

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

/* Copy a file via sceIo (used to seed the writable data dir from app0:). */
static void copy_file(const char *src, const char *dst) {
    SceUID in = sceIoOpen(src, SCE_O_RDONLY, 0);
    if (in < 0) {
        return;
    }
    SceUID out = sceIoOpen(dst, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
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

    char midp_home[256];
    char jar_path[256];
    char class_name[128];
    char orient[32];
    char classpath[512];

    freopen(DATA_DIR "/midp_stdout.log", "w", stdout);
    freopen(DATA_DIR "/midp_stderr.log", "w", stderr);
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    dlog("vita-port J2ME launcher\n");

    /* Writable runtime dirs */
    sceIoMkdir(DATA_DIR, 0777);
    sceIoMkdir(DATA_DIR "/appdb", 0777);
    sceIoMkdir(DATA_DIR "/lib", 0777);

    /* Default game = bundled Hello.jar; launch.cfg overrides.
     * launch.cfg lines: <jar path> / <class> / [portrait|landscape] */
    copy_file("app0:/data/J2ME00001/Hello.jar", DATA_DIR "/Hello.jar");
    snprintf(jar_path, sizeof(jar_path), DATA_DIR "/Hello.jar");
    snprintf(class_name, sizeof(class_name), "HelloMIDlet");
    snprintf(orient, sizeof(orient), "portrait");
    if (read_launch_cfg(jar_path, sizeof(jar_path),
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
        /* Seed config from the read-only VPK copy; the display properties
         * in internal.config must match the chosen orientation. */
        if (landscape) {
            copy_file("app0:/data/J2ME00001/lib/internal.landscape.config",
                      DATA_DIR "/lib/internal.config");
        } else {
            copy_file("app0:/data/J2ME00001/lib/internal.config",
                      DATA_DIR "/lib/internal.config");
        }
        copy_file("app0:/data/J2ME00001/lib/system.config",
                  DATA_DIR "/lib/system.config");
    }

    snprintf(midp_home, sizeof(midp_home), "%s", DATA_DIR);
    setenv("MIDP_HOME", midp_home, 1);
    midpSetAppDir(DATA_DIR "/appdb");
    midpSetConfigDir(DATA_DIR "/lib");

    /* Java heap before the VM starts */
    setHeapParameters();

    /* runMidlet arguments:
     *   -classpathext + <jar list> -> additional classpath (getClassPathPlus)
     *   "internal"                 -> INTERNAL_SUITE_ID (no AMS install)
     *   <classname>                -> MIDlet to launch                       */
    snprintf(classpath, sizeof(classpath),
             DATA_DIR "/midp_system.jar:%s", jar_path);

    char *run_argv[] = {
        "runMidlet",
        "-classpathext",
        classpath,
        "internal",
        class_name,
    };
    int run_argc = 5;

    dlog_str("classpath: ", classpath);
    dlog_str("starting MIDlet: ", class_name);

    int status = runMidlet(run_argc, run_argv);

    {
        char msg[64];
        snprintf(msg, sizeof(msg), "runMidlet returned %d\n", status);
        dlog(msg);
    }

    if (g_log != NULL) {
        fclose(g_log);
    }
    return status;
}
