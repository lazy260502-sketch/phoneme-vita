/*
 * main.c: PS Vita entry point for J2ME MIDP (phoneME)
 *
 * This program initializes the Vita SDK and phoneME MIDP runtime
 * to run J2ME MIDlet games.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/display.h>
#include <psp2/ctrl.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>

/* MIDP headers */
#include <midpAMS.h>
#include <midp_properties_port.h>

/* Heap parameters */
#include <heap.h>

/* Declaration of utility functions */
extern char* getApplicationDir(char *cmd);
extern char* getConfigurationDir(char *cmd);

/* runMidlet() is defined in runMidlet.o which is linked from libobj_no_main.a
 * into midp_vita. We declare it here so the compiler can check call sites. */
extern int runMidlet(int argc, char** argv);

/* Simple debug screen output */
static FILE* g_log = NULL;
static void debug_print(const char *s) {
    /* Output to stderr which goes to log */
    fprintf(stderr, "%s", s);
    /* Also write to file for debugging */
    if (g_log == NULL) {
        g_log = fopen("ux0:/data/debug_log.txt", "w");
    }
    if (g_log) {
        fprintf(g_log, "%s", s);
        fflush(g_log);
    }
}

int main(int argc, char *argv[]) {
    int status;
    char *appDir = NULL;
    char *confDir = NULL;
    char midp_home[256];
    char app_dir[256];
    char conf_dir[256];

    /* Redirect stdout/stderr to files for Vita3K debugging */
    freopen("ux0:/data/midp_stdout.log", "w", stdout);
    freopen("ux0:/data/midp_stderr.log", "w", stderr);
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    fprintf(stderr, "J2ME Emulator for PS Vita\n");
    debug_print("Starting phoneME MIDP...\n");

    /* Initialize Vita display */
    SceDisplayFrameBuf framebuf;
    memset(&framebuf, 0, sizeof(framebuf));
    framebuf.size = sizeof(framebuf);
    framebuf.pitch = 960;
    framebuf.pixelformat = SCE_DISPLAY_PIXELFORMAT_A8B8G8R8;
    /* We'll let the MIDP graphics handle the actual display */

    /* Initialize controller */
    sceCtrlSetSamplingMode(SCE_CTRL_MODE_DIGITAL);

    /* Set MIDP_HOME to a writable path on Vita */
    /* This is where appdb and lib directories will be created */
    snprintf(midp_home, sizeof(midp_home), "ux0:/data/J2ME00001");
    setenv("MIDP_HOME", midp_home, 1);
    debug_print("MIDP_HOME set to: ");
    debug_print(midp_home);
    debug_print("\n");

    /* Create writable directories for MIDP runtime */
    sceIoMkdir("ux0:/data/J2ME00001", 0777);
    sceIoMkdir("ux0:/data/J2ME00001/appdb", 0777);
    sceIoMkdir("ux0:/data/J2ME00001/lib", 0777);

    /* Copy config files from app0:/data/J2ME00001/lib/ to writable ux0:/data/J2ME00001/lib/ */
    {
        SceUID src_fd, dst_fd;
        char buf[4096];
        ssize_t bytes;

        /* Copy internal.config */
        src_fd = sceIoOpen("app0:/data/J2ME00001/lib/internal.config", SCE_O_RDONLY, 0);
        if (src_fd >= 0) {
            dst_fd = sceIoOpen("ux0:/data/J2ME00001/lib/internal.config",
                               SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
            if (dst_fd >= 0) {
                while ((bytes = sceIoRead(src_fd, buf, sizeof(buf))) > 0) {
                    sceIoWrite(dst_fd, buf, bytes);
                }
                sceIoClose(dst_fd);
            }
            sceIoClose(src_fd);
        }

        /* Copy system.config */
        src_fd = sceIoOpen("app0:/data/J2ME00001/lib/system.config", SCE_O_RDONLY, 0);
        if (src_fd >= 0) {
            dst_fd = sceIoOpen("ux0:/data/J2ME00001/lib/system.config",
                               SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
            if (dst_fd >= 0) {
                while ((bytes = sceIoRead(src_fd, buf, sizeof(buf))) > 0) {
                    sceIoWrite(dst_fd, buf, bytes);
                }
                sceIoClose(dst_fd);
            }
            sceIoClose(src_fd);
        }
    }

    /* Copy Hello.jar from app0:/data/J2ME00001/ to ux0:/data/J2ME00001/Hello.jar */
    {
        SceUID src_fd, dst_fd;
        char buf[4096];
        ssize_t bytes;

        src_fd = sceIoOpen("app0:/data/J2ME00001/Hello.jar", SCE_O_RDONLY, 0);
        if (src_fd >= 0) {
            dst_fd = sceIoOpen("ux0:/data/J2ME00001/Hello.jar",
                               SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
            if (dst_fd >= 0) {
                while ((bytes = sceIoRead(src_fd, buf, sizeof(buf))) > 0) {
                    sceIoWrite(dst_fd, buf, bytes);
                }
                sceIoClose(dst_fd);
                debug_print("Copied Hello.jar to ux0:/data/J2ME00001/Hello.jar\n");
            }
            sceIoClose(src_fd);
        } else {
            debug_print("Warning: Could not open app0:/Hello.jar\n");
        }
    }

    /* Set up MIDP directories */
    /* Since we set MIDP_HOME, getApplicationDir/getConfigurationDir won't work
       correctly (getMidpHome returns env var pointer, not dirBuffer).
       So we construct the paths ourselves. */
    snprintf(app_dir, sizeof(app_dir), "%s/appdb", midp_home);
    snprintf(conf_dir, sizeof(conf_dir), "%s/lib", midp_home);
    appDir = app_dir;
    confDir = conf_dir;
    midpSetAppDir(appDir);
    midpSetConfigDir(confDir);

    debug_print("App dir: ");
    debug_print(appDir);
    debug_print("\nConfig dir: ");
    debug_print(confDir);
    debug_print("\n");

    /* Initialize MIDP
     * NOTE: runMidlet() will call midpInitialize() itself, so we don't
     * call it here. runMidlet() is the proper AMS entry point that:
     *   1. Loads com.sun.midp.main.MIDletSuiteLoader as the VM main class
     *   2. Parses the classname of the MIDlet to run
     *   3. Creates the MIDlet instance and invokes its startApp/pauseApp/
     *      destroyApp lifecycle through the MIDP framework
     *   4. Drives the event loop until the MIDlet exits
     *
     * Previously we used midpRunMainClass(classpath, "HelloMIDlet", ...)
     * which directly invokes HelloMIDlet.main() via the JVM. But
     * HelloMIDlet extends MIDlet and has no static main() method, so
     * the JVM aborts with NoSuchMethodError, leading to the blank screen.
     */

    /* Set Java heap parameters before runMidlet() starts the VM */
    setHeapParameters();

    /*
     * Arguments to runMidlet(argc, argv):
     *   argv[0] = "runMidlet"     -> program name (placeholder)
     *   argv[1] = "-classpathext" -> additional classpath (parsed in runMidlet)
     *   argv[2] = "<multi-path>"  -> additionalPath: midp_system.jar + Hello.jar
     *                                 (':' separated, parsed by getClassPathPlus)
     *   argv[3] = "internal"      -> forces INTERNAL_SUITE_ID (no AMS install)
     *   argv[4] = "HelloMIDlet"   -> classname of MIDlet to launch
     *
     * Why this works:
     *   - "-classpathext" sets additionalPath to the multi-path string
     *   - "internal" makes strcmp return 0, so runMidlet does NOT
     *     overwrite additionalPath with the suiteId arg
     *   - INTERNAL_SUITE_ID makes getClassPathPlus take the multi-path
     *     branch, which builds the final classpath from additionalPath
     *   - "HelloMIDlet" is parsed as the MIDlet classname to launch
     */
    char *run_argv[] = {
        "runMidlet",                                              /* argv[0] program name */
        "-classpathext",                                          /* argv[1] */
        "ux0:/data/J2ME00001/midp_system.jar:ux0:/data/J2ME00001/Hello.jar", /* argv[2] additionalPath */
        "internal",                                               /* argv[3] suiteId -> INTERNAL_SUITE_ID */
        "HelloMIDlet",                                            /* argv[4] classname */
    };
    int run_argc = 5;

    debug_print("Calling runMidlet(internal, HelloMIDlet)...\n");
    debug_print("  additionalPath=ux0:/data/J2ME00001/midp_system.jar:ux0:/data/J2ME00001/Hello.jar\n");

    /* Call runMidlet from MIDP library (linked from runMidlet.o) */
    status = runMidlet(run_argc, run_argv);

    {
        char buf[64];
        snprintf(buf, sizeof(buf), "runMidlet returned %d\n", status);
        debug_print(buf);
    }

    return status;
}