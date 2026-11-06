/*
 * vita_ogg.c - Ogg Vorbis decoding for MMAPI on PS Vita (v04.45)
 *
 * Uses stb_vorbis (v1.22, public domain, https://nothings.org/stb_vorbis/).
 * Vendored as vita-port/src/stb_vorbis.c and pulled in HERE, not compiled as
 * its own translation unit: stb_vorbis.c is implementation + declarations in
 * one file, so compiling it separately and including it would define every
 * symbol twice.
 *
 * Why a decoder and not sceAudiodec: same reason as MP3 - Vita3K has to behave
 * like hardware, and stb_vorbis decodes the same code on both.
 *
 * Why the whole file in RAM: stb_vorbis v1.22 offers exactly three entry
 * points - open_memory, open_filename/open_file and open_pushdata - and only
 * open_memory gives the random access that stb_vorbis_seek() needs (pushdata
 * is forward-only, so a seek would mean restarting the decode from byte 0).
 * This version has NO read/seek callbacks API; an earlier draft of this file
 * assumed one and did not compile. A J2ME BGM OGG is 0.5-4 MB, which the
 * native heap holds comfortably; the buffer lives as long as the player and is
 * freed in ogg_close. ogg_open refuses anything above OGG_MAX_BYTES instead of
 * gambling on the heap.
 *
 * Resampling is nearest-neighbour on the source frame index, identical to the
 * MP3 path, so the two formats cannot drift apart in pitch or length.
 *
 * Unsupported by stb_vorbis, documented here so nobody re-reports it:
 *   - floor 0 (files encoded before 2004)
 *   - chained/multiplexed streams ("cannot concatenate multiple vorbis streams")
 * Both make stb_vorbis_open_callbacks return NULL, which the audio layer
 * reports as "cannot play this file" - no silent garbage.
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define STB_VORBIS_NO_STDIO          /* no stb_vorbis_open_filename: we seek */
#define STB_VORBIS_NO_PUSHDATA_API   /* unused; saves some code */
#include "stb_vorbis.c"

#include "vita_pcm_dec.h"

#define OUT_RATE VITA_DEC_OUT_RATE

/* Source frames decoded per refill (~186 ms at 44.1 kHz). Big enough that the
 * refill cost disappears into the 20 ms audio chunks, small enough that the
 * buffer is 32 KB rather than megabytes. */
#define OGG_WIN 8192

/* Hard ceiling for one OGG file. 8 MB of compressed Vorbis is ~80 minutes of
 * music; anything bigger is not a J2ME game asset. */
#define OGG_MAX_BYTES (8L * 1024L * 1024L)

typedef struct {
    unsigned char *data;     /* whole compressed stream (owned)    */
    long           len;
    stb_vorbis    *v;
    int            ch;
    int            rate;
    long           total_src;   /* source frames; 0 = unknown        */
    int16_t       *win;         /* OGG_WIN frames, interleaved       */
    long           win_start;   /* absolute source frame of win[0]   */
    long           win_n;       /* valid frames in win               */
    long           out_pos;     /* 48 kHz output frames produced     */
} VitaOgg;

/* ---- vtable --------------------------------------------------------- */

static int ogg_channels(void *h) {
    return (h != NULL) ? ((VitaOgg *)h)->ch : 0;
}

static int ogg_rate(void *h) {
    return (h != NULL) ? ((VitaOgg *)h)->rate : 0;
}

static long ogg_duration_ms(void *h) {
    VitaOgg *o = (VitaOgg *)h;
    if (o == NULL || o->rate <= 0 || o->total_src <= 0) {
        return 0;
    }
    return (o->total_src * 1000L) / (long)o->rate;
}

/* Decode one window whose first source frame is `start`. A forward step of
 * exactly one window continues the stream (no seek); anything else seeks,
 * which is what rewind or a getMediaTime()-style jump needs. */
static int ogg_fill_at(VitaOgg *o, long start) {
    int got;
    if (start < 0) {
        start = 0;
    }
    if (start != o->win_start + o->win_n) {
        if (!stb_vorbis_seek(o->v, (unsigned int)start)) {
            return 0;
        }
    }
    got = stb_vorbis_get_samples_short_interleaved(o->v, o->ch, o->win, OGG_WIN * 2);
    if (got <= 0) {
        return 0;
    }
    o->win_start = start;
    o->win_n = got;
    return 1;
}

static void ogg_rewind(void *h) {
    VitaOgg *o = (VitaOgg *)h;
    if (o == NULL) {
        return;
    }
    stb_vorbis_seek_start(o->v);
    o->win_start = 0;
    o->win_n = 0;
    o->out_pos = 0;
}

