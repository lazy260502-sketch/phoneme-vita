/*
 * vita_thread_audit.c - v02.02 OS-side thread forensics
 *
 * WHY THIS FILE EXISTS (log26 + the Vita3K-vs-real-HW gap):
 * The process mixes four classes of threads with priorities that were
 * never reconciled:
 *   - VM main thread (process main, default prio ~0x10000100 area)
 *   - direct sceKernelCreateThread threads (cldc_ticker, j2me_tone,
 *     j2me_watchdog - all 0x10000100, the only value PROVEN to work on
 *     fw 3.65 real hardware, see vita_watchdog.c)
 *   - ANI pool threads via pthread_create -> VitaSDK libpthread's
 *     pte_osThreadCreate (vita_osal.o): priority = 0x140 - pte_prio
 *     with pte prio range 0x80..0xBF (decoded from the shipped
 *     libpthread.a disassembly), i.e. REAL priorities 0x81..0xC0 -
 *     numerically LOWER (HIGHER priority) than the VM thread on the
 *     Vita's lower-number-wins scheduler.
 * Vita3K does not faithfully model priority preemption; real hardware
 * does. A hang that only reproduces on hardware is therefore fully
 * compatible with "VM thread preempted and never rescheduled" - the
 * log26 coredump froze on an ordinary strne at the birth stub +0x14,
 * an instruction that cannot block by itself.
 *
 * THIS FILE answers that with evidence instead of theory:
 *   1. --wrap=sceKernelCreateThread registers EVERY thread created in
 *      this process (both direct callers and libpthread's pte_osal
 *      call) into a fixed table with the caller's return address.
 *      Same technique as the proven --wrap=pte_osSemaphoreCreate in
 *      vita_crumb.c (v01.57); --wrap only affects calls from objects
 *      linked into this executable - exactly the population at issue.
 *   2. --wrap=sceKernelStartThread records actual start order.
 *   3. The watchdog (vita_watchdog.c) calls vita_thread_audit_dump()
 *      on HANG: for every registered tid it prints
 *      status/wait/prio/cpu via sceKernelGetThreadInfo plus
 *      sceKernelGetThreadRunStatus for the per-core run state.
 *      If the VM thread is READY (runnable, not running) while a
 *      higher-priority (lower number) thread is RUNNING/READY,
 *      starvation is proven in one snapshot.
 *
 * Concurrency: the registry is written from multiple native threads
 * (VM main during init, audio init, watchdog start). Registration
 * happens only inside create/start wrappers which the callers invoke
 * once per thread before it runs; the table is word-sized-slot based
 * with a busy flag, so concurrent registrations land in distinct
 * slots (worst case: a torn name string, tolerable in a forensic
 * dump; the tid field itself is a single word write).
 */

#include <psp2/kernel/threadmgr.h>
#include <psp2/io/fcntl.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include <png.h>

#include "vita_thread_audit.h"

#define AUDIT_MAX_THREADS 32

typedef struct {
    SceUID tid;
    int priority;              /* initPriority as passed to create */
    int stack_size;
    void *caller;              /* return address of the creator     */
    volatile int started;
    char name[24];             /* kernel-side name, truncated       */
} audit_entry;

static audit_entry audit_table[AUDIT_MAX_THREADS];
static volatile int audit_count = 0;

/* Registration is called from the create wrapper BEFORE the kernel
 * call returns the tid (we register after, with the tid in hand) and
 * from arbitrary native threads. Keep it simple and lock-free-ish:
 * claim a slot by atomic-ish increment, then fill. */
static void audit_register(SceUID tid, int priority, int stack_size,
                           void *caller, const char *name) {
    if (tid < 0) return;
    int slot = audit_count;
    if (slot >= AUDIT_MAX_THREADS) return; /* table full: dump still
                                            * lists kernel-visible
                                            * registered ones only */
    audit_count = slot + 1; /* single-word claim; races lose a slot,
                             * never corrupt the dump */
    audit_table[slot].tid = tid;
    audit_table[slot].priority = priority;
    audit_table[slot].stack_size = stack_size;
    audit_table[slot].caller = caller;
    audit_table[slot].started = 0;
    {
        int i = 0;
        if (name) {
            for (; i < 23 && name[i]; i++)
                audit_table[slot].name[i] = name[i];
        }
        audit_table[slot].name[i] = 0;
    }
}

/* ------------------------------------------------------------------ */
/* -Wl,--wrap=sceKernelCreateThread                                   */
/* ------------------------------------------------------------------ */

extern SceUID __real_sceKernelCreateThread(const char *name,
                                           SceKernelThreadEntry entry,
                                           int initPriority,
                                           SceSize stackSize, SceUInt attr,
                                           int cpuAffinityMask,
                                           const SceKernelThreadOptParam *option);

SceUID __wrap_sceKernelCreateThread(const char *name,
                                    SceKernelThreadEntry entry,
                                    int initPriority,
                                    SceSize stackSize, SceUInt attr,
                                    int cpuAffinityMask,
                                    const SceKernelThreadOptParam *option) {
    void *caller = __builtin_return_address(0);
    SceUID tid = __real_sceKernelCreateThread(name, entry, initPriority,
                                              stackSize, attr,
                                              cpuAffinityMask, option);
    audit_register(tid, initPriority, (int)stackSize, caller, name);
    /* First thread created through the wrapper is logged to crumb-style
     * file once so a missing audit.log on a dead run still names the
     * interception as active (same discipline as the watchdog's
     * liveness line). */
    {
        static int logged = 0;
        if (!logged) {
            logged = 1;
            SceUID fd = sceIoOpen("ux0:/data/J2ME00001/thread_audit.log",
                                  SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND,
                                  0777);
            if (fd >= 0) {
                char buf[96];
                int n = snprintf(buf, sizeof(buf),
                                 "[AUDIT] wrap active, first tid=%d '%s'\n",
                                 (int)tid, name ? name : "?");
                sceIoWrite(fd, buf, n);
                sceIoClose(fd);
            }
        }
    }
    return tid;
}

