/*
 * vita_amr.c - AMR-NB (adaptive multi-rate narrowband) decoding for MMAPI
 *
 * AMR is the other audio format J2ME games actually ship: voice clips,
 * NPC chatter, "you win" jingles and low-bitrate BGM in Chinese/Japanese
 * titles. Before this file Manager.createPlayer() threw MediaException
 * for audio/amr, so those games ran silently.
 *
 * Decoder: the 3GPP floating-point reference codec (TS 26.104), vendored
 * under src/amr3gpp/ because there is no AMR support anywhere in vitaSDK
 * (sceAudiodec covers AT9/MP3/AAC/CELP only). Floating point is fine on
 * the Vita's VFP; AMR-NB decodes 50 frames of 20 ms per second of audio,
 * i.e. a very light load compared with the MIDI synthesiser.
 *
 * Container: the "AMR storage format" (#!AMR\n, one 20 ms frame per
 * ToC+payload block). That is what J2ME games embed and what the
 * opencore/MMS decoders expect, and it is also the format the reference
 * decoder's MMS interface (DecoderMMS) consumes - our frames are passed
 * through untouched, ToC byte included.
 *
 * Three local patches were applied to the vendored 3GPP sources and are
 * documented in PROJECT_MEMORY.md (v02.88):
 *   - malloc -> calloc for the decoder state (the reference code reads
 *     parts of the state that Decoder_amr_reset() does not initialise);
 *   - one overlapping memcpy -> memmove in Decoder_amr();
 *   - a range clamp in Lsf_lsp() so a corrupted frame cannot index far
 *     outside cos_table (that used to segfault the whole emulator).
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "amr3gpp/interf_dec.h"

#include "vita_pcm_dec.h"

#define OUT_RATE      VITA_DEC_OUT_RATE
#define AMR_RATE      8000
#define AMR_FRAME_SZ  160            /* samples per 20 ms frame @ 8 kHz */
#define AMR_UPSAMPLE  (OUT_RATE / AMR_RATE)   /* 6 */
#define AMR_WIN_SIZE  4096

/* Bytes per frame in the storage format, ToC byte included. Index = the
 * 4-bit frame type of the ToC. 0 means "reserved, resynchronise". */
static const int8_t amr_frame_bytes[16] = {
    13, 14, 16, 18, 20, 21, 27, 32,   /* 4.75 .. 12.2 kbps */
    6,                                /* 8: SID (comfort noise) */
    0, 0, 0, 0, 0, 0,                 /* 9..14: reserved */
    1                                 /* 15: no data */
};

typedef struct VitaAmr VitaAmr;

struct VitaAmr {
    vita_src src;

    long   audio_start;
    long   abs_pos;        /* absolute byte cursor                */
    void  *dec;            /* Decoder_Interface() state           */
    int16_t pcm[AMR_FRAME_SZ];
    int    sub;            /* 0..(AMR_FRAME_SZ*6-1) in current frame */
    int    have_frame;
    long   frame_count;
    long   duration_ms;
    int    eof;
};

static int amr_next_frame(VitaAmr *m) {
    for (;;) {
        const uint8_t *p;
        long avail;
        int mode, size;

        if (m->abs_pos >= m->src.total) return 0;
        p = vita_src_view(&m->src, m->abs_pos, 32, &avail);
        if (p == NULL || avail <= 0) return 0;

        mode = (p[0] >> 3) & 0x0F;
        size = amr_frame_bytes[mode];
        if (size <= 0) {
            /* Reserved frame type: not a frame boundary after all.
             * Step forward and try again rather than aborting the song. */
            m->abs_pos++;
            continue;
        }
        if (avail < size) {
            /* Truncated tail: pad with a "no data" frame so the last
             * partial block still produces silence instead of a click. */
            uint8_t tmp[32];
            memset(tmp, 0, sizeof(tmp));
            memcpy(tmp, p, (size_t)avail);
            tmp[0] = 0x7C;   /* frame type 15 */
            Decoder_Interface_Decode(m->dec, tmp, m->pcm, 0);
            m->abs_pos += avail;
            m->have_frame = 1;
            return 1;
        }

        Decoder_Interface_Decode(m->dec, (unsigned char *)p, m->pcm, 0);
        m->abs_pos += size;
        m->have_frame = 1;
        return 1;
    }
}

/* The reference decoder's own Decoder_Interface_reset() only clears the
 * homing-frame bookkeeping, not the synthesis history, so it cannot be
 * used here. Re-creating the state is ~10 KB of calloc and only happens on
 * rewind/seek, so that is the honest choice. */
static void amr_hard_reset(VitaAmr *m) {
    if (m->dec != NULL) {
        Decoder_Interface_exit(m->dec);
        m->dec = NULL;
    }
    m->dec = Decoder_Interface_init();
    m->abs_pos = m->audio_start;
    m->sub = 0;
    m->have_frame = 0;
    m->eof = 0;
    m->src.stuck = 0;
    vita_src_invalidate(&m->src);
}

