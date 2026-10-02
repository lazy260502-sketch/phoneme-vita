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
#include <stdint.h>
#include <stdarg.h>

#include "vita_kernel_forensics.h"

/* kubridge: syscall-backed kernel read. Declared weak - the stub lib is
 * linked, but on a console without the kubridge plugin the syscall trap
 * itself is fatal, so we NEVER call it before the module check below
 * has actually succeeded through the module list. */
extern int kuKernelCpuUnrestrictedMemcpy(void *dst, const void *src,
                                         unsigned int len);
/* v02.23: the TERMINAL presence probe. kuPowerGetSysClockFrequency is
 * a plain kubridge export - if the module is resident the call crosses
 * the (weak) fstubs import and returns a sane clock (111/166/222/333/
 * 444 MHz); if the import was never resolved it returns 0 or a machine
 * error code. This needs NO module enumeration and NO file stat - it
 * answers "is the code in kernel memory" in one call. */
extern int kuPowerGetSysClockFrequency(void);

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
/* v02.23: direct-call probe result, cached (the call is cheap but we
 * only ever want one line in the log). -1 = not run, else the raw
 * return of kuPowerGetSysClockFrequency(). */
static int g_ku_clock = -1;

/* v02.23: why a WEAK import can coexist with "ABSENT" - the VitaSDK
 * stub libraries emit GEN_WEAK_EXPORTS symbols, so an unresolved
 * kubridge import does NOT block process start; the v02.21 inference
 * "strong import + process boots => module resident" was WRONG and is
 * formally retired here. The only decisive probe is CALLING one. */
static int ku_clock_probe(void)
{
    if (g_ku_clock >= 0)
        return g_ku_clock;
    g_ku_clock = kuPowerGetSysClockFrequency();
    return g_ku_clock;
}

static int ku_probe(void); /* defined below the flag-matrix comment */

/* v02.26: taiHEN cross-domain queries (module_get_by_name_nid with
 * KERNEL_PID, module_get_offset) are RETIRED ENTIRELY. log48 caught
 * the watchdog blocked forever inside that syscall; log49 caught the
 * MAIN THREAD dying in the SAME SVC veneer at BOOT - twice, one dump
 * per launch, game crashed at startup. On this console those
 * syscalls simply never return, healthy system or not. The
 * threadmgr address now comes from the userspace enum at boot,
 * cross-validated with a ku memcpy read-back. */

/* v02.25: shared threadmgr-address cache + provenance, written ONLY
 * at boot (kf_boot_cache), read by the HANG path. */
static unsigned int g_kf_tmgr_cache = 0;
static int g_kf_tried = 0;
static const char *g_kf_resolved_at = "none";

/* v02.33: VM TCB address found by the boot-time K0 signature scan.
 * This is the NAME-field address, not the struct base - the exact
 * name offset inside SceKernelThreadControlBlock is firmware data
 * we dump raw and resolve offline. Written ONLY at boot; the HANG
 * path just kf_reads it. */
static unsigned int g_kf_vm_tcb = 0;
static int g_kf_vm_tcb_tid_off = 0;   /* tid word offset rel. name */
static int kf_kaddr_plausible(unsigned int a);
static void kf_boot_cache(unsigned int addr, const char *where)
{
    if (addr && kf_kaddr_plausible(addr)) {
        g_kf_tmgr_cache = addr;
        g_kf_resolved_at = where;
    }
}

/* v02.24: the REAL availability gate. log47 proved the module enum is
 * structurally blind to taiHEN kernel plugins (flag 0x81 lists 15
 * modules, no kubridge) while the module IS resident - the direct
 * clock call returned 190 and the kernel boot log said "Starting...OK".
 * v02.23 and earlier gated kf_read/vita_kf_dump on ku_probe() alone,
 * so the kernel reads were SKIPPED on a console where they would have
 * worked. Availability is now: enum sees it OR the direct call returns
 * a sane clock (>= 41, lowest Vita bus clock). */
static int ku_available(void)
{
    if (ku_probe())
        return 1;
    return ku_clock_probe() >= 41;
}

/* v02.23: a kubridge.skprx rides INSIDE our own VPK (app0:), deployed
 * by the installer to ux0:app/J2ME00001/. taiHEN never loads it (it
 * only reads tai config.txt), but its presence makes the fix a single
 * config.txt line away. Probed at boot so the log teaches the exact
 * line to add. */
static int ku_bundled_probe(void)
{
    SceIoStat st;
    memset(&st, 0, sizeof(st));
    return sceIoGetstat("app0:kubridge.skprx", &st) >= 0;
}

