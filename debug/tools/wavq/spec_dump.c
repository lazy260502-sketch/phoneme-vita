/* Spectrum / noise analysis of a pcm_dump.raw (s16le stereo 48 kHz), no dependencies.
 *   /usr/bin/gcc -B/usr/bin -O1 spec_dump.c -lm -o /tmp/spec_dump && /tmp/spec_dump pcm_dump.raw
 * Reports: sample-value granularity (8-bit source), per-band energy, and for each strong note the energy that is NOT
 * at the fundamental's harmonics below 4 kHz ("in-band noise") and above 4 kHz (everything the 8 kHz source cannot contain =
 * resampler images), which is what is heard as hiss. */
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include <stdint.h>
#define SR 48000
#define N 8192
static void fft(double *re, double *im, int n) {
    int i, j = 0, k;
    for (i = 1; i < n; i++) { int b = n >> 1; for (; j & b; b >>= 1) j ^= b; j ^= b; if (i < j) { double t = re[i]; re[i] = re[j]; re[j] = t; t = im[i]; im[i] = im[j]; im[j] = t; } }
    for (k = 2; k <= n; k <<= 1) {
        double a = -2 * M_PI / k;
        for (i = 0; i < n; i += k) for (j = 0; j < k / 2; j++) {
            double wr = cos(a * j), wi = sin(a * j);
            double xr = re[i + j + k / 2] * wr - im[i + j + k / 2] * wi, xi = re[i + j + k / 2] * wi + im[i + j + k / 2] * wr;
            re[i + j + k / 2] = re[i + j] - xr; im[i + j + k / 2] = im[i + j] - xi; re[i + j] += xr; im[i + j] += xi;
        }
    }
}
int main(int argc, char **argv) {
    FILE *f = fopen(argv[1], "rb"); long n, i; int16_t *s; double *d; static double re[N], im[N], acc[N / 2 + 1];
    if (!f) return 2;
    fseek(f, 0, SEEK_END); n = ftell(f) / 4; fseek(f, 0, SEEK_SET);
    s = malloc(n * 4); if (fread(s, 4, n, f) != (size_t)n) return 2; fclose(f);
    d = malloc(n * sizeof(double)); for (i = 0; i < n; i++) d[i] = s[i * 2];
    { /* distinct values */
        static unsigned char seen[65536]; int c = 0; for (i = 0; i < n; i++) { int v = s[i * 2] + 32768; if (!seen[v]) { seen[v] = 1; c++; } }
        printf("frames %ld, distinct sample values %d\n", n, c);
    }
    { /* long-term spectrum over sounding windows */
        int cnt = 0, b; double tot = 0; static const int edges[] = {0, 500, 1000, 2000, 3000, 4000, 6000, 8000, 12000, 16000, 24001};
        for (i = 0; i + N <= n; i += N / 2) {
            double e = 0; int k; for (k = 0; k < N; k++) e += d[i + k] * d[i + k];
            if (e / N < 1e5) continue;                    /* skip silence */
            for (k = 0; k < N; k++) { re[k] = d[i + k] * (0.5 - 0.5 * cos(2 * M_PI * k / N)); im[k] = 0; }
            fft(re, im, N);
            for (k = 0; k <= N / 2; k++) acc[k] += re[k] * re[k] + im[k] * im[k];
            cnt++;
        }
        for (i = 0; i <= N / 2; i++) tot += acc[i];
        printf("long-term spectrum over %d sounding windows:\n", cnt);
        for (b = 0; b < 10; b++) {
            double e = 0; for (i = 0; i <= N / 2; i++) { double fr = (double)i * SR / N; if (fr >= edges[b] && fr < edges[b + 1]) e += acc[i]; }
            printf("  %5d-%5d Hz  %6.2f %%  %7.1f dB\n", edges[b], edges[b + 1], 100 * e / tot, 10 * log10(e / tot + 1e-30));
        }
    }
    { /* per steady window: fundamental, harmonic energy vs the rest, images above 4 kHz */
        int shown = 0;
        printf("steady windows (0.17 s): f0, energy above 4 kHz, in-band non-harmonic energy (all as %% of window energy)\n");
        for (i = 0; i + N <= n && shown < 14; i += N) {
            double e = 0, hi = 0, nh = 0, pk = 0; int k, pki = 0;
            for (k = 0; k < N; k++) e += d[i + k] * d[i + k];
            if (e / N < 1e6) continue;
            for (k = 0; k < N; k++) { re[k] = d[i + k] * (0.5 - 0.5 * cos(2 * M_PI * k / N)); im[k] = 0; }
            fft(re, im, N);
            for (k = 5; k < N / 2; k++) { double p = re[k] * re[k] + im[k] * im[k]; if (p > pk && (double)k * SR / N < 2500) { pk = p; pki = k; } }
            { double f0 = (double)pki * SR / N, tot = 0;
              for (k = 1; k <= N / 2; k++) { double p = re[k] * re[k] + im[k] * im[k], fr = (double)k * SR / N; tot += p;
                  if (fr > 4000) hi += p;
                  else { double h = fr / f0, dh = fabs(h - floor(h + 0.5)) * f0; if (dh > 40) nh += p; } }
              printf("  t=%5.2fs f0=%7.1f Hz  >4kHz %6.2f %%   in-band non-harmonic %6.2f %%\n", (double)i / SR, f0, 100 * hi / tot, 100 * nh / tot); }
            shown++;
        }
    }
    { /* note boundaries: size of the discontinuity at onset/offset relative to peak */
        long a, nb = 0; double pk = 0, sum = 0; int max = 0;
        for (i = 0; i < n; i++) if (fabs(d[i]) > pk) pk = fabs(d[i]);
        for (a = 1; a < n; a++) if ((d[a - 1] == 0) != (d[a] == 0)) { double j = fabs(d[a] - d[a - 1]); sum += j; nb++; if (j > max) max = (int)j; }
        printf("zero<->nonzero transitions: %ld, mean step %.0f, max %d, peak %.0f => mean step %.0f %% of peak\n", nb, nb ? sum / nb : 0, max, pk, nb ? 100 * sum / nb / pk : 0);
    }
    return 0;
}
