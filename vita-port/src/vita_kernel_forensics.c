/*
 * vita_kernel_forensics.c - v02.18 kernel-side stall forensics.
 *
 * Premise (log36..log42, six independent real-hardware runs):
 *   - the VM's host core stops scheduling ENTIRELY (idle clock, idle-exit
 *     count AND thread-switch count all frozen) while the other cores keep
 *     running the same process fine;
 *   - the stalled core FOLLOWS the VM (pin cpu0 -> cpu0 stalls, pin cpu1
 *     -> cpu1 stalls, unpinned -> the core it last ran on stalls);
 *   - every userspace kick (signal / affinity / priority / suspend-resume)
 *     is accepted by the kernel and changes nothing.
 * Userspace evidence is exhausted; the remaining question - WHAT is the
 * stalled core's kernel side doing - needs kernel memory, which the
 * coredump does NOT carry (SYSTEM_INFO2 is 4 MB of hashed/encrypted data,
 * no thread names survive in it).
 *
 * Approach: kubridge (kuKernelCpuUnrestrictedMemcpy) lets a USERSPACE
 * thread read arbitrary kernel addresses. All reads happen on the
 * WATCHDOG thread (alive by construction), so a dead core cannot stop
 * the probe. If kubridge is absent (no plugin / old CFW) every probe
 * degrades to a "kubridge unavailable" line - the build still runs and
 *   all v02.17 userspace readouts keep working.
 *
 * What we read (fw 3.60/3.65 SceKernelThreadmgr, well-documented offsets):
 *   1. per-CPU runqueues:  SceKernelThreadMgr (ksceKernelGetThreadInfo's
 *      module) globals. Rather than hardcoding module bases we enumerate
 *      kernel modules via sceKernelGetModuleList(0x00...) and take
 *      SceKernelThreadmgr's segment -> then walk the documented
 *      threadmgr globals.
 *   2. Dumber but robust fallback: dump the FIRST 0x40 words of the
 *      threadmgr data segment around the known thread-run-queue globals
 *      and record them twice (400 ms apart) like every other probe.
 *
 * v02.18 hardening against log42's death mode: there is NO catchable
 * fault path here (the kernel copy aborts in kernel context), so the
 * defense is upstream - kf_kaddr_plausible() gates every address fed
 * to the kernel copier to the two windows kernel memory actually
 * occupies, and module enumeration supplies segment addresses the
 * kernel itself reported. A bad offset degrades to a "skip" line
 * instead of a console panic. (log42's snapshot died differently: the
 * watchdog faulted inside vita_sched_nrt_info dereferencing a stale
 * Thread handle, and the resulting process death ate sample 2 +
 * dispatch ring + java-stack-top - fixed in Scheduler.cpp/OS_vita.cpp
 * by raw-word reads at Thread::*_offset() + range checks + snapshot
 * reordering.)
 */

#include <psp2/kernel/modulemgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/io/stat.h>
#include <psp2/io/fcntl.h>
#include <stdio.h>
#include <string.h>

#include "vita_kernel_forensics.h"

/* kubridge: syscall-backed kernel read. Declared weak - the stub lib is
 * linked, but on a console without the kubridge plugin the syscall trap
 * itself is fatal, so we NEVER call it before the module check below
 * has actually succeeded through the module list. */
extern int kuKernelCpuUnrestrictedMemcpy(void *dst, const void *src,
                                         unsigned int len);

#define KF_MAX_LINE 128

static int g_ku_ok = -1;             /* -1 unknown, 0 absent, 1 present */
/* v02.21 diagnostics for the ABSENT case: tells apart "plugin not
 * installed" from "installed but invisible to the userspace module
 * list" (the v02.20 real-hardware outcome: process booted fine, so the
 * strong fstubs import RESOLVED - the kernel module was there - yet the
 * enum saw nothing). Enum rc and module count come from the LAST list
 * attempt; the name-match verdict is ORed across all flags. */
static int g_ku_enum_rc = 0;         /* last sceKernelGetModuleList rc  */
static unsigned int g_ku_nmod = 0;   /* modules returned by that call   */
static unsigned int g_ku_flag = 0;   /* which list flag produced them   */