static int ogg_seek_ms(void *h, long ms) {
    VitaOgg *o = (VitaOgg *)h;
    long src;
    if (o == NULL || ms < 0) {
        return 0;
    }
    src = (ms * (long)o->rate) / 1000L;
    if (o->total_src > 0 && src >= o->total_src) {
        src = o->total_src - 1;
    }
    if (!stb_vorbis_seek(o->v, (unsigned int)src)) {
        return 0;
    }
    o->win_start = src;
    o->win_n = 0;
    o->out_pos = (ms * OUT_RATE) / 1000L;
    return 1;
}

static long ogg_read(void *h, int16_t *out, long out_frames, long volume) {
    VitaOgg *o = (VitaOgg *)h;
    long produced = 0;
    long rate;

    if (o == NULL || out == NULL || out_frames <= 0) {
        return 0;
    }
    rate = o->rate;

    while (produced < out_frames) {
        long t = o->out_pos + produced;
        /* v04.45: 64-bit intermediate. t*rate overflows 32-bit long (ARM) at
         * t = 48700 output frames for 44.1 kHz, i.e. after ~1 s of a 44.1 kHz
         * file; the same expression is in vita_mp3.c and was fixed with it.
         * OUT_RATE is a compile-time constant, so this stays a multiply. */
        long need_src = (long)(((long long)t * (long long)rate) / (long long)OUT_RATE);
        long idx;
        int32_t l, r;

        if (o->total_src > 0 && need_src >= o->total_src) {
            break;
        }
        if (!(need_src >= o->win_start && need_src < o->win_start + o->win_n)) {
            if (!ogg_fill_at(o, need_src)) {
                break;
            }
        }
        idx = need_src - o->win_start;
        if (idx < 0) {
            idx = 0;
        }
        if (idx >= o->win_n) {
            idx = o->win_n - 1;
        }
        if (o->ch == 1) {
            l = o->win[idx];
            r = l;
        } else {
            l = o->win[idx * 2 + 0];
            r = o->win[idx * 2 + 1];
        }
        if (volume >= 0 && volume < 100) {
            l = (l * volume) / 100;
            r = (r * volume) / 100;
        }
        out[produced * 2 + 0] = (int16_t)l;
        out[produced * 2 + 1] = (int16_t)r;
        produced++;
    }
    o->out_pos += produced;
    return produced;
}

static void *ogg_open(vita_dec_read_cb rd, void *ctx, long total_len) {
    VitaOgg *o;
    stb_vorbis_info info;
    int err = 0;
    long got = 0;

    if (rd == NULL || total_len <= 0 || total_len > OGG_MAX_BYTES) {
        return NULL;
    }
    o = (VitaOgg *)calloc(1, sizeof(VitaOgg));
    if (o == NULL) {
        return NULL;
    }
    o->len = total_len;
    o->data = (unsigned char *)malloc((size_t)total_len);
    if (o->data == NULL) {
        free(o);
        return NULL;
    }
    /* pcm_src_read returns short reads when the stream is split between the
     * RAM window and the file spill, so loop until full or EOF. */
    while (got < total_len) {
        long n = rd(ctx, got, o->data + got, total_len - got);
        if (n <= 0) {
            break;
        }
        got += n;
    }
    if (got < total_len) {
        free(o->data);
        free(o);
        return NULL;
    }
    /* open_memory does not copy: o->data must stay alive until ogg_close. */
    o->v = stb_vorbis_open_memory(o->data, (int)got, &err, NULL);
    if (o->v == NULL) {
        free(o->data);
        free(o);
        return NULL;
    }
    info = stb_vorbis_get_info(o->v);
    o->ch = info.channels;
    o->rate = info.sample_rate;
    if (o->ch <= 0 || o->rate <= 0) {
        stb_vorbis_close(o->v);
        free(o->data);
        free(o);
        return NULL;
    }
    o->total_src = (long)stb_vorbis_stream_length_in_samples(o->v);
    o->win = (int16_t *)malloc(sizeof(int16_t) * 2 * OGG_WIN);
    if (o->win == NULL) {
        stb_vorbis_close(o->v);
        free(o->data);
        free(o);
        return NULL;
    }
    o->win_start = 0;
    o->win_n = 0;
    return o;
}

static void ogg_close(void *h) {
    VitaOgg *o = (VitaOgg *)h;
    if (o == NULL) {
        return;
    }
    if (o->v != NULL) {
        stb_vorbis_close(o->v);
    }
    free(o->win);
    free(o->data);
    free(o);
}

const vita_pcm_dec vita_dec_ogg = {
    ogg_open,
    ogg_close,
    ogg_channels,
    ogg_rate,
    ogg_duration_ms,
    ogg_rewind,
    ogg_seek_ms,
    ogg_read
};
