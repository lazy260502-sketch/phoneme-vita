/*
 * vita_membw.c - framebuffer write-bandwidth probe (v03.45).
 * See vita_membw.h for why this exists and what the numbers mean.
 *
 * Implementation notes
 * --------------------
 * The probe is deliberately written with plain C, no intrinsics, because the
 * whole point is to see what the COMPILER generates for each shape of loop
 * under the same flags the real blit is built with (-O2, -mfpu=vfpv3, no
 * NEON). A hand-written assembly version would measure something we are not
 * shipping.
 *
 * Every variant ends by reading one element back into a volatile sink, so
 * neither the fills nor the copies can be optimised away, and the reported
 * checksum makes an "all variants look the same" result obviously wrong
 * rather than subtly wrong.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <psp2/io/fcntl.h>
#include <psp2/kernel/processmgr.h>

#include "vita_membw.h"
#include "vita_crumb.h"

/* 128 KB per pass: large enough to swamp call overhead and the wait for the
 * first line, small enough that three repetitions of six variants cost well
 * under 100 ms even at 50 MB/s. */
#define PROBE_BYTES   (128 * 1024)
#define PROBE_REPS    3

/* Read back so the compiler cannot drop the loop. Printing the final value
 * also proves the measurements were not all no-ops. */
static volatile uint32_t g_sink;

/* ---- the six variants --------------------------------------------- */
/* All take (dst, src, n) where n is a count of 32-bit words. The fills
 * ignore src, the copies ignore v - a single signature keeps the timing
 * helper simple. */

static void v_fill32(uint32_t *d, const uint32_t *s, int n, uint32_t v) {
    int i;
    (void)s;
    for (i = 0; i < n; i++) {
        d[i] = v;
    }
    g_sink = d[n - 1];
}

/* 64-bit stores: half the bus transactions of v_fill32 if uncached writes
 * are transaction-bound rather than bandwidth-bound. */
static void v_fill64(uint32_t *d, const uint32_t *s, int n, uint32_t v) {
    uint64_t v64 = ((uint64_t)v << 32) | (uint64_t)v;
    int i;
    (void)s;
    for (i = 0; i + 1 < n; i += 2) {
        *(uint64_t *)(void *)&d[i] = v64;
    }
    g_sink = d[n - 1];
}

/* Library copy: newlib's memcpy issues LDM/STM bursts where it can. This is
 * the "write into a cached staging buffer, then blit it out in one go"
 * design, measured directly. */
static void v_copy_lib(uint32_t *d, const uint32_t *s, int n, uint32_t v) {
    (void)v;
    memcpy(d, s, (size_t)n * sizeof(uint32_t));
    g_sink = d[0];
}

/* Hand-rolled 4-word burst. If this beats v_copy_lib, newlib is not using
 * the widest stores available and a staging buffer is worth it. */
static void v_copy_loop4(uint32_t *d, const uint32_t *s, int n, uint32_t v) {
    int i;
    (void)v;
    for (i = 0; i + 3 < n; i += 4) {
        d[i]     = s[i];
        d[i + 1] = s[i + 1];
        d[i + 2] = s[i + 2];
        d[i + 3] = s[i + 3];
    }
    g_sink = d[0];
}

/* The real hot loop: RGB565 -> 0xAABBGGRR, one pixel at a time, exactly what
 * blit_scaled() does (vita_display.c:447). n counts DESTINATION words, so n
 * source pixels are read as 16-bit each. */
static void v_conv565(uint32_t *d, const uint32_t *s, int n, uint32_t v) {
    const uint16_t *src = (const uint16_t *)(const void *)s;
    int i;
    (void)v;
    for (i = 0; i < n; i++) {
        uint32_t p  = src[i];
        uint32_t r5 = (p >> 11) & 0x1F;
        uint32_t g6 = (p >> 5) & 0x3F;
        uint32_t b5 = p & 0x1F;
        uint32_t r  = (r5 << 3) | (r5 >> 2);
        uint32_t g  = (g6 << 2) | (g6 >> 4);
        uint32_t b  = (b5 << 3) | (b5 >> 2);
        d[i] = 0xFF000000u | (b << 16) | (g << 8) | r;
    }
    g_sink = d[n - 1];
}

/* v03.59: conv565 with a 64-bit store, two pixels at a time.
 *
 * log153 reported "@32b=5.24ms @64b=1.45ms", which looked like a 3.6x win for
 * a 64-bit store - but the two numbers came from DIFFERENT loops (@32b was the
 * conversion, @64b was a plain fill). Same work, or the comparison is a lie:
 * this variant converts exactly like v_conv565 and only the store width
 * changes, so the two frame times are directly comparable. blit_scaled()'s
 * inner loop already produces two adjacent destination words per step, so this
 * is the honest candidate optimisation. */
