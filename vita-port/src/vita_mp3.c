/*
 * vita_mp3.c - MP3 (MPEG-1/2/2.5 Layer III) decoding for MMAPI on PS Vita
 *
 * Uses minimp3 (public domain / CC0, https://github.com/lieff/minimp3).
 *
 * Why a decoder and not sceAudiodec: the port has to behave the same in
 * Vita3K and on hardware, and sceAudiodec's availability in the emulator
 * is not something this tree can rely on. minimp3 is a single header,
 * has no allocation of its own and decodes the same code on both.
 *
 * Why streaming: an MP3 BGM is routinely 2-5 MB. Decoding it all up
 * front would need 20-60 MB of PCM (128 kbps -> ~11.5 bytes of PCM per
 * byte of MP3) on top of the 32 MB Java heap and two 4 MB CDRAM
 * framebuffers. Instead exactly ONE MP3 frame (1152 samples) is kept in
 * memory and the player thread pulls the PCM strictly sequentially; the
 * compressed bytes come through a 32 KB sliding window (vita_src), so the
 * decoder never needs the whole file at once either.
 *
 * Resampling: output is always 48 kHz stereo, the format the audio port
 * is opened with. Source frames are picked nearest-neighbour, which is
 * what the WAV path already does - see wav_render_player().
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MINIMP3_IMPLEMENTATION
#define MINIMP3_ONLY_MP3          /* Layer III only: smaller, no LSF tables */
#define MINIMP3_NO_STDIO
#include "minimp3.h"

#include "vita_pcm_dec.h"

#define OUT_RATE VITA_DEC_OUT_RATE

/* Window of compressed bytes held in RAM. Must comfortably exceed one
 * frame (1441 B) so a frame is always contiguous. */
#define MP3_WIN_SIZE 32768

typedef struct VitaMp3 VitaMp3;

struct VitaMp3 {
    vita_src src;
    long audio_start;      /* first byte after any ID3v2 tag */

    mp3dec_t dec;
    int      dec_inited;

    /* Current decoded frame (per-channel sample count in frame_samples) */
    int16_t pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
    int     frame_samples;
    long    frame_src_start;   /* absolute source frame index of pcm[0] */
    int     frame_ch;

    long    abs_pos;           /* decode cursor (absolute byte)      */
    long    next_src_frame;    /* absolute index the next frame gets */
    long    out_pos;           /* 48 kHz frames already emitted      */
    int     eof;

    int     channels;
    int     rate;
    long    duration_ms;
};

/* ID3v2 header: "ID3" + version(2) + flags(1) + syncsafe size(4).
 * The size counts everything after the 10-byte header, and the footer
 * flag (0x10) adds another 10 bytes. */
static long mp3_skip_id3v2(VitaMp3 *m) {
    long off = 0;

    for (;;) {
        const uint8_t *p;
        long avail;
        long size, total;

        p = vita_src_view(&m->src, off, 10, &avail);
        if (p == NULL || avail < 10) break;
        if (p[0] != 'I' || p[1] != 'D' || p[2] != '3') break;
        size = ((long)(p[6] & 0x7F) << 21) |
               ((long)(p[7] & 0x7F) << 14) |
               ((long)(p[8] & 0x7F) << 7) |
               (long)(p[9] & 0x7F);
        total = 10 + size + ((p[5] & 0x10) ? 10 : 0);
        if (total <= 0 || off + total > m->src.total) break;
        off += total;
    }
    return off;
}

/* Decode the next MP3 frame into m->pcm. Returns 1 on success, 0 at
 * end of stream / when no further frame can be decoded. */
static int mp3_next_frame(VitaMp3 *m) {
    for (;;) {
        mp3dec_frame_info_t info;
        const uint8_t *p;
        long avail;
        int samples;

        if (m->abs_pos >= m->src.total) return 0;
        p = vita_src_view(&m->src, m->abs_pos, 4096, &avail);
        if (p == NULL || avail <= 0) return 0;

        memset(&info, 0, sizeof(info));
        samples = mp3dec_decode_frame(&m->dec, p, (int)avail, m->pcm, &info);
        if (info.frame_bytes <= 0) {
            /* No sync here. Jump to the next likely frame start instead of
             * stepping one byte at a time (a long tag or a garbage tail
             * would otherwise cost thousands of decode attempts). */
            long i, skip = avail;
            for (i = 1; i < avail; i++) {
                if (p[i] == 0xFF) { skip = i; break; }
            }
            m->abs_pos += skip;
            if (m->abs_pos >= m->src.total) return 0;
            continue;
        }
        m->abs_pos += info.frame_bytes;
        if (samples <= 0) {
            continue;   /* header-only frame (Xing/Info/LAME) */
        }
        if (m->channels == 0 && info.channels > 0) {
            m->channels = info.channels;
            m->rate = info.hz;
        }
        m->frame_samples = samples;
        m->frame_ch = (info.channels > 0) ? info.channels : m->channels;
        m->frame_src_start = m->next_src_frame;
        m->next_src_frame += samples;
        return 1;
    }
}

static void mp3_reset(VitaMp3 *m) {
    mp3dec_init(&m->dec);
    m->abs_pos = m->audio_start;
    m->next_src_frame = 0;
    m->frame_samples = 0;
    m->frame_src_start = 0;
    m->out_pos = 0;
    m->eof = 0;
    m->src.stuck = 0;
    vita_src_invalidate(&m->src);
}

