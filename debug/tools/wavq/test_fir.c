/* Host test for the polyphase FIR in vita-port/src/vita_wav_resample.h (the header that ships is the header tested).
 *   /usr/bin/gcc -B/usr/bin -O1 -I../../../vita-port/src test_fir.c -lm -o /tmp/test_fir && /tmp/test_fir [src.wav out.raw]
 * With a WAV (8-bit mono PCM) it also writes the 48 kHz s16 stereo stream wav_render_player now produces, so
 * image_err can compare it with the ideal resample:   /tmp/image_err src.wav out.raw                         */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>
#include "vita_wav_resample.h"

#define OUT 48000
static int fails;
#define CK(c, ...) do { if (!(c)) { fails++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

static vwr_fir F;
static const int32_t *g_src;
static int32_t get(const void *ctx, long k, int ch) { (void)ctx; (void)ch; return g_src[k]; }

/* resample a buffer exactly as the render loop does */
static long resample(const int32_t *src, long frames, long rate, int32_t *out, long max_out) {
    long n = vwr_out_frames(frames, rate, OUT), m, fi = -0x40000000L; uint64_t pos = 0, step = vwr_step_q32(rate, OUT);
    int32_t win[VWR_TAPS];
    if (n > max_out) n = max_out;
    g_src = src;
    for (m = 0; m < n; m++, pos += step) {
        long si; unsigned ph;
        vwr_fir_locate(pos, &si, &ph);
        if (si != fi) { vwr_fir_window(win, si, frames, get, NULL, 0); fi = si; }
        out[m] = vwr_fir_dot(&F, ph, win);
    }
    return n;
}

/* same, linear interpolation (the v03.14..v03.31 path) */
static long resample_lin(const int32_t *src, long frames, long rate, int32_t *out, long max_out) {
    long n = vwr_out_frames(frames, rate, OUT), m; uint64_t pos = 0, st = vwr_step_q32(rate, OUT);
    if (n > max_out) n = max_out;
    for (m = 0; m < n; m++, pos += st) { long q = (long)(pos >> 32); out[m] = (q >= frames - 1) ? src[frames - 1] : vwr_lerp(src[q], src[q + 1], (uint32_t)((pos >> 16) & 0xFFFF)); }
    return n;
}

/* amplitude of a sine at freq f (Hz) in x[a..b) by Hann-windowed correlation, in dB re 32768 */
static double tone_db(const int32_t *x, long a, long b, double f) {
    double sr = 0, si = 0; long i;
    for (i = a; i < b; i++) { double w = 0.5 - 0.5 * cos(2 * M_PI * (i - a) / (b - a)); sr += x[i] * w * cos(2 * M_PI * f * i / OUT); si += x[i] * w * sin(2 * M_PI * f * i / OUT); }
    return 20 * log10(2 * sqrt(sr * sr + si * si) / (0.5 * (b - a)) / 32768.0 + 1e-12);
}

/* strongest image (dB re the tone) of an 8 kHz sine f after upsampling, over the images at 8k*m +- f below 24 kHz */
static double worst_image(const int32_t *y, long n, double f) {
    double ref = tone_db(y, 6000, n - 6000, f), worst = -300, k;
    for (k = 8000; k < 24000; k += 8000) {
        double a = tone_db(y, 6000, n - 6000, k - f) - ref, b = tone_db(y, 6000, n - 6000, k + f) - ref;
        if (a > worst) worst = a;
        if (b > worst) worst = b;
    }
    return worst;
}