/* v02.24-era taiHEN cross-domain imports (module_get_by_name_nid /
 * module_get_offset, KERNEL_PID) were REMOVED in v02.26: log49
 * core dumps caught the main thread dying inside that syscall at
 * BOOT - it never returns on this console. Do not re-add. */

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
    if (!ku_available())
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

/* v02.26: kf_find_threadmgr. The taiHEN cross-domain Route 1
 * (module_get_by_name_nid with KERNEL_PID) is RETIRED - log48 caught
 * the watchdog dying in that syscall, log49 caught the MAIN THREAD
 * dying in it at boot (startup crash). The ONLY route left is the
 * userspace enum: blind to kernel modules on log46/47-class firmware,
 * so it will usually find nothing - the dump then says
 * "resolved at: none". That is the honest answer on this console;
 * the address hunt moves to an offline scan of the core dumps. */
/* Single shared cache for the threadmgr data address. Seeded at BOOT
 * by vita_kf_boot_resolve() (enum + ku read-back self-check); the
 * HANG path only reads it. g_kf_tried guards the enum so it runs at
 * most once. Definitions live at the top of the file (shared with
 * kf_boot_cache). */
static unsigned int kf_find_threadmgr(void)
{
    SceUID uids[128];
    SceSize num = 128;
    int rc, i;

    /* v02.26: single file-scope cache (g_kf_tmgr_cache), seeded at
     * boot by kf_boot_cache(). The HANG path only READS it - zero
     * taiHEN syscalls anywhere (log48/49: they never return here),
     * the enum below runs at most once as the boot-time route. */
    if (g_kf_tmgr_cache)
        return g_kf_tmgr_cache;
    if (g_kf_tried)
        return 0;
    g_kf_tried = 1;

    /* Route 1 (taiHEN cross-domain, v02.24) was RETIRED in v02.26:
     * log49's core dumps caught the MAIN THREAD dying in that
     * syscall at BOOT (same SVC veneer, same args as log48's
     * watchdog). On this console it never returns in ANY context.
     * The boot resolve is the userspace enum below + a ku read-back
     * self-check. */

    /* Route 2: pre-v02.24 userspace enum (blind to kernel modules on
     * log46/47 firmware, kept as the taiHEN-less fallback). */
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
                g_kf_tmgr_cache =
                    (unsigned int)mi.segments[1].vaddr;
            else if (mi.segments[0].vaddr)
                g_kf_tmgr_cache =
                    (unsigned int)mi.segments[0].vaddr;
            return g_kf_tmgr_cache;
        }
    }
    return 0;
}

/* --------------------------------------------------------------- */
/* v02.33: K0 TCB SIGNATURE SCAN.                                   */
/*                                                                   */
/* Both threadmgr address routes are dead on this console: the      */
/* userspace enum is structurally blind to kernel modules (v02.21)  */
/* and the taiHEN cross-domain query never returns (log48/49). But  */
/* log55 proved kubridge memcpy works (clock=190) and log48 proved  */
/* ku reads return even from the HANG path. So skip the module      */
/* base entirely and find the VM thread's TCB by CONTENT:           */
/*                                                                   */
/*   scan the K0 direct map (0x81000000..0x84000000 - ALL physical   */
/*   RAM, no holes, so every read is abort-free) for the string     */
/*   "J2ME00001" (the main/VM thread name, set by SceAppMgr at      */
/*   process birth, present before this boot code runs), then       */
/*   verify the hit is a TCB by finding this thread's own tid       */
/*   (sceKernelGetThreadId) within +-0x100 words of the name.       */
/*                                                                   */
/* The vmalloc window (0x88800000+) is NOT scanned: it has          */
/* unmapped holes and a read there aborts in kernel context -       */
/* uncatchable, console-fatal.                                       */
/*                                                                   */
/* Runs at BOOT on the main thread (healthy system; the 48 MB scan  */
/* costs a few seconds); the HANG path only kf_reads the cached     */
/* address. Off-line, the raw TCB dump joins the coredump's         */
/* THREAD_REG_INFO (saved kernel pc/lr/sp) to name the kernel-side  */
/* wait point of the wedged VM thread.                               */
/* --------------------------------------------------------------- */
#define KF_K0_SCAN_START 0x81000000u
#define KF_K0_SCAN_END   0x84000000u
static unsigned char kf_scan_buf[0x8000];   /* 32 KB chunks */

