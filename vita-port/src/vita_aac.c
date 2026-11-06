/*
 * vita_aac.c - AAC (ADTS / AAC-LC) decoding for MMAPI on PS Vita
 *
 * AAC shows up in J2ME mostly as the audio track of 3GPP-style clips and
 * in a few 2007+ ports; before this file `Manager.createPlayer()` threw
 * MediaException for audio/aac, so those clips were silent.
 *
 * Decoder: sceAudiodec (the console's hardware codec block) rather than a
 * vendored AAC implementation. Writing an AAC-LC decoder is out of scope,
 * the permissively licensed alternatives are bulky, and the console
 * already has one. The trade-off is Vita3K: if the emulator lacks
 * SceAudiodec the probe below fails and AAC stays unsupported (exactly
 * today's behaviour), instead of crashing. MP3 deliberately does NOT use
 * this path - see vita_mp3.c.
 *
 * The whole elementary stream is kept in RAM: ADTS AAC in a J2ME game is
 * tens to a few hundred KB, and random access makes seek trivial.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <psp2/audiodec.h>
#include <psp2/sysmodule.h>

#include "vita_pcm_dec.h"

#define OUT_RATE     VITA_DEC_OUT_RATE
#define AAC_MAX_ES   (16 * 1024 * 1024)   /* refuse anything bigger */
#define AAC_PCM_MAX  (2048 * 2 * 2)       /* 2048 frames, 2 ch, 16 bit */

/* One-time capability probe. -1 = unknown, 0 = no decoder, 1 = ready. */
static int g_aac_ready = -1;

static int aac_probe(void) {
    SceAudiodecInitParam param;
    int rc;

    if (g_aac_ready >= 0) return g_aac_ready;
    g_aac_ready = 0;

    rc = sceSysmoduleLoadModule(SCE_SYSMODULE_AUDIOCODEC);
    if (rc < 0 && rc != (int)0x8002D003 /* already loaded */) {
        fprintf(stderr, "[AAC] LoadModule(AUDIOCODEC) rc=0x%08x\n", rc);
        return 0;
    }
    memset(&param, 0, sizeof(param));
    param.aac.size = sizeof(SceAudiodecInitStreamParam);
    param.aac.totalStreams = 1;
    rc = sceAudiodecInitLibrary(SCE_AUDIODEC_TYPE_AAC, &param);
    if (rc < 0) {
        fprintf(stderr, "[AAC] InitLibrary rc=0x%08x (no hw decoder)\n", rc);
        return 0;
    }
    g_aac_ready = 1;
    fprintf(stderr, "[AAC] sceAudiodec AAC ready\n");
    return 1;
}

typedef struct VitaAac VitaAac;

struct VitaAac {
    uint8_t *es;
    long     es_len;
    long     pos;              /* byte cursor into es[]            */

    SceAudiodecCtrl ctrl;
    SceAudiodecInfo info;
    int      dec_ok;

    int16_t  pcm[AAC_PCM_MAX / 2];   /* interleaved decoded frame   */
    int      frame_samples;          /* per channel                  */
    int      frame_ch;
    long     frame_src_start;
    long     next_src_frame;

    long     out_pos;                /* 48 kHz frames emitted        */
    int      rate;
    int      channels;
    long     duration_ms;
    long     frame_count;
};

/* Parse one ADTS header. Returns frame length (header included) or 0.
 * `rate` receives the sample rate implied by the frequency index. */
static int aac_adts_header(const uint8_t *p, long avail,
                           int *rate, int *ch, int *hdr_len) {
    static const int rates[13] = { 96000, 88200, 64000, 48000, 44100,
                                   32000, 24000, 22050, 16000, 12000,
                                   11025, 8000, 7350 };
    int idx, len;

    if (avail < 7) return 0;
    if (p[0] != 0xFF || (p[1] & 0xF0) != 0xF0) return 0;  /* syncword */
    if ((p[1] & 0x06) != 0) return 0;                      /* layer 0  */

    idx = (p[2] >> 2) & 0x0F;
    if (idx >= 13) return 0;                  /* 13/14 reserved, 15 = ESC */

    *rate = rates[idx];
    *ch = ((p[2] & 0x01) << 2) | ((p[3] >> 6) & 0x03);
    if (*ch <= 0 || *ch > 2) *ch = 2;          /* 0 = "in band": be safe */

    len = ((p[3] & 0x03) << 11) | (p[4] << 3) | ((p[5] >> 5) & 0x07);
    *hdr_len = (p[1] & 0x01) ? 7 : 9;          /* protection_absent */
    if (len < *hdr_len) return 0;
    return len;
}