/* ------------------------------------------------------------------ */
/* -Wl,--wrap=sceKernelStartThread                                    */
/* ------------------------------------------------------------------ */

extern int __real_sceKernelStartThread(SceUID tid, SceSize arglen, void *argp);

int __wrap_sceKernelStartThread(SceUID tid, SceSize arglen, void *argp) {
    int rc = __real_sceKernelStartThread(tid, arglen, argp);
    if (rc >= 0) {
        int i, n = audit_count;
        for (i = 0; i < n; i++) {
            if (audit_table[i].tid == tid) {
                audit_table[i].started = 1;
                break;
            }
        }
    }
    return rc;
}

/* ------------------------------------------------------------------ */
/* -Wl,--wrap=png_read_image                                          */
/* ------------------------------------------------------------------ */

volatile unsigned vita_png_reads = 0;

extern void __real_png_read_image(png_structp, png_bytepp);

void __wrap_png_read_image(png_structp png, png_bytepp image) {
    vita_png_reads++;
    __real_png_read_image(png, image);
}

/* ------------------------------------------------------------------ */
/* Watchdog dump entry point                                          */
/* ------------------------------------------------------------------ */

static const char *audit_status_name(SceUInt32 s) {
    switch (s) {
        case 1:  return "RUNNING";
        case 2:  return "READY";
        case 4:  return "STANDBY";
        case 8:  return "WAITING";
        case 16: return "DORMANT";
        case 32: return "DELETED";
        case 64: return "DEAD";
        default: return "?";
    }
}

static const char *audit_wait_name(SceUInt32 t) {
    switch (t) {
        case 0:  return "none";
        case 1:  return "delay";
        case 2:  return "eventflag";
        case 3:  return "mutex";
        case 4:  return "condvar";
        case 5:  return "cb";
        case 6:  return "lwcond";
        case 7:  return "lwmutex";
        case 8:  return "semaphore";
        case 9:  return "io";
        case 0x10: return "event";
        default: return "other";
    }
}

void vita_thread_audit_dump(const char *path) {
    SceUID fd = sceIoOpen(path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND,
                          0777);
    if (fd < 0) return;
    char buf[224];
    int n;

    n = snprintf(buf, sizeof(buf),
                 "[AUDIT] thread table (%d registered)\n", audit_count);
    sceIoWrite(fd, buf, n);

    /* The calling thread (watchdog) is not necessarily in the table
     * (it may pre-date the wrap if created before registration) - it
     * self-identifies below. */
    int i, cnt = audit_count;
    for (i = 0; i < cnt; i++) {
        SceKernelThreadInfo info;
        memset(&info, 0, sizeof(info));
        info.size = sizeof(info);
        SceUID tid = audit_table[i].tid;
        if (tid < 0 || sceKernelGetThreadInfo(tid, &info) < 0) {
            n = snprintf(buf, sizeof(buf),
                         "  [%2d] tid=%d '%s' <gone> creator=0x%08x\n",
                         i, (int)tid, audit_table[i].name,
                         (unsigned)(uintptr_t)audit_table[i].caller);
            sceIoWrite(fd, buf, n);
            continue;
        }
        n = snprintf(buf, sizeof(buf),
                     "  [%2d] tid=%d '%s' %s wait=%s(0x%x) wid=%d "
                     "prio=0x%x(init 0x%x) cpu=%d last=%d "
                     "preempt=%u creator=0x%08x\n",
                     i, (int)tid, info.name,
                     audit_status_name(info.status),
                     audit_wait_name(info.waitType), info.waitType,
                     (int)info.waitId,
                     info.currentPriority, info.initPriority,
                     info.currentCpuId, info.lastExecutedCpuId,
                     info.threadPreemptCount,
                     (unsigned)(uintptr_t)audit_table[i].caller);
        sceIoWrite(fd, buf, n);
    }

    /* log27 lesson: sceKernelGetThreadRunStatus NEVER RETURNS when
     * called from a user-mode thread on fw 3.65 real hardware - the
     * log27 watchdog froze inside the syscall (kernel PC e000df82),
     * truncating the whole forensic chain AFTER the thread table
     * (no per-core line, no site ring, no vm marker, no data abort;
     * the psp2core dump had to come from the system instead). The
     * GetThreadInfo loop above is proven safe (v01.75+), so the
     * per-core run state is simply dropped: the thread table plus
     * each thread's status field already decides starvation vs
     * userland-spin. */

    sceIoClose(fd);
}

/* VM tid is published by vita_main.c; the dump above prints names, but
 * an explicit marker line saves log cross-referencing. */
extern volatile SceUID vita_wd_vm_tid;

void vita_thread_audit_dump_vm_marker(const char *path) {
    SceUID fd = sceIoOpen(path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND,
                          0777);
    if (fd < 0) return;
    char buf[96];
    int n = snprintf(buf, sizeof(buf), "[AUDIT] vm tid=%d\n",
                     (int)vita_wd_vm_tid);
    sceIoWrite(fd, buf, n);
    sceIoClose(fd);
}
