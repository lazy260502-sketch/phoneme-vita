/*
 * vita_wav_resample.h - rate conversion for the WAV renderer (v03.14).
 *
 * Header-only and free of any Vita include so the exact code that ships can also be
 * compiled and tested on the build host (debug/tools/wavq/).
 *
 * Why this exists (log125: "audio plays and no longer stutters, but the quality is poor,
 * some of it cannot be made out"):
 *   wav_render_player used
 *       out_frames_total = frames * (OUT_SAMPLE_RATE / rate)       -- integer division
 *       src = rendered * frames / out_frames_total                 -- nearest sample
 *   1. 48000 / rate truncates, so every rate that does not divide 48000 was rendered at the
 *      wrong speed and pitch: 11025 and 22050 and 44100 Hz play 8.8 % fast (+147 cents),
 *      32000 Hz 50 % fast (+702 cents), and anything above 48000 Hz got a multiplier of 0
 *      (silence / division by zero). Only 8000, 12000, 16000, 24000 and 48000 were right.
 *   2. Nearest-sample (zero-order hold) upsampling leaves the full set of image tones in
 *      the output: measured SNR on a 1 kHz sine is 8.1 dB (8 kHz source) .. 21.7 dB (44.1 kHz)
 *      against 24.9 .. 54.6 dB with linear interpolation.
 *
 * The position is a 32.32 fixed-point source index advanced by rate/48000 per output frame,
 * so the output length is frames * 48000 / rate exactly (64-bit; long is 32 bits here).
 */
#ifndef VITA_WAV_RESAMPLE_H
#define VITA_WAV_RESAMPLE_H

#include <stdint.h>

/* Number of 48 kHz output frames for `frames` source frames at `rate` Hz (rounded down). */
static inline long vwr_out_frames(long frames, long rate, long out_rate) {
    if (frames <= 0 || rate <= 0) return 0;
    return (long)((long long)frames * out_rate / rate);
}

/* Source position of output frame `n` as 32.32 fixed point: n * rate / out_rate.
 * Integer part and remainder are computed separately: (n * rate) << 32 overflows uint64 as soon as
 * n * rate >= 2^32 (a 34 s tune at 8 kHz already gets there), the host test caught that. */
static inline uint64_t vwr_pos_q32(long n, long rate, long out_rate) {
    uint64_t nr = (uint64_t)n * (uint64_t)rate;
    uint64_t ip = nr / (uint64_t)out_rate;
    uint64_t rem = nr % (uint64_t)out_rate;                 /* < out_rate (<= 2^16 here), so rem << 32 fits */
    return (ip << 32) | ((rem << 32) / (uint64_t)out_rate);
}

/* Per-output-frame increment of the 32.32 source position: rate / out_rate. The render loop adds this
 * once per frame instead of calling vwr_pos_q32 (three 64-bit divisions = libgcc calls on ARMv7, 48000
 * times a second). Truncation error is < 2^-32 source frames per step: under 0.0005 frames over a
 * 34 s tune (checked against vwr_pos_q32 in debug/tools/wavq/test_resample.c). */
static inline uint64_t vwr_step_q32(long rate, long out_rate) {
    return ((uint64_t)rate << 32) / (uint64_t)out_rate;
}

/* Linear interpolation between sample a (at integer index) and b (next), frac = 0..65535. */
static inline int32_t vwr_lerp(int32_t a, int32_t b, uint32_t frac16) {
    return a + (int32_t)(((int64_t)(b - a) * (int64_t)frac16) >> 16);
}

/* v03.15: fill one stereo chunk with a linear ramp from the last submitted sample (lastl/lastr) down to 0.
 * Used after a pause/stop so the DAC does not step from the square level to silence in one sample (pop).
 * frames must be > 0; the first output frame is already one step below the last level, the last is 0. */
static inline void vwr_fadeout(int16_t *buf, int frames, int32_t lastl, int32_t lastr) {
    int i;
    for (i = 0; i < frames; i++) {
        int32_t gain = frames - 1 - i;                       /* frames-1 .. 0 */
        buf[i * 2 + 0] = (int16_t)(((int64_t)lastl * gain) / frames);
        buf[i * 2 + 1] = (int16_t)(((int64_t)lastr * gain) / frames);
    }
}

