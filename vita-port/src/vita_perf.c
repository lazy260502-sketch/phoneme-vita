/*
 * vita_perf.c - CPU clock boost + frame-time accounting (v02.97).
 * See vita_perf.h for the policy and the config keys.
 */

#include <stdio.h>
#include <string.h>

#include <psp2/io/fcntl.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/power.h>

#include "vita_perf.h"

/* The three paths are overridable so the host unit test can point them at
 * a scratch directory (/tmp/...) instead of ux0. On the console nothing
 * defines them and the default below is used. */
#define PERF_DIR        "ux0:/data/J2ME00001"
#ifndef PERF_CFG_PATH
#define PERF_CFG_PATH   PERF_DIR "/clock.cfg"
#endif
#ifndef PERF_LOG_PATH
#define PERF_LOG_PATH   PERF_DIR "/perf.log"
#endif
#define PERF_CFG        PERF_CFG_PATH
#define PERF_LOG        PERF_LOG_PATH

/* Clock actually in effect (for crumb/log lines and for the caller). */
static int g_arm, g_bus, g_gpu, g_xbar;

/* stats config */
static int g_stats_on   = 1;
static unsigned int g_stats_gap = 300;   /* frames between lines */
static unsigned int g_stats_max = 200;   /* lines per session */
static int g_inited = 0;

/* ---- config ------------------------------------------------------ */

/* Read "<key>=<int>" from path. Returns def when the file or key is
 * absent. Deliberately dumb: the file is three or four lines written by
 * a human, and a parser that cannot allocate cannot fail at boot. */
static int cfg_int(const char *path, const char *key, int def) {
    char buf[256];
    SceUID fd;
    int n, found = -1;

    fd = sceIoOpen(path, SCE_O_RDONLY, 0);
    if (fd < 0) {
        return def;
    }
    n = (int)sceIoRead(fd, buf, sizeof(buf) - 1);
    sceIoClose(fd);
    if (n <= 0) {
        return def;
    }
    buf[n] = '\0';

    {
        char *p = buf;
        size_t klen = strlen(key);
        while (*p) {
            char *eol = strpbrk(p, "\r\n");
            char *eq = strchr(p, '=');
            char *end = eol ? eol : (p + strlen(p));
            if (eq != NULL && eq < end && (size_t)(eq - p) == klen &&
                strncmp(p, key, klen) == 0) {
                const char *v = eq + 1;
                int val = 0, any = 0;
                while (v < end && (*v == ' ' || *v == '\t')) {
                    v++;
                }
                while (v < end && *v >= '0' && *v <= '9') {
                    val = val * 10 + (*v - '0');
                    any = 1;
                    v++;
                }
                if (any) {
                    found = val;
                }
                break;
            }
            if (!eol) {
                break;
            }
            p = eol + 1;
        }
    }
    return found >= 0 ? found : def;
}

/* ---- logging ----------------------------------------------------- */

static void perf_log(const char *line) {
    SceUID fd = sceIoOpen(PERF_LOG,
                          SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0777);
    if (fd >= 0) {
        sceIoWrite(fd, line, (SceSize)strlen(line));
        sceIoWrite(fd, "\n", 1);
        sceIoClose(fd);
    }
}

/* "12.34" from 12345 us - integer only; newlib's %f is not linked in a
 * way that is worth the bytes here. */
static void fmt_ms(char *out, unsigned int us) {
    sprintf(out, "%u.%02u", us / 1000u, (us % 1000u) / 10u);
}

/* ---- clock ------------------------------------------------------- */

/* Try one frequency. A Vita only accepts its documented steps, and a
 * rejected request is not fatal: log it and keep the old value. */
static int set_clock(const char *what, int want, int *cur,
                     int (*setter)(int), int (*getter)(void)) {
    char line[160];
    int rc;

    if (want <= 0) {
        *cur = getter();
        return 0;
    }
    rc = setter(want);
    *cur = getter();
    if (rc < 0) {
        snprintf(line, sizeof(line),
                 "[PERF] %s: request %d MHz failed rc=0x%08x, staying at %d MHz",
                 what, want, (unsigned)rc, *cur);
    } else {
        snprintf(line, sizeof(line), "[PERF] %s: set %d MHz, now %d MHz (rc=0)",
                 what, want, *cur);
    }
    perf_log(line);
    fprintf(stderr, "%s\n", line);
    return rc;
}