static int aac_next_frame(VitaAac *m) {
    for (;;) {
        int rate = 0, ch = 0, hdr = 0, len;
        long avail = m->es_len - m->pos;

        if (avail < 7) return 0;
        len = aac_adts_header(m->es + m->pos, avail, &rate, &ch, &hdr);
        if (len <= 0 || len > avail) {
            m->pos++;               /* resynchronise */
            continue;
        }

        if (!m->dec_ok) {
            int rc;
            if (rate <= 0) rate = 48000;
            m->rate = rate;
            m->channels = ch;

            memset(&m->info, 0, sizeof(m->info));
            m->info.aac.size = sizeof(SceAudiodecInfoAac);
            m->info.aac.isAdts = 1;
            m->info.aac.ch = (SceUInt32)ch;
            m->info.aac.samplingRate = (SceUInt32)m->rate;
            m->info.aac.isSbr = 0;

            memset(&m->ctrl, 0, sizeof(m->ctrl));
            m->ctrl.size = sizeof(SceAudiodecCtrl);
            m->ctrl.pInfo = &m->info;
            m->ctrl.wordLength = SCE_AUDIODEC_WORD_LENGTH_16BITS;

            rc = sceAudiodecCreateDecoder(&m->ctrl, SCE_AUDIODEC_TYPE_AAC);
            if (rc < 0) {
                fprintf(stderr, "[AAC] CreateDecoder rc=0x%08x\n", rc);
                m->dec_ok = 0;
                return 0;
            }
            m->dec_ok = 1;
        }

        m->ctrl.pEs = m->es + m->pos;
        m->ctrl.inputEsSize = (SceUInt32)len;
        m->ctrl.maxEsSize = (SceUInt32)len;
        m->ctrl.pPcm = m->pcm;
        m->ctrl.maxPcmSize = AAC_PCM_MAX;
        m->ctrl.outputPcmSize = 0;

        {
            int rc = sceAudiodecDecode(&m->ctrl);
            if (rc < 0) {
                /* One bad frame must not end the stream: skip it. */
                m->pos += len;
                continue;
            }
        }
        m->pos += len;

        {
            int ch2 = (m->channels > 0) ? m->channels : 2;
            long samples = (long)m->ctrl.outputPcmSize / (2 * ch2);
            if (samples <= 0) continue;      /* priming frame */
            if (samples > AAC_PCM_MAX / (2 * ch2))
                samples = AAC_PCM_MAX / (2 * ch2);
            m->frame_samples = (int)samples;
            m->frame_ch = ch2;
            m->frame_src_start = m->next_src_frame;
            m->next_src_frame += samples;
            m->frame_count++;
            return 1;
        }
    }
}

static void aac_reset(VitaAac *m) {
    m->pos = 0;
    m->next_src_frame = 0;
    m->frame_samples = 0;
    m->frame_src_start = 0;
    m->out_pos = 0;
    if (m->dec_ok) {
        sceAudiodecClearContext(&m->ctrl);
    }
}

