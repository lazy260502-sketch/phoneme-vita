/*
 * vita_perf.h - CPU clock boost + frame-time accounting (v02.97).
 *
 * P1-3 of COMPATIBILITY_ROADMAP.md opens with two items that need no
 * codec or GPU work:
 *
 *   1. ask the kernel for the high-performance clock instead of running
 *      at whatever the app was started with (the roadmap table prices
 *      this as "1 line, +25-33% execution speed, low risk");
 *   2. measure the frame instead of guessing - "先加帧耗时统计，再决定
 *      优化点" - so the NEON-vs-GPU decision is made on numbers.
 *
 * Both live here because both are pure measurement/hint code with no
 * dependency on the VM, the display or Java.
 *
 * Clock policy (ux0:/data/J2ME00001/clock.cfg, all keys optional):
 *
 *     arm=444      ARM core MHz, 0 = leave alone
 *     bus=222      BUS MHz,      0 = leave alone
 *     gpu=0        GPU MHz,      0 = leave alone (a software blitter
 *                  barely touches the GPU; the default keeps the unit
 *                  cooler and is only raised by an explicit request)
 *     mode=-1      scePowerSetConfigurationMode() argument. Absent/-1 means
 *                  "only try it if the plain ARM request did not take
 *                  effect, then try 1 and 0"; 0 or 1 forces that value.
 *
 * v03.06: log123 showed the plain request being accepted and ignored
 * (`arm: set 444 MHz, now 333 MHz (rc=0)`), so vita_perf_init() now
 * escalates: configuration mode, then a coupled BUS/GPU/GPU-XBAR raise
 * before retrying ARM. Every attempt logs its rc and readback; the final
 * line is a one-line VERDICT. The frame lines read the clock LIVE, because
 * the cached copy could not tell anyone whether the clock moved later.
 *
 * The file is only read at boot. A request the kernel rejects is logged
 * and ignored - a game must still start, just at the previous clock.
 *
 * Frame policy (same file):
 *
 *     stats=1      0 = no perf.log lines at all
 *     stats_gap=300   emit one line every N frames (~5 s at 60 fps)
 *     stats_max=200  stop logging after N lines (a long session must
 *                    not fill ux0; log storms have stalled this port
 *                    before, see PROJECT_MEMORY.md)
 */

#ifndef VITA_PERF_H
#define VITA_PERF_H

#ifdef __cplusplus
extern "C" {
#endif

/* Read clock.cfg, apply the clock requests, log the before/after values
 * to stderr + crumb.log. Call once during early boot, after
 * ux0:/data/J2ME00001 exists and before anything expensive runs.
 * Safe to call twice - the second call is a no-op. */
void vita_perf_init(void);

/* One frame's cost, in microseconds:
 *   prepare_us - inside flip_to_display() before the submit
 *                (letterbox clear + scaled blit + overlays)
 *   total_us   - same entry to return (includes sceDisplayWaitVblankStart
 *                and, for an IME frame, the GXM dialog submit)
 * The flip-to-flip period is derived here, so callers don't have to
 * timestamp the entry. Cheap enough for every frame: a few adds and,
 * once per stats_gap frames, one sceIoOpen/Write/Close. */
void vita_perf_frame(unsigned int prepare_us, unsigned int total_us);

/* v03.59: the split inside `prepare`. log153 measured the rgb565->abgr
 * conversion at 5.24 ms per frame while perf.log reported prepare=17.1 ms,
 * so 12 ms of our own CPU time was inside `prepare` and NOT inside the loop
 * we had just measured. Guessing which part it is would repeat the mistake
 * v03.45 made ("52 MB/s must be uncached memory" - it was not). Fill this
 * in from flip_to_display() and the log line names the culprit. All fields
 * are plain microseconds; leave any at 0 if not measured. */
typedef struct VitaFrameSeg {
    unsigned int blit_us;      /* blit_scaled(): the scaled RGB565->ABGR write */
    unsigned int letterbox_us; /* clear_letterbox(), when it runs at all     */
    unsigned int vkey_us;      /* virtual keypad + camera button             */
    unsigned int shot_us;      /* screenshot service + result banner         */
    unsigned int splash_us;    /* startup loading overlay                    */
} VitaFrameSeg;

void vita_perf_frame_seg(unsigned int prepare_us, unsigned int total_us,
                         const VitaFrameSeg *seg);

/* Current averaged numbers, for an on-screen overlay or a debug key.
 * Any of the pointers may be NULL. Values are 0 until 2 frames ran. */
void vita_perf_get(unsigned int *period_us, unsigned int *prepare_us,
                   unsigned int *total_us, unsigned int *fps10,
                   unsigned int *frames);

/* The clock actually in effect after vita_perf_init(), 0 before it. */
int vita_perf_clock_arm(void);
int vita_perf_clock_bus(void);
int vita_perf_clock_gpu(void);
/* v03.06: GPU crossbar; logged because it is a coupled domain we never
 * touched before and it is part of the "why did ARM stay at 333" story. */
int vita_perf_clock_xbar(void);

#ifdef __cplusplus
}
#endif

#endif /* VITA_PERF_H */
