/*
 * vita_wav.c - WAV (RIFF/PCM) player support for the JSR-135 javacall layer
 *
 * v01.96: WAV PCM playback (audio/x-wav), the biggest MMAPI gap.
 *
 * Data path (why javacall_media_download_handled_by_device lives here):
 *   The gen_stubs.py-generated libpcsl_multimedia.a stub returns
 *   JAVACALL_NOT_IMPLEMENTED and leaves the isHandled OUT parameter
 *   UNWRITTEN. KNIHighLevelPlayer.c's nIsHandledByDevice pre-initializes
 *   returnValue = KNI_TRUE, so an untouched OUT param reports "device
 *   handles the download" -> HighLevelPlayer never constructs its
 *   MediaDownload, nBuffering is never called, and no media bytes ever
 *   reach this port layer. Overriding the stub here (port objects are
 *   linked before libpcsl_multimedia.a and the link uses
 *   --allow-multiple-definition, so the FIRST definition wins) with a
 *   JAVACALL_FALSE answer makes Java stream file/http/memory content
 *   into javacall_media_do_buffering chunk by chunk.
 *
 * Buffering protocol (from KNIMediaDownload.c doBuffering + MediaDownload
 * .java download()): Java allocates a buffer of the size we report from
 * javacall_media_get_java_buffer_size(), then repeatedly fills a
 * packet-sized sliding window of it and calls nBuffering. KNI copies the
 * CURRENT window to the START of the native buffer via
 * KNI_GetRawArrayRegion - it does NOT accumulate - so we must append each
 * chunk into our own storage here before parsing/rendering.
 *
 * Rendering runs on the shared player thread (see tone_player_thread in
 * vita_audio_javacall.c) so the CLDC green-thread VM is never blocked;
 * a WAV request uses the same seq handshake as JTS playback.
 */

#include <string.h>
#include <stdint.h>

#include "javacall_multimedia.h"
#include "javacall_defs.h"

/* ---------------------------------------------------------------------------
 * Override the stub: Java must deliver media data to us (handledByDevice =
 * false). Called from KNI nIsHandledByDevice for every non-device player.
 * ------------------------------------------------------------------------- */
javacall_result vita_port_media_download_handled_by_device(
        javacall_handle handle, /*OUT*/ javacall_bool* isHandled) {
    (void)handle;
    if (isHandled == NULL) return JAVACALL_INVALID_ARGUMENT;
    *isHandled = JAVACALL_FALSE;   /* Java MediaDownload streams data to us */
    return JAVACALL_OK;
}

/* ---------------------------------------------------------------------------
 * RIFF header parsing
 * ------------------------------------------------------------------------- */

/* All fields little-endian; Vita is LE so plain loads are fine. */
#define WAV_FMT_PCM 1

typedef struct {
    long   data_off;      /* offset of PCM payload within wav_data    */
    long   data_len;      /* PCM payload length in bytes              */
    long   rate;          /* sample rate (Hz)                         */
    int    channels;      /* 1 or 2                                   */
    int    bits;          /* 8 or 16                                  */
} wav_info;

static long wav_rd32(const uint8_t *p) {
    return (long)p[0] | ((long)p[1] << 8) | ((long)p[2] << 16) |
           ((long)p[3] << 24);
}

static long wav_rd16(const uint8_t *p) {
    return (long)p[0] | ((long)p[1] << 8);
}

/**
 * Parse a RIFF/WAVE header located at the start of buf.
 * Returns 1 and fills *info on success, 0 on any malformed/unsupported
 * input (non-PCM compression, absurd rate/channels/bits, truncated
 * header, missing data chunk).
 */
static int wav_parse(const uint8_t *buf, long len, wav_info *info) {
    long pos;

    if (len < 12) return 0;
    if (memcmp(buf, "RIFF", 4) != 0 || memcmp(buf + 8, "WAVE", 4) != 0)
        return 0;

    memset(info, 0, sizeof(*info));

    pos = 12;
    while (pos + 8 <= len) {
        const uint8_t *id = buf + pos;
        long sz = wav_rd32(buf + pos + 4);
        long body = pos + 8;
        if (sz < 0) return 0;
        if (body + sz > len) {
            /* data chunk may be truncated in a damaged file - accept the
             * remainder if we already have a valid fmt; otherwise fail. */
            if (memcmp(id, "data", 4) == 0 && info->rate > 0) {
                sz = len - body;
            } else {
                return 0;
            }
        }

        if (memcmp(id, "fmt ", 4) == 0) {
            if (sz < 16) return 0;
            if (wav_rd16(buf + body) != WAV_FMT_PCM) return 0;
            info->channels = (int)wav_rd16(buf + body + 2);
            info->rate     = wav_rd32(buf + body + 4);
            info->bits     = (int)wav_rd16(buf + body + 14);
            if (info->channels < 1 || info->channels > 2) return 0;
            if (info->bits != 8 && info->bits != 16) return 0;
            if (info->rate < 4000 || info->rate > 96000) return 0;
        } else if (memcmp(id, "data", 4) == 0) {
            if (info->rate <= 0) return 0;   /* fmt chunk must come first */
            info->data_off = body;
            info->data_len = sz;
            return 1;
        }
        /* skip: chunk body + pad byte for odd sizes (RIFF rule) */
        pos = body + sz + (sz & 1);
    }
    return 0;
}

/**
 * Parse whatever is buffered so far and fill the WAV fields of the
 * player. Kept here so vita_audio_javacall.c does not need to know the
 * RIFF layout. Returns 1 when a data chunk has been located.
 */
int vita_wav_parse_header(const uint8_t *buf, long len, wav_info *info) {
    return wav_parse(buf, len, info);
}

/**
 * Duration in milliseconds of the parsed PCM payload, or -1 if info is
 * invalid. bytes/frame = channels * bits/8.
 */
long vita_wav_duration_ms(const wav_info *info) {
    long bytes_per_frame;
    if (info == NULL || info->rate <= 0 || info->channels <= 0 ||
        info->bits <= 0 || info->data_len <= 0) {
        return -1;
    }
    bytes_per_frame = (long)info->channels * (info->bits >> 3);
    if (bytes_per_frame <= 0) return -1;
    return (long)(((long long)info->data_len * 1000) /
                  ((long long)info->rate * bytes_per_frame));
}