int main(int argc, char **argv) {
    static int32_t src[8000], out[OUT], lin[OUT];
    static const double tones[] = {200, 1000, 2000, 3000, 3500};
    long i, n; int p, j; unsigned t;
    vwr_fir_init(&F);

    /* 1. every phase sums to exactly 32768 (unity DC gain); phase 0 is symmetric about the centre tap */
    for (p = 0; p < VWR_PHASES; p++) { int s = 0; for (j = 0; j < VWR_TAPS; j++) s += F.c[p][j]; CK(s == 32768, "phase %d DC gain %d", p, s); }
    for (j = 0; j < VWR_HW - 1; j++) CK(abs(F.c[0][j] - F.c[0][2 * (VWR_HW - 1) - j]) <= 1, "phase 0 asymmetric at tap %d", j);
    CK(F.c[0][VWR_HW - 1] > 30000, "phase 0 centre tap %d", F.c[0][VWR_HW - 1]);

    /* 2. sines through 8 kHz -> 48 kHz: passband gain and the strongest image, FIR vs linear */
    printf("tone(Hz)  gain(dB)  worst image dB re tone: FIR   linear\n");
    for (t = 0; t < sizeof tones / sizeof *tones; t++) {
        double f = tones[t], g, img, imgl;
        for (i = 0; i < 8000; i++) src[i] = (int32_t)lrint(sin(2 * M_PI * f * i / 8000) * 10000);
        n = resample(src, 8000, 8000, out, OUT);
        resample_lin(src, 8000, 8000, lin, OUT);
        g = tone_db(out, 6000, n - 6000, f) - 20 * log10(10000 / 32768.0);
        img = worst_image(out, n, f); imgl = worst_image(lin, n, f);
        printf("%6.0f   %7.2f              %7.1f   %7.1f\n", f, g, img, imgl);
        CK(fabs(g) < 0.5, "tone %.0f Hz gain %.2f dB", f, g);
        CK(img < -55, "tone %.0f Hz image %.1f dB re the tone", f, img);
        CK(img < imgl - 15, "tone %.0f Hz FIR image %.1f not clearly below linear %.1f", f, img, imgl);
    }

    /* 3. DC preserved; full-scale square never leaves s16 */
    for (i = 0; i < 8000; i++) src[i] = 12345;
    n = resample(src, 8000, 8000, out, OUT);
    { int bad = 0; for (i = 0; i < n; i++) if (abs(out[i] - 12345) > 2) bad++; CK(bad == 0, "DC: %d samples off by > 2", bad); }
    for (i = 0; i < 8000; i++) src[i] = ((i / 3) & 1) ? 32767 : -32768;
    n = resample(src, 8000, 8000, out, OUT);
    { int bad = 0; for (i = 0; i < n; i++) if (out[i] > 32767 || out[i] < -32768) bad++; CK(bad == 0, "clamp"); }

    /* 4. exact length; the first and last frames stay at the held edge value */
    for (i = 0; i < 100; i++) src[i] = 5000;
    n = resample(src, 100, 8000, out, OUT);
    CK(n == 600, "length %ld", n);
    CK(abs(out[0] - 5000) <= 2 && abs(out[n - 1] - 5000) <= 2, "edges %d %d", out[0], out[n - 1]);

    /* 5. optional: render a real 8-bit mono WAV exactly as the device does */
    if (argc >= 3) {
        FILE *f = fopen(argv[1], "rb"); long wl, fr, no, m; uint8_t *w; int32_t *s, *o; FILE *of; int rate;
        if (!f) return 2;
        fseek(f, 0, SEEK_END); wl = ftell(f); fseek(f, 0, SEEK_SET); w = malloc(wl);
        if (fread(w, 1, wl, f) != (size_t)wl) return 2;
        fclose(f);
        rate = w[24] | (w[25] << 8) | (w[26] << 16); fr = wl - 44;
        s = malloc(fr * 4); for (i = 0; i < fr; i++) s[i] = ((int)w[44 + i] - 128) << 8;
        o = malloc((fr * 6 + 100) * 4); no = resample(s, fr, rate, o, fr * 6 + 100);
        of = fopen(argv[2], "wb");
        for (m = 0; m < no; m++) { int16_t v[2]; v[0] = v[1] = (int16_t)o[m]; fwrite(v, 2, 2, of); }
        fclose(of);
        printf("wrote %ld frames\n", no);
    }
    printf("\n%s (%d failure%s)\n", fails ? "FAILED" : "ALL PASS", fails, fails == 1 ? "" : "s");
    return fails != 0;
}
