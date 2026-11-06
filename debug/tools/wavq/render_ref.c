/* Host reference renderer: turns an 8-bit/16-bit PCM WAV into exactly the stream wav_render_player produces
 * (48 kHz s16le stereo, 512-frame chunks, same header vita_wav_resample.h, volume 100). Used to diff against the
 * device's pcm_dump.raw.
 *   /usr/bin/gcc -B/usr/bin -O1 -I../../../vita-port/src render_ref.c -o /tmp/render_ref
 *   /tmp/render_ref game_tune.wav ref.raw [seconds]                                                              */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "vita_wav_resample.h"

#define OUT 48000
static uint8_t *w; static long wlen;
static int rd16(long o) { return (int16_t)(w[o] | (w[o + 1] << 8)); }
int main(int argc, char **argv) {
    FILE *f, *o; long frames, out, r = 0, limit; int ch, bits; long rate; uint64_t pos = 0, step;
    if (argc < 3) { fprintf(stderr, "usage: %s in.wav out.raw [seconds]\n", argv[0]); return 2; }
    f = fopen(argv[1], "rb"); if (!f) return 2;
    fseek(f, 0, SEEK_END); wlen = ftell(f); fseek(f, 0, SEEK_SET);
    w = malloc(wlen); if (fread(w, 1, wlen, f) != (size_t)wlen) return 2; fclose(f);
    ch = rd16(22); rate = (long)(w[24] | (w[25] << 8) | (w[26] << 16) | ((long)w[27] << 24)); bits = rd16(34);
    frames = (wlen - 44) / (ch * (bits >> 3));
    out = vwr_out_frames(frames, rate, OUT);
    limit = argc > 3 ? (long)(atof(argv[3]) * OUT) : out; if (limit > out) limit = out;
    step = vwr_step_q32(rate, OUT);
    o = fopen(argv[2], "wb");
    while (r < limit) {
        long i = (long)(pos >> 32); uint32_t fr = (uint32_t)((pos >> 16) & 0xFFFF); int c; int16_t s[2];
        for (c = 0; c < 2; c++) {
            int cc = (ch == 2) ? c : 0; int32_t a, b, v; long ia = i, ib = i + 1;
            if (ib > frames - 1) ib = frames - 1; if (ia > frames - 1) ia = frames - 1;
            if (bits == 8) { a = ((int32_t)w[44 + ia * ch + cc] - 128) << 8; b = ((int32_t)w[44 + ib * ch + cc] - 128) << 8; }
            else { a = rd16(44 + (ia * ch + cc) * 2); b = rd16(44 + (ib * ch + cc) * 2); }
            v = (i >= frames - 1) ? a : vwr_lerp(a, b, fr);
            s[c] = (int16_t)v;
        }
        fwrite(s, 2, 2, o); r++; pos += step;
    }
    fclose(o);
    fprintf(stderr, "%s: %ld Hz %d ch %d bit, %ld frames -> %ld out frames written (%.2f s)\n", argv[1], rate, ch, bits, frames, r, r / 48000.0);
    return 0;
}
