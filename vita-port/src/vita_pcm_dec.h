/*
 * vita_pcm_dec.h - common interface for the compressed-audio decoders
 *                  (MP3 / AMR-NB / AAC) used by vita_audio_javacall.c
 *
 * v02.88: MMAPI hands us one compressed stream and asks for 48 kHz
 * stereo PCM in fixed chunks. MP3, AMR-NB and AAC all fit that pattern
 * (sequential decode, occasional seek), so they share one vtable instead
 * of three near-identical sets of player-thread plumbing. Adding a
 * fourth format then costs one file, not another 120-line copy of the
 * render loop.
 */
#ifndef VITA_PCM_DEC_H
#define VITA_PCM_DEC_H

#include <stdint.h>

/* Output format every decoder produces. Matches OUT_SAMPLE_RATE in
 * vita_audio_javacall.c and the SceAudioOut port. */
#define VITA_DEC_OUT_RATE 48000

/* Random-access byte source. `off`/`n` are absolute file offsets.
 * Returns the number of bytes actually copied; a value < n means EOF
 * (or an I/O error - the decoder treats both as end of stream). */
typedef long (*vita_dec_read_cb)(void *ctx, long off, uint8_t *dst, long n);

typedef struct vita_pcm_dec {
    /* Returns NULL if the stream is not this format, or if the decoder
     * cannot run on this device. `total_len` is the exact byte count. */
    void *(*open)(vita_dec_read_cb rd, void *ctx, long total_len);
    void  (*close)(void *h);
    int   (*channels)(void *h);          /* source channel count  */
    int   (*rate)(void *h);              /* source sample rate    */
    long  (*duration_ms)(void *h);
    void  (*rewind)(void *h);
    /* Seek to `ms`. Returns 1 on success. May be NULL/refuse. */
    int   (*seek_ms)(void *h, long ms);
    /* Fill up to `out_frames` stereo frames (out_frames*2 int16) at
     * 48 kHz, scaled by `volume` (0..100). Returns frames written;
     * 0 means end of stream. A short return means "stream ended". */
    long  (*read)(void *h, int16_t *out, long out_frames, long volume);
} vita_pcm_dec;

/* --- shared sliding-window byte reader -------------------------------
 * The whole compressed stream is NOT kept in RAM: each decoder pulls it
 * through a small window (the "ring buffer" for large BGMs). Window size
 * is per decoder; MP3 needs >= one frame (1441 B) plus slack.
 */
typedef struct {
    vita_dec_read_cb rd;
    void  *ctx;
    long   total;          /* total bytes in the stream           */
    uint8_t *buf;          /* window storage (owned by the caller) */
    long   cap;
    long   off;            /* absolute offset of buf[0]           */
    long   len;            /* valid bytes in buf                  */
    int    stuck;          /* source returned 0: stop retrying    */
} vita_src;

void vita_src_init(vita_src *s, vita_dec_read_cb rd, void *ctx,
                   long total, uint8_t *buf, long cap);

/* Position the window so that the bytes at `pos` are readable. On
 * success returns a pointer valid for `*avail` bytes (avail may be less
 * than `need` near EOF). Returns NULL when nothing can be read there
 * (past EOF, or the source stopped delivering). */
const uint8_t *vita_src_view(vita_src *s, long pos, long need, long *avail);

/* Discard the window contents; the next view() re-reads from the source.
 * Needed because a seek moves the cursor backwards. */
void vita_src_invalidate(vita_src *s);

/* --- helper source: a plain memory buffer ---------------------------- */
typedef struct {
    const uint8_t *data;
    long           len;
} vita_mem_src;

void vita_mem_src_init(vita_mem_src *m, const uint8_t *data, long len);
long vita_dec_read_mem(void *ctx, long off, uint8_t *dst, long n);

/* The decoders. Each is a static vtable + open/close in its own file. */
extern const vita_pcm_dec vita_dec_mp3;
extern const vita_pcm_dec vita_dec_amr;
extern const vita_pcm_dec vita_dec_aac;
/* v04.45: Ogg Vorbis (stb_vorbis, vita_ogg.c). */
extern const vita_pcm_dec vita_dec_ogg;

#endif /* VITA_PCM_DEC_H */
