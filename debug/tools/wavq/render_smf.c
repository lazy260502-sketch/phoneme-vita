/* Render a Standard MIDI File exactly like vita-port/src/vita_midi.c does (TinySoundFont + TinyMidiLoader, TimGM6mb.sf2,
 * 48 kHz stereo, 512-frame chunks, same dispatch loop, 1500 ms release tail) and write s16le stereo.
 *   /usr/bin/gcc -B/usr/bin -O1 -I../../../vita-port/src render_smf.c -lm -o /tmp/render_smf
 *   /tmp/render_smf ../../../vita-port/assets/TimGM6mb.sf2 tune.mid out.raw [volume 0..100]
 * Prints the parsed info and, for the rendered stream: peak, rms, clipped count, DC offset and the A-weighted-free
 * share of energy above 4 kHz / 8 kHz (the "hiss" bands), so it can be set against the WAV path (image_err.c). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#define TSF_IMPLEMENTATION
#define TSF_NO_STDIO
#include "tsf.h"
#define TML_IMPLEMENTATION
#define TML_NO_STDIO
#include "tml.h"

static unsigned char *rd(const char *p, int *n) {
    FILE *f = fopen(p, "rb"); long sz; unsigned char *b;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); sz = ftell(f); fseek(f, 0, SEEK_SET);
    b = malloc(sz); if (fread(b, 1, sz, f) != (size_t)sz) return NULL;
    fclose(f); *n = (int)sz; return b;
}
static void fft(double *re, double *im, int n) {
    int i, j = 0, k;
    for (i = 1; i < n; i++) { int b = n >> 1; for (; j & b; b >>= 1) j ^= b; j ^= b; if (i < j) { double t = re[i]; re[i] = re[j]; re[j] = t; t = im[i]; im[i] = im[j]; im[j] = t; } }
    for (k = 2; k <= n; k <<= 1) { double a = -2 * M_PI / k; for (i = 0; i < n; i += k) for (j = 0; j < k / 2; j++) {
        double wr = cos(a * j), wi = sin(a * j), xr = re[i + j + k / 2] * wr - im[i + j + k / 2] * wi, xi = re[i + j + k / 2] * wi + im[i + j + k / 2] * wr;
        re[i + j + k / 2] = re[i + j] - xr; im[i + j + k / 2] = im[i + j] - xi; re[i + j] += xr; im[i + j] += xi; } }
}

int main(int argc, char **argv) {
    int sfn, mn, uc = 0, up = 0, tn = 0; unsigned tf = 0, tl = 0;
    unsigned char *sf, *mid; tsf *t; tml_message *msg, *cur; short buf[512 * 2]; double msec = 0;
    long rel, total = 0, cap = 0; short *all = NULL; FILE *o; int ch;
    if (argc < 4) { fprintf(stderr, "usage: %s font.sf2 in.mid out.raw [volume]\n", argv[0]); return 2; }
    sf = rd(argv[1], &sfn); mid = rd(argv[2], &mn);
    if (!sf || !mid) { fprintf(stderr, "read failed\n"); return 2; }
    t = tsf_load_memory(sf, sfn); msg = tml_load_memory(mid, mn);
    if (!t || !msg) { fprintf(stderr, "load failed (tsf %p tml %p)\n", (void *)t, (void *)msg); return 3; }
    tml_get_info(msg, &uc, &up, &tn, &tf, &tl);
    printf("midi: %d ch %d prog %d notes, first note %u ms, length %u ms\n", uc, up, tn, tf, tl);
    tsf_set_output(t, TSF_STEREO_INTERLEAVED, 48000, 0.0f);
    tsf_set_max_voices(t, 32);
    tsf_set_volume(t, (argc > 4 ? atoi(argv[4]) : 100) / 100.0f);
    tsf_reset(t);
    tsf_channel_set_bank_preset(t, 9, 128, 0);
    for (cur = msg; cur != NULL;) {
        while (cur != NULL && msec >= (double)cur->time) {
            switch (cur->type) {
            case TML_PROGRAM_CHANGE: tsf_channel_set_presetnumber(t, cur->channel, cur->program, cur->channel == 9); break;
            case TML_NOTE_ON: tsf_channel_note_on(t, cur->channel, cur->key, cur->velocity / 127.0f); break;
            case TML_NOTE_OFF: tsf_channel_note_off(t, cur->channel, cur->key); break;
            case TML_PITCH_BEND: tsf_channel_set_pitchwheel(t, cur->channel, cur->pitch_bend); break;
            case TML_CONTROL_CHANGE: tsf_channel_midi_control(t, cur->channel, cur->control, cur->control_value); break;
            default: break;
            }
            cur = cur->next;
        }
        tsf_render_short(t, buf, 512, 0);
        if (total + 512 > cap) { cap = cap ? cap * 2 : 1 << 20; all = realloc(all, cap * 4); }
        memcpy(all + total * 2, buf, 512 * 4); total += 512;
        msec += 512 * (1000.0 / 48000);
    }
    for (ch = 0; ch < 16; ch++) tsf_channel_note_off_all(t, ch);
    for (rel = 1500 * 48; rel > 0; rel -= 512) {
        tsf_render_short(t, buf, 512, 0);
        if (total + 512 > cap) { cap = cap * 2; all = realloc(all, cap * 4); }
        memcpy(all + total * 2, buf, 512 * 4); total += 512;
    }
    o = fopen(argv[3], "wb"); fwrite(all, 4, total, o); fclose(o);
    {
        long i, clip = 0; int peak = 0; double ss = 0, dc = 0;
        for (i = 0; i < total; i++) { int v = all[i * 2]; if (abs(v) > peak) peak = abs(v); if (v >= 32767 || v <= -32768) clip++; ss += (double)v * v; dc += v; }
        printf("rendered %ld frames (%.2f s): peak %d (%.0f %% FS), rms %.0f, clipped %ld, dc %.1f\n", total, total / 48000.0, peak, peak * 100.0 / 32768, sqrt(ss / total), clip, dc / total);
    }
    {   /* energy share by band over the sounding windows */
        enum { N = 8192 }; static double re[N], im[N], e[N / 2 + 1]; long i, k; int cnt = 0; double tot = 0, e4 = 0, e8 = 0;
        for (i = 4096; i + N <= total; i += N / 2) {
            double en = 0; for (k = 0; k < N; k++) { double v = all[(i + k) * 2]; en += v * v; } if (en / N < 1e5) continue;
            for (k = 0; k < N; k++) { re[k] = all[(i + k) * 2] * (0.5 - 0.5 * cos(2 * M_PI * k / N)); im[k] = 0; } fft(re, im, N);
            for (k = 0; k <= N / 2; k++) e[k] += re[k] * re[k] + im[k] * im[k]; cnt++;
        }
        for (k = 1; k <= N / 2; k++) { double f = (double)k * 48000 / N; tot += e[k]; if (f >= 4000) e4 += e[k]; if (f >= 8000) e8 += e[k]; }
        printf("%d sounding windows: energy above 4 kHz %.1f dB, above 8 kHz %.1f dB (re total)\n", cnt, 10 * log10(e4 / tot + 1e-30), 10 * log10(e8 / tot + 1e-30));
    }
    return 0;
}