void vita_perf_init(void) {
    char line[220];
    int want_arm, want_bus, want_gpu;
    int arm_now;

    if (g_inited) {
        return;
    }
    g_inited = 1;

    want_arm = cfg_int(PERF_CFG, "arm", 444);
    want_bus = cfg_int(PERF_CFG, "bus", 222);
    want_gpu = cfg_int(PERF_CFG, "gpu", 0);

    g_stats_on  = cfg_int(PERF_CFG, "stats", 1) != 0;
    {
        int gap = cfg_int(PERF_CFG, "stats_gap", 300);
        int max = cfg_int(PERF_CFG, "stats_max", 200);
        if (gap >= 60) {
            g_stats_gap = (unsigned int)gap;
        }
        if (max >= 0) {
            g_stats_max = (unsigned int)max;
        }
    }

    /* v03.06: log ALL four clocks plus the two power-state flags. log123
     * showed `arm: set 444 MHz, now 333 MHz (rc=0)` - the kernel ACCEPTED
     * the request and the clock did not move - and the same four lines on
     * every one of its four sessions. gpu=0 at boot is the second clue:
     * scePowerGetGpuClockFrequency() reports 0 while the GPU domain is not
     * clocked, which is the state before anything touches GXM.
     * Those two facts admit three explanations and this log cannot tell them
     * apart, so log everything that discriminates: gpuXbar (a coupled
     * domain we never touched), AC-vs-battery, and the wireless flag (the
     * system drops the clock when Wi-Fi is on and the battery is low). */
    {
        int arm0 = scePowerGetArmClockFrequency();
        int bus0 = scePowerGetBusClockFrequency();
        int gpu0 = scePowerGetGpuClockFrequency();
        int xb0  = scePowerGetGpuXbarClockFrequency();
        snprintf(line, sizeof(line),
                 "[PERF] boot clock arm=%d bus=%d gpu=%d xbar=%d MHz "
                 "ac=%d wireless=%d",
                 arm0, bus0, gpu0, xb0,
                 (int)scePowerIsPowerOnline(),
                 (int)scePowerGetUsingWireless());
        perf_log(line);
        fprintf(stderr, "%s\n", line);
    }

    /* ARM first: it is the one that matters, and if the kernel refuses
     * it there is no point asking for the rest. */
    set_clock("arm", want_arm, &g_arm, scePowerSetArmClockFrequency,
              scePowerGetArmClockFrequency);
    set_clock("bus", want_bus, &g_bus, scePowerSetBusClockFrequency,
              scePowerGetBusClockFrequency);
    set_clock("gpu", want_gpu, &g_gpu, scePowerSetGpuClockFrequency,
              scePowerGetGpuClockFrequency);

    /* v03.06: escalation. Only runs when the plain request demonstrably did
     * not take effect (rc==0 but the readback stayed put), so a machine
     * where 444 works pays nothing for this. Each attempt logs its own rc
     * and readback, so the next hardware log names the winning strategy
     * instead of us guessing again.
     *
     * Hypothesis A - configuration mode. scePowerSetConfigurationMode() is
     *   the only "may I run faster" switch the SDK exposes (its VitaSDK
     *   comment is literally "Set configuration mode ?"). An app that never
     *   calls it keeps the default power budget, and the clock is clamped to
     *   it - which is exactly "rc=0, value unchanged".
     * Hypothesis B - coupled domains. The ARM and GPU/XBAR clocks share a
     *   PLL group; raising ARM alone can be silently clamped. Trying again
     *   with GPU/XBAR at their coupled values (arm/2 and 166) is the order
     *   the overclocking homebrew uses.
     */
    arm_now = scePowerGetArmClockFrequency();
    if (want_arm > 0 && arm_now != want_arm) {
        int mode = cfg_int(PERF_CFG, "mode", -1);
        int m, rc;

        if (mode < 0) {
            /* Try 1 (unlimited) then 0 (default). Whichever makes the
             * readback move is the one this firmware wants. */
            for (m = 1; m >= 0; m--) {
                rc = scePowerSetConfigurationMode(m);
                snprintf(line, sizeof(line),
                         "[PERF] configmode(%d) rc=0x%08x arm_before=%d",
                         m, (unsigned)rc, scePowerGetArmClockFrequency());
                perf_log(line);
                fprintf(stderr, "%s\n", line);
                if (rc < 0) {
                    continue;
                }
                set_clock("arm", want_arm, &g_arm,
                          scePowerSetArmClockFrequency,
                          scePowerGetArmClockFrequency);
                if (g_arm == want_arm) {
                    break;
                }
            }
        } else {
            rc = scePowerSetConfigurationMode(mode);
            snprintf(line, sizeof(line),
                     "[PERF] configmode(%d) [cfg] rc=0x%08x",
                     mode, (unsigned)rc);
            perf_log(line);
            fprintf(stderr, "%s\n", line);
        }

        if (scePowerGetArmClockFrequency() != want_arm) {
            /* Hypothesis B: coupled domains first, then ARM again. */
            int half = want_arm / 2;
            set_clock("bus", want_bus > 0 ? want_bus : half, &g_bus,
                      scePowerSetBusClockFrequency,
                      scePowerGetBusClockFrequency);
            set_clock("gpu", half, &g_gpu, scePowerSetGpuClockFrequency,
                      scePowerGetGpuClockFrequency);
            set_clock("gpuXbar", 166, &g_xbar, scePowerSetGpuXbarClockFrequency,
                      scePowerGetGpuXbarClockFrequency);
            set_clock("arm(retry)", want_arm, &g_arm,
                      scePowerSetArmClockFrequency,
                      scePowerGetArmClockFrequency);
        }
    }

    /* Final state, read LIVE - never from the cached g_arm. log123's frame
     * lines all said "arm=333MHz" but that was this cached value, so they
     * could not answer "did the clock really move later?". */
    g_arm = scePowerGetArmClockFrequency();
    g_bus = scePowerGetBusClockFrequency();
    g_gpu = scePowerGetGpuClockFrequency();
    g_xbar = scePowerGetGpuXbarClockFrequency();
    snprintf(line, sizeof(line),
             "[PERF] final arm=%d bus=%d gpu=%d xbar=%d MHz (want arm=%d)",
             g_arm, g_bus, g_gpu, g_xbar, want_arm);
    perf_log(line);
    fprintf(stderr, "%s\n", line);

    /* A single verdict line so nobody has to read five lines to learn
     * whether the boost happened. */
    if (want_arm > 0 && g_arm != want_arm) {
        snprintf(line, sizeof(line),
                 "[PERF] VERDICT: arm stuck at %d MHz (asked %d, all "
                 "strategies failed) - try clock.cfg mode=0 or mode=1, "
                 "or run with Wi-Fi off / on AC", g_arm, want_arm);
    } else {
        snprintf(line, sizeof(line),
                 "[PERF] VERDICT: arm=%d MHz %s", g_arm,
                 (want_arm > 0) ? "(boost applied)" : "(boost not requested)");
    }
    perf_log(line);
    fprintf(stderr, "%s\n", line);

    /* First line of every perf.log: which build produced these numbers.
     * The version string is declared by CMake in vita_main.c; keep this
     * file free of that symbol so the host tests can link it. */
    snprintf(line, sizeof(line), "[PERF] ---- session start, stats=%s ----",
             g_stats_on ? "on" : "off");
    perf_log(line);
}

