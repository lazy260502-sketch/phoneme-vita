/* Host test for vita-port/src/vita_wav_resample.h - the header that ships is the header that is tested.
 *   /usr/bin/gcc -B/usr/bin -O1 -I../../../vita-port/src test_resample.c -lm -o /tmp/test_resample && /tmp/test_resample
 * (vitasdk's gcc/as are first in PATH here; -B/usr/bin picks the host assembler.) */
#include <stdio.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include "vita_wav_resample.h"

#define OUT 48000
static int fails;
#define CK(c, ...) do { if (!(c)) { fails++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

int main(void) {
    static const long rates[] = {8000, 11025, 12000, 16000, 22050, 24000, 32000, 44100, 48000, 96000};
    printf("rate   frames_in -> out frames   duration in / out (ms)   old multiplier -> old duration\n");
    for (unsigned k = 0; k < sizeof rates / sizeof *rates; k++) {
        long r = rates[k], frames = r * 3;                      /* 3 s of audio */
        long out = vwr_out_frames(frames, r, OUT);
        long oldm = OUT / r;
        long old_out = frames * oldm;
        double din = frames * 1000.0 / r, dout = out * 1000.0 / OUT, dold = old_out * 1000.0 / OUT;
        printf("%5ld  %9ld -> %8ld     %7.1f / %7.1f          x%ld -> %7.1f ms%s\n", r, frames, out, din, dout, oldm, dold,
               fabs(dold - din) > 2 ? "   <-- old was wrong" : "");
        CK(fabs(dout - din) < 1.0, "rate %ld: output %.2f ms vs input %.2f ms", r, dout, din);
    }

    /* position: monotonic, starts at 0, last frame stays inside the source */
    for (unsigned k = 0; k < sizeof rates / sizeof *rates; k++) {
        long r = rates[k], frames = r * 2, out = vwr_out_frames(frames, r, OUT);
        uint64_t prev = 0, first = vwr_pos_q32(0, r, OUT), last = vwr_pos_q32(out - 1, r, OUT);
        int mono = 1;
        for (long n = 0; n < out; n++) { uint64_t p = vwr_pos_q32(n, r, OUT); if (p < prev) mono = 0; prev = p; }
        CK(first == 0, "rate %ld: first position %llu", r, (unsigned long long)first);
        CK(mono, "rate %ld: position not monotonic", r);
        CK((long)(last >> 32) <= frames - 1, "rate %ld: last position %ld beyond %ld", r, (long)(last >> 32), frames - 1);
    }

    /* the long-tune case that overflowed 32 bits in v03.11: 270776 frames at 8 kHz */
    {
        long frames = 270776, out = vwr_out_frames(frames, 8000, OUT);
        long n = out - 1;
        CK(out == 1624656, "34 s tune out frames %ld", out);
        CK((long)(vwr_pos_q32(n, 8000, OUT) >> 32) == frames - 1, "34 s tune last src %ld", (long)(vwr_pos_q32(n, 8000, OUT) >> 32));
    }

    /* quality: 1 kHz sine, SNR of nearest vs linear against the ideal sine, same rates as the Python estimate */
    printf("\n1 kHz sine, SNR of the 48 kHz output against the ideal sine (dB)\n");
    static const long qr[] = {8000, 11025, 16000, 22050, 44100};
    for (unsigned k = 0; k < sizeof qr / sizeof *qr; k++) {
        long r = qr[k], n = r / 5;
        static int32_t src[20000];
        for (long i = 0; i < n; i++) src[i] = (int32_t)lrint(sin(2 * M_PI * 1000 * i / r) * 16000);
        long out = vwr_out_frames(n, r, OUT);
        double sn = 0, dn = 0, sl = 0, dl = 0;
        for (long m = 0; m < out - 2; m++) {
            uint64_t pos = vwr_pos_q32(m, r, OUT);
            long i = (long)(pos >> 32); uint32_t fr = (uint32_t)((pos >> 16) & 0xFFFF);
            double ref = sin(2 * M_PI * 1000 * (m / (double)OUT)) * 16000;
            int32_t nearest = src[i];
            int32_t lin = (i >= n - 1) ? src[n - 1] : vwr_lerp(src[i], src[i + 1], fr);
            sn += ref * ref; dn += (nearest - ref) * (nearest - ref);
            sl += ref * ref; dl += (lin - ref) * (lin - ref);
        }
        double snr_n = 10 * log10(sn / dn), snr_l = 10 * log10(sl / dl);
        printf("  %5ld Hz source: nearest %5.1f   linear %5.1f\n", r, snr_n, snr_l);
        CK(snr_l > snr_n + 10, "rate %ld: linear (%.1f) is not clearly better than nearest (%.1f)", r, snr_l, snr_n);
    }

    /* the render loop adds vwr_step_q32 per frame: it must track the exact formula over the whole tune */
    {
        static const long ir[] = {8000, 11025, 22050, 44100, 96000};
        for (unsigned k = 0; k < sizeof ir / sizeof *ir; k++) {
            long r = ir[k], frames = r * 90, out = vwr_out_frames(frames, r, OUT);   /* 90 s */
            uint64_t step = vwr_step_q32(r, OUT), pos = 0, worst = 0;
            for (long n = 0; n < out; n++, pos += step) {
                uint64_t ex = vwr_pos_q32(n, r, OUT);
                uint64_t d = pos > ex ? pos - ex : ex - pos;
                if (d > worst) worst = d;
            }
            printf("  incremental vs exact, %6ld Hz, 90 s: worst error %.6f source frames\n", r, worst / 4294967296.0);
            CK(worst / 4294967296.0 < 0.001, "rate %ld: incremental position drifts %.6f frames", r, worst / 4294967296.0);
            CK((long)(pos >> 32) >= frames - 1 && (long)((pos - step) >> 32) <= frames - 1, "rate %ld: end position", r);
        }
    }

    /* fade-out chunk: starts one step below the last level, ends at exactly 0, monotonic, never exceeds the last level */
    {
        static int16_t buf[512 * 2];
        static const int32_t lv[][2] = {{11264, -11264}, {-11264, 11264}, {32767, -32768}, {1, -1}, {0, 0}};
        for (unsigned k = 0; k < sizeof lv / sizeof *lv; k++) {
            vwr_fadeout(buf, 512, lv[k][0], lv[k][1]);
            int ok = buf[511 * 2] == 0 && buf[511 * 2 + 1] == 0;
            int mono = 1, bound = 1;
            for (int i = 0; i < 512; i++) {
                if (i > 0 && abs(buf[i * 2]) > abs(buf[(i - 1) * 2])) mono = 0;
                if (abs(buf[i * 2]) > abs(lv[k][0]) || abs(buf[i * 2 + 1]) > abs(lv[k][1])) bound = 0;
            }
            int step = abs(buf[0] - lv[k][0]);   /* jump from the last submitted sample to the first fade sample */
            printf("  fadeout from (%6d,%6d): first step %d (was a %d step without it), last (%d,%d)\n", lv[k][0], lv[k][1], step, abs(lv[k][0]), buf[511 * 2], buf[511 * 2 + 1]);
            CK(ok, "fadeout %d: does not end at 0", k);
            CK(mono, "fadeout %d: not monotonic", k);
            CK(bound, "fadeout %d: exceeds the start level", k);
            CK(step <= abs(lv[k][0]) / 400 + 1, "fadeout %d: first step %d is still a click", k, step);
        }
    }

    /* lerp edge cases */
    CK(vwr_lerp(100, 200, 0) == 100, "lerp frac 0");
    CK(vwr_lerp(100, 200, 32768) == 150, "lerp frac .5");
    CK(vwr_lerp(-32768, 32767, 65535) >= 32765, "lerp extreme");
    CK(vwr_lerp(-100, -200, 32768) == -150, "lerp negative");

    printf("\n%s (%d failure%s)\n", fails ? "FAILED" : "ALL PASS", fails, fails == 1 ? "" : "s");
    return fails != 0;
}
