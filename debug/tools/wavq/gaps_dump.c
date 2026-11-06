/* Note-boundary check of a pcm_dump.raw (s16le stereo 48 kHz): finds silent gaps (>= MINGAP frames of exactly 0 between sound)
 * and prints the signal right before/after each, i.e. whether notes start/stop with a step (click) or ramp.
 *   /usr/bin/gcc -B/usr/bin -O1 gaps_dump.c -o /tmp/gaps_dump && /tmp/gaps_dump pcm_dump.raw */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#define MINGAP 48
int main(int argc, char **argv) {
    FILE *f = fopen(argv[1], "rb"); long n, i, g = 0, a, b; int16_t *s; double sumon = 0, sumoff = 0; int pk = 0;
    if (!f) return 2;
    fseek(f, 0, SEEK_END); n = ftell(f) / 4; fseek(f, 0, SEEK_SET);
    s = malloc(n * 4); if (fread(s, 4, n, f) != (size_t)n) return 2; fclose(f);
    for (i = 0; i < n; i++) { int v = abs(s[i * 2]); if (v > pk) pk = v; }
    printf("peak %d; gaps (run of >=%d exact zeros between sound):\n", pk, MINGAP);
    for (i = 0; i < n;) {
        if (s[i * 2] == 0) {
            long j = i; while (j < n && s[j * 2] == 0) j++;
            if (j - i >= MINGAP && i > 0 && j < n) {
                int last = s[(i - 1) * 2], first = s[j * 2];
                int slast = abs(s[(i - 1) * 2] - s[(i - 2) * 2]), sfirst = abs(s[(j + 1) * 2] - s[j * 2]);
                if (g < 24) printf("  gap %2ld at %.3fs len %4ld frames (%.1f ms): last before = %6d (|%3.0f%%| of peak)  first after = %6d (|%3.0f%%|)\n",
                                   g, (double)i / 48000, j - i, (j - i) / 48.0, last, 100.0 * abs(last) / pk, first, 100.0 * abs(first) / pk);
                sumoff += abs(last); sumon += abs(first); g++; (void)slast; (void)sfirst;
            }
            i = j;
        } else i++;
    }
    printf("%ld gaps; mean |last sample before a gap| = %.0f (%.0f%% of peak), mean |first sample after| = %.0f (%.0f%%)\n",
           g, g ? sumoff / g : 0, g ? 100 * sumoff / g / pk : 0, g ? sumon / g : 0, g ? 100 * sumon / g / pk : 0);
    /* sound immediately adjacent notes: a pitch change with no gap */
    a = b = 0;
    for (i = 2; i < n; i++) { int d = abs(s[i * 2] - s[(i - 1) * 2]); if (d > pk / 2) a++; if (d > pk / 4) b++; }
    printf("sample-to-sample steps > 50%% of peak: %ld ; > 25%% of peak: %ld  (a clean band-limited square has few)\n", a, b);
    return 0;
}
