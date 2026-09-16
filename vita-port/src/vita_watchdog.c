/*
 * vita_watchdog.c - v01.72 hang forensics: snapshot what the VM thread
 * is doing when the event pump stops.
 *
 * v01.71 verdict (log4): the pump froze for good (last session CE #115
 * ~= 4.4 s after launch) with ani_mark = "OUT", i.e. the VM thread did
 * NOT die inside the ANI wait - it simply never came back to the pump.
 * Audio thread innocent (tone done chunks=38, math checks out). Clock
 * LOOKED alive but the heartbeat samples once per 1024 pump calls
 * (~40 s), way too coarse for a 2-10 s hang - H2 was never excluded.
 *
 * Two suspects remain, and ONE snapshot tells them apart:
 *   (a) VM thread blocked in a kernel wait (sceIo*, mutex, cond, delay)
 *       -> SceKernelThreadInfo.status == WAITING, waitType names the class
 *   (b) VM thread running hot (Java/GC/interpreter loop, no native block)
 *       -> status == RUNNING/READY, waitType 0
 *
 * How: a low-priority thread samples the pump counter (vita_ce_count
 * from vita_checkevents.c). When it stops advancing for >3 s AND a
 * MIDlet round is active, it snapshots VM + tone threads via
 * sceKernelGetThreadInfo and takes TWO gettimeofday readings 100 ms
 * apart (freeze = H2 confirmed on the spot). One report per hang.
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
#define WD_STALL_MS 3000
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
    wd_jlong last_ce_change = 0;
    unsigned int last_ce = 0;
    int reported = 0;

    for (;;) {
        sceKernelDelayThread(WD_POLL_MS * 1000);

        if (!vita_wd_round_active) {
            /* menu runs its own loop - only watch MIDlet rounds */
            last_ce = vita_ce_count;
            last_ce_change = wd_now_ms();
            reported = 0;
            continue;
        }

        unsigned int ce = vita_ce_count;
        wd_jlong now = wd_now_ms();

        if (ce != last_ce) {
            last_ce = ce;
            last_ce_change = now;
            continue;          /* pump moved - hang not present */
        }

        /* counter frozen; require WD_STALL_MS before declaring a hang */
        if (now - last_ce_change < WD_STALL_MS) continue;
        if (reported) continue;  /* one report per hang */

        /* ---- hang detected: forensic snapshot ----
         * clock liveness: two readings 100 ms apart must differ */
        wd_jlong t0 = wd_now_ms();
        sceKernelDelayThread(100 * 1000);
        wd_jlong t1 = wd_now_ms();
        {
            char buf[128];
            int n = snprintf(buf, sizeof(buf),
                             "[WD] HANG ce=%u clock t0=%lld t1=%lld d=%lld\n",
                             ce, (long long)t0, (long long)t1,
                             (long long)(t1 - t0));
            wd_log(buf, n);
        }

        wd_dump_thread("vm", vita_wd_vm_tid);
        wd_dump_thread("tone", vita_tone_tid);
        /* the watchdog itself, as a sanity marker */
        wd_dump_thread("wd", sceKernelGetThreadId());

        reported = 1;
    }
    return 0;
}

void vita_watchdog_start(void) {
    SceUID t = sceKernelCreateThread("j2me_watchdog", wd_thread_routine,
                                     0x10000300, 0x2000, 0, 0, NULL);
    if (t < 0) return;
    sceKernelStartThread(t, 0, NULL);
}