static void v_conv565_64(uint32_t *d, const uint32_t *s, int n, uint32_t v) {
    const uint16_t *src = (const uint16_t *)(const void *)s;
    int i;
    (void)v;
    for (i = 0; i + 1 < n; i += 2) {
        uint32_t p0 = src[i];
        uint32_t p1 = src[i + 1];
        uint32_t r0 = (p0 >> 11) & 0x1F, g0 = (p0 >> 5) & 0x3F, b0 = p0 & 0x1F;
        uint32_t r1 = (p1 >> 11) & 0x1F, g1 = (p1 >> 5) & 0x3F, b1 = p1 & 0x1F;
        uint32_t o0 = 0xFF000000u | (((b0 << 3) | (b0 >> 2)) << 16) |
                      (((g0 << 2) | (g0 >> 4)) << 8) | ((r0 << 3) | (r0 >> 2));
        uint32_t o1 = 0xFF000000u | (((b1 << 3) | (b1 >> 2)) << 16) |
                      (((g1 << 2) | (g1 >> 4)) << 8) | ((r1 << 3) | (r1 >> 2));
        *(uint64_t *)(void *)&d[i] = ((uint64_t)o1 << 32) | (uint64_t)o0;
    }
    g_sink = d[n - 1];
}

/* ---- timing ------------------------------------------------------- */

typedef void (*membw_fn)(uint32_t *d, const uint32_t *s, int n, uint32_t v);

/* Run `reps` times and keep the FASTEST. The minimum is the right statistic
 * here: every perturbation (an interrupt, a competing thread, a cache miss
 * on the instruction stream) can only make a run slower, so the floor is the
 * machine's actual capability. */
static unsigned int time_min_us(membw_fn fn, uint32_t *dst,
                               const uint32_t *src, int n, uint32_t v,
                               int reps) {
    unsigned int best = 0;
    int r;
    for (r = 0; r < reps; r++) {
        unsigned long long t0 = sceKernelGetProcessTimeWide();
        fn(dst, src, n, v);
        {
            unsigned long long dt = sceKernelGetProcessTimeWide() - t0;
            unsigned int us = (unsigned int)dt;
            if (us == 0) {
                us = 1;    /* never divide by zero on a very fast path */
            }
            if (best == 0 || us < best) {
                best = us;
            }
        }
    }
    return best;
}

/* bytes/us == MB/s exactly (1e6 bytes / 1e6 us). Tenths to avoid floats. */
static unsigned int mbps_x10(int bytes, unsigned int us) {
    if (us == 0) {
        return 0;
    }
    return (unsigned int)(((unsigned long long)bytes * 10ull) / us);
}

static void fmt_mbps(char *out, size_t cap, int bytes, unsigned int us) {
    unsigned int x10 = mbps_x10(bytes, us);
    snprintf(out, cap, "%u.%u", x10 / 10u, x10 % 10u);
}

/* us needed to write `frame_bytes` at the measured rate */
static unsigned int frame_us(int frame_bytes, int bytes, unsigned int us) {
    if (bytes == 0 || us == 0) {
        return 0;
    }
    return (unsigned int)(((unsigned long long)frame_bytes * us) / (unsigned)bytes);
}

/* ---- config ------------------------------------------------------- */

/* ux0:/data/J2ME00001/membw.cfg : "on=0" skips the probe. Default ON,
 * because one 60 ms measurement per process buys a number that answers the
 * "is the blit ALU-bound or memory-bound" question for every later round. */
static int membw_enabled(void) {
    int fd = sceIoOpen("ux0:/data/J2ME00001/membw.cfg", SCE_O_RDONLY, 0);
    char buf[64];
    int n;
    int quit = 0;
    if (fd < 0) {
        return 1;
    }
    n = (int)sceIoRead(fd, buf, sizeof(buf) - 1);
    sceIoClose(fd);
    if (n <= 0) {
        return 1;
    }
    buf[n] = '\0';
    {
        const char *p = strstr(buf, "on=");
        if (p != NULL) {
            p += 3;
            while (*p == ' ') p++;
            if (*p == '0' || *p == 'n' || *p == 'N') {
                quit = 1;
            }
        }
    }
    return !quit;
}

/* ---- results ------------------------------------------------------ */

static unsigned int r_fill32, r_fill64, r_copy_c, r_copy_u, r_conv_c, r_conv_u;
static unsigned int r_frame32, r_frame64;
static int g_ran = 0;