int vita_perf_clock_arm(void) { return g_arm; }
int vita_perf_clock_bus(void) { return g_bus; }
int vita_perf_clock_gpu(void) { return g_gpu; }
int vita_perf_clock_xbar(void) { return g_xbar; }

/* ---- frame accounting -------------------------------------------- */

static struct {
    unsigned long long last_flip_us;
    unsigned long long acc_prepare, acc_total, acc_period;
    unsigned int prepare_min, prepare_max;
    unsigned int total_min, total_max;
    unsigned int period_min, period_max;
    unsigned int samples;         /* frames in the current window */
    unsigned int period_samples;  /* of those, the ones with a period */
    unsigned int frames;          /* since boot */
    unsigned int lines;           /* lines written this session */
    unsigned int period_last;
    unsigned int prepare_last;
    unsigned int total_last;
    /* v03.59: the prepare split (see VitaFrameSeg). Summed only over frames
     * that actually reported segments, so the averages are per-sample and
     * can be compared directly with prepare_avg. */
    unsigned long long acc_blit, acc_letterbox, acc_vkey, acc_shot, acc_splash;
    unsigned int seg_samples;
} fp;

static void put_max(unsigned int *slot, unsigned int v) {
    if (v > *slot) {
        *slot = v;
    }
}

void vita_perf_frame(unsigned int prepare_us, unsigned int total_us) {
    vita_perf_frame_seg(prepare_us, total_us, NULL);
}