static void *amr_open(vita_dec_read_cb rd, void *ctx, long total_len) {
    VitaAmr *m;
    const uint8_t *p;
    long avail;
    long pos, frames = 0;

    if (rd == NULL || total_len <= 6) return NULL;

    m = (VitaAmr *)calloc(1, sizeof(*m));
    if (m == NULL) return NULL;
    m->src.buf = (uint8_t *)malloc(AMR_WIN_SIZE);
    if (m->src.buf == NULL) {
        free(m);
        return NULL;
    }
    vita_src_init(&m->src, rd, ctx, total_len, m->src.buf, AMR_WIN_SIZE);

    /* "#!AMR\n" - wideband ("#!AMR-WB\n") is a different codec and is
     * rejected here so the caller reports "no data" instead of noise. */
    m->audio_start = -1;
    p = vita_src_view(&m->src, 0, 6, &avail);
    if (p != NULL && avail >= 6 && memcmp(p, "#!AMR\n", 6) == 0) {
        m->audio_start = 6;
    } else if (p != NULL && avail >= 9 && memcmp(p, "#!AMR-WB\n", 9) == 0) {
        m->audio_start = -1;          /* unsupported */
    }
    if (m->audio_start < 0) {
        free(m->src.buf);
        free(m);
        return NULL;
    }

    m->dec = Decoder_Interface_init();
    if (m->dec == NULL) {
        free(m->src.buf);
        free(m);
        return NULL;
    }

    /* Count frames for the duration (getDuration() is queried by games
     * before playback; a wrong value is visible in progress bars). */
    pos = m->audio_start;
    while (pos < total_len) {
        const uint8_t *q;
        long av;
        int mode, size;

        q = vita_src_view(&m->src, pos, 32, &av);
        if (q == NULL || av <= 0) break;
        mode = (q[0] >> 3) & 0x0F;
        size = amr_frame_bytes[mode];
        if (size <= 0) { pos++; continue; }
        pos += size;
        frames++;
    }
    m->frame_count = frames;
    m->duration_ms = frames * 20;
    if (frames == 0) {
        Decoder_Interface_exit(m->dec);
        free(m->src.buf);
        free(m);
        return NULL;
    }

    amr_hard_reset(m);
    return m;
}

static void amr_close(void *h) {
    VitaAmr *m = (VitaAmr *)h;
    if (m == NULL) return;
    if (m->dec != NULL) Decoder_Interface_exit(m->dec);
    free(m->src.buf);
    free(m);
}

static int amr_channels(void *h) {
    (void)h;
    return 1;
}

static int amr_rate(void *h) {
    (void)h;
    return AMR_RATE;
}

static long amr_duration_ms(void *h) {
    VitaAmr *m = (VitaAmr *)h;
    return (m != NULL) ? m->duration_ms : 0;
}

static void amr_rewind(void *h) {
    VitaAmr *m = (VitaAmr *)h;
    if (m != NULL) amr_hard_reset(m);
}

static int amr_seek_ms(void *h, long ms) {
    VitaAmr *m = (VitaAmr *)h;
    long want, i;

    if (m == NULL || ms < 0) return 0;
    /* AMR has a fixed 20 ms frame grid, so the target frame index is
     * exact and the seek lands on the requested frame - no drift. */
    want = ms / 20;
    amr_hard_reset(m);
    for (i = 0; i < want; i++) {
        if (!amr_next_frame(m)) return 0;
        m->have_frame = 0;      /* decoded only to be thrown away */
    }
    m->sub = 0;
    return 1;
}

static long amr_read(void *h, int16_t *out, long out_frames, long volume) {
    VitaAmr *m = (VitaAmr *)h;
    long produced = 0;

    if (m == NULL || out == NULL || out_frames <= 0) return 0;

    while (produced < out_frames) {
        int16_t s;
        int32_t v;

        if (!m->have_frame || m->sub >= AMR_FRAME_SZ * AMR_UPSAMPLE) {
            if (!amr_next_frame(m)) {
                m->eof = 1;
                break;      /* short return tells the caller about EOF */
            }
            m->sub = 0;
        }
        s = m->pcm[m->sub / AMR_UPSAMPLE];
        v = s;
        if (volume >= 0 && volume < 100) v = (v * volume) / 100;
        out[produced * 2 + 0] = (int16_t)v;
        out[produced * 2 + 1] = (int16_t)v;
        m->sub++;
        produced++;
    }
    return produced;
}

const vita_pcm_dec vita_dec_amr = {
    amr_open,
    amr_close,
    amr_channels,
    amr_rate,
    amr_duration_ms,
    amr_rewind,
    amr_seek_ms,
    amr_read
};
