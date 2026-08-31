/*
 * vita_audio_javacall.c - JSR-135 (MMAPI) javacall layer for PS Vita
 *
 * Implements the javacall multimedia API required by phoneME's jsr135
 * subsystem. This provides tone playing via SceAudioOut.
 */

#include <psp2/audioout.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "javacall_multimedia.h"
#include "javacall_defs.h"

/* ---------------------------------------------------------------------------
 * Tone generation constants
 * ------------------------------------------------------------------------- */
#define TONE_SAMPLE_RATE      16000
#define OUT_SAMPLE_RATE       48000
#define MAX_TONE_DURATION_MS  10000
#define MAX_TONE_SAMPLES      (TONE_SAMPLE_RATE * MAX_TONE_DURATION_MS / 1000)
#define OUT_CHUNK_FRAMES      512

/* ---------------------------------------------------------------------------
 * State
 * ------------------------------------------------------------------------- */
static int g_audio_port = -1;
static volatile int g_tone_stop_requested = 0;

static int16_t g_tone_buffer[MAX_TONE_SAMPLES];
static int g_tone_len = 0;

static int16_t g_out_buffer[OUT_CHUNK_FRAMES * 2];

/* ---------------------------------------------------------------------------
 * caps / configuration (static, tone-only)
 * ------------------------------------------------------------------------- */
static const char g_tone_content_type[] = "audio/x-tone-seq";

static javacall_media_caps g_caps_tone = {
    JAVACALL_MEDIA_FORMAT_TONE,
    g_tone_content_type,
    JAVACALL_MEDIA_MEMORY_PROTOCOL | JAVACALL_MEDIA_FILE_LOCAL_PROTOCOL,
    0
};

static javacall_media_configuration g_cfg = {
    JAVACALL_FALSE,   /* supportMixing         */
    JAVACALL_FALSE,   /* supportRecording      */
    NULL,             /* audioEncoding         */
    NULL,             /* videoEncoding         */
    NULL,             /* videoSnapshotEncoding */
    JAVACALL_TRUE,    /* supportDeviceTone     */
    JAVACALL_FALSE,   /* supportDeviceMIDI     */
    JAVACALL_FALSE,   /* supportCaptureRadio   */
    NULL              /* mediaCaps             */
};

/* ---------------------------------------------------------------------------
 * Helpers
 * ------------------------------------------------------------------------- */

static double midi_note_to_freq(long note) {
    return 440.0 * pow(2.0, ((double)note - 69.0) / 12.0);
}

/**
 * Generate a softly-clipped square wave into g_tone_buffer.
 * 5 ms fade in/out to avoid clicks. Returns sample count.
 */
static int generate_tone_pcm(double freq_hz, int duration_ms, long volume) {
    int num_samples = (int)((long long)TONE_SAMPLE_RATE * duration_ms / 1000);
    if (num_samples > MAX_TONE_SAMPLES) num_samples = MAX_TONE_SAMPLES;
    if (num_samples <= 0) return 0;

    double vol = (double)volume / 100.0;
    if (vol > 1.0) vol = 1.0;
    if (vol < 0.0) vol = 0.0;

    int fade = TONE_SAMPLE_RATE * 5 / 1000;
    if (fade > num_samples / 2) fade = num_samples / 2;

    double period = TONE_SAMPLE_RATE / freq_hz;
    double half = period / 2.0;

    for (int i = 0; i < num_samples; i++) {
        double phase = fmod((double)i, period);
        double s = (phase < half) ? 1.0 : -1.0;
        double v = s * 20000.0 * vol;

        if (i < fade)              v *= (double)i / fade;
        if (i >= num_samples-fade) v *= (double)(num_samples-i) / fade;

        g_tone_buffer[i] = (int16_t)v;
    }
    return num_samples;
}

/** Open SceAudioOut port lazily. 0 on success, -1 on failure. */
static int audio_out_open(void) {
    if (g_audio_port >= 0) return 0;
    g_audio_port = sceAudioOutOpenPort(
        SCE_AUDIO_OUT_PORT_TYPE_MAIN,
        OUT_CHUNK_FRAMES,
        OUT_SAMPLE_RATE,
        SCE_AUDIO_OUT_PARAM_FORMAT_S16_STEREO);
    if (g_audio_port < 0) return -1;
    int vol[2] = { SCE_AUDIO_OUT_MAX_VOL, SCE_AUDIO_OUT_MAX_VOL };
    sceAudioOutSetVolume(g_audio_port,
        SCE_AUDIO_VOLUME_FLAG_L_CH | SCE_AUDIO_VOLUME_FLAG_R_CH, vol);
    return 0;
}