void vita_perf_frame_seg(unsigned int prepare_us, unsigned int total_us,
                         const VitaFrameSeg *seg) {
    unsigned long long now = sceKernelGetProcessTimeWide();
    unsigned int period_us = 0;

    if (total_us > 60000u) {
        /* A 60 s "frame" is not a frame but a stall (round change, jar
         * load). Reporting it as 0.0 fps would poison the average and
         * hide the stall, so it starts a new window instead. */
        fp.last_flip_us = now;
        fp.samples = 0;
        fp.period_samples = 0;
        fp.seg_samples = 0;
        fp.acc_prepare = fp.acc_total = fp.acc_period = 0;
        fp.acc_blit = fp.acc_letterbox = fp.acc_vkey = 0;
        fp.acc_shot = fp.acc_splash = 0;
        return;
    }

    if (fp.last_flip_us != 0) {
        period_us = (unsigned int)(now - fp.last_flip_us);
        if (period_us > 4000000u) {   /* >4 s: same reasoning */
            period_us = 0;
            fp.samples = 0;
            fp.period_samples = 0;
            fp.seg_samples = 0;
            fp.acc_prepare = fp.acc_total = fp.acc_period = 0;
            fp.acc_blit = fp.acc_letterbox = fp.acc_vkey = 0;
            fp.acc_shot = fp.acc_splash = 0;
        }
    }
    fp.last_flip_us = now;
    fp.frames++;

    fp.prepare_last = prepare_us;
    fp.total_last = total_us;
    if (period_us != 0) {
        fp.period_last = period_us;
    }

    if (fp.samples == 0 || prepare_us < fp.prepare_min) {
        fp.prepare_min = prepare_us;
    }
    if (fp.samples == 0 || total_us < fp.total_min) {
        fp.total_min = total_us;
    }
    put_max(&fp.prepare_max, prepare_us);
    put_max(&fp.total_max, total_us);
    if (period_us != 0) {
        /* The window's first frame (and the first frame after any reset)
         * has no period - there is no previous flip in this window - so
         * it must not be divided in, or every window would under-report
         * the average by 1/period_samples and the derived fps would drift
         * down as stats_gap shrinks. */
        fp.period_samples++;
        if (fp.period_samples == 1 || period_us < fp.period_min) {
            fp.period_min = period_us;
        }
        put_max(&fp.period_max, period_us);
        fp.acc_period += period_us;
    }
    fp.acc_prepare += prepare_us;
    fp.acc_total += total_us;
    fp.samples++;
    if (seg != NULL) {
        fp.acc_blit += seg->blit_us;
        fp.acc_letterbox += seg->letterbox_us;
        fp.acc_vkey += seg->vkey_us;
        fp.acc_shot += seg->shot_us;
        fp.acc_splash += seg->splash_us;
        fp.seg_samples++;
    }

    if (!g_stats_on || fp.samples < g_stats_gap || fp.lines >= g_stats_max) {
        return;
    }

    {
        char line[256];
        char p_avg[16], p_min[16], p_max[16];
        char t_avg[16], t_min[16], t_max[16];
        char f_avg[16], f_min[16], f_max[16];
        unsigned int n = fp.samples;
        unsigned int pn = fp.period_samples;
        unsigned int t_avg_us = (unsigned int)(fp.acc_total / n);
        unsigned int f_avg_us = pn ? (unsigned int)(fp.acc_period / pn) : 0;
        unsigned int fps10 = 0;

        if (f_avg_us > 0) {
            /* tenths of frames per second: 1e6*10/period */
            unsigned long long v = 10000000ull / f_avg_us;
            fps10 = (unsigned int)v;
        }
        /* Frame budget: what the VM threads get is the period minus the
         * time spent inside the flip, so print the split rather than a
         * bare fps number - the whole point is to see whether we are
         * vsync-bound (period ~16667, prepare small) or CPU-bound. */
        fmt_ms(p_avg, (unsigned int)(fp.acc_prepare / n));
        fmt_ms(p_min, fp.prepare_min);
        fmt_ms(p_max, fp.prepare_max);
        fmt_ms(t_avg, t_avg_us);
        fmt_ms(t_min, fp.total_min);
        fmt_ms(t_max, fp.total_max);
        fmt_ms(f_avg, f_avg_us);
        fmt_ms(f_min, fp.period_min);
        fmt_ms(f_max, fp.period_max);

        snprintf(line, sizeof(line),
                 "[PERF] f=%u fps=%u.%u period=%s(%s..%s) flip=%s(%s..%s) "
                 "prepare=%s(%s..%s) arm=%dMHz",
                 fp.frames, fps10 / 10u, fps10 % 10u,
                 f_avg, f_min, f_max,
                 t_avg, t_min, t_max,
                 p_avg, p_min, p_max,
                 /* v03.06: LIVE read, not the cached g_arm. log123 printed
                  * "arm=333MHz" on every line while nobody knew whether the
                  * clock had moved after boot; a live read makes each line
                  * independent evidence (and costs one getter per stats_gap
                  * frames, i.e. once every ~5 s). */
                 (g_arm > 0) ? scePowerGetArmClockFrequency() : 0);
        perf_log(line);
        fp.lines++;

        /* v03.59: where did `prepare` go? Same window, same denominator
         * (seg_samples), so `blit + letterbox + vkey + shot + splash` can be
         * subtracted from `prepare` directly and the remainder is the part
         * nobody has instrumented yet. Only emitted when segments were
         * actually supplied (an IME frame path or an older caller does not),
         * so a missing line means "not measured", never "zero". */
        if (fp.seg_samples > 0) {
            unsigned int sn = fp.seg_samples;
            char s1[16], s2[16], s3[16], s4[16], s5[16], s6[16];
            unsigned int sum_us = 0;
            unsigned int prep_avg_us = (unsigned int)(fp.acc_prepare / n);

            fmt_ms(s1, (unsigned int)(fp.acc_blit / sn));
            fmt_ms(s2, (unsigned int)(fp.acc_letterbox / sn));
            fmt_ms(s3, (unsigned int)(fp.acc_vkey / sn));
            fmt_ms(s4, (unsigned int)(fp.acc_shot / sn));
            fmt_ms(s5, (unsigned int)(fp.acc_splash / sn));
            sum_us = (unsigned int)((fp.acc_blit + fp.acc_letterbox +
                                     fp.acc_vkey + fp.acc_shot +
                                     fp.acc_splash) / sn);
            /* the unaccounted remainder - the whole point of the split */
            fmt_ms(s6, (prep_avg_us > sum_us) ? (prep_avg_us - sum_us) : 0u);

            snprintf(line, sizeof(line),
                     "[PERF] seg(n=%u) blit=%s ltbox=%s vkey=%s shot=%s "
                     "splash=%s | other=%s of %s",
                     sn, s1, s2, s3, s4, s5, s6, p_avg);
            perf_log(line);
            fp.lines++;
        }

        fp.samples = 0;
        fp.period_samples = 0;
        fp.seg_samples = 0;
        fp.acc_prepare = fp.acc_total = fp.acc_period = 0;
        fp.acc_blit = fp.acc_letterbox = fp.acc_vkey = 0;
        fp.acc_shot = fp.acc_splash = 0;
        fp.prepare_min = fp.prepare_max = 0;
        fp.total_min = fp.total_max = 0;
        fp.period_min = fp.period_max = 0;
    }
}

void vita_perf_get(unsigned int *period_us, unsigned int *prepare_us,
                   unsigned int *total_us, unsigned int *fps10,
                   unsigned int *frames) {
    unsigned int p = fp.period_last;
    if (period_us != NULL) {
        *period_us = p;
    }
    if (prepare_us != NULL) {
        *prepare_us = fp.prepare_last;
    }
    if (total_us != NULL) {
        *total_us = fp.total_last;
    }
    if (fps10 != NULL) {
        *fps10 = p ? (unsigned int)(10000000ull / p) : 0;
    }
    if (frames != NULL) {
        *frames = fp.frames;
    }
}
