/* vita_audio.c - MMAPI audio backend for PS Vita
 *
 * Provides:
 *   Java_com_vita_mmedia_AudioBridge_playToneNative(III)V
 *
 * Generates a square-wave PCM burst and plays it through SceAudioOut.
 * Square wave is chosen for simplicity (fewer CPU cycles than sine).
 * Games expecting richer audio (WAV/MP3) will still fail gracefully
 * since Manager.createPlayer() throws MediaException (unchanged).
 *
 * Copyright (c) 2024 VitaSDK / J2ME Port Project
 */

#include <psp2/audioout.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* KNI headers - same path as other vita_*.c files in this project */
#include <kni.h>

#define SAMPLE_RATE     8000    /* Low rate sufficient for 8-bit-ish game audio */
#define BUFFER_MS       400     /* Max tone duration per call */
#define BUFFER_SAMPLES  (SAMPLE_RATE * BUFFER_MS / 1000)

/* Global audio port (lazy-initialized, reused across calls) */
static int g_audio_port = -1;
static int g_audio_port_refs = 0;

/* Ensure audio port is open */
static int ensure_audio_port(void) {
    if (g_audio_port < 0) {
        /* SceAudioOutPort param: channel count (1=mono), 8000Hz, 16-bit LE */
        g_audio_port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_MAIN, 1,
                                           SAMPLE_RATE, SCE_AUDIO_OUT_PARAM_FORMAT_S16_STEREO);
        if (g_audio_port < 0) {
            return g_audio_port;
        }
    }
    g_audio_port_refs++;
    return 0;
}

/* Release audio port (reference counted) */
static void release_audio_port(void) {
    if (g_audio_port_refs > 0) {
        g_audio_port_refs--;
    }
    if (g_audio_port_refs == 0 && g_audio_port >= 0) {
        sceAudioOutClosePort(g_audio_port);
        g_audio_port = -1;
    }
}

/* Map MIDI note (0-127) to frequency in Hz.
 * A4 (note 69) = 440 Hz, equal-temperament: f = 440 * 2^((n-69)/12)
 */
static double midi_note_to_freq(int note) {
    if (note < 0) note = 0;
    if (note > 127) note = 127;
    return 440.0 * pow(2.0, (note - 69) / 12.0);
}

/* Generate a square-wave PCM buffer.
 * Each sample is a 16-bit signed value (S16_STEREO interleaved L/R).
 * Volume: 0-100 -> scale factor for amplitude.
 */
static void generate_square_wave(int16_t* buf, int num_samples,
                                  double freq_hz, int volume) {
    double samples_per_cycle = SAMPLE_RATE / freq_hz;
    double amplitude = (volume / 100.0) * 18000.0; /* headroom below 32767 */
    int i;

    for (i = 0; i < num_samples; i++) {
        double phase = fmod((double)i, samples_per_cycle) / samples_per_cycle;
        int16_t s = (phase < 0.5) ? (int16_t)amplitude : (int16_t)(-amplitude);
        /* S16_STEREO: L and R same */
        buf[i * 2 + 0] = s;
        buf[i * 2 + 1] = s;
    }
}

/* JNI implementation.
 *
 * Signature: (III)V
 *   arg0: int note    - MIDI note number (0-127), 60 = middle C
 *   arg1: int dur_ms  - duration in milliseconds (typically 10-60000)
 *   arg2: int vol     - volume 0-100
 *
 * This function may be called from the Java thread. On Vita, sceAudioOut*
 * calls are thread-safe enough for homebrew use. We do NOT spawn a
 * separate thread (keeps the implementation minimal and avoids threading
 * complexity in the VM).
 */
KNIEXPORT void KNICALL
Java_com_vita_mmedia_AudioBridge_playToneNative(KNIDECL_ARGS,
                                                 jint note, jint dur_ms, jint vol) {
    int16_t pcm_buffer[BUFFER_SAMPLES * 2]; /* stereo samples */
    int num_samples;
    double freq;
    int ret;
    int audio_vol = (vol * 32767) / 100; /* convert vol 0-100 -> SceAudioOut vol */
    int prev_vol;

    (void) env;
    (void) obj;

    /* Clamp parameters */
    if (note < 0) note = 0;
    if (note > 127) note = 127;
    if (dur_ms < 1) dur_ms = 1;
    if (dur_ms > BUFFER_MS) dur_ms = BUFFER_MS;
    if (vol < 0) vol = 0;
    if (vol > 100) vol = 100;

    freq = midi_note_to_freq(note);
    num_samples = (SAMPLE_RATE * dur_ms) / 1000;
    if (num_samples > BUFFER_SAMPLES) num_samples = BUFFER_SAMPLES;
    if (num_samples < 1) num_samples = 1;

    ret = ensure_audio_port();
    if (ret < 0) {
        /* Audio unavailable, fail silently */
        return;
    }

    /* Generate PCM */
    generate_square_wave(pcm_buffer, num_samples, freq, vol);

    /* Set output volume (all channels) */
    sceAudioOutSetVolume(g_audio_port,
                         SCE_AUDIO_OUT_VOLUME_FLAG_FL | SCE_AUDIO_OUT_VOLUME_FLAG_FR,
                         &audio_vol);

    /* Output the buffer (blocking). sceAudioOutOutput returns when samples
     * have been queued to hardware buffer. */
    ret = sceAudioOutOutput(g_audio_port, pcm_buffer);
    (void) ret; /* ignore; audio queued */

    /* Small delay to let the tone complete before the function returns.
     * We approximate this with a short sleep rather than busy-waiting. */
    /* Note: sceKernelSleapThread is in SceLibKernel. We assume SceLibKernel_stub
     * is linked (it is, via SceLibKernel_stub in CMakeLists). */
    /* Actually we can't easily call sceKernelSleepThread here because
     * it would block the Java thread too long. Just return quickly.
     * The hardware FIFO will continue playing. */

    release_audio_port();
}