static void kf_scan_log(const char *fmt, ...)
{
    SceUID fd = sceIoOpen("ux0:/data/J2ME00001/kernel_forensics.log",
                          SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0777);
    char line[160];
    va_list ap;
    int n;
    if (fd < 0)
        return;
    va_start(ap, fmt);
    n = vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    sceIoWrite(fd, line, n);
    sceIoClose(fd);
}

static int kf_scan_k0(unsigned int self_tid)
{
    /* chunk overlap 0x120 so a name at a chunk edge still gets its
     * full +-0x100 tid-verification window on the next pass */
    const unsigned int step = sizeof(kf_scan_buf) - 0x120;
    unsigned int base;
    int nhit = 0;

    if (!ku_available())
        return 0;

    for (base = KF_K0_SCAN_START; base < KF_K0_SCAN_END; base += step) {
        unsigned int len = sizeof(kf_scan_buf);
        unsigned int i;
        if (base + len > KF_K0_SCAN_END)
            len = KF_K0_SCAN_END - base;
        if (len < 0x140)
            break;
        if (kf_read(kf_scan_buf, base, len) != 0)
            continue;       /* should not happen inside K0 */
        for (i = 0; i + 10 <= len; i += 4) {
            unsigned int hit;
            int tid_off = -1;
            unsigned int j, j0, j1;
            if (kf_scan_buf[i] != 'J' ||
                memcmp(kf_scan_buf + i, "J2ME00001", 10) != 0)
                continue;
            hit = base + i;
            nhit++;
            /* tid self-check: this thread's own tid value must live
             * within +-0x100 of the name inside a real TCB */
            j0 = (i > 0x100) ? i - 0x100 : 0;
            j1 = i + 0x100;
            if (j1 + 4 > len)
                j1 = len - 4;
            for (j = j0; j <= j1; j += 4) {
                unsigned int v;
                memcpy(&v, kf_scan_buf + j, 4);
                if (v == self_tid) {
                    tid_off = (int)(j - i);
                    break;
                }
            }
            kf_scan_log("v02.33 K0 scan hit %d: name @ 0x%08x "
                        "tid %+d\n", nhit, hit, tid_off);
            if (tid_off >= -0x100 && tid_off <= 0x100) {
                g_kf_vm_tcb = hit;
                g_kf_vm_tcb_tid_off = tid_off;
                return 1;
            }
        }
    }
    kf_scan_log("v02.33 K0 scan: %d hits, none tid-verified "
                "(TCB not in K0?)\n", nhit);
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

    /* v02.25: the HANG path issues NO taiHEN syscall. The threadmgr
     * address must already be cached (vita_kf_boot_resolve ran at
     * boot, main thread, healthy system); if it is not, say so and
     * stop - log48 proved a fresh taiHEN query here blocks forever
     * (taiHEN's internal lock vs the wedged core). Only
     * kuKernelCpuUnrestrictedMemcpy reads happen below; log48 also
     * proved THOSE return fine from the HANG path (the clock line in
     * kernel_forensics.log was written after the watchdog entered
     * the dump). */
    kf_find_threadmgr();            /* consumes the boot cache only */

    fd = sceIoOpen(path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0777);
    if (fd < 0)
        return;
    kf_puts(fd, "\n===== v02.18 kernel forensics =====\n");

    n = snprintf(line, sizeof(line), "kubridge: %s\n",
                 ku_available() ? "present" : "ABSENT (kernel probes skipped)");
    sceIoWrite(fd, line, n);
    /* v02.23: direct-call probe runs FIRST - it works even when the
     * module enum is blind, and its result outranks the enum verdict. */
    {
        int clk = ku_clock_probe();
        n = snprintf(line, sizeof(line), "kuPowerGetSysClockFrequency: %d\n",
                     clk);
        sceIoWrite(fd, line, n);
    }
    if (!ku_available()) {
        /* v02.21: enum diagnostics - separates "no plugin" from
         * "plugin invisible to the userspace module list". v02.22 adds
         * the flag domain that actually produced the modules. */
        n = snprintf(line, sizeof(line),
                     "enum rc=%d flag=0x%02x nmod=%u\n",
                     g_ku_enum_rc, g_ku_flag, g_ku_nmod);
        sceIoWrite(fd, line, n);
        /* v02.23: the skprx ships inside our own VPK; if it survived
         * install, the fix is ONE config.txt line away - say so with
         * the exact line instead of telling the user to hunt files. */
        if (ku_bundled_probe()) {
            kf_puts(fd, "fix: add to tai config.txt (*KERNEL section):\n"
                        "  *KERNEL app0:kubridge.skprx\n"
                        "  (or copy to ux0:tai/ and use "
                        "*KERNEL ux0:tai/kubridge.skprx)\n");
        }
        kf_puts(fd, "note: kubridge ABSENT - copy kubridge.skprx to ux0:tai/, "
                    "add '*KERNEL ux0:tai/kubridge.skprx' to config.txt\n");
        sceIoClose(fd);
        return;
    }

    tmgr = kf_find_threadmgr();
    n = snprintf(line, sizeof(line),
                 "SceKernelThreadmgr data @ 0x%08x (resolved at %s)\n",
                 tmgr, g_kf_resolved_at);
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

    /* v02.33: the VM thread's kernel TCB, located by the boot-time
     * K0 signature scan. Dumped raw twice (400 ms apart, same
     * discipline as everything else): the SceKernelThreadControlBlock
     * layout is firmware data, so the dump goes to the log as hex
     * words and the offline join with THREAD_REG_INFO / runClocks
     * values resolves which word is what. The two samples also show
     * kernel-side mutation: a frozen TCB across the window is itself
     * evidence the scheduler stopped touching this thread. */
    if (g_kf_vm_tcb) {
        unsigned char tb[0x100];
        unsigned int tb_base = g_kf_vm_tcb - 0x80;
        int smp;
        n = snprintf(line, sizeof(line),
                     "\n--- v02.33 VM TCB @ 0x%08x (name @ +0x80, "
                     "tid %+d) ---\n", g_kf_vm_tcb,
                     g_kf_vm_tcb_tid_off);
        sceIoWrite(fd, line, n);
        for (smp = 0; smp < 2; smp++) {
            if (kf_read(tb, tb_base, sizeof(tb)) == 0) {
                int o;
                for (o = 0; o < (int)sizeof(tb); o += 16) {
                    unsigned int *w = (unsigned int *)(tb + o);
                    n = snprintf(line, sizeof(line),
                                 "s%d +0x%02x: %08x %08x %08x %08x\n",
                                 smp + 1, o, w[0], w[1], w[2], w[3]);
                    sceIoWrite(fd, line, n);
                }
            } else {
                kf_puts(fd, "TCB read FAILED\n");
            }
            if (smp == 0)
                sceKernelDelayThread(400 * 1000);
        }
    } else {
        kf_puts(fd, "\n--- v02.33 VM TCB: boot scan found none ---\n");
    }

    kf_puts(fd, "===== end kernel forensics =====\n");
    sceIoClose(fd);
}
/* v02.26: BOOT-time resolution of the threadmgr data address - now a
 * PURE USERSPACE route. Runs on the MAIN thread before the VM starts:
 * enum via sceKernelGetModuleList/info, then cross-validate the
 * segments[1] candidate with a ku memcpy read-back (first word must
 * come back non-garbage) before caching it. No taiHEN syscall is
 * issued anywhere - log49 proved those never return on this console
 * (boot crash, both dumps). Called right after vita_kf_precheck(). */
