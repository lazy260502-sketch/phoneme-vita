/*
 * vita_crumb.c - stdio-free diagnostic breadcrumbs for the on-device
 * crash hunt (v01.57).
 *
 * WHY THIS EXISTS
 * ===============
 * The psp2dmp analysis pinned the faulting PC at
 * pte_osSemaphoreCreate: strge r3,[r5] with r5=0 - a NULL-pointer
 * store right AFTER sceKernelCreateSema succeeded. Prime suspect:
 * newlib's lazy stdio FILE-lock init (first write to a FILE* from a
 * VM worker thread builds its lock semaphore via pte_os) blowing up,
 * or some caller passing a NULL pHandle into a pthread-primitive
 * init.
 *
 * CONSEQUENCE FOR DIAGNOSTICS: any fprintf(...) breadcrumb on the
 * crash path would itself risk detonating the same bug (or hiding
 * it). So the helpers below deliberately BYPASS stdio: sceIo straight
 * into ux0:/data/J2ME00001/*.log, no FILE*, no locks, no malloc.
 * sceIoWrite is unbuffered, so text survives a crash even with
 * cached handles.
 *
 * PARTS
 * =====
 *   crumb_marker / crumb_printf   section bars + lines into crumb.log
 *   crumb_append(path, s, len)    generic append channel (per-path
 *                                 cached handle, up to 4) - backs the
 *                                 VM's JVMSPI_PrintRaw /
 *                                 pcsl_print_chars so VM stdout/stderr
 *                                 no longer touch stdio
 *   __wrap_pte_osSemaphoreCreate  linker -Wl,--wrap interception:
 *                                 logs every semaphore create (init
 *                                 value, pHandle, result, CALLER) and
 *                                 refuses the NULL-pHandle call that
 *                                 crashes, so the log shows exactly
 *                                 who asked for it
 *   crumb_flush                   close cached handles
 *
 * The files are appended, so reruns accumulate; delete them between
 * runs if you want a clean view.
 */

#include <stdio.h>      /* vsnprintf only - no stdio I/O performed */
#include <stdarg.h>
#include <string.h>
#include <psp2/io/fcntl.h>
#include <psp2/kernel/clib.h>

#define CRUMB_PATH "ux0:/data/J2ME00001/crumb.log"

/* Tiny spin gate: VM threads can race during bootstrap. This is NOT a
 * general-purpose mutex, just enough serialization for a diagnostic
 * path. sceKernelCreateSema is avoided ON PURPOSE (same subsystem the
 * crash lives in). */
static volatile int g_gate;

#define CRUMB_GATE() \
    while (__atomic_exchange_n(&g_gate, 1, __ATOMIC_SEQ_CST) != 0) { \
        /* spin */ \
    }
#define CRUMB_UNGATE() (g_gate = 0)

/* ------------------------------------------------------------------ */
/* Generic append channel with per-path cached handles                */
/* ------------------------------------------------------------------ */

#define CRUMB_MAX_CACHED 4
static struct {
    const char *path;
    SceUID fd;
} g_cache[CRUMB_MAX_CACHED];

static SceUID crumb_fd_for(const char *path) {
    int i;
    for (i = 0; i < CRUMB_MAX_CACHED; i++) {
        if (g_cache[i].path != NULL && strcmp(g_cache[i].path, path) == 0) {
            return g_cache[i].fd;
        }
    }
    for (i = 0; i < CRUMB_MAX_CACHED; i++) {
        if (g_cache[i].path == NULL) {
            SceUID fd = sceIoOpen(path, SCE_O_WRONLY | SCE_O_CREAT |
                                           SCE_O_APPEND, 0777);
            if (fd >= 0) {
                g_cache[i].path = path;
                g_cache[i].fd = fd;
            }
            return fd;
        }
    }
    return -1;
}

void crumb_append(const char *path, const char *s, int len) {
    if (path == NULL || s == NULL || len <= 0) {
        return;
    }
    CRUMB_GATE();
    {
        SceUID fd = crumb_fd_for(path);
        if (fd >= 0) {
            sceIoWrite(fd, s, (SceSize)len);
        }
    }
    CRUMB_UNGATE();
}

static void crumb_write_raw(const char *s, int len) {
    crumb_append(CRUMB_PATH, s, len);
}

void crumb_marker(const char *label) {
    char buf[160];
    int n = snprintf(buf, sizeof(buf),
                     "\r\n==== %s ====\r\n", label ? label : "?");
    crumb_write_raw(buf, n);
}

void crumb_printf(const char *fmt, ...) {
    char buf[256];
    va_list ap;
    int n;

    buf[0] = '\0';
    va_start(ap, fmt);
    n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n < 0) {
        return;
    }
    if (n > (int)sizeof(buf) - 3) {
        n = (int)sizeof(buf) - 3;
    }
    buf[n] = '\r';
    buf[n + 1] = '\n';
    crumb_write_raw(buf, n + 2);
}

void crumb_flush(void) {
    int i;
    CRUMB_GATE();
    for (i = 0; i < CRUMB_MAX_CACHED; i++) {
        if (g_cache[i].path != NULL && g_cache[i].fd >= 0) {
            sceIoClose(g_cache[i].fd);
        }
        g_cache[i].path = NULL;
        g_cache[i].fd = -1;
    }
    CRUMB_UNGATE();
}

/* ------------------------------------------------------------------ */
/* -Wl,--wrap=pte_osSemaphoreCreate interception                      */
/* ------------------------------------------------------------------ */

/* Real implementation lives in VitaSDK libpthread.a (vita_osal.o).
 * Disassembly of the current binary confirms the contract:
 *   r0 = initialValue, r1 = pHandle; creates a kernel semaphore and
 *   stores its id through pHandle on success (rc=0), returns 2 on
 *   failure. The crash was the SUCCESS-path store with pHandle==NULL
 *   (strge r3,[r5] with r5=0 right after sceKernelCreateSema). */
extern int __real_pte_osSemaphoreCreate(int initialValue, void **pHandle);

int __wrap_pte_osSemaphoreCreate(int initialValue, void **pHandle) {
    void *caller = __builtin_return_address(0);

    if (pHandle == NULL) {
        /* The exact call shape that killed the process before: refuse
         * it and leave the evidence in crumb.log instead of a silent
         * coredump. rc=2 is the same pte_osResult the kernel-failure
         * branch already produces, so callers take their normal error
         * path. */
        crumb_printf("[pte] BLOCKED NULL-pHandle sem create from %p",
                     caller);
        return 2;
    }

    {
        int rc = __real_pte_osSemaphoreCreate(initialValue, pHandle);
        crumb_printf("[pte] sem create init=%d handle=%p -> %p rc=%d from %p",
                     initialValue, (void *)pHandle,
                     rc == 0 ? *pHandle : (void *)0, rc, caller);
        return rc;
    }
}

