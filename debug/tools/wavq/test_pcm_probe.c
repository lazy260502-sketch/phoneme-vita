/* Host test for vita-port/src/vita_pcm_probe.h (the SHIPPED header). Simulates a render thread feeding the port and checks that
 * the interval histogram separates a healthy stream from one with late chunks (underruns), that the capture keeps exactly the
 * first VPP_CAPTURE_CHUNKS chunks verbatim, and that vpp_reset_run stops the gap between two tunes from being counted.
 *   /usr/bin/gcc -B/usr/bin -O1 -Wall -I../../../vita-port/src test_pcm_probe.c -o /tmp/tpp && /tmp/tpp   */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "vita_pcm_probe.h"

static int fails;
#define CK(c, ...) do { if (!(c)) { fails++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

static vita_pcm_probe P;   /* ~1.2 MB: keep it static */

int main(void) {
    int16_t chunk[VPP_CHUNK_FRAMES * 2];
    unsigned long long t = 1000000ULL;
    int i, k;

    /* healthy: 200 submissions exactly 10667 us apart */
    memset(&P, 0, sizeof P);
    vpp_reset_run(&P);
    for (i = 0; i < 200; i++) {
        for (k = 0; k < VPP_CHUNK_FRAMES * 2; k++) chunk[k] = (int16_t)(i * 7 + k);
        vpp_before_submit(&P, chunk, t);
        vpp_after_submit(&P, 9000);
        t += 10667;
    }
    CK(P.n_chunks == 200, "n_chunks %llu", P.n_chunks);
    CK(P.n_intervals == 199, "n_intervals %llu", P.n_intervals);
    CK(P.hist[vpp_bucket(10667)] == 199, "healthy intervals not all in the 10.5-10.9 ms bucket");
    CK(P.n_blocked_short == 0, "healthy stream reported quick returns: %llu", P.n_blocked_short);
    printf("healthy : buckets");
    for (i = 0; i < VPP_HIST_BUCKETS; i++) printf(" %llu", P.hist[i]);
    printf("  max %llu us\n", P.max_interval_us);

    /* capture is verbatim and bounded */
    CK(P.cap_chunks == 200, "cap_chunks %d", P.cap_chunks);
    for (i = 0; i < 200; i++) {
        int ok = 1;
        for (k = 0; k < VPP_CHUNK_FRAMES * 2; k++) if (P.cap[i][k] != (int16_t)(i * 7 + k)) { ok = 0; break; }
        CK(ok, "captured chunk %d differs", i);
    }
    CK(!P.cap_ready, "cap_ready set before the buffer is full");

    /* starved: every 10th chunk arrives 40 ms late, and the call after a late chunk returns at once */
    memset(&P, 0, sizeof P);
    vpp_reset_run(&P);
    t = 5000000ULL;
    for (i = 0; i < 100; i++) {
        vpp_before_submit(&P, chunk, t);
        vpp_after_submit(&P, (i % 10 == 9) ? 300 : 9000);
        t += (i % 10 == 8) ? 40000 : 10667;
    }
    CK(P.hist[vpp_bucket(40000)] == 10, "late chunks counted %llu (want 10)", P.hist[vpp_bucket(40000)]);
    CK(P.n_blocked_short == 10, "quick returns %llu (want 10)", P.n_blocked_short);
    CK(P.max_interval_us == 40000, "max interval %llu", P.max_interval_us);
    printf("starved : buckets");
    for (i = 0; i < VPP_HIST_BUCKETS; i++) printf(" %llu", P.hist[i]);
    printf("  max %llu us, quick returns %llu\n", P.max_interval_us, P.n_blocked_short);

    /* two tunes: the long gap between them must not appear as an interval once vpp_reset_run is called */
    memset(&P, 0, sizeof P);
    vpp_reset_run(&P);
    t = 1000;
    for (i = 0; i < 20; i++) { vpp_before_submit(&P, chunk, t); t += 10667; }
    vpp_reset_run(&P);
    t += 3000000ULL;                                   /* 3 s between tunes */
    for (i = 0; i < 20; i++) { vpp_before_submit(&P, chunk, t); t += 10667; }
    CK(P.n_intervals == 38, "intervals across two tunes %llu (want 38)", P.n_intervals);
    CK(P.hist[VPP_HIST_BUCKETS - 1] == 0, "the inter-tune gap leaked into the >=50 ms bucket");

    /* capture stops at VPP_CAPTURE_CHUNKS and flags ready */
    memset(&P, 0, sizeof P);
    vpp_reset_run(&P);
    t = 1000;
    for (i = 0; i < VPP_CAPTURE_CHUNKS + 50; i++) { chunk[0] = (int16_t)i; vpp_before_submit(&P, chunk, t); t += 10667; }
    CK(P.cap_chunks == VPP_CAPTURE_CHUNKS, "cap_chunks %d", P.cap_chunks);
    CK(P.cap_ready, "cap_ready not set when full");
    CK(P.cap[VPP_CAPTURE_CHUNKS - 1][0] == (int16_t)(VPP_CAPTURE_CHUNKS - 1), "last captured chunk is not chunk %d", VPP_CAPTURE_CHUNKS - 1);

    /* bucket edges */
    CK(vpp_bucket(8999) == 0 && vpp_bucket(9000) == 1, "bucket edge 9000");
    CK(vpp_bucket(10499) == 2 && vpp_bucket(10500) == 3, "bucket edge 10500");
    CK(vpp_bucket(49999) == 8 && vpp_bucket(50000) == 9 && vpp_bucket(9999999) == 9, "bucket edge 50000");

    printf("\n%s (%d failure%s)\n", fails ? "FAILED" : "ALL PASS", fails, fails == 1 ? "" : "s");
    return fails != 0;
}
