/*
 * vita_watchdog.c - v01.72 hang forensics: snapshot what the VM thread
 * is doing when the event pump stops.
 *
 * v01.73 revision (log5 lesson): the v01.72 watchdog stayed SILENT while
 * the pump froze with the exact log4 signature (ani_mark = "OUT"). Two
 * possible causes, both fixed here:
 *   1. silent startup failure - thread creation error returned without
 *      a trace. Now every outcome is logged (watchdog.log + crumb).
 *   2. clock-dependent stall detection - the 3 s threshold was measured
 *      with gettimeofday. If the clock itself is frozen (H2),
 *      "now - last_ce_change" NEVER exceeds the threshold - the watchdog
 *      is blind to the very scenario it hunts. Detection now counts
 *      poll ITERATIONS (12 x 250 ms ~= 3 s), no clock involved; the dual
 *      gettimeofday readings are kept in the REPORT for H2 evidence.
 * v01.75 polish (code review against vitasdk headers):
 *   - thread stack 0x2000 -> 0x4000: the dump path goes through
 *     snprintf + sceIo* with a ~200 byte SceKernelThreadInfo on stack;
 *     0x4000 matches j2me_tone, the project's verified floor. A stack
 *     overflow here gets the thread KILLED by the kernel - exactly the
 *     silent-watchdog failure this file exists to prevent.
 *   - the HANG report now samples BOTH clocks: gettimeofday (newlib/
 *     sceRtc链) AND sceKernelGetSystemTimeWide (kernel). If H2 is real,
 *     WHICH of the two froze pinpoints the broken layer.
 *   - cpuAffinityMask uses SCE_KERNEL_THREAD_CPU_AFFINITY_MASK_DEFAULT
 *     instead of a bare 0.
 */

#include <psp2/kernel/threadmgr.h>
#include <psp2/io/fcntl.h>
#include <psp2/kernel/processmgr.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <sys/time.h>

#include "vita_watchdog.h"
#include "vita_trace.h"
/* v02.02: OS-side thread forensics (--wrap= registration table). */
#include "vita_thread_audit.h"
/* v02.18: kubridge kernel-side forensics (HANG path). */
#include "vita_kernel_forensics.h"

typedef int64_t wd_jlong;

/* Pump entry counter - owned by vita_checkevents.c (v01.71 probe) */
extern volatile unsigned int vita_ce_count;
/* v01.83: tick count maintained by the CLDC cldc_ticker thread
 * (OS_vita.cpp). HANG report prints it to separate "ticker
 * dead/starved" from "ticks delivered but pump gate closed". */
extern volatile unsigned int vita_tick_count;
/* Set by vita_main.c: 1 while a MIDlet round runs (0 in the menu) */
volatile int vita_wd_round_active = 0;
/* VM thread id - recorded by vita_main.c right after the VM starts */
volatile SceUID vita_wd_vm_tid = -1;
/* Tone thread id - exported by vita_audio_javacall.c */
extern volatile SceUID vita_tone_tid;

/* v01.84: site-ring dump (defined in the VM's OS_vita.cpp, C linkage).
 * Writes the last-executed-native-site ring so a HANG names the loop
 * even when the coredump cannot see the Java heap. */
extern void vita_site_dump(const char *path);
extern void vita_site_dump_verify(const char *path);
extern void vita_site_dump_clinit(const char *path);
extern void vita_site_dump_java(const char *path);
/* v01.95: live-globals snapshot compiled into the VM image - reads
 * _current_thread/status/pending/jsp, VerifyOnly, scheduler counters
 * from the frozen-but-mapped process (the coredump cannot see them). */
extern void vita_wd_snapshot(const char *path);
#define WD_LOG_PATH "ux0:/data/J2ME00001/watchdog.log"
#define WD_STALL_POLLS 12   /* 12 x 250 ms ~= 3 s, CLOCK-FREE threshold */
#define WD_POLL_MS  250
/* v01.90: max extra polls while clinit/GC counters still advance
 * (~30 s at 250 ms): a real class-load storm converges well within
 * this; past it the round is effectively dead and we take the dump. */
#define WD_STORM_MAX_POLLS 120

static const char *wd_wait_type_name(SceUInt32 t) {
    /* waitType encoding follows the kernel's wait-category numbering
     * (vitasdk docs / Vita3K HLE): 0 none, then the classes below.
     * Report the raw number too - the name is a courtesy. */
    switch (t) {
        case 0:  return "none";
        case 1:  return "delay";       /* sceKernelDelayThread       */
        case 2:  return "eventflag";
        case 3:  return "mutex";
        case 4:  return "condvar";
        case 5:  return "cb";
        case 6:  return "lwcond";
        case 7:  return "lwmutex";
        case 8:  return "semaphore";
        case 9:  return "io";          /* sceIo* sync operations     */
        case 0x10: return "event";
        default: return "other";
    }
}