void vita_membw_get(unsigned int *fill32, unsigned int *fill64,
                    unsigned int *copy_lib_cached,
                    unsigned int *copy_lib_uncached,
                    unsigned int *conv_cached, unsigned int *conv_uncached,
                    unsigned int *frame_us_32b, unsigned int *frame_us_64b) {
    if (fill32)            *fill32 = r_fill32;
    if (fill64)            *fill64 = r_fill64;
    if (copy_lib_cached)   *copy_lib_cached = r_copy_c;
    if (copy_lib_uncached) *copy_lib_uncached = r_copy_u;
    if (conv_cached)       *conv_cached = r_conv_c;
    if (conv_uncached)     *conv_uncached = r_conv_u;
    if (frame_us_32b)      *frame_us_32b = r_frame32;
    if (frame_us_64b)      *frame_us_64b = r_frame64;
}

static void emit(const char *line) {
    fprintf(stderr, "%s\n", line);
    crumb_printf("%s", line);
}

int vita_membw_probe(uint32_t *fb, int bytes,
                     const uint16_t *src565, int src_pixels,
                     int frame_bytes) {
    uint32_t *cached = NULL;
    uint32_t *cached2 = NULL;
    int words;
    unsigned int us_fill32_u, us_fill64_u, us_copy_u, us_conv_u, us_conv64_u;
    unsigned int us_copy4_u, us_fill32_c, us_copy_c, us_conv_c, us_conv64_c;
    char line[340];
    char b1[16], b2[16], b4[16], b5[16], b6[16], b7[16], b8[16];

    if (g_ran) {
        return 0;
    }
    if (fb == NULL || src565 == NULL || bytes < 4096) {
        return 0;
    }
    if (!membw_enabled()) {
        emit("[MBW] disabled by membw.cfg");
        g_ran = 1;
        return 0;
    }
    g_ran = 1;

    if (bytes > PROBE_BYTES) {
        bytes = PROBE_BYTES;
    }
    words = bytes / 4;
    /* the conversion variant reads words*2 bytes of RGB565 source */
    if (src_pixels > 0 && words > src_pixels) {
        words = src_pixels;
        bytes = words * 4;
    }
    /* even word count keeps the 64-bit variant from leaving a tail */
    words &= ~1;
    bytes = words * 4;
    if (words < 1024) {
        emit("[MBW] source too small, skipped");
        return 0;
    }

    cached = (uint32_t *)malloc((size_t)bytes);
    /* v03.59: a SECOND cached buffer. v03.45 passed `cached` as both source
     * and destination of the library memcpy, which is undefined behaviour
     * and got optimised to nothing - log153 then reported
     * "memcpy cached=131072.0 MB/s", i.e. 128 KB in under 1 us. That number
     * was garbage and it was the only "memcpy on this target" evidence we
     * had. Two distinct buffers make the comparison real. */
    cached2 = (uint32_t *)malloc((size_t)bytes);
    if (cached == NULL || cached2 == NULL) {
        free(cached);
        free(cached2);
        emit("[MBW] no cached staging buffer, skipped");
        return 0;
    }
    /* Fill it with a non-trivial pattern so a copy that silently does
     * nothing cannot look fast. */
    {
        int i;
        for (i = 0; i < words; i++) {
            cached[i] = 0x01020304u + (uint32_t)i;
            cached2[i] = 0u;
        }
    }

    /* --- fills: pure write cost, no read traffic --- */
    us_fill32_c = time_min_us(v_fill32, cached, NULL, words, 0xA5A5A5A5u,
                              PROBE_REPS);
    us_fill32_u = time_min_us(v_fill32, fb, NULL, words, 0xA5A5A5A5u,
                              PROBE_REPS);
    us_fill64_u = time_min_us(v_fill64, fb, NULL, words, 0xA5A5A5A5u,
                              PROBE_REPS);

    /* --- copies: read cached, write cached (distinct dst!) vs uncached --- */
    us_copy_c = time_min_us(v_copy_lib, cached2, cached, words, 0,
                            PROBE_REPS);
    us_copy_u = time_min_us(v_copy_lib, fb, cached, words, 0, PROBE_REPS);
    /* ... and the same copy hand-unrolled 4 words at a time, to see whether
     * newlib's memcpy is already using the widest stores on this target. */
    us_copy4_u = time_min_us(v_copy_loop4, fb, cached, words, 0, PROBE_REPS);

    /* --- the real hot loop, both destinations and both store widths --- */
    us_conv_c = time_min_us(v_conv565, cached2, (const uint32_t *)(const void *)
                            src565, words, 0, PROBE_REPS);
    us_conv_u = time_min_us(v_conv565, fb, (const uint32_t *)(const void *)
                            src565, words, 0, PROBE_REPS);
    us_conv64_c = time_min_us(v_conv565_64, cached2,
                              (const uint32_t *)(const void *)src565, words, 0,
                              PROBE_REPS);
    us_conv64_u = time_min_us(v_conv565_64, fb,
                              (const uint32_t *)(const void *)src565, words, 0,
                              PROBE_REPS);

    r_fill32 = mbps_x10(bytes, us_fill32_u);
    r_fill64 = mbps_x10(bytes, us_fill64_u);
    r_copy_c = mbps_x10(bytes, us_copy_c);
    r_copy_u = mbps_x10(bytes, us_copy_u);
    r_conv_c = mbps_x10(bytes, us_conv_c);
    r_conv_u = mbps_x10(bytes, us_conv_u);
    r_frame32 = frame_us(frame_bytes, bytes, us_conv_u);
    /* v03.59: the 64-bit figure now comes from the SAME loop (conversion
     * included) with only the store width changed, so @32b -> @64b is a real
     * A/B. log153's 5.24 vs 1.45 ms compared a conversion against a fill. */
    r_frame64 = frame_us(frame_bytes, bytes, us_conv64_u);

    fmt_mbps(b1, sizeof(b1), bytes, us_fill32_c);
    fmt_mbps(b2, sizeof(b2), bytes, us_fill32_u);
    fmt_mbps(b4, sizeof(b4), bytes, us_fill64_u);
    fmt_mbps(b5, sizeof(b5), bytes, us_copy_c);
    fmt_mbps(b6, sizeof(b6), bytes, us_copy_u);
    fmt_mbps(b7, sizeof(b7), bytes, us_copy4_u);
    snprintf(line, sizeof(line),
             "[MBW] n=%dKB reps=%d | fill32 cached=%s uncached=%s MB/s | "
             "fill64 uncached=%s | memcpy cached=%s uncached=%s MB/s | "
             "copy4 uncached=%s MB/s",
             bytes / 1024, PROBE_REPS, b1, b2, b4, b5, b6, b7);
    emit(line);

    fmt_mbps(b1, sizeof(b1), bytes, us_conv_c);
    fmt_mbps(b2, sizeof(b2), bytes, us_conv_u);
    fmt_mbps(b7, sizeof(b7), bytes, us_conv64_u);
    fmt_mbps(b8, sizeof(b8), bytes, us_conv64_c);
    snprintf(line, sizeof(line),
             "[MBW] rgb565->abgr 32b cached=%s uncached=%s MB/s "
             "| 64b cached=%s uncached=%s MB/s (us %u / %u / %u)",
             b1, b2, b8, b7, us_conv_c, us_conv_u, us_conv64_u);
    emit(line);

    /* The verdict: does the measured uncached rate explain the observed
     * `prepare` time, and what would a 64-bit store buy for a real frame?
     * v03.59: frame_bytes is what blit_scaled() actually writes in a steady
     * frame (the game rectangle). The letterbox is NOT written every frame -
     * clear_letterbox() only runs on the two frames after an overlay ends -
     * so quoting "game rect + bars" as the per-frame figure would inflate
     * the estimate for every normal frame. The bars are reported separately
     * because they DO dominate any frame that clears them. */
    snprintf(line, sizeof(line),
             "[MBW] VERDICT frame=%dKB @32b=%u.%ums @64b=%u.%ums "
             "| 64b gain=%u%% | uncached/cached=%u%%",
             frame_bytes / 1024,
             r_frame32 / 1000u, (r_frame32 % 1000u) / 10u,
             r_frame64 / 1000u, (r_frame64 % 1000u) / 10u,
             (r_frame64 > 0 && r_frame32 > r_frame64)
                 ? ((r_frame32 - r_frame64) * 100u / r_frame32) : 0u,
             us_conv_c ? (us_conv_u * 100u / us_conv_c) : 0u);
    emit(line);

    /* The single line a reader should look at: if @32b lands near the
     * perf.log `prepare` figure, the blit is memory-bound and the fix is a
     * wider store (or a cached staging buffer + one burst copy), not NEON. */
    if (r_frame32 > 0) {
        unsigned int implied_x10 =
            (unsigned int)(((unsigned long long)frame_bytes * 10ull) /
                           r_frame32);
        snprintf(line, sizeof(line),
                 "[MBW] hint: @32b frame time = %u.%u ms (implied %u.%u MB/s); "
                 "perf.log prepare=17.1 ms in log140",
                 r_frame32 / 1000u, (r_frame32 % 1000u) / 10u,
                 implied_x10 / 10u, implied_x10 % 10u);
        emit(line);
    }

    free(cached);
    free(cached2);
    return 1;
}