void vita_kf_boot_resolve(void)
{
    unsigned int addr;

    /* v02.33: content-address the VM TCB first - the module-base
     * routes are dead on this console, the name scan is not. The
     * main thread IS the VM thread-to-be (same tid for its whole
     * life), so its own tid self-verifies the hit. */
    kf_scan_k0((unsigned int)sceKernelGetThreadId());

    addr = kf_find_threadmgr();   /* userspace enum route */

    if (addr && ku_available()) {
        /* read-back self-check: can kubridge actually fetch from this
         * address? If not, the enum gave us a bad guess - drop it so
         * the HANG dump says "resolved at: none" instead of feeding
         * garbage offsets to the kernel copier. */
        unsigned int probe = 0;
        if (kf_read(&probe, addr, sizeof(probe)) != 0)
            addr = 0;
    }

    kf_boot_cache(addr, addr ? "boot(enum+readback)" : "none");
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

    /* v02.23: the enum says ABSENT, but the enum has been wrong before
     * (v02.20 wrong flags, v02.21 wrong inference). The direct call is
     * the tiebreaker: a sane clock (>= 41, the lowest Vita bus clock)
     * means the module IS resident regardless of what the enum sees. */
    {
        int clk = ku_clock_probe();
        if (clk >= 41)
            return "CALLABLE (enum blind)";
    }

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
                 "tai ux0:%d ur0:%d bundled(app0):%d\n",
                 sceIoGetstat("ux0:tai/kubridge.skprx", &st) >= 0,
                 sceIoGetstat("ur0:tai/kubridge.skprx", &st) >= 0,
                 ku_bundled_probe());
    sceIoWrite(fd, line, n);
    kf_puts(fd, "===== end module table =====\n");
    sceIoClose(fd);
}
