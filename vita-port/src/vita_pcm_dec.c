/*
 * vita_pcm_dec.c - shared sliding-window byte reader for the audio
 *                  decoders (see vita_pcm_dec.h)
 *
 * Why a window instead of "the whole file in RAM": a J2ME BGM is
 * routinely 2-5 MB and used to be pinned in the PCSL heap for as long as
 * the player lived, on top of the 32 MB Java heap. Keeping only a window
 * caps the cost at (window + one decoded frame) per player, whatever the
 * file size. The audio layer additionally spills big MP3s to a temp file
 * (see mp3_src_read in vita_audio_javacall.c), so both RAM and the
 * download buffer stay bounded.
 */
#include <string.h>

#include "vita_pcm_dec.h"

void vita_src_init(vita_src *s, vita_dec_read_cb rd, void *ctx,
                   long total, uint8_t *buf, long cap) {
    if (s == NULL) return;
    memset(s, 0, sizeof(*s));
    s->rd = rd;
    s->ctx = ctx;
    s->total = (total > 0) ? total : 0;
    s->buf = buf;
    s->cap = cap;
}

void vita_src_invalidate(vita_src *s) {
    if (s == NULL) return;
    s->len = 0;
    s->off = 0;
}

const uint8_t *vita_src_view(vita_src *s, long pos, long need, long *avail) {
    long end;

    if (avail != NULL) *avail = 0;
    if (s == NULL || s->buf == NULL || s->cap <= 0) return NULL;
    if (pos < 0 || pos >= s->total) return NULL;
    if (need > s->cap) need = s->cap;

    end = s->off + s->len;

    /* Refill until `need` bytes are available ahead of pos, the source
     * is exhausted, or the source stops answering. */
    while (end - pos < need && end < s->total && !s->stuck) {
        long want;

        if (pos > s->off) {
            /* slide the live tail to the front of the window */
            long keep = end - pos;
            if (keep < 0) keep = 0;
            memmove(s->buf, s->buf + (pos - s->off), (size_t)keep);
            s->off = pos;
            s->len = keep;
            end = s->off + s->len;
        }

        want = s->cap - s->len;
        if (want > s->cap / 2) want = s->cap / 2;   /* keep refills small */
        if (s->off + s->len + want > s->total)
            want = s->total - (s->off + s->len);
        if (want <= 0) break;

        {
            long got = s->rd(s->ctx, s->off + s->len,
                             s->buf + s->len, want);
            if (got <= 0) {
                s->stuck = 1;
                break;
            }
            s->len += got;
            end = s->off + s->len;
        }
    }

    if (pos < s->off) return NULL;          /* window moved past pos */
    if (pos > end) return NULL;             /* gap: cannot serve pos  */
    if (avail != NULL) *avail = end - pos;
    return s->buf + (pos - s->off);
}

/* --- memory-backed source ------------------------------------------- */

void vita_mem_src_init(vita_mem_src *m, const uint8_t *data, long len) {
    if (m == NULL) return;
    m->data = data;
    m->len = (len > 0) ? len : 0;
}

long vita_dec_read_mem(void *ctx, long off, uint8_t *dst, long n) {
    vita_mem_src *m = (vita_mem_src *)ctx;
    long left;

    if (m == NULL || dst == NULL || n <= 0) return 0;
    if (off < 0 || off >= m->len) return 0;
    left = m->len - off;
    if (n > left) n = left;
    memcpy(dst, m->data + off, (size_t)n);
    return n;
}
