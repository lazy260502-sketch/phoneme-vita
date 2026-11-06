/* How far is the device output (linear-interpolated 8 kHz -> 48 kHz) from the ideal band-limited resample of the same WAV?
 * The difference is the resampler's imaging distortion. Reports its level and A-weighted spectrum.
 *   /usr/bin/gcc -B/usr/bin -O1 image_err.c -lm -o /tmp/image_err && /tmp/image_err src.wav pcm_dump.raw
 * src.wav: 8-bit mono PCM WAV (44-byte header), rate must divide 48000. */
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <stdint.h>
#define SR 48000
#define N 8192
#define HW 24                                   /* source-sample half width of the sinc */
static void fft(double *re, double *im, int n) {
    int i, j = 0, k;
    for (i = 1; i < n; i++) { int b = n >> 1; for (; j & b; b >>= 1) j ^= b; j ^= b; if (i < j) { double t = re[i]; re[i] = re[j]; re[j] = t; t = im[i]; im[i] = im[j]; im[j] = t; } }
    for (k = 2; k <= n; k <<= 1) { double a = -2 * M_PI / k; for (i = 0; i < n; i += k) for (j = 0; j < k / 2; j++) {
        double wr = cos(a * j), wi = sin(a * j), xr = re[i + j + k / 2] * wr - im[i + j + k / 2] * wi, xi = re[i + j + k / 2] * wi + im[i + j + k / 2] * wr;
        re[i + j + k / 2] = re[i + j] - xr; im[i + j + k / 2] = im[i + j] - xi; re[i + j] += xr; im[i + j] += xi; } }
}
static double aw(double f) {                    /* A-weighting, linear power gain */
    double f2 = f * f, a = 12194.0 * 12194.0 * f2 * f2 / ((f2 + 20.6 * 20.6) * sqrt((f2 + 107.7 * 107.7) * (f2 + 737.9 * 737.9)) * (f2 + 12194.0 * 12194.0));
    return a * a * 1.2589 * 1.2589;              /* +2 dB so that 1 kHz is 0 dB */
}
int main(int argc, char **argv) {
    FILE *f; uint8_t *w; long wl, nf, i, m, n; int16_t *s; double *ideal, *dump; int ratio, rate;
    static double re[N], im[N], es[N / 2 + 1], ee[N / 2 + 1];
    if (argc < 3) return 2;
    f = fopen(argv[1], "rb"); fseek(f, 0, SEEK_END); wl = ftell(f); fseek(f, 0, SEEK_SET); w = malloc(wl); if (fread(w, 1, wl, f) != (size_t)wl) return 2; fclose(f);
    rate = w[24] | (w[25] << 8) | (w[26] << 16); ratio = SR / rate; nf = wl - 44;
    f = fopen(argv[2], "rb"); fseek(f, 0, SEEK_END); n = ftell(f) / 4; fseek(f, 0, SEEK_SET); s = malloc(n * 4); if (fread(s, 4, n, f) != (size_t)n) return 2; fclose(f);
    ideal = malloc(n * sizeof(double)); dump = malloc(n * sizeof(double));
    for (m = 0; m < n; m++) {
        double x = (double)m / ratio, acc = 0; long c = (long)floor(x), k;
        for (k = c - HW + 1; k <= c + HW; k++) {
            double t = x - k, win, sc; if (k < 0 || k >= nf) continue;
            win = 0.5 + 0.5 * cos(M_PI * t / HW); sc = (fabs(t) < 1e-9) ? 1.0 : sin(M_PI * t) / (M_PI * t);
            acc += (((int)w[44 + k]) - 128) * 256.0 * sc * win;
        }
        ideal[m] = acc; dump[m] = s[m * 2];
    }
    { double se = 0, ss = 0; long cnt = 0; for (m = 2000; m < n - 2000; m++) { double e = dump[m] - ideal[m]; se += e * e; ss += ideal[m] * ideal[m]; cnt++; }
      printf("rate %d, ratio %d, %ld samples compared\n", rate, ratio, cnt);
      printf("error (device - ideal band-limited): rms %.1f vs signal rms %.1f => %.1f dB below signal (unweighted)\n", sqrt(se / cnt), sqrt(ss / cnt), 10 * log10(se / ss)); }
    { double sA = 0, eA = 0, e_lo = 0, e_hi = 0, eAhi = 0; int cnt = 0;
      for (i = 2048; i + N <= n - 2048; i += N / 2) {
        double e = 0; long k; for (k = 0; k < N; k++) e += ideal[i + k] * ideal[i + k]; if (e / N < 1e5) continue;
        for (k = 0; k < N; k++) { re[k] = ideal[i + k] * (0.5 - 0.5 * cos(2 * M_PI * k / N)); im[k] = 0; } fft(re, im, N);
        for (k = 0; k <= N / 2; k++) es[k] += re[k] * re[k] + im[k] * im[k];
        for (k = 0; k < N; k++) { re[k] = (dump[i + k] - ideal[i + k]) * (0.5 - 0.5 * cos(2 * M_PI * k / N)); im[k] = 0; } fft(re, im, N);
        for (k = 0; k <= N / 2; k++) ee[k] += re[k] * re[k] + im[k] * im[k];
        cnt++; }
      for (i = 1; i <= N / 2; i++) { double fr = (double)i * SR / N, g = aw(fr); sA += es[i] * g; eA += ee[i] * g; if (fr < 4000) e_lo += ee[i]; else { e_hi += ee[i]; eAhi += ee[i] * g; } }
      printf("%d sounding windows. error energy: below 4 kHz %.1f%%, above 4 kHz %.1f%% ; A-weighted error is %.1f dB below the A-weighted signal; %.0f%% of that A-weighted error lies above 4 kHz\n",
             cnt, 100 * e_lo / (e_lo + e_hi), 100 * e_hi / (e_lo + e_hi), 10 * log10(eA / sA), 100 * eAhi / eA);
      { static const int ed[] = {0, 1000, 2000, 3000, 4000, 5000, 6000, 7000, 8000, 10000, 12000, 16000, 24001}; int b;
        printf("band(Hz)        signal(A,dB)  error(A,dB)  error-minus-signal\n");
        for (b = 0; b < 12; b++) { double a = 0, c = 0; for (i = 1; i <= N / 2; i++) { double fr = (double)i * SR / N; if (fr >= ed[b] && fr < ed[b + 1]) { a += es[i] * aw(fr); c += ee[i] * aw(fr); } }
            printf("%5d-%5d   %9.1f   %9.1f   %9.1f\n", ed[b], ed[b + 1], 10 * log10(a / sA + 1e-30), 10 * log10(c / sA + 1e-30), 10 * log10((c + 1e-30) / (a + 1e-30))); } } }
    return 0;
}