/* --------------------------------------------------------------- */
/* kubridge presence: enumerate kernel modules, look for kubridge.  */
/* sceKernelGetModuleList(0x00,...) lists KERNEL modules on fw 3.60 */
/* (0x01 = process modules, the app's own).                         */
/* --------------------------------------------------------------- */
static int ku_probe(void)
{
    SceUID uids[128];
    SceSize num;
    /* v02.22: the v02.21 real-hardware verdict "rc=0 nmod=0" exposed the
     * real bug - the FLAG DOMAIN was wrong, not the visibility. The SDK's
     * own sample (samples/prx_loader/main.c) uses 0x80|0x1 ("0/1 = user,
     * 0x80 = system") and sees ~13 modules from a plain userspace app;
     * bare 0x00..0x04 list nothing. So try the full system-domain matrix
     * and remember which flag produced how many modules. */
    static const unsigned int flags[] = {
        0x00,
        0x01, 0x02, 0x04,
        0x80 | 0x01, 0x80 | 0x02, 0x80 | 0x03, 0x80 | 0x04
    };
    int rc = -1, f, i;

    if (g_ku_ok >= 0)
        return g_ku_ok;

    for (f = 0; f < (int)(sizeof(flags) / sizeof(flags[0])); f++) {
        num = sizeof(uids) / sizeof(uids[0]);
        memset(uids, 0, sizeof(uids));
        rc = sceKernelGetModuleList(flags[f], uids, &num);
        if (rc < 0) {
            g_ku_enum_rc = rc;
            continue;
        }
        /* remember the first flag that yields ANY module - the log line
         * "enum flag=0x81 nmod=13" is the sanity anchor for the probe. */
        if (g_ku_nmod == 0) {
            g_ku_enum_rc = rc;
            g_ku_nmod = num;
            g_ku_flag = flags[f];
        }
        for (i = 0; (SceSize)i < num; i++) {
            SceKernelModuleInfo mi;
            memset(&mi, 0, sizeof(mi));
            mi.size = sizeof(mi);
            if (sceKernelGetModuleInfo(uids[i], &mi) < 0)
                continue;
            if (strcmp(mi.module_name, "kubridge") == 0) {
                g_ku_ok = 1;
                g_ku_enum_rc = rc;
                g_ku_nmod = num;
                g_ku_flag = flags[f];
                return 1;
            }
        }
    }
    g_ku_ok = 0;
    return 0;
}

/* Read kernel memory safely-or-report. Returns 0 on success.
 * NOTE (kubridge src, main.c:62): the copy is DACR-open + byte-wise
 * ldrbt/strbt IN KERNEL MODE - a genuinely unmapped src aborts in
 * kernel context and can panic the console. It is NOT catchable from
 * userspace. Therefore every address we feed it must be either
 * (a) returned by sceKernelGetModuleInfo (the kernel's own report of
 *     where a module segment lives), or
 * (b) pass kf_kaddr_plausible(): inside the two windows kernel memory
 *     actually occupies on fw 3.60/3.65 - the K0 direct map
 *     (0x81000000..0x83ffffff) and the kernel vmalloc/heap region
 *     (0x88800000..0x8affffff). A firmware offset shift produces an
 *     implausible word, which we SKIP rather than dereference. */
static int kf_kaddr_plausible(unsigned int a) {
    if (a == 0 || a == 0xFFFFFFFFu) return 0;
    if (a >= 0x81000000u && a < 0x84000000u) return 1; /* K0 direct  */
    if (a >= 0x88800000u && a < 0x8b000000u) return 1; /* kernel vm  */
    return 0;
}

static int kf_read(void *dst, unsigned int kaddr, unsigned int len)
{
    if (!ku_probe())
        return -1;
    if (!kf_kaddr_plausible(kaddr) ||
        !kf_kaddr_plausible(kaddr + len - 1))
        return -2;
    return kuKernelCpuUnrestrictedMemcpy(dst, (const void *)kaddr, len);
}

/* --------------------------------------------------------------- */
/* Kernel-side per-core scheduler state.                            */
/*                                                                   */
/* fw3.60/3.65 SceKernelThreadmgr layout (community-documented,      */
/* e.g. by the SceShell / VitaOS RE work):                           */
/*   threadmgr_base + 0x54  : SceCoreThreadContext *core_ctx[4]      */
/*   (array of pointers, one per CPU; NULL for unused cores)         */
/*   SceCoreThreadContext (per-CPU):                                 */
/*     +0x00 u32 current_thread SCEUID                               */
/*     +0x04 u32 ?                                                   */
/*     +0x08 u64 idle_counter (approx; sampled twice for delta)      */
/*                                                                   */
/* We do NOT trust these offsets blindly: kf_dump_corectx prints the  */
/* pointer array itself first, so a firmware mismatch is visible in   */
/* the log instead of silently reading garbage.                      */
/* --------------------------------------------------------------- */

