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
/* v02.02: OS-side thread forensics (--wrap= registration table). */
#include "vita_thread_audit.h"

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

        /* v02.09: per-core "what is running RIGHT NOW" - SAFELY.
         * log33 lesson (the hard way): the SDK header's
         * SceKernelThreadRunStatus is 52 bytes, but the kernel writes
         * 0x178 bytes regardless of the size field - the coredump's
         * THREAD_REG_INFO blocks ARE 0x178 each. The v02.08 stack
         * object got smashed past its return address and the
         * watchdog died right after the (already logged) runClocks
         * verdict. Now: a STATIC 1 KiB buffer well past anything the
         * kernel writes, raw hex dump decoded offline against the
         * THREAD_REG_INFO layout, plus three samples 200 ms apart -
         * a moving per-core table means the scheduler is alive and
         * the vm tid's presence/absence in it is the ghost verdict. */
        {
            static unsigned int rs_a[256], rs_b[256];
            char buf[160];
            int line, i, rc, moved = 0;
            memset(rs_a, 0xCC, sizeof(rs_a));
            rc = sceKernelGetThreadRunStatus(
                vita_wd_vm_tid, (SceKernelThreadRunStatus *)rs_a);
            for (line = 0; line < 6; line++) {
                int n = snprintf(buf, sizeof(buf),
                                 "[WD] runstatus vm rc=0x%x +%03x:",
                                 rc, line * 16);
                for (i = 0; i < 4; i++)
                    n += snprintf(buf + n, sizeof(buf) - n,
                                  " %08x", rs_a[line * 4 + i]);
                n += snprintf(buf + n, sizeof(buf) - n, "\n");
                wd_log(buf, n);
            }
            for (i = 1; i <= 2; i++) {
                sceKernelDelayThread(200 * 1000);
                memset(rs_b, 0xCC, sizeof(rs_b));
                sceKernelGetThreadRunStatus(
                    vita_wd_vm_tid, (SceKernelThreadRunStatus *)rs_b);
                if (memcmp(rs_a, rs_b, sizeof(rs_a)) != 0) moved = 1;
                memcpy(rs_a, rs_b, sizeof(rs_a));
            }
            {
                int n = snprintf(buf, sizeof(buf),
                                 "[WD] runstatus vm 3-sample %s\n",
                                 moved ? "MOVING" : "frozen");
                wd_log(buf, n);
            }
        }

        /* v02.06: THERAPEUTIC KICK (log30 hypothesis: kernel scheduler
         * lost the VM thread's run bit - real HW dual-core per-core
         * runqueues, thread stuck READY but never picked; Vita3K has
         * no such scheduler, hence emulator-fine / hardware-dead).
         * After ALL read-only forensics are on disk, rewrite the VM
         * thread's CPU affinity (mask 3 = both user cores) and see if
         * it resumes:
         *   - ce counter moves again => KICK-RECOVERED: hypothesis
         *     PROVEN, and this very call is the shipping workaround.
         *   - no movement in 12 x 250 ms => fall through to the data
         *     abort and take the coredump as before.
         * Either outcome advances the case; the kick itself cannot
         * make things worse (a frozen process is already dead). */
        {
            unsigned int ce_before = vita_ce_count;
            int moved = 0;
            int kpoll;
            char buf[128];
            int n;
            int rc;
            /* log31 lesson: mask 3 was REJECTED (rc=0x80028025
             * SCE_KERNEL_ERROR_ILLEGAL_CPU_AFFINITY_MASK). Vita's
             * affinity encoding is NOT 1<<cpu - the vm thread reports
             * aff=0x70000 (bits 16..18 = all three user cores), other
             * threads show 0x0 = MASK_DEFAULT. Merely rewriting the
             * SAME value could be a kernel no-op, so force a real
             * runqueue migration: narrow to cores 0+1 (0x30000) first,
             * restore all-core (0x70000) second - each step must take
             * the thread OFF its current per-core runqueue and requeue
             * it, which is exactly what a lost-run-bit thread needs. */
            /* log33 lesson (rc semantics): KICK1 in log32 returned
             * 0x70000 and KICK2 returned 0x30000 - the syscall
             * returns the OLD mask on success; 0x8xxxxxxx is error.
             * The three kicks all "succeeded" and all did nothing,
             * which log33's runClocks d=0 finally explains: the
             * kernel ledger says the vm thread is RUNNING on cpu 1,
             * so affinity/priority changes never touch a runqueue -
             * you cannot requeue a thread the kernel believes is
             * on-core. */
            /* v02.09 KICK0: sceKernelSendSignal - forces the kernel
             * down the thread-signal path (sets the thread's signal
             * pattern and, for a mis-marked thread, re-marks it
             * runnable). This is the one primitive that touches the
             * dispatch state of a thread the ledger calls RUNNING. */
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
                /* last prong: priority nudge - a scheduler-visible
                 * change that must requeue the thread (0x99 is one
                 * step above the 0xa0 every other thread uses);
                 * restore afterwards either way. */
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
                /* v02.09 KICK4: ChangeThreadPriority2 - exported by
                 * the stub but absent from SDK headers; the "2"
                 * variant applies the change immediately (vs. the
                 * plain one which is documented as taking effect at
                 * the next scheduling point - useless for a thread
                 * that never reaches one). */
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
            } /* end KICK0 !moved guard (KICK1..KICK4) */
        }

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
