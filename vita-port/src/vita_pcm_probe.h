/*
 * vita_pcm_probe.h - capture what the renderer really hands to sceAudioOutOutput, and measure underruns (v03.22).
 *
 * Header-only, no Vita include, so debug/tools/wavq/test_pcm_probe.c compiles and tests the shipped code on the host.
 *
 * Why (log129, user: "always crackle - clicks and hiss - and J2ME Loader on a phone sounds better"):
 * the logs have no audio-quality signal, so every earlier "fix" was reasoned from a WAV rendered on the HOST.
 * Two things were never measured on the device:
 *   1. the samples that reach the DAC (is the device output what the host renderer produces?);
 *   2. underruns. The old evidence "ran - cur within a few ms" cannot see them: when the audio thread is late the
 *      port's queue is empty, sceAudioOutOutput returns at once and the thread simply catches up, so wall time and
 *      submitted audio stay equal. A late chunk is audible as a click. The right measure is the interval between
 *      two consecutive submissions: a 512-frame chunk at 48 kHz is 10.667 ms; the call blocks until a queue slot
 *      frees, so submissions should be spaced ~10.7 ms apart. Gaps well above that are underruns.
 */
#ifndef VITA_PCM_PROBE_H
#define VITA_PCM_PROBE_H

#include <stdint.h>
#include <string.h>

#define VPP_CHUNK_FRAMES   512
#define VPP_CAPTURE_CHUNKS 600              /* 600 * 512 frames = 6.4 s of stereo s16 = 1.2 MB */
#define VPP_HIST_BUCKETS   10

typedef struct {
    /* capture */
    int16_t  cap[VPP_CAPTURE_CHUNKS][VPP_CHUNK_FRAMES * 2];
    int      cap_chunks;                    /* filled so far */
    int      cap_ready;                     /* 1 once full or flushed */
    int      cap_written;                   /* 1 once the file has been written (only ever once per run) */
    /* timing of consecutive submissions, in microseconds */
    unsigned long long last_submit_us;
    unsigned long long hist[VPP_HIST_BUCKETS];
    unsigned long long n_intervals;
    unsigned long long max_interval_us;
    unsigned long long n_blocked_short;     /* sceAudioOutOutput returned in < 2 ms: the queue was empty (underrun) */
    unsigned long long n_chunks;
} vita_pcm_probe;

/* interval buckets (us): <9000, <10000, <10500, <10900, <11500, <13000, <16000, <25000, <50000, >=50000 */
static inline int vpp_bucket(unsigned long long us) {
    static const unsigned long long lim[VPP_HIST_BUCKETS - 1] = {9000, 10000, 10500, 10900, 11500, 13000, 16000, 25000, 50000};
    int i;
    for (i = 0; i < VPP_HIST_BUCKETS - 1; i++) if (us < lim[i]) return i;
    return VPP_HIST_BUCKETS - 1;
}

static inline void vpp_reset_run(vita_pcm_probe *p) {
    p->last_submit_us = 0;
}

/* Call right BEFORE sceAudioOutOutput with the chunk about to be played; `now_us` = process time. */
static inline void vpp_before_submit(vita_pcm_probe *p, const int16_t *chunk, unsigned long long now_us) {
    if (p->last_submit_us) {
        unsigned long long d = now_us - p->last_submit_us;
        p->hist[vpp_bucket(d)]++;
        p->n_intervals++;
        if (d > p->max_interval_us) p->max_interval_us = d;
    }
    p->last_submit_us = now_us;
    p->n_chunks++;
    if (p->cap_chunks < VPP_CAPTURE_CHUNKS) {
        memcpy(p->cap[p->cap_chunks], chunk, VPP_CHUNK_FRAMES * 2 * sizeof(int16_t));
        p->cap_chunks++;
        if (p->cap_chunks == VPP_CAPTURE_CHUNKS) p->cap_ready = 1;
    }
}

/* Call right AFTER sceAudioOutOutput returns. A call that returns almost immediately means no queue slot had to be
 * waited for, i.e. the hardware had run dry before this chunk arrived. */
static inline void vpp_after_submit(vita_pcm_probe *p, unsigned long long call_us) {
    if (call_us < 2000ULL) p->n_blocked_short++;
}

#endif /* VITA_PCM_PROBE_H */