static void kf_puts(SceUID fd, const char *s)
{
    int l = 0;
    if (!s) return;
    while (s[l]) l++;
    if (l > 0) sceIoWrite(fd, s, l);
}

/* Find SceKernelThreadmgr's kernel data segment via the module list.
 * The returned address comes from the kernel's OWN module table (not
 * from any hardcoded base), so a firmware shift moves it with us. */
static unsigned int kf_find_threadmgr(void)
{
    SceUID uids[128];
    SceSize num = 128;
    int rc, i;
    static unsigned int cached = 0;

    if (cached)
        return cached;

    rc = sceKernelGetModuleList(0x00, uids, &num);
    if (rc < 0)
        return 0;
    for (i = 0; (SceSize)i < num; i++) {
        SceKernelModuleInfo mi;
        memset(&mi, 0, sizeof(mi));
        mi.size = sizeof(mi);
        if (sceKernelGetModuleInfo(uids[i], &mi) < 0)
            continue;
        if (strcmp(mi.module_name, "SceKernelThreadmgr") == 0 ||
            strcmp(mi.module_name, "SceThreadmgrForKernel") == 0) {
            /* segments[1] is the RW (data) segment on Vita kernel mods */
            if (mi.segments[1].vaddr && mi.segments[1].memsz)
                cached = (unsigned int)mi.segments[1].vaddr;
            else if (mi.segments[0].vaddr)
                cached = (unsigned int)mi.segments[0].vaddr;
            return cached;
        }
    }
    return 0;
}

void vita_kf_dump(const char *path)
{
    SceUID fd;
    char line[KF_MAX_LINE];
    int n;
    unsigned int tmgr;
    unsigned int ptrs[4];
    int i;

    fd = sceIoOpen(path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0777);
    if (fd < 0)
        return;
    kf_puts(fd, "\n===== v02.18 kernel forensics =====\n");

    n = snprintf(line, sizeof(line), "kubridge: %s\n",
                 ku_probe() ? "present" : "ABSENT (kernel probes skipped)");
    sceIoWrite(fd, line, n);
    if (!ku_probe()) {
        /* v02.21: enum diagnostics - separates "no plugin" from
         * "plugin invisible to the userspace module list". v02.22 adds
         * the flag domain that actually produced the modules. */
        n = snprintf(line, sizeof(line),
                     "enum rc=%d flag=0x%02x nmod=%u\n",
                     g_ku_enum_rc, g_ku_flag, g_ku_nmod);
        sceIoWrite(fd, line, n);
        kf_puts(fd, "note: kubridge ABSENT - copy kubridge.skprx to ux0:tai/, "
                    "add '*KERNEL ux0:tai/kubridge.skprx' to config.txt\n");
        sceIoClose(fd);
        return;
    }

    tmgr = kf_find_threadmgr();
    n = snprintf(line, sizeof(line),
                 "SceKernelThreadmgr data @ 0x%08x\n", tmgr);
    sceIoWrite(fd, line, n);
    if (!tmgr) {
        kf_puts(fd, "threadmgr module not found - no kernel read\n");
        sceIoClose(fd);
        return;
    }
    /* The ONLY hardcoded offset in this probe: fw 3.60/3.65 community
     * RE places the per-core context pointer array at threadmgr data
     * +0x54. If a future firmware moves it, the words below fail the
     * plausibility gate and the log says so instead of the probe
     * feeding garbage to the kernel copier. */

    /* Sample 1: the per-core context pointer array. */
    kf_puts(fd, "--- sample 1 ---\n");
    if (kf_read(ptrs, tmgr + 0x54, sizeof(ptrs)) == 0) {
        for (i = 0; i < 4; i++) {
            n = snprintf(line, sizeof(line),
                         "core%d ctx ptr = 0x%08x\n", i, ptrs[i]);
            sceIoWrite(fd, line, n);
        }
        /* Per-core current thread + idle counter, twice for delta. */
        for (i = 0; i < 4; i++) {
            unsigned int w[4];
            if (!kf_kaddr_plausible(ptrs[i]))
                continue;
            if (kf_read(w, ptrs[i], sizeof(w)) == 0) {
                n = snprintf(line, sizeof(line),
                             "core%d: cur_tid=0x%08x w1=0x%08x "
                             "w2=0x%08x w3=0x%08x\n",
                             i, w[0], w[1], w[2], w[3]);
                sceIoWrite(fd, line, n);
            }
        }
    } else {
        kf_puts(fd, "corectx read FAILED\n");
    }

    sceKernelDelayThread(400 * 1000);

    kf_puts(fd, "--- sample 2 (+400ms) ---\n");
    if (kf_read(ptrs, tmgr + 0x54, sizeof(ptrs)) == 0) {
        for (i = 0; i < 4; i++) {
            unsigned int w[4];
            if (!kf_kaddr_plausible(ptrs[i]))
                continue;
            if (kf_read(w, ptrs[i], sizeof(w)) == 0) {
                n = snprintf(line, sizeof(line),
                             "core%d: cur_tid=0x%08x w1=0x%08x "
                             "w2=0x%08x w3=0x%08x\n",
                             i, w[0], w[1], w[2], w[3]);
                sceIoWrite(fd, line, n);
            }
        }
    }

    kf_puts(fd, "===== end kernel forensics =====\n");
    sceIoClose(fd);
}

