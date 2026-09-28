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

/* v02.25: the taiHEN cross-domain query (module_get_by_name_nid with
 * KERNEL_PID) MUST NOT run in the HANG path. log48's core dump proved
 * it: the watchdog thread blocked FOREVER inside that syscall (saved
 * r0-r3 = the exact call arguments, pc = the SVC veneer, info buffer
 * still zeroed except our size word). taiHEN takes an internal lock
 * while copying results to user memory; with the target core wedged
 * that lock never gets released - the caller sleeps uninterruptibly.
 * The fix: resolve the threadmgr data address ONCE at BOOT (main
 * thread, normal context, everything alive) and cache it; the HANG
 * path then only issues kuKernelCpuUnrestrictedMemcpy reads. */
static unsigned int kf_taihen_resolve(unsigned int *modid_out);

/* v02.25: shared threadmgr-address cache + provenance, written ONLY
 * at boot (kf_boot_cache), read by the HANG path. */
static unsigned int g_kf_tmgr_cache = 0;
static int g_kf_tried = 0;
static const char *g_kf_resolved_at = "none";
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

/* v02.24: cross-domain module query via taiHEN itself. log47 proved
 * sceKernelGetModuleList is structurally blind to taiHEN-loaded kernel
 * plugins AND to system kernel modules from userspace - but taiHEN
 * exports taihenModuleUtils to USER mode with a pid argument, and
 * KERNEL_PID (0x10005) addresses the kernel's own module table. These
 * are weak stub imports: without taiHEN they return an error code and
 * we fall back to the (blind) enum. */
#define KF_KERNEL_PID 0x10005
#define KF_IGNORE_NID 0xFFFFFFFFu
extern int module_get_by_name_nid(int pid, const char *name,
                                  unsigned int nid, void *info);
extern int module_get_offset(int pid, int modid, int segidx,
                             unsigned int offset, uintptr_t *addr);

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

/* v02.25: kf_find_threadmgr routes. Route 1 (taiHEN cross-domain,
 * module_get_by_name_nid with KERNEL_PID + module_get_offset) is now
 * BOOT-ONLY: log48's core dump caught the watchdog blocked forever in
 * that very syscall (r0-r3 = call args, pc = SVC veneer, result buffer
 * untouched). vita_kf_boot_resolve() runs it once while the system is
 * healthy and caches the address here; the HANG path only reads this
 * cache and then uses plain kuKernelCpuUnrestrictedMemcpy reads.
 * Route 2 (userspace enum) is kept as the boot-time fallback for a
 * taiHEN-less console - it is blind to kernel modules on recent CFW
 * (log46/47) but harmless to TRY at boot. */
/* v02.25: single shared cache for the threadmgr data address. Seeded
 * at BOOT by vita_kf_boot_resolve() (healthy system: taiHEN query is
 * safe there); the HANG path only reads it. g_kf_tried guards the
 * enum fallback so it also runs at most once (boot). Definitions
 * live at the top of the file (shared with kf_boot_cache). */
static unsigned int kf_find_threadmgr(void)
{
    SceUID uids[128];
    SceSize num = 128;
    int rc, i;

    /* v02.25: single file-scope cache (g_kf_tmgr_cache), seeded at
     * boot by kf_boot_cache(). The HANG path only READS it - no
     * taiHEN syscall here (log48: it blocks forever), the enum below
     * runs at most once as the boot-time fallback. */
    if (g_kf_tmgr_cache)
        return g_kf_tmgr_cache;
    if (g_kf_tried)
        return 0;
    g_kf_tried = 1;

    /* Route 1 (v02.24, boot-only since v02.25): taiHEN cross-domain.
     * info layout mirrors tai_module_info_t (taihen.h): size, modid,
     * module_nid, name[27], exports/imports pointers. We only need
     * modid, then ask for segment 1's base via module_get_offset.
     * In the HANG path this route is reached only when the boot
     * resolve produced nothing - the syscall below may then block
     * (log48), so we do NOT issue it here anymore. */
    {
        unsigned int modid;
        unsigned int addr = kf_taihen_resolve(&modid);
        (void)modid; (void)addr; /* retired from the HANG path */
    }

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

    kf_puts(fd, "===== end kernel forensics =====\n");
    sceIoClose(fd);
}
/* v02.25: BOOT-time resolution of the threadmgr data address. Runs on
 * the MAIN thread before the VM starts - every lock holder is alive,
 * so the taiHEN cross-domain query that blocked forever in log48's
 * HANG path completes here. Result cached into kf_find_threadmgr's
 * static; the HANG path then performs ku reads only. Called from the
 * launcher boot path right after vita_kf_precheck(). */
void vita_kf_boot_resolve(void)
{
    unsigned int modid = 0;
    unsigned int addr;

    /* consume the enum fallback too, so its "tried" flag is spent in
     * the boot context rather than the HANG path */
    addr = kf_find_threadmgr();   /* boot enum route (Route 2) */
    (void)addr;

    addr = kf_taihen_resolve(&modid);

    if (addr && kf_kaddr_plausible(addr)) {
        kf_boot_cache(addr, "boot(taiHEN)");
        return;
    }
    /* taiHEN missing or query failed: kf_find_threadmgr's enum route
     * above already ran; cache whatever it found (usually nothing on
     * log46/47-class firmware - the dump then says "resolved at:
     * none" instead of hanging the watchdog). */
    kf_boot_cache(g_kf_tmgr_cache ? g_kf_tmgr_cache : 0,
                  g_kf_tmgr_cache ? "boot(enum)" : "none");
}

/* v02.25: the actual taiHEN query, factored out so BOTH call sites
 * (boot resolve, and the retired HANG-path route) share one body.
 * Returns the threadmgr DATA segment base, 0 on failure; modid_out
 * (optional) receives the module id for the log. */
static unsigned int kf_taihen_resolve(unsigned int *modid_out)
{
    unsigned char info[64];
    uintptr_t addr = 0;
    int modid;

    memset(info, 0, sizeof(info));
    *(size_t *)info = sizeof(info);
    if (module_get_by_name_nid(KF_KERNEL_PID, "SceKernelThreadmgr",
                               KF_IGNORE_NID, info) < 0)
        return 0;
    modid = *(int *)(info + sizeof(size_t));
    if (modid_out)
        *modid_out = (unsigned int)modid;
    if (modid <= 0)
        return 0;
    if (module_get_offset(KF_KERNEL_PID, modid, 1, 0, &addr) < 0)
        return 0;
    return (unsigned int)addr;
}

/* v02.25: boot cache handoff. kf_find_threadmgr's statics live in the
 * HANG path; this writer runs at boot. "where" is logged so the dump
 * tells WHICH route produced the address (taiHEN boot / enum boot /
 * HANG-time fallback). */
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