static void *aac_open(vita_dec_read_cb rd, void *ctx, long total_len) {
    VitaAac *m;
    long got, off = 0;

    if (rd == NULL || total_len <= 7 || total_len > AAC_MAX_ES) return NULL;
    if (!aac_probe()) return NULL;

    m = (VitaAac *)calloc(1, sizeof(*m));
    if (m == NULL) return NULL;

    m->es = (uint8_t *)malloc((size_t)total_len);
    if (m->es == NULL) {
        free(m);
        return NULL;
    }
    m->es_len = total_len;
    while (off < total_len) {
        got = rd(ctx, off, m->es + off, total_len - off);
        if (got <= 0) break;
        off += got;
    }
    m->es_len = off;

    /* Validate: refuse anything whose first bytes are not ADTS. Without
     * this a mis-detected file would be reported as AAC and stay silent. */
    {
        int ri = 0, ch = 0, hl = 0;
        if (aac_adts_header(m->es, m->es_len, &ri, &ch, &hl) <= 0) {
            free(m->es);
            free(m);
            return NULL;
        }
    }

    aac_reset(m);
    if (!aac_next_frame(m)) {
        if (m->dec_ok) sceAudiodecDeleteDecoder(&m->ctrl);
        free(m->es);
        free(m);
        return NULL;
    }

    /* Duration from the frame count (AAC-LC: 1024 samples per frame, so
     * frames * 1024 / rate). Cheap, and exact enough for a progress bar. */
    {
        long frames = 0;
        long save = m->pos, dst = 0;
        m->pos = 0;
        /* rough scan: walk ADTS headers only */
        while (m->pos < m->es_len) {
            int ri = 0, ch = 0, hl = 0, len;
            len = aac_adts_header(m->es + m->pos, m->es_len - m->pos,
                                  &ri, &ch, &hl);
            if (len <= 0) { m->pos++; continue; }
            m->pos += len;
            frames++;
        }
        (void)dst;
        m->pos = save;
        if (m->rate > 0) m->duration_ms = frames * 1024 * 1000 / m->rate;
    }
    return m;
}

static void aac_close(void *h) {
    VitaAac *m = (VitaAac *)h;
    if (m == NULL) return;
    if (m->dec_ok) sceAudiodecDeleteDecoder(&m->ctrl);
    free(m->es);
    free(m);
}

static int aac_channels(void *h) {
    VitaAac *m = (VitaAac *)h;
    return (m != NULL && m->channels > 0) ? m->channels : 2;
}

static int aac_rate(void *h) {
    VitaAac *m = (VitaAac *)h;
    return (m != NULL && m->rate > 0) ? m->rate : 44100;
}

static long aac_duration_ms(void *h) {
    VitaAac *m = (VitaAac *)h;
    return (m != NULL) ? m->duration_ms : 0;
}

static void aac_rewind(void *h) {
    VitaAac *m = (VitaAac *)h;
    if (m == NULL) return;
    aac_reset(m);
    (void)aac_next_frame(m);
}

static int aac_seek_ms(void *h, long ms) {
    VitaAac *m = (VitaAac *)h;
    long target_out;

    if (m == NULL || ms < 0) return 0;
    target_out = (ms * OUT_RATE) / 1000;
    aac_reset(m);
    for (;;) {
        long frame_out = (m->frame_src_start * OUT_RATE) / aac_rate(m);
        if (frame_out >= target_out || m->frame_samples <= 0) break;
        if (!aac_next_frame(m)) return 0;
    }
    m->out_pos = (m->frame_src_start * OUT_RATE) / aac_rate(m);
    return 1;
}

static long aac_read(void *h, int16_t *out, long out_frames, long volume) {
    VitaAac *m = (VitaAac *)h;
    long produced = 0;
    long rate;

    if (m == NULL || out == NULL || out_frames <= 0) return 0;
    rate = aac_rate(m);

    while (produced < out_frames) {
        long t = m->out_pos + produced;
        long need_src = (t * rate) / OUT_RATE;

        while (need_src >= m->frame_src_start + m->frame_samples) {
            if (!aac_next_frame(m)) {
                break;      /* short return tells the caller about EOF */
            }
        }
        {
            long src = need_src - m->frame_src_start;
            int32_t l, r;
            int ch = (m->frame_ch > 0) ? m->frame_ch : 2;

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

const vita_pcm_dec vita_dec_aac = {
    aac_open,
    aac_close,
    aac_channels,
    aac_rate,
    aac_duration_ms,
    aac_rewind,
    aac_seek_ms,
    aac_read
};