/* v02.20: boot-time probe. Runs sceKernelGetModuleList(0x00) on the
 * MAIN thread at launcher start, so a round's kubridge visibility is
 * recorded in boot_log.txt even if the HANG path never fires. Same
 * tri-state as the HANG path: "present" (plugin visible), "ABSENT"
 * (enumeration works, no kubridge in the list), "err" (enumeration
 * itself failed - probe power unknown, log says so). */
const char *vita_kf_precheck(void)
{
    SceIoStat st;

    if (ku_probe())
        return "present";

    /* v02.21: the real-hardware v02.20 case. The process booted, so the
     * strong kubridge fstubs import RESOLVED at load time - the kernel
     * module IS resident; the enum just cannot see kernel modules from
     * userspace. If the plugin file exists on either tai dir and the
     * enum succeeded but listed no kubridge, that distinction is now
     * decidable from boot_log.txt: the path forward is a different
     * readout route (kernel-side dumper plugin), NOT re-installing. */
    if (g_ku_enum_rc >= 0 && g_ku_nmod > 0) {
        memset(&st, 0, sizeof(st));
        if (sceIoGetstat("ux0:tai/kubridge.skprx", &st) >= 0 ||
            sceIoGetstat("ur0:tai/kubridge.skprx", &st) >= 0)
            return "installed-NOT-visible";
    }
    return "ABSENT"; /* no plugin file: install it (or enum failed) */
}

/* v02.22: dump the module table itself (first entries) so ONE boot log
 * answers "what does the userspace enum actually see" - flag domain,
 * module count and names, no further round needed. */
void vita_kf_list_modules(const char *path)
{
    static const unsigned int flags[] = {
        0x80 | 0x01, 0x80 | 0x02, 0x80 | 0x03, 0x80 | 0x04
    };
    SceUID uids[128];
    SceIoStat st;
    SceUID fd;
    char line[KF_MAX_LINE];
    int n, f, i, shown;

    fd = sceIoOpen(path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0777);
    if (fd < 0)
        return;
    kf_puts(fd, "\n===== v02.22 module table =====\n");

    for (f = 0; f < (int)(sizeof(flags) / sizeof(flags[0])); f++) {
        SceSize num = sizeof(uids) / sizeof(uids[0]);
        memset(uids, 0, sizeof(uids));
        if (sceKernelGetModuleList(flags[f], uids, &num) < 0 || num == 0) {
            n = snprintf(line, sizeof(line), "flag 0x%02x: empty\n",
                         flags[f]);
            sceIoWrite(fd, line, n);
            continue;
        }
        n = snprintf(line, sizeof(line), "flag 0x%02x: %u modules\n",
                     flags[f], (unsigned int)num);
        sceIoWrite(fd, line, n);
        shown = 0;
        for (i = 0; (SceSize)i < num && shown < 24; i++) {
            SceKernelModuleInfo mi;
            memset(&mi, 0, sizeof(mi));
            mi.size = sizeof(mi);
            if (sceKernelGetModuleInfo(uids[i], &mi) < 0)
                continue;
            n = snprintf(line, sizeof(line), "  %s\n", mi.module_name);
            sceIoWrite(fd, line, n);
            shown++;
        }
    }
    memset(&st, 0, sizeof(st));
    n = snprintf(line, sizeof(line),
                 "tai ux0:%d ur0:%d\n",
                 sceIoGetstat("ux0:tai/kubridge.skprx", &st) >= 0,
                 sceIoGetstat("ur0:tai/kubridge.skprx", &st) >= 0);
    sceIoWrite(fd, line, n);
    kf_puts(fd, "===== end module table =====\n");
    sceIoClose(fd);
}
