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

typedef int64_t wd_jlong;

/* Pump entry counter - owned by vita_checkevents.c (v01.71 probe) */
extern volatile unsigned int vita_ce_count;
/* Set by vita_main.c: 1 while a MIDlet round runs (0 in the menu) */
volatile int vita_wd_round_active = 0;
/* VM thread id - recorded by vita_main.c right after the VM starts */
volatile SceUID vita_wd_vm_tid = -1;
/* Tone thread id - exported by vita_audio_javacall.c */
extern volatile SceUID vita_tone_tid;

#define WD_LOG_PATH "ux0:/data/J2ME00001/watchdog.log"
#define WD_STALL_POLLS 12   /* 12 x 250 ms ~= 3 s, CLOCK-FREE threshold */
#define WD_POLL_MS  250

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
                     "prio=0x%x stack=%d\n",
                     tag, info.name, wd_status_name(info.status),
                     wd_wait_type_name(info.waitType), info.waitType,
                     (int)info.waitId, info.currentPriority,
                     (int)info.stackSize);
    wd_log(buf, n);
}

static int wd_thread_routine(SceSize args, void *argp) {
    (void)args; (void)argp;
    unsigned int last_ce = 0;
    int stalled_polls = 0;
    int reported = 0;

    for (;;) {
        sceKernelDelayThread(WD_POLL_MS * 1000);

        if (!vita_wd_round_active) {
            /* menu runs its own loop - only watch MIDlet rounds */
            last_ce = vita_ce_count;
            stalled_polls = 0;
            reported = 0;
            continue;
        }

        if (vita_ce_count != last_ce) {
            last_ce = vita_ce_count;
            stalled_polls = 0;   /* pump moved - hang not present */
            continue;
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
                             "[WD] HANG ce=%u polls=%d "
                             "clock t0=%lld t1=%lld d=%lld | "
                             "kclock k0=%lld k1=%lld kd=%lld us\n",
                             last_ce, stalled_polls,
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