/* ---------------------------------------------------------------------------------------------------------
 * v03.32: band-limited polyphase FIR upsampler (replaces linear interpolation when rate < out_rate).
 *
 * Why (log135, device dump vs an ideal windowed-sinc resample of the same 8 kHz WAV, A-weighted):
 *   source energy above 5 kHz is -96..-105 dB, the device output has -30..-42 dB there, i.e. 60-70 dB of
 *   pure imaging from the linear interpolator (4-8 kHz is where the ear is most sensitive): the steady
 *   "sizzle" reported on the xiyangyang ring tones. Linear interpolation also droops 2-4 kHz (sinc^2).
 *   Everything else measured on the device was clean (no underrun, no clipping, no seam, no edge click).
 *
 * Kernel: Kaiser-windowed sinc, VWR_TAPS taps, VWR_PHASES sub-sample phases (nearest phase, so the timing
 * error is <= 1/1024 source sample; the measured effect is in debug/tools/wavq/test_fir.c), cutoff
 * VWR_CUTOFF * source Nyquist, Q15 coefficients normalised per phase
 * so DC gain is exactly 1. Used only for rate < out_rate; rate >= out_rate keeps vwr_lerp.
 * The table is 32 KB; vwr_fir_init() builds it once with libm (a few thousand sin() calls).
 * ------------------------------------------------------------------------------------------------------- */
#include <math.h>

#define VWR_HW          16                         /* source samples on each side of the position */
#define VWR_TAPS        (2 * VWR_HW)
#define VWR_PHASE_BITS  9
#define VWR_PHASES      (1 << VWR_PHASE_BITS)
#ifndef VWR_CUTOFF                                  /* overridable with -D for the host sweep in test_fir.c */
#define VWR_CUTOFF      0.98                       /* fraction of the source Nyquist frequency */
#endif
#ifndef VWR_KAISER_BETA
#define VWR_KAISER_BETA 7.0                        /* sweep 0.94..0.98 x 7..8 in test_fir.c: 0.98/7 keeps 3.5 kHz within 0.14 dB */
#endif

typedef struct { int16_t c[VWR_PHASES][VWR_TAPS]; } vwr_fir;

static inline double vwr_i0(double x) {            /* modified Bessel function of order 0 */
    double s = 1.0, t = 1.0;
    int k;
    for (k = 1; k < 60; k++) {
        double h = x / (2.0 * k);
        t *= h * h;
        s += t;
        if (t < 1e-14 * s) break;
    }
    return s;
}

static inline void vwr_fir_init(vwr_fir *f) {
    const double pi = 3.14159265358979323846;
    const double i0b = vwr_i0(VWR_KAISER_BETA);
    int p, j;
    for (p = 0; p < VWR_PHASES; p++) {
        double h[VWR_TAPS], sum = 0.0;
        int32_t q[VWR_TAPS], qsum = 0;
        for (j = 0; j < VWR_TAPS; j++) {
            /* tap j reads source index i - HW + 1 + j; distance from the output position i + frac */
            double t = (double)p / VWR_PHASES + (VWR_HW - 1 - j);
            double r = t / VWR_HW;
            double s = (t == 0.0) ? VWR_CUTOFF : sin(pi * VWR_CUTOFF * t) / (pi * t);
            double w = (r <= -1.0 || r >= 1.0) ? 0.0 : vwr_i0(VWR_KAISER_BETA * sqrt(1.0 - r * r)) / i0b;
            h[j] = s * w;
            sum += h[j];
        }
        for (j = 0; j < VWR_TAPS; j++) {
            q[j] = (int32_t)floor(h[j] / sum * 32768.0 + 0.5);
            qsum += q[j];
        }
        q[VWR_HW - 1] += 32768 - qsum;             /* exact DC gain: put the rounding residue on the centre tap */
        for (j = 0; j < VWR_TAPS; j++) f->c[p][j] = (int16_t)q[j];
    }
}

/* Integer source index and nearest phase of a 32.32 position. */
static inline void vwr_fir_locate(uint64_t pos_q32, long *i, unsigned *ph) {
    uint64_t p = pos_q32 + (1ULL << (32 - VWR_PHASE_BITS - 1));
    *i = (long)(p >> 32);
    *ph = (unsigned)((p >> (32 - VWR_PHASE_BITS)) & (VWR_PHASES - 1));
}

typedef int32_t (*vwr_get_fn)(const void *ctx, long frame, int ch);

/* win[j] = source sample i - HW + 1 + j of channel ch, edges held (clamped to 0 .. frames-1). */
static inline void vwr_fir_window(int32_t *win, long i, long frames, vwr_get_fn get, const void *ctx, int ch) {
    int j;
    for (j = 0; j < VWR_TAPS; j++) {
        long k = i - VWR_HW + 1 + j;
        if (k < 0) k = 0;
        if (k > frames - 1) k = frames - 1;
        win[j] = get(ctx, k, ch);
    }
}

/* Filtered value for phase ph over a window from vwr_fir_window, clamped to the s16 range. */
static inline int32_t vwr_fir_dot(const vwr_fir *f, unsigned ph, const int32_t *win) {
    int64_t acc = 0;
    int32_t v;
    int j;
    for (j = 0; j < VWR_TAPS; j++) acc += (int64_t)f->c[ph][j] * win[j];
    v = (int32_t)((acc + 16384) >> 15);
    if (v > 32767) v = 32767;
    if (v < -32768) v = -32768;
    return v;
}

#endif /* VITA_WAV_RESAMPLE_H */