static const char *wd_status_name(SceUInt32 s) {
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

static wd_jlong wd_now_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (wd_jlong)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

/* Kernel clock (microseconds), independent of the gettimeofday chain:
 * if H2 (clock freeze) is real, which of the two froze names the layer. */
static wd_jlong wd_now_kernel_us(void) {
    return (wd_jlong)sceKernelGetSystemTimeWide();
}

/* Write one line to the watchdog log (append, crash-safe like crumb) */
static void wd_log(const char *buf, int n) {
    SceUID fd = sceIoOpen(WD_LOG_PATH,
                          SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0777);
    if (fd < 0) return;
    sceIoWrite(fd, buf, n);
    sceIoClose(fd);
}

static void wd_dump_thread(const char *tag, SceUID tid) {
    char buf[192];
    SceKernelThreadInfo info;
    memset(&info, 0, sizeof(info));
    info.size = sizeof(info);
    if (tid < 0 ||
        sceKernelGetThreadInfo(tid, &info) < 0) {
        int n = snprintf(buf, sizeof(buf), "[WD] %s tid=%d <unavailable>\n",
                         tag, (int)tid);
        wd_log(buf, n);
        return;
    }
    int n = snprintf(buf, sizeof(buf),
                     "[WD] %s '%s' status=%s wait=%s(0x%x) wid=%d "
                     "prio=0x%x stack=%d attr=0x%x aff=0x%x(init 0x%x) "
                     "cpu=%d lastcpu=%d preempt=%u\n",
                     tag, info.name, wd_status_name(info.status),
                     wd_wait_type_name(info.waitType), info.waitType,
                     (int)info.waitId, info.currentPriority,
                     (int)info.stackSize,
                     info.attr,
                     info.currentCpuAffinityMask, info.initCpuAffinityMask,
                     info.currentCpuId, info.lastExecutedCpuId,
                     info.threadPreemptCount);
    wd_log(buf, n);
    /* v02.08: runClocks is the thread's accumulated CPU time. Sample
     * it twice, 500 ms apart, INSIDE the hang window: if the kernel
     * really has this thread on a core the counter must advance by
     * hundreds of millions of cycles; if it is a bookkeeping ghost
     * (READY-but-never-picked / RUNNING-in-ledger-only) the delta is
     * 0. This is the one instrument that splits "kernel-mode spin"
     * from "scheduler lost the thread" without another coredump. */
    {
        SceUInt64 c0 = info.runClocks;
        SceUInt64 r0 = info.threadReleaseCount;
        sceKernelDelayThread(500 * 1000);
        memset(&info, 0, sizeof(info));
        info.size = sizeof(info);
        if (sceKernelGetThreadInfo(tid, &info) >= 0) {
            n = snprintf(buf, sizeof(buf),
                         "[WD] %s runClocks %llu -> %llu (d=%lld) "
                         "release=%llu->%llu chgcpu=%d\n",
                         tag, (unsigned long long)c0,
                         (unsigned long long)info.runClocks,
                         (long long)(info.runClocks - c0),
                         (unsigned long long)r0,
                         (unsigned long long)info.threadReleaseCount,
                         (int)info.changeCpuCount);
            wd_log(buf, n);
        }
    }
}

/* v02.15: VM-side progress, dual-sampled INSIDE the hang window.
 * log39 verdict: the per-core ledger froze (cpu1 idleN/switchN d=0) and
 * the vm thread's runClocks delta was 0 - but both of those are KERNEL
 * bookkeeping, and the VM's own scheduler counters had only ever been
 * read ONCE (in vita_wd_snapshot), where a frozen value is
 * indistinguishable from "healthy but slow". Sampling the VM's own
 * counters twice, 500 ms apart, separates the surviving hypotheses:
 *   switch/fast/site/jsmp move -> the VM thread IS executing Java
 *   tick (cldc_ticker) moves    -> cpu1's scheduler still dispatches
 * The ticker is pinned to the VM's core at the SAME priority 0xa0
 * (v02.14), so a frozen tick with a live cpu1 is impossible unless the
 * core itself stopped. Four combinations, four named failures - see
 * the verdict line. All counters are written by other threads
 * (single-writer, torn values tolerated) and reach this C TU through
 * C-linkage symbols (OS_vita.cpp / Scheduler.cpp). */
extern volatile unsigned int vita_site_seq;
extern volatile unsigned int vita_java_sample_count;
extern unsigned int vita_sched_yield_fast(void);
extern unsigned int vita_sched_pump_runs(void);
extern unsigned int vita_sched_switch_count(void);
extern unsigned int vita_sched_ttk_count(void);

static void wd_dump_vm_progress(void) {
    char buf[256];
    int n;
    unsigned int sw0, sf0, s0, jm0, tc0, tk0, pm0;
    unsigned int sw1, sf1, s1, jm1, tc1, tk1, pm1;
    int exec_moved, tick_moved;
    const char *verdict;

    sw0 = vita_sched_switch_count();
    sf0 = vita_sched_yield_fast();
    s0  = vita_site_seq;
    jm0 = vita_java_sample_count;
    tc0 = vita_tick_count;
    tk0 = vita_sched_ttk_count();
    pm0 = vita_sched_pump_runs();

    sceKernelDelayThread(500 * 1000);

    sw1 = vita_sched_switch_count();
    sf1 = vita_sched_yield_fast();
    s1  = vita_site_seq;
    jm1 = vita_java_sample_count;
    tc1 = vita_tick_count;
    tk1 = vita_sched_ttk_count();
    pm1 = vita_sched_pump_runs();

    n = snprintf(buf, sizeof(buf),
                 "[WD] VMPROG-A vm-exec switch=%u->%u(d=%d) "
                 "fast=%u->%u(d=%d) site=%u->%u(d=%d) "
                 "jsmp=%u->%u(d=%d)\n",
                 sw0, sw1, (int)(sw1 - sw0), sf0, sf1,
                 (int)(sf1 - sf0), s0, s1, (int)(s1 - s0),
                 jm0, jm1, (int)(jm1 - jm0));
    wd_log(buf, n);

    n = snprintf(buf, sizeof(buf),
                 "[WD] VMPROG-B tick-ch tick=%u->%u(d=%d) "
                 "ttk=%u->%u(d=%d) pump=%u->%u(d=%d)\n",
                 tc0, tc1, (int)(tc1 - tc0), tk0, tk1,
                 (int)(tk1 - tk0), pm0, pm1, (int)(pm1 - pm0));
    wd_log(buf, n);

    /* The verdict: which of the two liveness sources survived. */
    exec_moved = (sw1 != sw0) || (sf1 != sf0) ||
                 (s1 != s0) || (jm1 != jm0);
    tick_moved = (tc1 != tc0);
    if (exec_moved && tick_moved) {
        verdict = "BOTH ALIVE -> VM/Java-layer livelock";
    } else if (exec_moved) {
        verdict = "VM RUNS, ticker STARVED -> core1 dispatches, ticker starved";
    } else if (tick_moved) {
        verdict = "ticker RUNS, VM NOT EXECUTING -> VM stuck in a kernel wait";
    } else {
        verdict = "NEITHER -> cpu1 scheduler DEAD (direction B: kubridge)";
    }
    n = snprintf(buf, sizeof(buf),
                 "[WD] VMPROG verdict exec=%d tick=%d -> %s\n",
                 exec_moved, tick_moved, verdict);
    wd_log(buf, n);
}

/* v01.90: VM-side progress counters (OS_vita.cpp). A class-load storm
 * (log14..16) or a GC cycle advances these while the CE pump is starved:
 * that is "pathologically slow", not "dead" - the watchdog must not
 * kill the process while forensics are still improving. */
extern volatile unsigned int vita_clinit_count;
extern volatile unsigned int vita_gc_count;
/* v01.91: eager-preload progress (one per successfully defined jar
 * class during the boot-time define sweep, see JVM::load_main_class).
 * The sweep freezes the CE pump by design for its whole duration;
 * without this counter the watchdog would kill the process mid-boot. */
extern volatile unsigned int vita_preload_count;

static int wd_thread_routine(SceSize args, void *argp) {
    (void)args; (void)argp;
    unsigned int last_ce = 0;
    unsigned int last_clinit = 0;
    unsigned int last_gc = 0;
    unsigned int last_preload = 0;
    int stalled_polls = 0;
    int reported = 0;
    int storm_noted = 0;
    int preload_extensions = 0;   /* v01.92: bounded grace for the sweep */
    int kick_recoveries = 0;      /* v02.06: KICK-RECOVERED count */

    for (;;) {
        sceKernelDelayThread(WD_POLL_MS * 1000);

        if (!vita_wd_round_active) {
            /* menu runs its own loop - only watch MIDlet rounds */
            last_ce = vita_ce_count;
            last_clinit = vita_clinit_count;
            last_gc = vita_gc_count;
            last_preload = vita_preload_count;
            stalled_polls = 0;
            reported = 0;
            storm_noted = 0;
            preload_extensions = 0;
            continue;
        }

        if (vita_ce_count != last_ce) {
            last_ce = vita_ce_count;
            last_clinit = vita_clinit_count;
            last_gc = vita_gc_count;
            last_preload = vita_preload_count;
            stalled_polls = 0;   /* pump moved - hang not present */
            storm_noted = 0;
            preload_extensions = 0;
            continue;
        }

        /* v01.90: CE frozen, but if clinit/GC counters still advance the
         * VM is making progress through a class-load storm or a GC run
         * on real I/O - the emulator hides this (fast x86 JIT + host
         * fs), the real handheld does not. Keep waiting (up to
         * WD_STORM_MAX_POLLS) instead of killing the process, but note
         * the storm once in the log so post-mortems see it.
         * v01.91: vita_preload_count joins the group - the boot-time
         * define sweep advances it once per class while CE is frozen
         * by design. */
        if (vita_clinit_count != last_clinit ||
            vita_gc_count != last_gc ||
            vita_preload_count != last_preload) {
            /* capture BEFORE reassigning the baselines below */
            int preload_advanced = (vita_preload_count != last_preload);
            last_clinit = vita_clinit_count;
            last_gc = vita_gc_count;
            last_preload = vita_preload_count;
            if (!storm_noted) {
                char buf[128];
                int n = snprintf(buf, sizeof(buf),
                                 "[WD] STORM ce=%u clinit=%u gc=%u "
                                 "preload=%u "
                                 "(pump starved but progressing)\n",
                                 vita_ce_count, last_clinit, last_gc,
                                 last_preload);
                wd_log(buf, n);
                storm_noted = 1;
            }
            if (preload_advanced) {
                /* v01.92: the boot-time define sweep is bounded by the
                 * jar size, but log18 showed "advancing preload" can
                 * still mask a real stall (the frozen VM was mid
                 * sceIoWrite, one native class parse still short of
                 * the next counter tick). Grant the sweep a generous
                 * extension instead of no deadline: each advancing
                 * poll resets the stall counter, but WD_STORM_MAX_POLLS
                 * polls (~30 s) without ANY progress since the last
                 * extension still falls through to the HANG report so
                 * a dead sweep always leaves a core dump. */
                stalled_polls = 0;
                if (++preload_extensions > WD_STORM_MAX_POLLS) {
                    /* every poll advanced preload yet extensions
                     * exceeded the cap - impossible unless the
                     * counter is broken; stop trusting it. */
                    preload_advanced = 0;
                } else {
                    continue;
                }
            }
            if (stalled_polls < WD_STORM_MAX_POLLS) {
                stalled_polls++;  /* count slowly: 1 per poll while the
                                   * counters move; full stall trips */
                if (stalled_polls < WD_STALL_POLLS)
                    continue;
            }
            /* counters still moving after WD_STORM_MAX_POLLS => treat as
             * hung anyway (a storm must converge within ~30 s or it is
             * effectively dead); fall through to the HANG report. */
        }

        /* counter frozen; require WD_STALL_POLLS consecutive frozen
         * polls before declaring a hang (iteration count, NOT clock -
         * a frozen gettimeofday must not blind the watchdog). */
        if (++stalled_polls < WD_STALL_POLLS) continue;
        if (reported) continue;  /* one report per hang */

        /* ---- hang detected: forensic snapshot ----
         * clock liveness: two readings 100 ms apart must differ.
         * BOTH clocks are sampled: gettimeofday (newlib/sceRtc) and the
         * kernel time (sceKernelGetSystemTimeWide). kd ~= 100000 us if
         * the kernel clock is healthy; d==0 with kd!=0 means only the
         * gettimeofday chain froze (layer pinned), both stuck = deeper. */
        wd_jlong t0 = wd_now_ms();
        wd_jlong k0 = wd_now_kernel_us();
        sceKernelDelayThread(100 * 1000);
        wd_jlong t1 = wd_now_ms();
        wd_jlong k1 = wd_now_kernel_us();
        {
            char buf[256];
            int n = snprintf(buf, sizeof(buf),
                             "[WD] HANG ce=%u polls=%d tick=%u "
                             "clock t0=%lld t1=%lld d=%lld | "
                             "kclock k0=%lld k1=%lld kd=%lld us\n",
                             last_ce, stalled_polls, vita_tick_count,
                             (long long)t0, (long long)t1,
                             (long long)(t1 - t0),
                             (long long)k0, (long long)k1,
                             (long long)(k1 - k0));
            wd_log(buf, n);
        }

        /* v02.15: VM-side dual sampling FIRST - before the runClocks
         * dumps and, critically, before the KICK block below mutates
         * the VM thread's affinity/priority. This is the baseline that
         * says whether the VM thread or the core died. */
        wd_dump_vm_progress();

        wd_dump_thread("vm", vita_wd_vm_tid);
        wd_dump_thread("tone", vita_tone_tid);
        /* the watchdog itself, as a sanity marker */
        wd_dump_thread("wd", sceKernelGetThreadId());
        /* v02.02: full registered-thread table + per-core run state.
         * Proves or disproves "VM thread starved by higher-priority
         * natives" in one snapshot - the OS-side evidence the
         * Vita3K-vs-real-HW gap analysis needs. */
        vita_thread_audit_dump("ux0:/data/J2ME00001/site_ring.log");
        vita_thread_audit_dump_vm_marker("ux0:/data/J2ME00001/site_ring.log");

        /* v02.32: THREAD CENSUS - enumerate EVERY thread in the
         * process, not just the registered ones. log54 verdict:
         * cpu0 showed a 200 ms zero-switch busy owner that is NOT any
         * sampled thread (VM d=0, wd asleep, tone in delay); the
         * dump's THREAD_INFO note names exactly 5 threads, one of
         * them 'SceCommonDialogWorker' (a Vita3K HLE worker we never
         * sample) - and in log53 AND log54 that thread's registers
         * froze bit-identical at pc=0xe0009304 inside the HLE stub
         * range. The census must name the spinner directly.
         * sceKernelGetThreadIdList is NOT exported to user mode
         * (checked the stub archive), so enumerate by UID-space scan:
         * tids are dense (0x40010001..0x400101ff covers every thread
         * the audit table plus any HLE workers), holes simply return
         * an error which we skip silently.
         * SAFETY (log33/34 lesson): log the tid BEFORE querying it -
         * if GetThreadInfo ever aborts in the kernel on a corrupt
         * TCB, the last logged tid is the corrupt one (built-in
         * bisect marker). GetThreadInfo on a healthy-looking tid has
         * survived 15 hang rounds (wd_dump_thread), and 0x80020008
         * on KICK5 says the VM tid's bookkeeping is odd but its
         * INFO read is fine. Per thread: name/status/cpu/aff +
         * runClocks sampled twice 200 ms apart - the spinner is the
         * tid whose d(runClocks) ~= the full window while idle/switch
         * ledgers say a core never yielded. */
        {
            char cbuf[192];
            SceKernelThreadInfo ci;
            SceUID tids[64];
            SceUInt64 rc0[64];
            int ntid = 0;
            int ci_;
            int m = snprintf(cbuf, sizeof(cbuf),
                             "[WD] census: uid-scan 0x40010001..0x400101ff\n");
            wd_log(cbuf, m);
            for (SceUID t = 0x40010001; t <= 0x400101ff && ntid < 64; t++) {
                int nl = snprintf(cbuf, sizeof(cbuf),
                                  "[WD] census probe tid=0x%x\n", t);
                wd_log(cbuf, nl);   /* log BEFORE query (log33/34) */
                memset(&ci, 0, sizeof(ci));
                ci.size = sizeof(ci);
                if (sceKernelGetThreadInfo(t, &ci) < 0)
                    continue;       /* hole: tid never existed / freed */
                rc0[ntid] = ci.runClocks;
                tids[ntid] = t;
                ntid++;
                m = snprintf(cbuf, sizeof(cbuf),
                             "[WD] census '%s' status=%s cpu=%d "
                             "lastcpu=%d aff=0x%x prio=0x%x "
                             "runClocks=%llu\n",
                             ci.name, wd_status_name(ci.status),
                             ci.currentCpuId, ci.lastExecutedCpuId,
                             ci.currentCpuAffinityMask,
                             ci.currentPriority,
                             (unsigned long long)ci.runClocks);
                wd_log(cbuf, m);
            }
            m = snprintf(cbuf, sizeof(cbuf),
                         "[WD] census found %d threads, sampling "
                         "+200ms\n", ntid);
            wd_log(cbuf, m);
            sceKernelDelayThread(200 * 1000);
            for (ci_ = 0; ci_ < ntid; ci_) {
                memset(&ci, 0, sizeof(ci));
                ci.size = sizeof(ci);
                if (sceKernelGetThreadInfo(tids[ci_], &ci) < 0) {
                    m = snprintf(cbuf, sizeof(cbuf),
                                 "[WD] census tid=0x%x <gone>\n",
                                 tids[ci_]);
                    wd_log(cbuf, m);
                    ci_++;
                    continue;
                }
                m = snprintf(cbuf, sizeof(cbuf),
                             "[WD] census '%s' runClocks d=%lld "
                             "release d=%lld\n",
                             ci.name,
                             (long long)(ci.runClocks - rc0[ci_]),
                             (long long)(ci.threadReleaseCount -
                                         (SceUInt64)0));
                wd_log(cbuf, m);
                ci_++;
            }
        }

        /* v02.10: THERAPEUTIC KICK - MOVED UP, before the supplementary
         * ring/snapshot captures. log33/34 already banked two complete
         * forensic sets (identical picture: d=0 ghost, site ring
         * wait_for+0x54<-pump_events_vita+0x8, clinit/UI verify
         * residue); the kick is now the main experiment and must not
         * be blocked by anything before it. Order preserved from the
         * verdict instruments above (comparability with log33/34).
         * Escalating invasiveness:
         *   KICK0 sceKernelSendSignal      - signal path, re-mark
         *   KICK1 aff->0x30000             - forced runqueue migration
         *   KICK2 aff->0x70000             - migration back
         *   KICK3 prio->0x99               - priority requeue
         *   KICK4 ChangeThreadPriority2    - immediate priority apply
         *   KICK5 Suspend/ResumeThreadForVM- forced stop/restart of
         *                                    the thread's dispatch
         *                                    state (v02.10, the only
         *                                    suspend primitive the
         *                                    stub exports; ForVM
         *                                    variants are absent
         *                                    from SDK headers, hence
         *                                    extern here). If suspend
         *                                    or resume trips the same
         *                                    corrupt-TCB path that
         *                                    killed the runstatus
         *                                    probe, the kernel data
         *                                    abort still produces a
         *                                    psp2core dump - either
         *                                    way we learn.
         * Recovery (ce moves) => RE-ARM and keep the process alive:
         * the winning kick is the shipping workaround. All prongs
         * dead => fall through to the remaining captures + abort. */
        {
            unsigned int ce_before = vita_ce_count;
            int moved = 0;
            int kpoll;
            char buf[128];
            int n;
            int rc;
            rc = sceKernelSendSignal(vita_wd_vm_tid);
            n = snprintf(buf, sizeof(buf),
                         "[WD] KICK0 signal rc=0x%x\n", rc);
            wd_log(buf, n);
            for (kpoll = 0; kpoll < 4; kpoll++) {
                sceKernelDelayThread(WD_POLL_MS * 1000);
                if (vita_ce_count != ce_before) { moved = 1; break; }
            }
            if (!moved) {
            rc = sceKernelChangeThreadCpuAffinityMask(vita_wd_vm_tid,
                                                      0x30000);
            n = snprintf(buf, sizeof(buf),
                         "[WD] KICK1 aff->0x30000 rc=0x%x\n", rc);
            wd_log(buf, n);
            for (kpoll = 0; kpoll < 4; kpoll++) {
                sceKernelDelayThread(WD_POLL_MS * 1000);
                if (vita_ce_count != ce_before) { moved = 1; break; }
            }
            if (!moved) {
                rc = sceKernelChangeThreadCpuAffinityMask(vita_wd_vm_tid,
                                                          0x70000);
                n = snprintf(buf, sizeof(buf),
                             "[WD] KICK2 aff->0x70000 rc=0x%x\n", rc);
                wd_log(buf, n);
                for (kpoll = 0; kpoll < 8; kpoll++) {
                    sceKernelDelayThread(WD_POLL_MS * 1000);
                    if (vita_ce_count != ce_before) { moved = 1; break; }
                }
            }
            if (!moved) {
                rc = sceKernelChangeThreadPriority(vita_wd_vm_tid, 0x99);
                n = snprintf(buf, sizeof(buf),
                             "[WD] KICK3 prio->0x99 rc=0x%x\n", rc);
                wd_log(buf, n);
                for (kpoll = 0; kpoll < 4; kpoll++) {
                    sceKernelDelayThread(WD_POLL_MS * 1000);
                    if (vita_ce_count != ce_before) { moved = 1; break; }
                }
                sceKernelChangeThreadPriority(vita_wd_vm_tid, 0xa0);
            }
            if (!moved) {
                extern int sceKernelChangeThreadPriority2(SceUID thid,
                                                          int priority);
                rc = sceKernelChangeThreadPriority2(vita_wd_vm_tid,
                                                    0x99);
                n = snprintf(buf, sizeof(buf),
                             "[WD] KICK4 prio2->0x99 rc=0x%x\n", rc);
                wd_log(buf, n);
                for (kpoll = 0; kpoll < 4; kpoll++) {
                    sceKernelDelayThread(WD_POLL_MS * 1000);
                    if (vita_ce_count != ce_before) { moved = 1; break; }
                }
                sceKernelChangeThreadPriority(vita_wd_vm_tid, 0xa0);
            }
            if (!moved) {
                extern int sceKernelSuspendThreadForVM(SceUID thid);
                extern int sceKernelResumeThreadForVM(SceUID thid);
                int rs, rr;
                rs = sceKernelSuspendThreadForVM(vita_wd_vm_tid);
                sceKernelDelayThread(WD_POLL_MS * 1000);
                rr = sceKernelResumeThreadForVM(vita_wd_vm_tid);
                n = snprintf(buf, sizeof(buf),
                             "[WD] KICK5 susp/res rc=0x%x/0x%x\n",
                             rs, rr);
                wd_log(buf, n);
                for (kpoll = 0; kpoll < 8; kpoll++) {
                    sceKernelDelayThread(WD_POLL_MS * 1000);
                    if (vita_ce_count != ce_before) { moved = 1; break; }
                }
            }
            n = snprintf(buf, sizeof(buf),
                         "[WD] KICK %s (ce %u -> %u)\n",
                         moved ? "RECOVERED" : "no effect",
                         ce_before, vita_ce_count);
            wd_log(buf, n);
            if (moved) {
                /* recovered: keep the process alive, re-arm for the
                 * next hang so we learn whether one kick is enough
                 * or the loss recurs (recurrence count matters). */
                reported = 0;
                stalled_polls = 0;
                storm_noted = 0;
                preload_extensions = 0;
                last_ce = vita_ce_count;
                last_clinit = vita_clinit_count;
                last_gc = vita_gc_count;
                last_preload = vita_preload_count;
                if (++kick_recoveries < 4) continue;
                /* >3 recoveries in one session: the bit is being lost
                 * repeatedly; stop re-arming and take a coredump at
                 * the next hang so the kernel-side state is captured
                 * while the phenomenon is fresh. */
                {
                    int m = snprintf(buf, sizeof(buf),
                                     "[WD] KICK recovery #%d - arming "
                                     "final capture\n", kick_recoveries);
                    wd_log(buf, m);
                }
            }
            } /* end KICK0 !moved guard (KICK1..KICK5) */
        }

        /* v01.84: dump the VM's site ring BEFORE the coredump request.
         * log10 proved the dump cannot name a native loop running on a
         * green thread's Java stack; the ring can. It writes with
         * sceIo* (no stdio) and is safe from this thread. */
        vita_site_dump("ux0:/data/J2ME00001/site_ring.log");
        /* v01.85: name the class.method the verifier was on when the
         * hang began (log11 froze inside verify_class itself). */
        vita_site_dump_verify("ux0:/data/J2ME00001/site_ring.log");
        /* v01.88: name the class whose <clinit> was executing when the
         * hang began (log14 froze with the verifier off, spin moved
         * into clinit execution itself). */
        vita_site_dump_clinit("ux0:/data/J2ME00001/site_ring.log");
        /* v02.05: name the Java class.method the interpreter was in at
         * the last timer tick (log29: every lane/pump counter frozen
         * while pc sat in Thread::timer_tick - this names the loop
         * and its sample count separates the surviving hypotheses). */
        vita_site_dump_java("ux0:/data/J2ME00001/site_ring.log");
        /* v01.95: live-globals snapshot FIRST (it dereferences VM data
         * structures; if the VM image ever gets unmapped mid-crash the
         * ring dumps below still ran). log21: the coredump's capture
         * holes (.data/.bss head, whole Java heap) hid exactly these
         * values - read them from the patient, not the corpse. */
        vita_wd_snapshot("ux0:/data/J2ME00001/site_ring.log");
        /* v02.27: syscall deadpoint trace. Per-thread slots remember
         * the last call each thread ENTERED; ODD seq = never returned.
         * This runs BEFORE the coredump abort so the frozen process
         * itself tells us which syscall (if any) the VM thread died
         * in - the question 13 rounds of offline forensics could not
         * answer (kernel dump encrypted, register pc a dump artifact). */
        vita_trace_dump("ux0:/data/J2ME00001/trace_ring.log");

        /* v02.10: per-core scheduler ledger - SAFELY.
         * log33/34 verdict: sceKernelGetThreadRunStatus on the ghost
         * thread kills the CALLER inside the kernel (log34: wd died
         * with pc on the instruction after the blx, args still in
         * r0/r1, static 1KiB buffer untouched - a kernel-side data
         * abort on the ghost's corrupt TCB). That probe is RETIRED
         * permanently. The replacement is the GLOBAL ledger:
         * sceKernelGetSystemInfo reports per-core idle clock,
         * idle-exit count and thread SWITCH count and never
         * dereferences an individual thread's control block.
         * Two samples 200 ms apart split the remaining cases:
         * switchN frozen on the cpu the ledger names while our own
         * polling thread runs => the per-core scheduler itself is
         * stuck (worse than a single-thread ghost); switchN moving
         * => cores dispatch fine, only the vm thread ghosts. */
        {
            SceKernelSystemInfo si, si2;
            char buf[160];
            int n, rc, i;
            memset(&si, 0xCC, sizeof(si));
            si.size = sizeof(si);
            rc = sceKernelGetSystemInfo(&si);
            sceKernelDelayThread(200 * 1000);
            memset(&si2, 0xCC, sizeof(si2));
            si2.size = sizeof(si2);
            sceKernelGetSystemInfo(&si2);
            n = snprintf(buf, sizeof(buf),
                         "[WD] sysinfo rc=0x%x active=0x%x size=%u\n",
                         rc, si.activeCpuMask, si.size);
            wd_log(buf, n);
            for (i = 0; i < 4; i++) {
                n = snprintf(buf, sizeof(buf),
                             "[WD] sysinfo cpu%d idle %llu->%llu "
                             "idleN %u->%u switchN %u->%u\n",
                             i,
                             (unsigned long long)si.cpuInfo[i].idleClock,
                             (unsigned long long)si2.cpuInfo[i].idleClock,
                             si.cpuInfo[i].comesOutOfIdleCount,
                             si2.cpuInfo[i].comesOutOfIdleCount,
                             si.cpuInfo[i].threadSwitchCount,
                             si2.cpuInfo[i].threadSwitchCount);
                wd_log(buf, n);
            }
        }

        /* v02.18: kernel-side forensics via kubridge. The sysinfo block
         * above can NAME the stalled core but says nothing about WHY it
         * stopped; the coredump's SYSTEM_INFO2 is hashed so the kernel
         * half was never readable offline. If the kubridge plugin is
         * installed this reads the threadmgr's per-core context from
         * kernel memory (twice, 400 ms apart, same discipline as every
         * other probe); without the plugin it degrades to one explicit
         * "kubridge ABSENT" line so the log states WHY there is no
         * kernel data this round. */
        vita_kf_dump("ux0:/data/J2ME00001/kernel_forensics.log");

        /* v01.82: log8 nailed the shape of the hang (VM thread RUNNING,
         * wait=none, both clocks alive) but the kernel API cannot name
         * WHERE in userland it spins. A coredump can: faulting PC + the
         * full stack memory pin the exact interpreter/GC/native site.
         * Vita writes psp2core-*.psp2dmp on a fatal signal; the cleanest
         * userspace way to request one is sceKernelSendSignal 
         * followed by an intentional data abort. We deliberately write
         * to a read-only address (the .text segment base): the resulting
         * SIGSEGV/data-abort is exactly what the dumper catches. 
         * NOTE: this kills the process on purpose - the session is
         * already lost (every round so far ended in a force-kill), and
         * the dump is worth more than a frozen process. */
        {
            char buf[128];
            int n = snprintf(buf, sizeof(buf),
                             "[WD] requesting coredump via data abort\n");
            wd_log(buf, n);
            /* give the log write a moment to hit the disk */
            sceKernelDelayThread(200 * 1000);
        }
        {
            volatile int *text_base = (volatile int *)0x81000000;
            *text_base = 0x53455244; /* "DRES" - write to read-only .text */
        }
        /* not reached on success */

        reported = 1;
    }
    return 0;
}

void vita_watchdog_start(void) {
    /* log6 lesson: priority 0x10000300 -> SCE_KERNEL_ERROR_ILLEGAL_PRIORITY
     * (0x80028023) on EVERY boot on real hardware.
     * log7 lesson (the important one): v01.74's "fix" to 0x10000150
     * FAILED THE SAME WAY - watchdog.log shows `create FAILED
     * rc=0x80028023` on every v01.80 boot too. This thread has NEVER
     * run once on real hardware: every "no HANG report" so far meant
     * "no watchdog", not "no hang", and the whole v01.71-v01.75
     * forensics apparatus was dead weight. The only priority
     * empirically PROVEN to create+run on this firmware (3.65) is
     * 0x10000100 - j2me_tone and cldc_ticker both use it and tone
     * requests demonstrably execute. There is no verified "band",
     * only that one value. Same priority as the VM/tone threads is
     * fine: the watchdog sleeps 250 ms at a time and the Vita has
     * three user cores, so it cannot starve behind the VM thread. */
    SceUID t = sceKernelCreateThread("j2me_watchdog", wd_thread_routine,
                                     0x10000100, 0x4000, 0,
                                     SCE_KERNEL_THREAD_CPU_AFFINITY_MASK_DEFAULT,
                                     NULL);
    if (t < 0) {
        /* log5 lesson: a silent return here is indistinguishable from
         * "watchdog fired but found nothing". Every failure is visible. */
        char buf[96];
        int n = snprintf(buf, sizeof(buf),
                         "[WD] create FAILED rc=0x%08x\n", (unsigned)t);
        wd_log(buf, n);
        return;
    }
    int rc = sceKernelStartThread(t, 0, NULL);
    {
        /* liveness proof: one line at startup so a missing watchdog.log
         * can only mean "thread never ran", never "ran and saw nothing" */
        char buf[96];
        int n = snprintf(buf, sizeof(buf),
                         "[WD] alive tid=%d start rc=0x%08x\n",
                         (int)t, (unsigned)rc);
        wd_log(buf, n);
    }
}