/** Resample mono 16k buffer to 48k stereo chunks; block until done. */
static void output_tone_blocking(void) {
    int produced = 0;
    const int repeat = OUT_SAMPLE_RATE / TONE_SAMPLE_RATE; /* 3 */

    for (int i = 0; i < g_tone_len; i++) {
        if (g_tone_stop_requested) break;
        for (int r = 0; r < repeat; r++) {
            if (produced >= OUT_CHUNK_FRAMES) {
                sceAudioOutOutput(g_audio_port, g_out_buffer);
                produced = 0;
            }
            g_out_buffer[produced*2+0] = g_tone_buffer[i];
            g_out_buffer[produced*2+1] = g_tone_buffer[i];
            produced++;
        }
    }
    if (produced > 0) {
        if (produced < OUT_CHUNK_FRAMES) {
            memset(&g_out_buffer[produced*2], 0,
                   (OUT_CHUNK_FRAMES-produced) * 2 * sizeof(int16_t));
        }
        sceAudioOutOutput(g_audio_port, g_out_buffer);
    }
}

/* ---------------------------------------------------------------------------
 * Mandatory API (signatures must match javacall_multimedia.h)
 * ------------------------------------------------------------------------- */

javacall_result javacall_media_initialize(void) {
    g_audio_port = -1;
    g_tone_stop_requested = 0;
    g_cfg.mediaCaps = NULL;
    return JAVACALL_OK;
}

javacall_result javacall_media_finalize(void) {
    if (g_audio_port >= 0) {
        sceAudioOutReleasePort(g_audio_port);
        g_audio_port = -1;
    }
    return JAVACALL_OK;
}

javacall_result javacall_media_get_configuration(
        /*OUT*/ const javacall_media_configuration** configuration) {
    if (configuration == NULL) return JAVACALL_INVALID_ARGUMENT;
    g_cfg.mediaCaps = &g_caps_tone;
    *configuration = &g_cfg;
    return JAVACALL_OK;
}

javacall_result javacall_media_play_tone(int appID, long note,
                                         long duration, long volume) {
    (void)appID;
    if (note < 0 || note > 127)          return JAVACALL_INVALID_ARGUMENT;
    if (duration <= 0)                   return JAVACALL_INVALID_ARGUMENT;
    if (duration > MAX_TONE_DURATION_MS) duration = MAX_TONE_DURATION_MS;
    if (volume < 0)   volume = 0;
    if (volume > 100) volume = 100;

    if (audio_out_open() != 0) return JAVACALL_NO_AUDIO_DEVICE;

    g_tone_len = generate_tone_pcm(midi_note_to_freq(note),
                                   (int)duration, volume);
    g_tone_stop_requested = 0;
    output_tone_blocking();
    return JAVACALL_OK;
}

javacall_result javacall_media_stop_tone(int appID) {
    (void)appID;
    g_tone_stop_requested = 1;
    return JAVACALL_OK;
}

javacall_result javacall_media_get_event_data(javacall_handle handle,
        int eventType, void *pResult, int numArgs, void *args[]) {
    (void)handle; (void)eventType; (void)pResult; (void)numArgs; (void)args;
    return JAVACALL_INVALID_ARGUMENT;
}

javacall_result javacall_media_create(int appID, int playerID,
                                      javacall_const_utf16_string uri,
                                      long uriLength,
                                      /*OUT*/ javacall_handle* handle) {
    (void)appID; (void)playerID; (void)uri; (void)uriLength; (void)handle;
    /* URI-based players (wav/mp3/midi files) unsupported; tone-only build */
    return JAVACALL_INVALID_ARGUMENT;
}

javacall_result javacall_media_close(javacall_handle handle) {
    (void)handle;
    return JAVACALL_OK;
}

javacall_result javacall_media_destroy(javacall_handle handle) {
    (void)handle;
    return JAVACALL_OK;
}

javacall_result javacall_media_get_player_controls(javacall_handle handle,
                              /*OUT*/ int* controls) {
    (void)handle;
    if (controls) *controls = 0;
    return JAVACALL_OK;
}

javacall_result javacall_media_get_format(javacall_handle handle,
        /*OUT*/ javacall_media_format_type* format) {
    (void)handle;
    if (format) *format = NULL;
    return JAVACALL_OK;
}

javacall_result javacall_media_acquire_device(javacall_handle handle) {
    (void)handle;
    return JAVACALL_OK;
}

javacall_result javacall_media_release_device(javacall_handle handle) {
    (void)handle;
    return JAVACALL_OK;
}
