/* Host-side exercise of vita_dec_ogg, run on the build machine.
 * Not part of the VPK: it proves the decoder actually produces audio (and that
 * seek works) so the Vita build is not shipping "it compiled" as evidence. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "vita_pcm_dec.h"

extern const vita_pcm_dec vita_dec_ogg;

typedef struct {
    const uint8_t *d;
    long len;
    long reads;
} Mem;

static long mem_rd(void *ctx, long off, uint8_t *dst, long n) {
    Mem *m = (Mem *)ctx;
    if (off < 0 || off >= m->len || n <= 0) {
        return 0;
    }
    if (off + n > m->len) {
        n = m->len - off;
    }
    memcpy(dst, m->d + off, (size_t)n);
    m->reads++;
    return n;
}

static double rms_of(const int16_t *buf, long frames) {
    long i;
    double s = 0.0;
    for (i = 0; i < frames * 2; i++) {
        s += (double)buf[i] * (double)buf[i];
    }
    return (frames > 0) ? sqrt(s / (double)(frames * 2)) : 0.0;
}

int main(int argc, char **argv) {
    FILE *f;
    long len, got;
    uint8_t *data;
    Mem mem;
    void *h;
    int16_t *out;
    long frames, f1, f2;
    double r1, r2;
    static int16_t big[48000 * 2];

    if (argc < 2) {
        fprintf(stderr, "usage: %s file.ogg\n", argv[0]);
        return 2;
    }
    f = fopen(argv[1], "rb");
    if (f == NULL) { perror("open"); return 2; }
    fseek(f, 0, SEEK_END);
    len = ftell(f);
    fseek(f, 0, SEEK_SET);
    data = (uint8_t *)malloc((size_t)len);
    got = (long)fread(data, 1, (size_t)len, f);
    fclose(f);
    if (got != len) { fprintf(stderr, "short read\n"); return 2; }

    memset(&mem, 0, sizeof mem);
    mem.d = data;
    mem.len = len;

    h = vita_dec_ogg.open(mem_rd, &mem, len);
    if (h == NULL) {
        fprintf(stderr, "FAIL: open returned NULL\n");
        return 1;
    }
    printf("open OK: %d Hz, %d ch, %ld ms, %ld B\n",
           vita_dec_ogg.rate(h), vita_dec_ogg.channels(h),
           vita_dec_ogg.duration_ms(h), len);

    frames = vita_dec_ogg.read(h, big, 4800, 100);
    r1 = rms_of(big, frames);
    printf("first 4800 frames: %ld returned, rms %.1f\n", frames, r1);

    frames = vita_dec_ogg.read(h, big, 48000, 100);
    f1 = frames;
    printf("next 1 s: %ld frames\n", f1);

    /* 30 s in: either real audio or past the end for this short sample. */
    if (vita_dec_ogg.seek_ms(h, 5000)) {
        out = big;
        f2 = vita_dec_ogg.read(h, out, 4800, 100);
        r2 = rms_of(out, f2);
        printf("seek 5000 ms: OK, %ld frames, rms %.1f\n", f2, r2);
    } else {
        printf("seek 5000 ms: refused\n");
    }

    vita_dec_ogg.rewind(h);
    f2 = vita_dec_ogg.read(h, big, 4800, 100);
    printf("rewind: %ld frames, rms %.1f\n", f2, rms_of(big, f2));

    /* Drain to the end: must terminate and report a positive total. */
    {
        long total = 0, n;
        while ((n = vita_dec_ogg.read(h, big, 4700, 100)) > 0) {
            total += n;
        }
        printf("drained: %ld more frames, reads=%ld\n", total, mem.reads);
    }

    vita_dec_ogg.close(h);
    free(data);
    return 0;
}