static void *mp3_open(vita_dec_read_cb rd, void *ctx, long total_len) {
    VitaMp3 *m;

    if (rd == NULL || total_len <= 0) return NULL;

    m = (VitaMp3 *)calloc(1, sizeof(*m));
    if (m == NULL) return NULL;

    m->src.buf = (uint8_t *)malloc(MP3_WIN_SIZE);
    if (m->src.buf == NULL) {
        free(m);
        return NULL;
    }
    vita_src_init(&m->src, rd, ctx, total_len, m->src.buf, MP3_WIN_SIZE);

    m->audio_start = mp3_skip_id3v2(m);
    if (m->audio_start >= total_len) {
        free(m->src.buf);
        free(m);
        return NULL;
    }

    mp3_reset(m);

    /* Prime the decoder: the first successful frame tells us the sample
     * rate / channel count, which the MMAPI layer needs before it plays
     * anything. */
    if (!mp3_next_frame(m)) {
        free(m->src.buf);
        free(m);
        return NULL;
    }

    /* Duration estimate. Exact frame counting would mean decoding the
     * whole file (seconds of CPU for a BGM), while this is good enough
     * for getMediaTime()/END_OF_MEDIA pacing: bytes -> bits -> ms at a
     * neutral 128 kbps. VBR files drift; the renderer stops on real EOF
     * anyway. */
    {
        long bytes_left = total_len - m->audio_start;
        long ms = (bytes_left * 8) / 128;
        m->duration_ms = (ms > 0) ? ms : 0;
    }
    return m;
}

static void mp3_close(void *h) {
    VitaMp3 *m = (VitaMp3 *)h;
    if (m == NULL) return;
    free(m->src.buf);
    free(m);
}

static int mp3_channels(void *h) {
    VitaMp3 *m = (VitaMp3 *)h;
    return (m != NULL && m->channels > 0) ? m->channels : 2;
}

static int mp3_rate(void *h) {
    VitaMp3 *m = (VitaMp3 *)h;
    return (m != NULL && m->rate > 0) ? m->rate : 44100;
}

static long mp3_duration_ms(void *h) {
    VitaMp3 *m = (VitaMp3 *)h;
    return (m != NULL) ? m->duration_ms : 0;
}

static void mp3_rewind(void *h) {
    VitaMp3 *m = (VitaMp3 *)h;
    if (m == NULL) return;
    mp3_reset(m);
    (void)mp3_next_frame(m);       /* frame 0 cached, like at open time */
}

/* Forward scan to `ms`.
 *
 * There is no frame index, so this decodes (and throws away) everything
 * before the target. A 3-minute BGM is ~7000 frames - a few tens of
 * milliseconds on this CPU, and setMediaTime is not a hot path. Keeping a
 * sparse index would cost RAM for a call most games never make. */
static int mp3_seek_ms(void *h, long ms) {
    VitaMp3 *m = (VitaMp3 *)h;
    long target_out;

    if (m == NULL || ms < 0) return 0;
    target_out = (ms * OUT_RATE) / 1000;

    mp3_reset(m);
    for (;;) {
        long frame_out = (m->frame_src_start * OUT_RATE) / mp3_rate(m);
        if (frame_out >= target_out || m->frame_samples <= 0) break;
        if (!mp3_next_frame(m)) return 0;
    }
    m->out_pos = (m->frame_src_start * OUT_RATE) / mp3_rate(m);
    return 1;
}

/* Read up to out_frames stereo frames at 48 kHz, volume-scaled.
 * Returns the number of frames written (0 = end of stream). */
static long mp3_read(void *h, int16_t *out, long out_frames, long volume) {
    VitaMp3 *m = (VitaMp3 *)h;
    long produced = 0;
    long rate;

    if (m == NULL || out == NULL || out_frames <= 0) return 0;
    rate = mp3_rate(m);

    while (produced < out_frames) {
        long t = m->out_pos + produced;
        /* v04.45: 64-bit intermediate, same fix as vita_ogg.c. With 32-bit
         * long this overflowed at t = 48700 output frames (44.1 kHz, ~1 s in),
         * which made long BGMs read source frames from a wrapped index. */
        long need_src = (long)(((long long)t * (long long)rate) / (long long)OUT_RATE);

        while (need_src >= m->frame_src_start + m->frame_samples) {
            if (!mp3_next_frame(m)) {
                m->eof = 1;
                /* Return the REAL frame count and let the caller pad: a
                 * short return is how it learns the stream ended. Padding
                 * here as well would make getMediaTime() overshoot. */
                break;
            }
        }
        {
            long src = need_src - m->frame_src_start;
            int32_t l, r;
            int ch = (m->frame_ch > 0) ? m->frame_ch : 1;

            if (src < 0) src = 0;
            if (src >= m->frame_samples) src = m->frame_samples - 1;
            if (ch >= 2) {
                l = m->pcm[src * ch + 0];
                r = m->pcm[src * ch + 1];
            } else {
                l = m->pcm[src];
                r = l;
            }
            if (volume >= 0 && volume < 100) {
                l = (l * volume) / 100;
                r = (r * volume) / 100;
            }
            out[produced * 2 + 0] = (int16_t)l;
            out[produced * 2 + 1] = (int16_t)r;
        }
        produced++;
    }
    m->out_pos += produced;
    return produced;
}

const vita_pcm_dec vita_dec_mp3 = {
    mp3_open,
    mp3_close,
    mp3_channels,
    mp3_rate,
    mp3_duration_ms,
    mp3_rewind,
    mp3_seek_ms,
    mp3_read
};
