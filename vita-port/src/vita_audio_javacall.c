/*
 * vita_audio_javacall.c - JSR-135 (MMAPI) javacall layer for PS Vita
 *
 * Implements the javacall multimedia API required by phoneME's jsr135
 * subsystem. This provides tone playing via SceAudioOut:
 *   - Manager.playTone() simple tones (async, dedicated player thread)
 *   - Full MMAPI Player state machine for device://tone / audio/x-tone-seq
 *     including JTS (Java Tone Sequence) buffering and playback.
 *
 * All Player entry points return synchronously (JAVACALL_OK / FAIL);
 * actual PCM rendering happens on the dedicated player thread so the
 * CLDC green-thread VM is never blocked.
 */

#include <psp2/audioout.h>
#include <psp2/kernel/threadmgr.h> /* sceKernelDelayThread (alog spinlock) */
#include <stdlib.h>
#include <stdio.h>   /* vsnprintf (alog) */
#include <string.h>
#include <stdint.h>
#include <math.h>

#include "javacall_multimedia.h"
#include "javacall_defs.h"
#include "javacall_memory.h"
#include "javanotify_multimedia.h"
#include "vita_midi.h"      /* v02.60: SMF (MIDI) playback */
#include "vita_pcm_dec.h"   /* v02.88: MP3 / AMR-NB / AAC decoders */

/* v01.96: WAV (RIFF/PCM) support, implemented in vita_wav.c */
typedef struct {
    long data_off, data_len, rate;
    int  channels, bits;
} wav_info;
int  vita_wav_parse_header(const uint8_t *buf, long len, wav_info *info);
long vita_wav_duration_ms(const wav_info *info);
javacall_result vita_port_media_download_handled_by_device(
        javacall_handle handle, /*OUT*/ javacall_bool* isHandled);

/* Stub override: Java MediaDownload must stream data to us (see
 * vita_wav.c for the full stub-vs-KNI-default-TRUE analysis). */
javacall_result javacall_media_download_handled_by_device(
        javacall_handle handle, /*OUT*/ javacall_bool* isHandled) {
    return vita_port_media_download_handled_by_device(handle, isHandled);
}

/* ---------------------------------------------------------------------------
 * Audio diagnosis log (independent fd, not affected by VM stderr redirect)
 * ------------------------------------------------------------------------- */
#include <psp2/io/fcntl.h>
#include <psp2/kernel/processmgr.h>   /* sceKernelGetProcessTimeWide: v03.12 timing probe */
#include <stdarg.h>
#include <sys/time.h>
#include "vita_wav_resample.h"   /* v03.14: exact-ratio linear-interpolating rate conversion */
/* v03.13: wall clock in ms, the SAME source as the HEARTBEAT ms= field in input_debug.log
 * (gettimeofday), so a pause/resume line here can be placed on the heartbeat timeline. */
static long long wall_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (long long)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}
static SceUID alog_fd = -2;
static void alog(const char *fmt, ...) {
    /* v01.76: atomic test-and-set spinlock. The old "while (lock) ...; 
     * lock = 1;" was check-then-set - NOT atomic on the Vita's 3 user
     * cores, and alog IS called concurrently (VM thread logs play_tone,
     * tone thread logs req/done). A garbled line in audio_debug.log
     * would poison the very evidence the hang hunt depends on. */
    static volatile int lock = 0;
    char buf[192];
    va_list ap;
    int n;
    if (alog_fd == -2) {
        /* APPEND, not TRUNC: the launcher process survives across MIDlet
         * rounds and users hard-restart between tests. TRUNC destroyed
         * the ToneTest session's records when the next app (UC) started -
         * the single most valuable piece of audio evidence was lost. */
        alog_fd = sceIoOpen("ux0:/data/J2ME00001/audio_debug.log",
                            SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0777);
    }
    if (alog_fd < 0) return;
    while (__sync_lock_test_and_set(&lock, 1)) {
        sceKernelDelayThread(100);
    }
    va_start(ap, fmt);
    n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n > 0) {
        /* v03.12 probe: measure what one synchronous log write really costs on this device
         * (log124 estimate was 9-22 ms from older logs, never measured with the VM thread
         * waiting). Totals are reported once per 64 writes. */
        static unsigned long long w_sum_us, w_max_us;
        static unsigned w_cnt;
        unsigned long long t0 = sceKernelGetProcessTimeWide(), dt;
        sceIoWrite(alog_fd, buf, n);
        dt = sceKernelGetProcessTimeWide() - t0;
        w_sum_us += dt;
        if (dt > w_max_us) w_max_us = dt;
        if (++w_cnt == 64) {
            char sbuf[96];
            int sn = snprintf(sbuf, sizeof(sbuf),
                              "[T] t=%lld 64 log writes: avg %llu us max %llu us\n",
                              wall_ms(), w_sum_us / 64, w_max_us);
            if (sn > 0) sceIoWrite(alog_fd, sbuf, sn);
            w_sum_us = w_max_us = 0; w_cnt = 0;
        }
    }
    __sync_lock_release(&lock);
}
#define ALOG(name) alog("[IN ] %s\n", name)
/* v02.64: per-call logging of polled entry points. Every alog() line is a
 * synchronous sceIoWrite (12-22 ms measured on this device); log110 had
 * 2757 of them from the VM thread (get_time 1231, get_duration 607, ...).
 * Log only every 64th call, with the running count. */
#define ALOG_HOT(name) do { static unsigned _hot_n; \
        if ((_hot_n++ & 63u) == 0) \
            alog("[IN ] %s (call #%u)\n", name, _hot_n); } while (0)


/* ---------------------------------------------------------------------------
 * Tone generation constants
 * ------------------------------------------------------------------------- */
#define TONE_SAMPLE_RATE      16000
#define OUT_SAMPLE_RATE       48000
#define MAX_TONE_DURATION_MS  10000
#define MAX_TONE_SAMPLES      (TONE_SAMPLE_RATE * MAX_TONE_DURATION_MS / 1000)
#define OUT_CHUNK_FRAMES      512

/* ---------------------------------------------------------------------------
 * JTS (Java Tone Sequence) constants - see javax.microedition.media.control
 * .ToneControl. Every element is a (control, parameter) byte pair.
 * ------------------------------------------------------------------------- */
#define JTS_SILENCE     (-1)
#define JTS_VERSION     (-2)
#define JTS_TEMPO       (-3)
#define JTS_RESOLUTION  (-4)
#define JTS_BLOCK_START (-5)
#define JTS_BLOCK_END   (-6)
#define JTS_PLAY_BLOCK  (-7)
#define JTS_SET_VOLUME  (-8)
#define JTS_REPEAT      (-9)

#define JTS_MAX_SIZE        32768   /* native buffer offered to Java layer */
#define JTS_FIRST_CHUNK     1024    /* first packet size reported to Java  */
#define JTS_MAX_REPEAT      8       /* REPEAT expansion guard              */
#define JTS_MAX_EVENTS      4096    /* expanded event list guard           */
/* v01.97: hard ceiling for the dynamically grown WAV buffer (32 MB
 * ~ 3 minutes of 44.1 kHz stereo 16-bit; allocated on demand only). */
#define WAV_MAX_BUFFER      (32 * 1024 * 1024)
/* v02.64: ceiling / initial size of the MIDI accumulator (a Standard MIDI
 * File is tens of KB; 4 MB is far beyond any J2ME game). */
#define MIDI_MAX_BUFFER     (4 * 1024 * 1024)
#define MIDI_INIT_CAP       (64 * 1024)
/* v02.66: WAV accumulator. Same aliasing problem as MIDI (see the
 * vita_player comment); the ceiling stays WAV_MAX_BUFFER above. */
#define WAV_INIT_CAP        (64 * 1024)

/* One expanded playback event */
typedef struct {
    int8_t  note;      /* MIDI note 0..127, or JTS_SILENCE for a rest */
    int32_t duration;  /* milliseconds */
    int32_t volume;    /* 0..100 */
} jts_event;

/* WAV playback state, filled by do_buffering (see vita_wav.c) */
typedef struct {
    int      valid;        /* header parsed, ready to play           */
    long     data_off;     /* PCM payload offset within wav_data     */
    long     data_len;     /* PCM payload bytes                      */
    long     rate;         /* sample rate                            */
    int      channels;     /* 1 or 2                                 */
    int      bits;         /* 8 or 16                                */
} wav_state;

/* Native player handle created by javacall_media_create */
typedef struct {
    int      app_id;
    int      player_id;
    int      state;        /* 0=idle 1=realized 2=prefetched 3=playing */
    /* v01.97: jts_data is heap-allocated (initial block JTS_MAX_SIZE,
     * grown on demand for large WAV in do_buffering). JTS sequences
     * still fit the initial block, so their path only gains an
     * indirection; the struct shrinks from ~32.5 KB to ~200 bytes. */
    uint8_t  *jts_data;    /* fixed JTS_MAX_SIZE: KNI staging only */
    long     jts_len;      /* bytes buffered */
    long     whole_size;   /* set via javacall_media_set_whole_content_size */
    long     duration_ms;  /* computed total sequence duration, -1 unknown */
    long     volume;       /* 0..100 */
    javacall_bool mute;
    /* v01.96: WAV (audio/x-wav) support */
    wav_state wav;
    uint32_t  wav_buf_len; /* accumulated bytes in wav_acc for WAV    */
    /* v02.66: same KNI aliasing as MIDI (get_buffer_address hands out
     * jts_data, KNI copies each Java window to its START, then calls
     * do_buffering with buffer == jts_data). Appending to
     * jts_data + wav_buf_len therefore clobbered the bytes already
     * collected from the 2nd 4 KB chunk on, and wav_acc_ensure's realloc
     * could MOVE jts_data while `buffer` still pointed at the freed block
     * (use-after-free). Long WAVs never worked; no archived log has a
     * "wav buffered" line to prove otherwise. */
    uint8_t  *wav_acc;
    long      wav_acap;
    /* v02.60: MIDI (audio/midi) support - SMF bytes accumulate in
     * jts_data exactly like WAV; midi is the MIME discriminator set in
     * realize(), midi_len the byte count. */
    int      midi;        /* 1 = this player is a MIDI file           */
    uint32_t midi_len;    /* accumulated SMF bytes in midi_acc        */
    /* v02.64: the SMF accumulates in its OWN heap block. KNI copies every
     * Java window to the START of jts_data (the buffer we hand out in
     * get_buffer_address) and then calls do_buffering with buffer ==
     * jts_data; appending to jts_data + midi_len therefore overwrote the
     * bytes accumulated so far (MThd header included) from the 2nd chunk
     * on. log110: 198x "[MIDI] parse FAILED (29139 bytes)". jts_data is now
     * only the fixed KNI staging buffer for MIDI. */
    uint8_t *midi_acc;
    long     midi_cap;
    int      midi_bad;    /* parse failed once - don't retry/log again */
    /* v02.88: compressed audio with its own decoder (MP3 / AMR-NB /
     * AAC). All three share one path: the stream is accumulated during
     * do_buffering, the decoder is created at the end-of-buffering
     * marker, and the player thread pulls PCM from it through
     * vita_pcm_dec.read(). Adding a format is one file, not another copy
     * of the render loop. */
    const vita_pcm_dec *pcm;   /* NULL = not this kind of player     */
    void    *pcm_dec;          /* decoder handle                     */
    uint32_t pcm_len;          /* compressed bytes accepted          */
    uint32_t pcm_mem;          /* bytes of it kept in wav_acc        */
    SceUID   pcm_fd;           /* spill-to-file handle (-1 = none)    */
    char     pcm_tmp[48];      /* spill path (per player - two players
                                * must not share one file)           */
    long     pcm_seek_ms;      /* pending seek request (-1 = none)   */
    /* playback progress (touched by player thread) */
    volatile int      playing;
    volatile long     cur_time_ms;
    volatile long     stop_flag;
    /* v03.13: pause/resume timing probe (see javacall_media_pause) */
    volatile long long pause_req_ms;     /* wall ms when pause()/stop() set stop_flag, 0 = none  */
    volatile long long play_start_ms;    /* wall ms when start() queued the render request        */
    volatile long      pause_n;          /* pause() calls on this player                          */
} vita_player;

/* ---------------------------------------------------------------------------
 * State
 * ------------------------------------------------------------------------- */
static int g_audio_port = -1;
/* v04.35: set while the system UI covers us (or the app is in the background).
 * The render loop then stops submitting chunks instead of fighting a port the
 * system has taken; see vita_lifecycle.c for the detector. */
static volatile int g_audio_suspended = 0;
static volatile int g_tone_stop_requested = 0;

static int16_t g_tone_buffer[MAX_TONE_SAMPLES];
static int g_tone_len = 0;

static int16_t g_out_buffer[OUT_CHUNK_FRAMES * 2];

/* v03.22: device-side PCM capture and submission-interval histogram (vita_pcm_probe.h). 1.2 MB of .bss. */
#include "vita_pcm_probe.h"
static vita_pcm_probe g_pcm_probe;

/* Print the interval histogram for the run that just ended, and write the captured PCM once per process.
 * File: ux0:/data/J2ME00001/pcm_dump.raw = raw s16le stereo 48000 Hz, the first VPP_CAPTURE_CHUNKS chunks that went to the
 * DAC (6.4 s). Analyse with debug/tools/wavq/analyze_dump.py. Written from the audio thread AFTER playback stopped, so it
 * cannot disturb the timing it measures. */
static void pcm_probe_report(const char *tag) {
    static const char *names[VPP_HIST_BUCKETS] = {
        "<9.0", "9-10", "10-10.5", "10.5-10.9", "10.9-11.5", "11.5-13", "13-16", "16-25", "25-50", ">=50"
    };
    char line[400];
    int n, i;
    if (g_pcm_probe.n_intervals == 0) return;
    n = snprintf(line, sizeof(line), "[PCM] %s submissions=%llu intervals(ms):", tag, g_pcm_probe.n_chunks);
    for (i = 0; i < VPP_HIST_BUCKETS && n < (int)sizeof(line) - 24; i++)
        n += snprintf(line + n, sizeof(line) - n, " %s=%llu", names[i], g_pcm_probe.hist[i]);
    snprintf(line + n, sizeof(line) - n, " max=%llu us, quick-returns(<2ms)=%llu\n",
             g_pcm_probe.max_interval_us, g_pcm_probe.n_blocked_short);
    alog("%s", line);
    /* reset the counters for the next tune; keep the capture buffer */
    memset(g_pcm_probe.hist, 0, sizeof(g_pcm_probe.hist));
    g_pcm_probe.n_intervals = g_pcm_probe.max_interval_us = g_pcm_probe.n_blocked_short = g_pcm_probe.n_chunks = 0;
    if (g_pcm_probe.cap_chunks > 0 && !g_pcm_probe.cap_written &&
        (g_pcm_probe.cap_ready || g_pcm_probe.cap_chunks >= 100)) {
        SceUID fd = sceIoOpen("ux0:/data/J2ME00001/pcm_dump.raw", SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
        if (fd >= 0) {
            int c;
            long long total = 0;
            for (c = 0; c < g_pcm_probe.cap_chunks; c++) {
                int w = sceIoWrite(fd, g_pcm_probe.cap[c], VPP_CHUNK_FRAMES * 2 * sizeof(int16_t));
                if (w > 0) total += w;
            }
            sceIoClose(fd);
            alog("[PCM] dump written: %d chunks, %lld bytes\n", g_pcm_probe.cap_chunks, total);
        } else {
            alog("[PCM] dump open FAILED 0x%08x\n", fd);
        }
        g_pcm_probe.cap_written = 1;
    }
}

/* Player volume shared by playTone and Player-based playback */
static volatile long g_master_volume = 100;

/*
 * playTone must be ASYNC: the CLDC VM uses green threads, so blocking
 * inside nPlayTone freezes every Java thread (games call it from their
 * main loop for key clicks). The producer (VM thread) only bumps a
 * sequence number; a dedicated player thread generates and streams the
 * PCM, preempting itself when a newer request arrives.
 */
#include <psp2/kernel/threadmgr.h>

static volatile long g_req_note = -1, g_req_dur = 0, g_req_vol = 0;
static volatile unsigned int g_req_seq = 0, g_played_seq = 0;

/* JTS playback request (Player.start) - same handshake pattern */
static vita_player *g_jts_req = NULL;          /* sequence to play next   */
static volatile unsigned int g_jts_seq = 0;    /* bumped on each request  */
static volatile unsigned int g_jts_done = 0;   /* last finished request   */

/* WAV playback request (Player.start) - same handshake pattern.
 * A player is either JTS or WAV (mutually exclusive by content type),
 * so the WAV fields of the vita_player are stable once start is given. */
static vita_player *g_wav_req = NULL;          /* player to play next     */
static volatile unsigned int g_wav_seq = 0;    /* bumped on each request  */
static volatile unsigned int g_wav_done = 0;   /* last finished request   */

/* MIDI playback request (Player.start) - same handshake pattern. */
static vita_player *g_midi_req = NULL;         /* player to play next     */
static volatile unsigned int g_midi_seq = 0;   /* bumped on each request  */
static volatile unsigned int g_midi_done = 0;  /* last finished request   */

/* v02.87: MP3 playback request. v02.88: shared by every decoder that
 * goes through vita_pcm_dec (MP3, AMR-NB, AAC). */
static vita_player *g_pcm_req = NULL;
static volatile unsigned int g_pcm_seq = 0;
static volatile unsigned int g_pcm_done = 0;

/* ---------------------------------------------------------------------------
 * caps / configuration (static, tone-only)
 * ------------------------------------------------------------------------- */
static const char g_tone_content_type[] = "audio/x-tone-seq";

/* KNIDirectConfig walks mediaCaps as an ARRAY terminated by an
 * all-zero entry (mediaFormat == 0 / contentTypes == NULL). A single
 * element made caps++ read whatever follows in .data - strlen() on a
 * wild pointer froze the VM the moment the game called createPlayer
 * (music toggle). Always keep the terminator. */
static javacall_media_caps g_caps_tone[] = {
    { JAVACALL_MEDIA_FORMAT_TONE,
      g_tone_content_type,
      JAVACALL_MEDIA_MEMORY_PROTOCOL | JAVACALL_MEDIA_FILE_LOCAL_PROTOCOL,
      0 },
    /* v01.96: WAV PCM. MS_PCM matches JAVACALL_MEDIA_FORMAT_MS_PCM;
     * HighLevelPlayer throws Unsupported media format unless this call
     * returns a known format string, and KNIMediaDownload needs the
     * memory protocol to stream bytes via nBuffering. */
    { JAVACALL_MEDIA_FORMAT_MS_PCM,
      "audio/x-wav",
      JAVACALL_MEDIA_MEMORY_PROTOCOL | JAVACALL_MEDIA_FILE_LOCAL_PROTOCOL,
      0 },
    /* v02.60: MIDI (audio/midi). Manager.createPlayer(DataSource) checks
     * getSupportedContentTypes(protocol) against the javacall caps, so
     * without this entry the game's BGM .mid fails with
     * "Player cannot be created for audio/midi" (log97). The bytes are
     * streamed via nBuffering like WAV and synthesized in vita_midi.c. */
    { JAVACALL_MEDIA_FORMAT_MIDI,
      "audio/midi",
      JAVACALL_MEDIA_MEMORY_PROTOCOL | JAVACALL_MEDIA_FILE_LOCAL_PROTOCOL,
      0 },
    /* v02.87: MP3. Three content-type spellings because games use all of
     * them (the MIME check in realize() below accepts the same set).
     * JAVACALL_MEDIA_FORMAT_MS_PCM is the closest formal slot: phoneME's
     * media format enum has no MP3/AMR/AAC member, and the Java layer only
     * compares the content-type strings it got from this table. The
     * decoder is selected by the stream header, not by this number. */
    { JAVACALL_MEDIA_FORMAT_MS_PCM,
      "audio/mpeg",
      JAVACALL_MEDIA_MEMORY_PROTOCOL | JAVACALL_MEDIA_FILE_LOCAL_PROTOCOL,
      0 },
    /* v02.88: AMR-NB - the other format J2ME games really ship (voice
     * clips, short sound effects). Decoded by the 3GPP reference codec
     * in src/amr3gpp via vita_amr.c. */
    { JAVACALL_MEDIA_FORMAT_MS_PCM,
      "audio/amr",
      JAVACALL_MEDIA_MEMORY_PROTOCOL | JAVACALL_MEDIA_FILE_LOCAL_PROTOCOL,
      0 },
    /* v02.88: AAC (ADTS) through the console's own decoder block
     * (sceAudiodec). Unsupported in emulators that lack SceAudiodec - the
     * probe in vita_aac.c then fails and the player stays silent instead
     * of crashing. */
    { JAVACALL_MEDIA_FORMAT_MS_PCM,
      "audio/aac",
      JAVACALL_MEDIA_MEMORY_PROTOCOL | JAVACALL_MEDIA_FILE_LOCAL_PROTOCOL,
      0 },
    /* v04.45: Ogg Vorbis via stb_vorbis. Two spellings: JSR-135 games ask
     * for "audio/ogg", .jad/.jar descriptors sometimes say "application/ogg".
     * Decoded natively on both hardware and Vita3K (no sceAudiodec). */
    { JAVACALL_MEDIA_FORMAT_MS_PCM,
      "audio/ogg",
      JAVACALL_MEDIA_MEMORY_PROTOCOL | JAVACALL_MEDIA_FILE_LOCAL_PROTOCOL,
      0 },
    { JAVACALL_MEDIA_FORMAT_MS_PCM,
      "application/ogg",
      JAVACALL_MEDIA_MEMORY_PROTOCOL | JAVACALL_MEDIA_FILE_LOCAL_PROTOCOL,
      0 },
    { 0, NULL, 0, 0 } /* array terminator - do not remove */
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

/*
 * v04.32: called from vita_lifecycle.c when the console reports a resume.
 *
 * sceAudioOut ports are bound to the suspend cycle: a handle opened before
 * the suspend keeps returning an error from sceAudioOutOutput() afterwards.
 * Because audio_out_open() only opens a port when g_audio_port < 0, the
 * stale handle was reused forever and ALL audio stayed dead for the rest of
 * the session (the game keeps calling output; every call fails silently).
 * Drop the stale port and open a fresh one.
 */
void vita_audio_after_resume(void) {
    int old = g_audio_port;

    g_audio_port = -1;
    if (old >= 0) {
        sceAudioOutReleasePort(old);
    }
    if (audio_out_open() < 0) {
        alog("resume: audio reopen FAILED (old port %d)\n", old);
    } else {
        alog("resume: audio reopened (old port %d -> %d)\n", old, g_audio_port);
    }
}

/* v04.35: mute/unmute without touching the port. Called from vita_lifecycle.c
 * while the system UI is over us, so the render loop stops submitting and the
 * track resumes at the same position once the UI goes away. */
void vita_audio_set_suspended(int suspended) {
    g_audio_suspended = suspended ? 1 : 0;
}

/** Resample mono 16k buffer to 48k stereo chunks; block until done.
 *  Aborts early when stop is requested or a newer tone supersedes. */
static void output_tone_blocking(unsigned int my_seq) {
    int produced = 0;
    int chunks = 0;
    int rc;
    const int repeat = OUT_SAMPLE_RATE / TONE_SAMPLE_RATE; /* 3 */

    for (int i = 0; i < g_tone_len; i++) {
        if (g_tone_stop_requested || g_req_seq != my_seq) break;
        for (int r = 0; r < repeat; r++) {
            if (produced >= OUT_CHUNK_FRAMES) {
                rc = sceAudioOutOutput(g_audio_port, g_out_buffer);
                if (rc < 0) alog("out chunk rc=0x%08x\n", rc);
                chunks++;
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
        rc = sceAudioOutOutput(g_audio_port, g_out_buffer);
        if (rc < 0) alog("out tail rc=0x%08x\n", rc);
        chunks++;
    }
    alog("tone done: chunks=%d len=%d seq=%u\n", chunks, g_tone_len, my_seq);
}

/* ---------------------------------------------------------------------------
 * JTS parsing
 * ------------------------------------------------------------------------- */

/** Validate a JTS byte sequence (mirrors upstream ToneControl grammar). */
static int jts_check_sequence(const uint8_t *seq, long len) {
    long pos = 0;
    int blk = -1;
    uint8_t blk_defined[128];
    int tmp_defined = 0, res_defined = 0;

    memset(blk_defined, 0, sizeof(blk_defined));
    if (len < 2) return 0;

    while (pos < len) {
        int t = (int8_t)seq[pos++];
        switch (t) {
        case JTS_VERSION:
            if (pos >= len || seq[pos] != 1) return 0;
            pos++;
            break;
        case JTS_TEMPO:
            if (tmp_defined || pos >= len ||
                seq[pos] < 5 || seq[pos] > 127) return 0;
            tmp_defined = 1; pos++;
            break;
        case JTS_RESOLUTION:
            if (res_defined || pos >= len ||
                seq[pos] < 1 || seq[pos] > 127) return 0;
            res_defined = 1; pos++;
            break;
        case JTS_BLOCK_START:
            if (pos >= len || seq[pos] > 127) return 0;
            blk = seq[pos++];
            break;
        case JTS_BLOCK_END:
            if (blk < 0 || pos >= len || (int)seq[pos] != blk) return 0;
            blk_defined[blk] = 1; blk = -1; pos++;
            break;
        case JTS_PLAY_BLOCK:
            if (pos >= len || seq[pos] > 127 ||
                !blk_defined[seq[pos]]) return 0;
            pos++;
            break;
        case JTS_SET_VOLUME:
            if (pos >= len || seq[pos] > 100) return 0;
            pos++;
            break;
        case JTS_REPEAT:
            /* REPEAT multiplier tone_event (4 bytes total) */
            if (pos + 2 >= len) return 0;
            if (seq[pos] < 2 || seq[pos] > 127) return 0;   /* multiplier */
            {
                int evt = (int8_t)seq[pos + 1];
                if (evt != JTS_SILENCE && (evt < 0 || evt > 127)) return 0;
                if (evt != JTS_SILENCE &&
                    (seq[pos + 2] < 1 || seq[pos + 2] > 127)) return 0;
            }
            pos += 3;
            break;
        case JTS_SILENCE:
            if (pos >= len || seq[pos] < 1 || seq[pos] > 127) return 0;
            pos++;
            break;
        default:
            if (t < 0 || t > 127) return 0;              /* invalid note */
            if (pos >= len || seq[pos] < 1 || seq[pos] > 127) return 0;
            pos++;
            break;
        }
    }
    return 1;
}

/** Effective duration in ms of one (note,duration) event. */
static long jts_event_ms(int dur, int tempo, int resolution) {
    return (long)dur * 240000L / ((long)tempo * resolution);
}

/**
 * Expand a JTS byte sequence into a flat event list (blocks resolved,
 * REPEAT expanded). Returns event count, or -1 on error/overflow.
 */
static long jts_expand(const uint8_t *seq, long len,
                       jts_event *out, long max_events,
                       int *out_tempo, int *out_resolution) {
    int tempo = 120, resolution = 64;
    long n = 0;
    long pos = 0;

    /* block start offsets: block body runs from start+2 to end */
    long blk_start[128];
    int  blk_defined[128];
    memset(blk_defined, 0, sizeof(blk_defined));

    /* pass 1: locate block definitions */
    while (pos < len) {
        int t = (int8_t)seq[pos];
        switch (t) {
        case JTS_VERSION:    pos += 2; break;
        case JTS_TEMPO:      tempo = seq[pos+1]; pos += 2; break;
        case JTS_RESOLUTION: resolution = seq[pos+1]; pos += 2; break;
        case JTS_BLOCK_START:
            blk_start[seq[pos+1]] = pos;
            pos += 2;
            break;
        case JTS_BLOCK_END:
            blk_defined[seq[pos+1]] = 1;
            pos += 2;
            break;
        case JTS_PLAY_BLOCK: pos += 2; break;
        case JTS_SET_VOLUME: pos += 2; break;
        case JTS_REPEAT:     pos += 4; break;   /* multiplier + tone_event */
        case JTS_SILENCE:    pos += 2; break;
        default:             pos += 2; break;
        }
    }

    /* pass 2: emit events */
    pos = 0;
    while (pos < len) {
        int t = (int8_t)seq[pos];
        int p = seq[pos+1];
        pos += 2;

        switch (t) {
        case JTS_VERSION:
        case JTS_TEMPO:
        case JTS_RESOLUTION:
            break;
        case JTS_BLOCK_START:
        case JTS_BLOCK_END:
            break;
        case JTS_PLAY_BLOCK: {
            /* copy the block body as events */
            long i = blk_start[p] + 2;
            long end = blk_start[p] + 2;
            /* find matching BLOCK_END (REPEAT spans 4 bytes) */
            while (end + 1 < len &&
                   (int8_t)seq[end] != JTS_BLOCK_END)
                end += ((int8_t)seq[end] == JTS_REPEAT) ? 4 : 2;
            while (i < end) {
                int nt = (int8_t)seq[i];
                int np = seq[i+1];
                i += 2;
                if (nt == JTS_SET_VOLUME) {
                    if (n > 0) out[n-1].volume = np;
                    continue;
                }
                if (nt == JTS_REPEAT) {
                    /* REPEAT multiplier tone_event inside a block */
                    int reps = (np < JTS_MAX_REPEAT) ? np : JTS_MAX_REPEAT;
                    int et, ed;
                    if (np < 2 || i + 2 > end) return -1;
                    et = (int8_t)seq[i];
                    ed = seq[i+1];
                    i += 2;
                    if (et != JTS_SILENCE && (et < 0 || et > 127)) return -1;
                    for (int r = 0; r < reps; r++) {
                        if (n >= max_events) return -1;
                        out[n].note =
                            (et == JTS_SILENCE) ? JTS_SILENCE : (int8_t)et;
                        out[n].duration =
                            (int)jts_event_ms(ed, tempo, resolution);
                        out[n].volume = 100;
                        n++;
                    }
                    continue;
                }
                if (n >= max_events) return -1;
                out[n].note = (nt == JTS_SILENCE) ? JTS_SILENCE : (int8_t)nt;
                out[n].duration = (int)jts_event_ms(np, tempo, resolution);
                out[n].volume = 100;
                n++;
            }
            break;
        }
        case JTS_SET_VOLUME:
            if (n > 0) out[n-1].volume = p;
            break;
        case JTS_REPEAT: {
            /* REPEAT multiplier tone_event: emit the following
             * tone_event (note,dur) multiplier times, then skip it. */
            if (pos + 2 > len) return -1;
            if (p < 2 || p > 127) return -1;
            {
                int nt = (int8_t)seq[pos];
                int np = seq[pos + 1];
                int reps = (p < JTS_MAX_REPEAT) ? p : JTS_MAX_REPEAT;
                if (nt != JTS_SILENCE && (nt < 0 || nt > 127)) return -1;
                for (int r = 0; r < reps; r++) {
                    if (n >= max_events) return -1;
                    out[n].note = (nt == JTS_SILENCE) ? JTS_SILENCE : (int8_t)nt;
                    out[n].duration = (int)jts_event_ms(np, tempo, resolution);
                    out[n].volume = 100;
                    n++;
                }
            }
            pos += 2;   /* skip the repeated tone_event */
            break;
        }
        case JTS_SILENCE:
            if (n >= max_events) return -1;
            out[n].note = JTS_SILENCE;
            out[n].duration = (int)jts_event_ms(p, tempo, resolution);
            out[n].volume = 100;
            n++;
            break;
        default:
            if (n >= max_events) return -1;
            out[n].note = (int8_t)t;
            out[n].duration = (int)jts_event_ms(p, tempo, resolution);
            out[n].volume = 100;
            n++;
            break;
        }
    }

    *out_tempo = tempo;
    *out_resolution = resolution;
    return n;
}

/** Render one JTS event into g_tone_buffer. Returns sample count. */
static int jts_render_event(const jts_event *ev) {
    if (ev->note == JTS_SILENCE || ev->duration <= 0) {
        /* silence: zero-fill */
        int num = (int)((long long)TONE_SAMPLE_RATE * ev->duration / 1000);
        if (num > MAX_TONE_SAMPLES) num = MAX_TONE_SAMPLES;
        if (num > 0) memset(g_tone_buffer, 0, num * sizeof(int16_t));
        return num;
    }
    return generate_tone_pcm(midi_note_to_freq(ev->note),
                             ev->duration, ev->volume);
}

/** Stream one rendered event out through SceAudioOut. */
static void jts_output_event(int samples, unsigned int my_seq,
                             vita_player *pl) {
    int produced = 0;
    const int repeat = OUT_SAMPLE_RATE / TONE_SAMPLE_RATE; /* 3 */

    for (int i = 0; i < samples; i++) {
        if (g_tone_stop_requested || pl->stop_flag || g_jts_seq != my_seq)
            break;
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

/** Compute total duration of an expanded event list. */
static long jts_total_duration(const jts_event *evs, long n) {
    long total = 0;
    for (long i = 0; i < n; i++) total += evs[i].duration;
    return total;
}

/* ---------------------------------------------------------------------------
 * WAV playback (v01.96) - see vita_wav.c for the header parser.
 * PCM lives in pl->jts_data[pl->wav.data_off .. +data_len); the player
 * thread converts it chunk by chunk to 48 kHz stereo, so multi-MB BGM
 * never needs a second full-size buffer.
 * ------------------------------------------------------------------------- */

/** Volume-scaled sample fetch; 8-bit WAV is unsigned, 16-bit signed. */
static int32_t wav_sample_raw(const vita_player *pl, long frame, int ch) {
    /* v02.66: the PCM lives in the accumulator, not in the KNI staging
     * buffer (see the vita_player comment). */
    const uint8_t *base = pl->wav_acc + pl->wav.data_off;
    long bytes_per_frame = (long)pl->wav.channels * (pl->wav.bits >> 3);
    const uint8_t *p = base + frame * bytes_per_frame + ch * (pl->wav.bits >> 3);
    int32_t s;
    if (pl->wav.bits == 8) {
        s = ((int32_t)p[0] - 128) << 8;          /* u8 -> s16-ish */
    } else {
        s = (int32_t)(int16_t)(p[0] | (p[1] << 8));
    }
    return s;
}

/* v03.14: sample at a 32.32 fixed-point source position, linearly interpolated, then volume. */
static int32_t wav_sample_interp(const vita_player *pl, uint64_t pos_q32, long frames, int ch) {
    long i = (long)(pos_q32 >> 32);
    uint32_t frac16 = (uint32_t)((pos_q32 >> 16) & 0xFFFF);
    int32_t a, b, s;
    if (i >= frames - 1) {
        s = wav_sample_raw(pl, frames - 1, ch);
    } else {
        a = wav_sample_raw(pl, i, ch);
        b = wav_sample_raw(pl, i + 1, ch);
        s = vwr_lerp(a, b, frac16);
    }
    return (s * (int32_t)pl->volume) / 100;
}

/* v03.32: band-limited upsampler (vita_wav_resample.h). 32 KB, built on first use. */
static vwr_fir g_fir;
static int g_fir_ready = 0;

static int32_t wav_fir_get(const void *ctx, long frame, int ch) {
    return wav_sample_raw((const vita_player *)ctx, frame, ch);
}

/** Render a buffered WAV on the player thread; handshake like JTS. */
static void wav_render_player(vita_player *pl, unsigned int my_seq) {
    long frames, out_frames_total, rendered = 0;
    int bytes_per_frame;

    pl->playing = 1;
    /* v02.66: stop_flag is cleared by javacall_media_start(), not here. */
    pl->cur_time_ms = 0;

    if (audio_out_open() != 0) {
        pl->playing = 0;
        return;
    }

    bytes_per_frame = pl->wav.channels * (pl->wav.bits >> 3);
    frames = pl->wav.data_len / bytes_per_frame;
    /* v03.14: exact length. The old frames * (48000 / rate) truncated the ratio: 11025/22050/44100 Hz
     * played 8.8 % fast, 32000 Hz 50 % fast, > 48000 Hz not at all (see vita_wav_resample.h). */
    out_frames_total = vwr_out_frames(frames, pl->wav.rate, OUT_SAMPLE_RATE);
    {
    uint64_t pos = 0;
    const uint64_t pos_step = vwr_step_q32(pl->wav.rate, OUT_SAMPLE_RATE);
    /* v03.32: upsampling (rate < 48 kHz) goes through the polyphase FIR; the window of 32 source frames only
     * changes when the integer source index does (every 6th output frame at 8 kHz), so it is cached. */
    const int use_fir = pl->wav.rate < OUT_SAMPLE_RATE;
    const int nch = (pl->wav.channels == 2) ? 2 : 1;
    int32_t fir_win[2][VWR_TAPS];
    long fir_i = -0x40000000L;
    if (use_fir && !g_fir_ready) {
        vwr_fir_init(&g_fir);
        g_fir_ready = 1;
    }
    vpp_reset_run(&g_pcm_probe);      /* the gap between two different tunes is not an underrun */

    while (rendered < out_frames_total) {
        int produced = 0;
        if (pl->stop_flag || g_wav_seq != my_seq) break;
        while (produced < OUT_CHUNK_FRAMES && rendered < out_frames_total) {
            /* v03.14: source position as 32.32 fixed point = rendered * rate / 48000, interpolated
             * linearly between the two neighbouring source frames (was: nearest sample, which
             * keeps every image tone of the zero-order hold: SNR 8 dB on an 8 kHz source).
             * 64-bit throughout; long is 32 bits here (v03.12 fixed the 2^31 overflow). */
            int32_t l, r;
            if (use_fir) {
                long si; unsigned ph; int c;
                vwr_fir_locate(pos, &si, &ph);
                if (si != fir_i) {
                    for (c = 0; c < nch; c++) vwr_fir_window(fir_win[c], si, frames, wav_fir_get, pl, c);
                    fir_i = si;
                }
                l = (vwr_fir_dot(&g_fir, ph, fir_win[0]) * (int32_t)pl->volume) / 100;
                r = (nch == 2) ? (vwr_fir_dot(&g_fir, ph, fir_win[1]) * (int32_t)pl->volume) / 100 : l;
            } else if (pl->wav.channels == 2) {
                l = wav_sample_interp(pl, pos, frames, 0);
                r = wav_sample_interp(pl, pos, frames, 1);
            } else {
                l = wav_sample_interp(pl, pos, frames, 0);
                r = l;
            }
            g_out_buffer[produced*2+0] = (int16_t)l;
            g_out_buffer[produced*2+1] = (int16_t)r;
            produced++;
            rendered++;
            pos += pos_step;
        }
        if (produced < OUT_CHUNK_FRAMES) {
            memset(&g_out_buffer[produced*2], 0,
                   (OUT_CHUNK_FRAMES-produced) * 2 * sizeof(int16_t));
        }
        {
            unsigned long long o0 = sceKernelGetProcessTimeWide(), od;
            /* v04.35: while the system UI is up, do not submit. Sleeping one
             * chunk and re-checking keeps the tune paused where it is (the
             * loop condition then re-tests stop_flag) instead of racing the
             * whole track into a port that is not listening. */
            if (g_audio_suspended) {
                sceKernelDelayThread(10 * 1000);
                continue;
            }
            /* v03.22: device-side capture + submission-interval histogram (see vita_pcm_probe.h) */
            vpp_before_submit(&g_pcm_probe, g_out_buffer, o0);
            sceAudioOutOutput(g_audio_port, g_out_buffer);
            od = sceKernelGetProcessTimeWide() - o0;
            vpp_after_submit(&g_pcm_probe, od);
            /* one 512-frame chunk at 48 kHz is 10.7 ms; the call normally returns after about that.
             * > 50 ms means the audio port stalled (underrun / the system took the port). */
            if (od > 50000ULL)
                alog("[PR] t=%lld sceAudioOutOutput blocked %llu us at out frame %ld\n", wall_ms(), od, rendered);
        }
        pl->cur_time_ms = (long)(((long long)rendered * 1000) / OUT_SAMPLE_RATE);
    }
    }   /* pos / pos_step scope */

    /* v03.15: de-click a pause/stop. The loop above leaves at a chunk boundary, mid-note, and nothing
     * follows, so the DAC goes from the square level (+-11264, 34 % of full scale) to 0 in one sample:
     * a broadband pop. 91 % of cuts land on full-level sound (176 ms notes, 2 ms fades) and log126 had
     * 10 pauses. Ramp the last submitted sample to 0 over one more 512-frame chunk (10.7 ms) instead.
     * Only for stop_flag / superseded exits; a natural end already finishes on the note's fade. */
    if (rendered > 0 && rendered < out_frames_total && (pl->stop_flag || g_wav_seq != my_seq)) {
        int32_t lastl = g_out_buffer[(OUT_CHUNK_FRAMES - 1) * 2 + 0];
        int32_t lastr = g_out_buffer[(OUT_CHUNK_FRAMES - 1) * 2 + 1];
        /* The loop only exits between whole chunks, so g_out_buffer still holds the last full chunk. */
        vwr_fadeout(g_out_buffer, OUT_CHUNK_FRAMES, lastl, lastr);
        sceAudioOutOutput(g_audio_port, g_out_buffer);
    }

    pl->playing = 0;
    pl->cur_time_ms = pl->duration_ms;
    /* v03.13 probe: the render thread has left its loop. If a pause/stop was requested,
     * report how long that took (the loop only checks stop_flag once per 512-frame chunk,
     * ~10 ms, so anything much larger means sceAudioOutOutput blocked). */
    if (pl->stop_flag && pl->pause_req_ms) {
        long long now = wall_ms();
        alog("[PR] t=%lld render stopped, %lldms after the request, at out frame %ld of %ld\n",
             now, now - pl->pause_req_ms, rendered, out_frames_total);
    }

    if (!pl->stop_flag && g_wav_seq == my_seq) {
        ALOG("notify END_OF_MEDIA (wav done)");
        javanotify_on_media_notification(
            JAVACALL_EVENT_MEDIA_END_OF_MEDIA,
            pl->app_id, pl->player_id, JAVACALL_OK,
            (void *)(long)(pl->duration_ms / 1000));
    }
    pl->stop_flag = 0;
    alog("wav done: frames=%ld dur=%ld seq=%u\n", frames, pl->duration_ms, my_seq);
    pcm_probe_report("wav");
}

/* v02.88: byte source for the PCM decoders.
 *
 * The compressed stream arrives from Java in 4 KB packets during
 * do_buffering, long before start() is called - MMAPI cannot begin
 * playback earlier, because nPrefetch() only returns once the download
 * loop reports EOF. So the stream cannot be "streamed" from the network,
 * but it also must not sit in RAM: a 3 MB MP3 used to be pinned in the
 * PCSL heap for the lifetime of the player.
 *
 * Compromise (the "ring buffer"): the first PCM_MEM_KEEP bytes stay in
 * wav_acc, everything after that is appended to a temp file on ux0:. The
 * decoder then reads through a small window (vita_src), so at most
 * PCM_MEM_KEEP + window + one frame is resident instead of the whole
 * file. If the temp file cannot be created (read-only card) we fall back
 * to keeping everything in memory - the old behaviour, never a failure. */
#define PCM_MEM_KEEP   (512 * 1024)
#define PCM_TMP_FMT    "ux0:/data/J2ME00001/.pcm%d.tmp"

static void pcm_spill_close(vita_player *pl) {
    if (pl->pcm_fd >= 0) {
        sceIoClose(pl->pcm_fd);
        pl->pcm_fd = -1;
        if (pl->pcm_tmp[0] != '\0') {
            sceIoRemove(pl->pcm_tmp);
            pl->pcm_tmp[0] = '\0';
        }
    }
}

/* Feed the decoder both halves of the stream as one contiguous file. */
static long pcm_src_read(void *ctx, long off, uint8_t *dst, long n) {
    vita_player *pl = (vita_player *)ctx;
    long got = 0;

    if (pl == NULL || dst == NULL || n <= 0 || off < 0) return 0;

    if (off < (long)pl->pcm_mem) {
        long take = (long)pl->pcm_mem - off;
        if (take > n) take = n;
        if (pl->wav_acc != NULL) {
            memcpy(dst, pl->wav_acc + off, (size_t)take);
        } else {
            memset(dst, 0, (size_t)take);
        }
        got = take;
        if (got == n) return got;
        off += got;
    }
    if (pl->pcm_fd >= 0) {
        long want = n - got;
        long foff = off - (long)pl->pcm_mem;
        long r = sceIoPread(pl->pcm_fd, dst + got, want, foff);
        if (r > 0) got += r;
    }
    return got;
}

/* v02.88: playback for every vita_pcm_dec decoder (MP3 / AMR-NB / AAC).
 *
 * Each chunk is rendered at 48 kHz stereo, exactly like WAV, so
 * sceAudioOutOutput() paces the loop to real time. The decoder itself
 * only keeps one frame of PCM, so this is O(1) memory whatever the
 * source rate or length. */
static void pcm_render_player(vita_player *pl, unsigned int my_seq) {
    int16_t *chunk = (int16_t *)javacall_malloc(OUT_CHUNK_FRAMES * 2 *
                                                sizeof(int16_t));

    pl->playing = 1;
    pl->cur_time_ms = 0;

    if (chunk == NULL || pl->pcm == NULL || pl->pcm_dec == NULL ||
        audio_out_open() != 0) {
        if (chunk != NULL) javacall_free(chunk);
        pl->playing = 0;
        return;
    }

    for (;;) {
        long got;

        if (pl->stop_flag || g_pcm_seq != my_seq) {
            break;
        }

        /* A setMediaTime() from the VM thread is applied here, so the
         * decoder is never touched from two threads at once. */
        if (pl->pcm_seek_ms >= 0) {
            long want = pl->pcm_seek_ms;
            pl->pcm_seek_ms = -1;
            if (pl->pcm->seek_ms != NULL &&
                pl->pcm->seek_ms(pl->pcm_dec, want)) {
                pl->cur_time_ms = want;
                alog("pcm seek -> %ld ms\n", want);
            } else {
                pl->pcm->rewind(pl->pcm_dec);
                pl->cur_time_ms = 0;
                alog("pcm seek FAILED (%ld ms), rewound\n", want);
            }
        }

        got = pl->pcm->read(pl->pcm_dec, chunk, OUT_CHUNK_FRAMES,
                            pl->volume);
        if (got <= 0) {
            break;   /* end of stream */
        }
        if (got < OUT_CHUNK_FRAMES) {
            memset(chunk + got * 2, 0,
                   (size_t)(OUT_CHUNK_FRAMES - got) * 2 * sizeof(int16_t));
        }
        if (pl->mute) {
            /* Same reason as MIDI: without the output call nothing paces
             * the loop, so a muted song would render in seconds and pin a
             * core. Sleep one chunk's worth of real time instead. */
            sceKernelDelayThread((unsigned int)((unsigned long long)
                                                OUT_CHUNK_FRAMES * 1000000ULL /
                                                OUT_SAMPLE_RATE));
        } else {
            sceAudioOutOutput(g_audio_port, chunk);
        }
        pl->cur_time_ms += (long)((long long)got * 1000 / OUT_SAMPLE_RATE);
    }

    javacall_free(chunk);
    pl->playing = 0;
    if (!pl->stop_flag && g_pcm_seq == my_seq) {
        pl->cur_time_ms = pl->duration_ms;
        ALOG("notify END_OF_MEDIA (pcm done)");
        javanotify_on_media_notification(
            JAVACALL_EVENT_MEDIA_END_OF_MEDIA,
            pl->app_id, pl->player_id, JAVACALL_OK,
            (void *)(long)(pl->duration_ms / 1000));
    }
    pl->stop_flag = 0;
}

/* v01.72: the tone player thread's id, published for the hang
 * watchdog (vita_watchdog.c). -1 until javacall_media_initialize runs. */
volatile SceUID vita_tone_tid = -1;
/* v02.60: MIDI playback callbacks for vita_midi_play. The context
 * carries the player (for mute/stop_flag) and the request sequence so a
 * newer start() can preempt the current render. */
typedef struct {
    vita_player *pl;
    unsigned int my_seq;
    unsigned long frames;     /* v02.65: frames handed out so far */
} midi_ctx_t;

static void midi_out_cb(const int16_t *stereo, int frames, void *ctx) {
    midi_ctx_t *mc = (midi_ctx_t *)ctx;
    /* v02.65: keep getMediaTime() moving. cur_time_ms used to stay 0 for the
     * whole song (only set to the duration at the end), so a game that waits
     * for or syncs to the media clock saw it frozen. */
    mc->frames += (unsigned long)frames;
    mc->pl->cur_time_ms =
        (long)(((unsigned long long)mc->frames * 1000ULL) / OUT_SAMPLE_RATE);
    if (mc->pl->mute) {
        /* v02.65: sceAudioOutOutput() is what paces the render loop to real
         * time. A muted player skipped it and rendered flat out (a whole song
         * in seconds, one core pinned) - sleep one chunk instead. */
        sceKernelDelayThread((unsigned int)((unsigned long long)frames *
                                            1000000ULL / OUT_SAMPLE_RATE));
        return;
    }
    /* v03.37: same device-side probe as the WAV path (interval histogram + first 6.4 s dump, vita_pcm_probe.h). */
    {
        unsigned long long o0 = sceKernelGetProcessTimeWide();
        vpp_before_submit(&g_pcm_probe, stereo, o0);
        sceAudioOutOutput(g_audio_port, stereo);
        vpp_after_submit(&g_pcm_probe, sceKernelGetProcessTimeWide() - o0);
    }
}

static int midi_stop_cb(void *ctx) {
    midi_ctx_t *mc = (midi_ctx_t *)ctx;
    return (mc->pl->stop_flag != 0) || (g_midi_seq != mc->my_seq);
}

/* Dedicated tone player thread: waits for requests, streams them out.
 * Handles simple playTone, JTS sequence, WAV and MIDI playback. */
static int tone_player_thread(SceSize args, void *argp) {
    (void)args; (void)argp;

    static jts_event events[JTS_MAX_EVENTS];

    for (;;) {
        /* --- JTS playback request? --- */
        if (g_jts_seq != g_jts_done) {
            vita_player *pl = g_jts_req;
            unsigned int my_seq = g_jts_seq;
            g_jts_done = my_seq;

            if (pl != NULL && pl->jts_len >= 2) {
                int tempo_dummy = 0, res_dummy = 0;
                long n = jts_expand(pl->jts_data, pl->jts_len,
                                    events, JTS_MAX_EVENTS,
                                    &tempo_dummy, &res_dummy);
                if (n > 0) {
                    pl->playing = 1;
                    /* v02.66: stop_flag is cleared by javacall_media_start(),
                     * not here - see the comment there. */
                    pl->cur_time_ms = 0;
                    if (audio_out_open() == 0) {
                        for (long i = 0; i < n; i++) {
                            if (pl->stop_flag || g_jts_seq != my_seq) break;
                            int samples = jts_render_event(&events[i]);
                            jts_output_event(samples, my_seq, pl);
                            pl->cur_time_ms += events[i].duration;
                        }
                    }
                    pl->playing = 0;
                    pl->cur_time_ms = pl->duration_ms;

                    /* natural end (not preempted, not stopped):
                     * notify Java so PlayerListener gets END_OF_MEDIA.
                     * Java multiplies the data by 1000 (expects seconds). */
                    if (!pl->stop_flag && g_jts_seq == my_seq) {
                        ALOG("notify END_OF_MEDIA (jts done)");
                        javanotify_on_media_notification(
                            JAVACALL_EVENT_MEDIA_END_OF_MEDIA,
                            pl->app_id, pl->player_id, JAVACALL_OK,
                            (void *)(long)(pl->duration_ms / 1000));
                    }
                    pl->stop_flag = 0;
                }
            }
            continue;
        }

        /* --- WAV playback request? (v01.96) --- */
        if (g_wav_seq != g_wav_done) {
            vita_player *pl = g_wav_req;
            unsigned int my_seq = g_wav_seq;
            g_wav_done = my_seq;
            if (pl != NULL && pl->wav.valid) {
                wav_render_player(pl, my_seq);
            }
            continue;
        }

        /* --- compressed PCM decode request? (MP3/AMR/AAC, v02.88) --- */
        if (g_pcm_seq != g_pcm_done) {
            vita_player *pl = g_pcm_req;
            unsigned int my_seq = g_pcm_seq;
            g_pcm_done = my_seq;
            if (pl != NULL && pl->pcm != NULL && pl->pcm_dec != NULL) {
                pcm_render_player(pl, my_seq);
            }
            continue;
        }

        /* --- MIDI playback request? (v02.60) --- */
        if (g_midi_seq != g_midi_done) {
            vita_player *pl = g_midi_req;
            unsigned int my_seq = g_midi_seq;
            g_midi_done = my_seq;
            if (pl != NULL && pl->midi && pl->midi_len > 0) {
                midi_ctx_t mc;
                long dur = -1;
                int failed = 0;
                pl->playing = 1;
                /* v02.66: stop_flag is cleared by javacall_media_start(),
                 * not here - see the comment there. */
                pl->cur_time_ms = 0;
                mc.pl = pl;
                mc.my_seq = my_seq;
                mc.frames = 0;
                if (pl->midi_bad || pl->midi_acc == NULL) {
                    failed = 1;      /* known-bad: no re-parse, no log */
                } else if (audio_out_open() == 0) {
                    /* v03.37: log what actually happened. The [MIDI] lines go to midp_stderr.log, which the next
                     * game launch truncates (log137 lost them). */
                    unsigned long long t_start = sceKernelGetProcessTimeWide();
                    int r;
                    vpp_reset_run(&g_pcm_probe);
                    r = vita_midi_play(pl->midi_acc, (long)pl->midi_len,
                                       pl->volume, midi_out_cb,
                                       midi_stop_cb, &mc, &dur);
                    alog("[MIDI] play r=%d smf=%ld bytes vol=%ld mute=%d smf_len=%ld ms, rendered %lu frames = %lu ms, "
                         "wall %llu ms\n", r, (long)pl->midi_len, (long)pl->volume, (int)pl->mute, dur, mc.frames,
                         mc.frames * 1000UL / OUT_SAMPLE_RATE,
                         (sceKernelGetProcessTimeWide() - t_start) / 1000ULL);
                    pcm_probe_report("midi");
                    if (dur > 0) pl->duration_ms = dur;
                    if (r < 0) {
                        pl->midi_bad = 1;
                        failed = 1;
                    }
                }
                /* v02.64: a failed play must not report end-of-media at
                 * once - a looping game answers with set_time(0)+start()
                 * immediately, which log110 showed as 308 rounds in 133 s
                 * (each with a dozen synchronous log writes). Pace it to
                 * ~1/s, interruptibly, on this thread (not the VM thread). */
                if (failed) {
                    int w;
                    for (w = 0; w < 20 && !pl->stop_flag &&
                                g_midi_seq == my_seq; w++) {
                        sceKernelDelayThread(50 * 1000);
                    }
                }
                pl->playing = 0;
                pl->cur_time_ms = pl->duration_ms;

                if (!pl->stop_flag && g_midi_seq == my_seq) {
                    ALOG("notify END_OF_MEDIA (midi done)");
                    javanotify_on_media_notification(
                        JAVACALL_EVENT_MEDIA_END_OF_MEDIA,
                        pl->app_id, pl->player_id, JAVACALL_OK,
                        (void *)(long)(pl->duration_ms / 1000));
                }
                pl->stop_flag = 0;
            }
            continue;
        }

        /* --- simple playTone request? --- */
        if (g_req_seq == g_played_seq) {
            sceKernelDelayThread(4000); /* 4ms idle poll */
            continue;
        }
        unsigned int my_seq = g_req_seq;
        g_played_seq = my_seq;
        g_tone_stop_requested = 0;
        alog("tone req: note=%ld dur=%ld vol=%ld seq=%u port=%d\n",
             g_req_note, g_req_dur, g_req_vol, my_seq, g_audio_port);
        if (audio_out_open() != 0) {
            alog("audio_out_open FAILED\n");
            continue; /* retry on next request */
        }
        g_tone_len = generate_tone_pcm(midi_note_to_freq(g_req_note),
                                       (int)g_req_dur, g_req_vol);
        output_tone_blocking(my_seq);
    }
    return 0;
}

/* ---------------------------------------------------------------------------
 * Mandatory API (signatures must match javacall_multimedia.h)
 * ------------------------------------------------------------------------- */

javacall_result javacall_media_initialize(void) {
    ALOG("initialize");
    g_audio_port = -1;
    g_tone_stop_requested = 0;
    g_cfg.mediaCaps = NULL;
    /* NOTE: ANI thread pool init (ANI_Initialize) intentionally omitted -
     * linking against libcldc_vm_ani.a's ani.o fails because GNU ld's
     * archive scanner doesn't pull ani.o for this symbol (stale/missing
     * symbol index in the archive). b137 worked without this call.
     * Re-enable only if ANI-related crashes reappear. */
    {
        SceUID tid = sceKernelCreateThread("j2me_tone", tone_player_thread,
                                           0x10000100, 0x4000, 0,
                                           SCE_KERNEL_THREAD_CPU_AFFINITY_MASK_DEFAULT,
                                           NULL);
        if (tid >= 0) {
            sceKernelStartThread(tid, 0, NULL);
        }
        /* v01.72: publish the tid for the hang watchdog
         * (vita_watchdog.c snapshots this thread on a pump stall) */
        vita_tone_tid = tid;
        /* Without the player thread the VM would stay silent rather than
         * hang - acceptable degraded mode. */
    }
    return JAVACALL_OK;
}

javacall_result javacall_media_finalize(void) {
    ALOG("finalize");
    if (g_audio_port >= 0) {
        sceAudioOutReleasePort(g_audio_port);
        g_audio_port = -1;
    }
    /* v02.60: free the MIDI sound bank on VM shutdown. */
    vita_midi_shutdown();
    return JAVACALL_OK;
}

javacall_result javacall_media_get_configuration(
        /*OUT*/ const javacall_media_configuration** configuration) {
    ALOG("get_configuration");
    if (configuration == NULL) return JAVACALL_INVALID_ARGUMENT;
    g_cfg.mediaCaps = g_caps_tone;
    *configuration = &g_cfg;
    return JAVACALL_OK;
}

javacall_result javacall_media_play_tone(int appID, long note,
                                         long duration, long volume) {
    ALOG("play_tone");
    (void)appID;
    if (note < 0 || note > 127)          return JAVACALL_INVALID_ARGUMENT;
    if (duration <= 0)                   return JAVACALL_INVALID_ARGUMENT;
    if (duration > MAX_TONE_DURATION_MS) duration = MAX_TONE_DURATION_MS;
    if (volume < 0)   volume = 0;
    if (volume > 100) volume = 100;

    /* Hand off to the player thread and return immediately - blocking
     * here would freeze all CLDC green threads (VM-wide stall). The
     * player thread opens the audio port itself. */
    g_req_note = note;
    g_req_dur = duration;
    g_req_vol = volume;
    g_req_seq++;
    return JAVACALL_OK;
}

javacall_result javacall_media_stop_tone(int appID) {
    ALOG("stop_tone");
    (void)appID;
    g_tone_stop_requested = 1;
    return JAVACALL_OK;
}

javacall_result javacall_media_get_event_data(javacall_handle handle,
        int eventType, void *pResult, int numArgs, void *args[]) {
    ALOG("get_event_data");
    (void)handle; (void)eventType; (void)pResult; (void)numArgs; (void)args;
    /* All entry points complete synchronously - no reentry data needed */
    return JAVACALL_INVALID_ARGUMENT;
}

/* URI is a UTF-16 string; compare against "device://tone" (ASCII subset).
 * v01.96: no longer used by create() - WAV support made it MIME-driven;
 * kept for reference/debug use. */
#if 0
static int uri_is_device_tone(javacall_const_utf16_string uri, long len) {
    static const char dev[] = "device://tone";
    if (uri == NULL || len < (long)sizeof(dev) - 1) return 0;
    for (long i = 0; i < (long)(sizeof(dev) - 1); i++) {
        if (uri[i] != (javacall_utf16)dev[i]) return 0;
    }
    return 1;
}
#endif

javacall_result javacall_media_create(int appID, int playerID,
                                      javacall_const_utf16_string uri,
                                      long uriLength,
                                      /*OUT*/ javacall_handle* handle) {
    ALOG("create");
    /* v03.15: wall time of every stage a new Sound() goes through, same clock as input_debug HEARTBEAT ms=.
     * Lines: "[NS] t=.. create" ... "[NS] t=.. buffered" ... "[NS] t=.. prefetch" ... "[NS] t=.. start".
     * Differences between consecutive [NS] lines say which stage owns the ~2.7 s of log126. */
    alog("[NS] t=%lld create\n", wall_ms());
    vita_player *pl;

    if (handle == NULL) return JAVACALL_INVALID_ARGUMENT;
    *handle = NULL;

    /* v01.96: accept any URI and decide by the requested MIME in realize
     * instead. Non-WAV non-tone content still fails fast there (see
     * realize's mime check) so Java gets a clean MediaException. */
    (void)uri; (void)uriLength;

    pl = (vita_player *)javacall_malloc(sizeof(vita_player));
    if (pl == NULL) return JAVACALL_OUT_OF_MEMORY;

    memset(pl, 0, sizeof(*pl));
    pl->app_id = appID;
    pl->player_id = playerID;
    pl->state = 0;
    pl->jts_len = 0;
    pl->whole_size = -1;
    pl->duration_ms = -1;
    pl->volume = g_master_volume;
    pl->mute = JAVACALL_FALSE;
    /* v02.88: -1 is "none", 0 would be a valid descriptor/seek target. */
    pl->pcm_fd = -1;
    pl->pcm_seek_ms = -1;

    /* v01.97: initial heap block; small WAVs and every JTS sequence fit
     * without realloc churn. v02.66: WAV/MIDI payloads accumulate in
     * their own blocks (wav_acc / midi_acc), this one stays fixed size
     * as the KNI staging buffer. */
    pl->jts_data = (uint8_t *)javacall_malloc(JTS_MAX_SIZE);
    if (pl->jts_data == NULL) {
        javacall_free(pl);
        return JAVACALL_OUT_OF_MEMORY;
    }

    *handle = (javacall_handle)pl;
    return JAVACALL_OK;
}

javacall_result javacall_media_close(javacall_handle handle) {
    ALOG("close");
    vita_player *pl = (vita_player *)handle;
    int spins = 0;
    unsigned long long t_close0 = sceKernelGetProcessTimeWide();
    if (pl == NULL) return JAVACALL_OK;
    /* request stop; the player thread copies what it needs per event */
    pl->stop_flag = 1;
    if (g_jts_req == pl) g_jts_req = NULL;
    if (g_wav_req == pl) g_wav_req = NULL;
    if (g_midi_req == pl) g_midi_req = NULL;
    if (g_pcm_req == pl) g_pcm_req = NULL;
    /* v01.97: jts_data is heap now and a long BGM render can still be
     * inside wav_render_player - give the player thread time to notice
     * stop_flag before freeing. Bounded: one output chunk is ~10 ms,
     * 50 x 4 ms covers it plus slack (close is not latency-critical). */
    while (pl->playing && spins < 50) {
        sceKernelDelayThread(4000);
        spins++;
    }
    if (pl->jts_data != NULL) javacall_free(pl->jts_data);
    if (pl->midi_acc != NULL) javacall_free(pl->midi_acc);
    if (pl->pcm_dec != NULL) {
        if (pl->pcm != NULL) {
            pl->pcm->close(pl->pcm_dec);
        }
        pl->pcm_dec = NULL;
    }
    pcm_spill_close(pl);
    if (pl->wav_acc != NULL) javacall_free(pl->wav_acc);   /* v02.66 */
    javacall_free(pl);
    /* v03.12 probe: close() waits (up to 50 x 4 ms) for the render thread to leave its
     * loop. A render thread blocked inside sceAudioOutOutput would show up here. */
    {
        unsigned long long dt = sceKernelGetProcessTimeWide() - t_close0;
        if (dt > 20000ULL || spins > 0)
            alog("[T] t=%lld close took %llu us, waited %d x 4 ms for the render thread\n", wall_ms(), dt, spins);
    }
    return JAVACALL_OK;
}

javacall_result javacall_media_destroy(javacall_handle handle) {
    ALOG("destroy");
    (void)handle;
    return JAVACALL_OK;
}

javacall_result javacall_media_get_player_controls(javacall_handle handle,
                              /*OUT*/ int* controls) {
    ALOG("get_player_controls");
    vita_player *pl = (vita_player *)handle;
    if (controls == NULL) return JAVACALL_INVALID_ARGUMENT;
    /* v02.61: advertise the controls that match THIS player's content.
     * HighLevelPlayer.getPlayerFromType() is an else-if over
     * nIsToneControlSupported -> nIsMIDIControlSupported -> DirectPlayer.
     * Returning TONE for a MIDI player routed it to DirectTone, whose JTS
     * validator (checkSequence) rejects SMF bytes with "Tone sequence
     * check failed ... missing version information" (log109). A MIDI
     * player must advertise MIDI (not TONE) so it lands in DirectMIDI,
     * which inherits DirectPlayer's javacall-backed doBuffering/start. */
    if (pl != NULL && pl->midi) {
        *controls = JAVACALL_MEDIA_CTRL_MIDI | JAVACALL_MEDIA_CTRL_VOLUME;
    } else if (pl != NULL && pl->wav.valid) {
        /* v02.100 (log122): a WAV player fell into the final else and
         * advertised TONE, so HighLevelPlayer built a DirectTone whose JTS
         * validator rejected the RIFF bytes: "Tone sequence check failed at
         * offset 0: missing version information". log122 shows the WAV
         * itself was fine ("wav buffered: 8000 Hz 1 ch 8 bit, 8999 ms,
         * 71992 bytes") and the exception came from Sound.play(). Same
         * remedy as MP3/AMR below: report neither TONE nor MIDI so the
         * plain DirectPlayer, which plays through javacall, is chosen. */
        *controls = JAVACALL_MEDIA_CTRL_VOLUME;
    } else if (pl != NULL && pl->pcm != NULL) {
        /* v02.87: an MP3 player must report NEITHER tone NOR midi so it
         * lands in the plain DirectPlayer. If TONE were reported,
         * getPlayerFromType() would pick DirectTone, whose doRealize()
         * re-reads the stream and runs the JTS validator on the bytes -
         * for an MP3 (or a source whose length is unknown, so the reader
         * returns an EMPTY array) that throws
         *   "Tone sequence check failed at offset 0: missing version
         *    information"
         * which is exactly how MIDI broke in v02.60 (fixed in v02.61 by
         * reporting MIDI - see PROJECT_MEMORY log109). DirectPlayer is
         * the base class; it inherits the javacall-backed
         * doPrefetch/doStart and does no sequence validation at all.
         * v02.88: AMR and AAC need the same treatment. */
        *controls = JAVACALL_MEDIA_CTRL_VOLUME;
    } else {
        *controls = JAVACALL_MEDIA_CTRL_TONE | JAVACALL_MEDIA_CTRL_VOLUME;
    }
    return JAVACALL_OK;
}

javacall_result javacall_media_get_format(javacall_handle handle,
        /*OUT*/ javacall_media_format_type* format) {
    ALOG("get_format");
    vita_player *pl = (vita_player *)handle;
    if (format == NULL) return JAVACALL_INVALID_ARGUMENT;
    if (pl != NULL && pl->wav.valid) {
        /* HighLevelPlayer throws Unsupported unless realize's
         * nGetMediaFormat returns a known caps format string. */
        *format = JAVACALL_MEDIA_FORMAT_MS_PCM;
        return JAVACALL_OK;
    }
    if (pl != NULL && pl->pcm != NULL) {
        /* v02.87: same caps slot as the WAV entry (see the note in
         * g_caps_tone); the Java layer only needs a known format. */
        *format = JAVACALL_MEDIA_FORMAT_MS_PCM;
        return JAVACALL_OK;
    }
    if (pl != NULL && pl->midi) {
        /* Same for MIDI: report the known caps format string so
         * HighLevelPlayer does not throw Unsupported. */
        *format = JAVACALL_MEDIA_FORMAT_MIDI;
        return JAVACALL_OK;
    }
    /* UNKNOWN lets HighLevelPlayer map device://tone to DEVICE_TONE
     * itself (handledByDevice=true, no source.connect/download). */
    *format = JAVACALL_MEDIA_FORMAT_UNKNOWN;
    return JAVACALL_OK;
}

javacall_result javacall_media_acquire_device(javacall_handle handle) {
    alog("[NS] t=%lld acquire_device\n", wall_ms());
    ALOG("acquire_device");
    vita_player *pl = (vita_player *)handle;
    (void)pl;
    return JAVACALL_OK;
}

javacall_result javacall_media_release_device(javacall_handle handle) {
    ALOG("release_device");
    vita_player *pl = (vita_player *)handle;
    (void)pl;
    return JAVACALL_OK;
}

/* ---------------------------------------------------------------------------
 * Player state machine (device tone)
 * ------------------------------------------------------------------------- */

/* Case-sensitive ASCII match of a UTF-16 MIME string (the MIDP media
 * subsystem passes content types as ASCII, so the UTF-16 units are the
 * ASCII code points). */
static int mime_equals(javacall_const_utf16_string mime, long mimeLength,
                       const char *ascii) {
    long m = (long)strlen(ascii);
    long i;
    if (mime == NULL || mimeLength < m) return 0;
    for (i = 0; i < m; i++) {
        if (mime[i] != (javacall_utf16)ascii[i]) return 0;
    }
    return 1;
}

javacall_result javacall_media_realize(javacall_handle handle,
                                       javacall_const_utf16_string mime,
                                       long mimeLength) {
    ALOG("realize");
    vita_player *pl = (vita_player *)handle;
    if (pl == NULL) return JAVACALL_INVALID_ARGUMENT;

    /* Classify the player by its requested MIME ("audio/x-wav" /
     * "audio/midi" arrive as UTF-16 from HighLevelPlayer.realize). Tone
     * is handled elsewhere in Java; the format check that surfaces an
     * Unsupported MediaException is nGetMediaFormat below. */
    pl->wav.valid = 0;
    pl->midi = 0;
    /* v02.88: drop any previous decoder AND its spill file - realize may
     * be called again after a failed download. */
    if (pl->pcm_dec != NULL && pl->pcm != NULL) {
        pl->pcm->close(pl->pcm_dec);
    }
    pl->pcm_dec = NULL;
    pl->pcm = NULL;
    pcm_spill_close(pl);
    pl->pcm_len = 0;
    pl->pcm_mem = 0;
    pl->pcm_seek_ms = -1;
    if (mime_equals(mime, mimeLength, "audio/x-wav")) {
        pl->wav.valid = 1;   /* MIME says WAV; header check in do_buffering */
        /* v02.66: a re-download starts from empty (mirrors the MIDI reset
         * below); leftover data_off would skip the end-of-stream parse. */
        pl->wav_buf_len = 0;
        pl->wav.data_off = 0;
        pl->wav.data_len = 0;
    } else if (mime_equals(mime, mimeLength, "audio/mpeg") ||
               mime_equals(mime, mimeLength, "audio/mp3") ||
               mime_equals(mime, mimeLength, "audio/x-mp3") ||
               mime_equals(mime, mimeLength, "audio/mpeg3") ||
               mime_equals(mime, mimeLength, "audio/x-mpeg")) {
        /* v02.87: MP3 reuses the WAV accumulator; the decoder is chosen
         * by the stream itself, so wav.valid stays 0 (the WAV header
         * parser must not see this data). */
        pl->pcm = &vita_dec_mp3;
        pl->wav_buf_len = 0;
        pl->wav.data_off = 0;
        pl->wav.data_len = 0;
    } else if (mime_equals(mime, mimeLength, "audio/amr") ||
               mime_equals(mime, mimeLength, "audio/x-amr") ||
               mime_equals(mime, mimeLength, "audio/amr-nb")) {
        /* v02.88: AMR-NB (storage format). Chosen by MIME, validity
         * checked by the decoder itself when buffering ends. */
        pl->pcm = &vita_dec_amr;
        pl->wav_buf_len = 0;
        pl->wav.data_off = 0;
        pl->wav.data_len = 0;
    } else if (mime_equals(mime, mimeLength, "audio/aac") ||
               mime_equals(mime, mimeLength, "audio/aacp") ||
               mime_equals(mime, mimeLength, "audio/x-aac") ||
               mime_equals(mime, mimeLength, "audio/mp4a-latm")) {
        /* v02.88: AAC (ADTS). sceAudiodec probe happens in open(). */
        pl->pcm = &vita_dec_aac;
        pl->wav_buf_len = 0;
        pl->wav.data_off = 0;
        pl->wav.data_len = 0;
    } else if (mime_equals(mime, mimeLength, "audio/ogg") ||
               mime_equals(mime, mimeLength, "audio/x-ogg") ||
               mime_equals(mime, mimeLength, "audio/vorbis") ||
               mime_equals(mime, mimeLength, "application/ogg") ||
               mime_equals(mime, mimeLength, "application/x-ogg")) {
        /* v04.45: Ogg Vorbis (stb_vorbis). Same accumulator as MP3: the
         * decoder validates the stream itself when buffering ends. */
        pl->pcm = &vita_dec_ogg;
        pl->wav_buf_len = 0;
        pl->wav.data_off = 0;
        pl->wav.data_len = 0;
    } else if (mime_equals(mime, mimeLength, "audio/midi") ||
               mime_equals(mime, mimeLength, "audio/x-midi") ||
               mime_equals(mime, mimeLength, "audio/mid") ||
               mime_equals(mime, mimeLength, "audio/sp-midi")) {
        pl->midi = 1;        /* MIME says MIDI; parsed in do_buffering */
        /* v02.64: realize precedes the download; start from empty so a
         * re-download can never append to a previous copy. */
        pl->midi_len = 0;
        pl->midi_bad = 0;
    }

    if (pl->state == 0) pl->state = 1;
    return JAVACALL_OK;
}

javacall_result javacall_media_prefetch(javacall_handle handle) {
    alog("[NS] t=%lld prefetch\n", wall_ms());
    ALOG("prefetch");
    vita_player *pl = (vita_player *)handle;
    if (pl == NULL) return JAVACALL_INVALID_ARGUMENT;
    /* nPrefetch is synchronous in KNIDirectPlayer.c: returning OK here
     * sets isAcquire and lets DirectTone.doPrefetch() succeed. */
    if (pl->state < 1) pl->state = 1;
    pl->state = 2;
    return JAVACALL_OK;
}

javacall_result javacall_media_start(javacall_handle handle) {
    ALOG("start");
    vita_player *pl = (vita_player *)handle;
    if (pl == NULL) return JAVACALL_INVALID_ARGUMENT;

    /* v02.66: clear the stop request HERE (VM thread, before the request is
     * queued). The player thread used to clear it after entering its branch,
     * which lost a pause()/stop() that arrived before the thread was
     * scheduled: playing was still 0, so "if (pl->playing) stop_flag = 1"
     * did nothing, and even an unconditional set was wiped by the thread.
     * log111 shows start -> pause on every player. */
    pl->stop_flag = 0;
    pl->state = 3;
    pl->pause_req_ms = 0;
    pl->play_start_ms = wall_ms();
    alog("[NS] t=%lld start\n", pl->play_start_ms);

    if (pl->midi) {
        /* MIDI: hand off to the player thread. The whole SMF must have
         * been accumulated in do_buffering; otherwise report EOM. */
        if (pl->midi_len > 0) {
            g_midi_req = pl;
            g_midi_seq++;
        } else {
            ALOG("notify END_OF_MEDIA (midi no data)");
            javanotify_on_media_notification(JAVACALL_EVENT_MEDIA_END_OF_MEDIA,
                                             pl->app_id, pl->player_id,
                                             JAVACALL_OK, (void *)0);
        }
    } else if (pl->pcm != NULL) {
        /* v02.88: MP3 / AMR / AAC. The decoder handle was created when
         * buffering ended; a plain restart replays from the start. A
         * pending setMediaTime() is NOT overridden by the rewind - the
         * renderer applies it on its first iteration instead. */
        if (pl->pcm_dec != NULL) {
            if (pl->pcm_seek_ms < 0) {
                pl->pcm->rewind(pl->pcm_dec);
            }
            g_pcm_req = pl;
            g_pcm_seq++;
        } else {
            ALOG("notify END_OF_MEDIA (pcm no data)");
            javanotify_on_media_notification(JAVACALL_EVENT_MEDIA_END_OF_MEDIA,
                                             pl->app_id, pl->player_id,
                                             JAVACALL_OK, (void *)0);
        }
    } else if (pl->wav.valid) {
        /* WAV: hand off to the player thread like JTS. Header must have
         * been parsed in do_buffering; otherwise report END_OF_MEDIA. */
        if (pl->wav_buf_len > 0) {
            g_wav_req = pl;
            g_wav_seq++;
        } else {
            ALOG("notify END_OF_MEDIA (wav no data)");
            javanotify_on_media_notification(JAVACALL_EVENT_MEDIA_END_OF_MEDIA,
                                             pl->app_id, pl->player_id,
                                             JAVACALL_OK, (void *)0);
        }
    } else if (pl->jts_len >= 2) {
        /* Hand off to the player thread; never block the VM thread. */
        g_jts_req = pl;
        g_jts_seq++;
    } else {
        /* No sequence set: zero-duration device player. Java's
         * hasZeroDuration() normally handles this, but if we get here
         * just report END_OF_MEDIA right away (data is in seconds). */
        ALOG("notify END_OF_MEDIA (zero-dur)");
        javanotify_on_media_notification(JAVACALL_EVENT_MEDIA_END_OF_MEDIA,
                                         pl->app_id, pl->player_id,
                                         JAVACALL_OK, (void *)0);
    }
    return JAVACALL_OK;
}

javacall_result javacall_media_stop(javacall_handle handle) {
    ALOG("stop");
    vita_player *pl = (vita_player *)handle;
    if (pl == NULL) return JAVACALL_INVALID_ARGUMENT;
    /* v02.66: unconditional. Gating on pl->playing lost a stop that arrived
     * before the player thread had started (it set playing = 1 itself). */
    pl->stop_flag = 1;
    if (pl->state == 3) pl->state = 2;
    pl->pause_req_ms = wall_ms();
    alog("[PR] t=%lld stop  playing=%d cur=%ldms\n", pl->pause_req_ms, pl->playing, pl->cur_time_ms);
    return JAVACALL_OK;
}

javacall_result javacall_media_pause(javacall_handle handle) {
    ALOG("pause");
    vita_player *pl = (vita_player *)handle;
    if (pl == NULL) return JAVACALL_INVALID_ARGUMENT;
    /* v02.66: unconditional - see javacall_media_stop. */
    pl->stop_flag = 1;
    if (pl->state == 3) pl->state = 2;
    /* v03.13 probe. This is the path Player.stop() takes (DirectPlayer.doStop -> nPause).
     * "ran" = how long since start() queued the render; "playing" = the render thread has
     * not left its loop yet. The matching "[PR] ... render stopped" line is written by the
     * render thread itself, so (stopped.t - pause.t) is the real pause latency. */
    pl->pause_req_ms = wall_ms();
    pl->pause_n++;
    alog("[PR] t=%lld pause#%ld ran=%lldms playing=%d cur=%ldms\n", pl->pause_req_ms, pl->pause_n,
         pl->play_start_ms ? pl->pause_req_ms - pl->play_start_ms : -1LL, pl->playing, pl->cur_time_ms);
    return JAVACALL_OK;
}

javacall_result javacall_media_resume(javacall_handle handle) {
    ALOG("resume");
    {
        vita_player *rp = (vita_player *)handle;
        alog("[PR] t=%lld resume (restarts from 0) playing=%d cur=%ldms\n", wall_ms(),
             rp ? rp->playing : -1, rp ? rp->cur_time_ms : -1L);
    }
    /* Restart from the beginning (simplest correct behavior) */
    return javacall_media_start(handle);
}

javacall_result javacall_media_get_time(javacall_handle handle,
                                        /*OUT*/ long* ms) {
    ALOG_HOT("get_time");
    vita_player *pl = (vita_player *)handle;
    if (pl == NULL || ms == NULL) return JAVACALL_INVALID_ARGUMENT;
    *ms = pl->cur_time_ms;
    return JAVACALL_OK;
}

javacall_result javacall_media_set_time(javacall_handle handle,
                                        /*INOUT*/ long* ms) {
    ALOG("set_time");
    vita_player *pl = (vita_player *)handle;
    if (pl == NULL || ms == NULL) return JAVACALL_INVALID_ARGUMENT;
    /* v02.88: seeking works for the compressed decoders (MP3/AMR/AAC).
     * The request is queued and applied by the player thread, so the
     * decoder state is only ever touched from one thread. Tone sequences
     * still clamp to 0. */
    if (pl->pcm != NULL && pl->pcm_dec != NULL) {
        long want = *ms;
        if (want < 0) want = 0;
        if (pl->duration_ms > 0 && want > pl->duration_ms) {
            want = pl->duration_ms;
        }
        pl->pcm_seek_ms = want;
        pl->cur_time_ms = want;
        *ms = want;
        return JAVACALL_OK;
    }
    /* Seeking within a tone sequence is not supported; clamp to 0. */
    *ms = 0;
    pl->cur_time_ms = 0;
    return JAVACALL_OK;
}

javacall_result javacall_media_get_duration(javacall_handle handle,
                                            /*OUT*/ long* ms) {
    ALOG_HOT("get_duration");
    vita_player *pl = (vita_player *)handle;
    if (pl == NULL || ms == NULL) return JAVACALL_INVALID_ARGUMENT;
    *ms = pl->duration_ms;   /* -1 until a sequence has been buffered */
    return JAVACALL_OK;
}

/* ---------------------------------------------------------------------------
 * Buffering (JTS sequence delivery from MediaDownload.nBuffering)
 * ------------------------------------------------------------------------- */

javacall_result javacall_media_get_java_buffer_size(javacall_handle handle,
                                 /*OUT*/ long* java_buffer_size,
                                 /*OUT*/ long* first_chunk_size) {
    ALOG("get_java_buffer_size");
    vita_player *pl = (vita_player *)handle;
    (void)pl;
    if (java_buffer_size == NULL || first_chunk_size == NULL)
        return JAVACALL_INVALID_ARGUMENT;
    /* Non-zero values make MediaDownload.download() actually run and
     * deliver the JTS bytes via nBuffering. */
    *java_buffer_size = JTS_MAX_SIZE;
    /* v01.96: WAV slides a window through this buffer, so the first
     * packet is bigger (must cover a whole RIFF header comfortably);
     * MIDI does the same; JTS keeps its original small first chunk. */
    *first_chunk_size = (pl != NULL && (pl->wav.valid || pl->midi ||
                                        pl->pcm != NULL))
                            ? 4096 : JTS_FIRST_CHUNK;
    return JAVACALL_OK;
}

javacall_result javacall_media_set_whole_content_size(javacall_handle handle,
                                 long whole_content_size) {
    ALOG("set_whole_content_size");
    vita_player *pl = (vita_player *)handle;
    if (pl == NULL) return JAVACALL_INVALID_ARGUMENT;
    pl->whole_size = whole_content_size;
    return JAVACALL_OK;
}

javacall_result javacall_media_get_buffer_address(javacall_handle handle,
                                 /*OUT*/ const void** buffer,
                                 /*OUT*/ long* max_size) {
    /* v03.12: HOT. One call per 4 KB packet of the tune (68 for a 270 KB WAV), each a
     * synchronous sceIoWrite on the VM thread - see ALOG_HOT. */
    ALOG_HOT("get_buffer_address");
    vita_player *pl = (vita_player *)handle;
    if (pl == NULL || buffer == NULL || max_size == NULL)
        return JAVACALL_INVALID_ARGUMENT;
    *buffer = pl->jts_data;
    *max_size = JTS_MAX_SIZE;
    return JAVACALL_OK;
}

/* v01.97: grow the heap buffer so a whole Java packet always fits.
 * The Java download loop advances its read offset by the length we
 * report, so dropping bytes would desync the stream permanently -
 * growth (doubling, WAV_MAX_BUFFER ceiling) is the only safe answer.
 * v02.66: grows the separate WAV accumulator, never the KNI staging
 * buffer pl->jts_data (reallocing that would free the `buffer` the caller
 * passed in - see the vita_player comment).
 * Returns 0 on success, -1 on allocation failure/ceiling. */
static int wav_acc_ensure(vita_player *pl, long needed) {
    long new_cap;
    uint8_t *nb;
    if (needed <= pl->wav_acap) return 0;
    if (needed > WAV_MAX_BUFFER) return -1;
    new_cap = pl->wav_acap > 0 ? pl->wav_acap : WAV_INIT_CAP;
    while (new_cap < needed) new_cap *= 2;
    if (new_cap > WAV_MAX_BUFFER) new_cap = WAV_MAX_BUFFER;
    nb = (uint8_t *)javacall_realloc(pl->wav_acc, (unsigned int)new_cap);
    if (nb == NULL) return -1;
    pl->wav_acc = nb;
    pl->wav_acap = new_cap;
    return 0;
}

/* v02.64: grow the MIDI accumulator (separate from the KNI staging buffer
 * pl->jts_data, see the vita_player comment). Returns 0 on success. */
static int midi_acc_ensure(vita_player *pl, long needed) {
    long new_cap;
    uint8_t *nb;
    if (needed <= pl->midi_cap) return 0;
    if (needed > MIDI_MAX_BUFFER) return -1;
    new_cap = pl->midi_cap > 0 ? pl->midi_cap : MIDI_INIT_CAP;
    while (new_cap < needed) new_cap *= 2;
    if (new_cap > MIDI_MAX_BUFFER) new_cap = MIDI_MAX_BUFFER;
    nb = (uint8_t *)javacall_realloc(pl->midi_acc, (unsigned int)new_cap);
    if (nb == NULL) return -1;
    pl->midi_acc = nb;
    pl->midi_cap = new_cap;
    return 0;
}

javacall_result javacall_media_do_buffering(javacall_handle handle,
                                 const void* buffer,
                                 /*INOUT*/ long* length,
                                 /*OUT*/ javacall_bool* need_more_data,
                                 /*OUT*/ long* next_chunk_size) {
    /* v03.12: HOT, same reason as get_buffer_address. The interesting outcome of the
     * download is logged separately ("wav buffered ..."). */
    ALOG_HOT("do_buffering");
    vita_player *pl = (vita_player *)handle;
    int tempo = 120, resolution = 64;
    jts_event probe[JTS_MAX_EVENTS];
    long n;

    if (pl == NULL || length == NULL || need_more_data == NULL ||
        next_chunk_size == NULL) {
        return JAVACALL_INVALID_ARGUMENT;
    }

    if (buffer != NULL && *length > 0 && pl->wav_buf_len == 0 && pl->pcm_len == 0) {
        alog("[NS] t=%lld first packet (%ld bytes)\n", wall_ms(), *length);
    }
    if (buffer == NULL || *length <= 0) {
        /* End of buffering marker (Java sends NULL/-1). JTS always
         * arrives in one chunk; for WAV the whole file is already
         * accumulated, so finish the header parse now. */
        *length = 0;
        *need_more_data = JAVACALL_FALSE;
        *next_chunk_size = 0;
        alog("[NS] t=%lld buffered (end marker, %lu bytes received)\n", wall_ms(), (unsigned long)pl->wav_buf_len);
        if (pl->pcm != NULL) {
            /* v02.88: every byte is buffered; build the decoder now. On
             * failure, fail the stream cleanly like WAV does. */
            if (pl->pcm_dec != NULL) {
                pl->pcm->close(pl->pcm_dec);
                pl->pcm_dec = NULL;
            }
            if (pl->pcm_len > 0) {
                void *h = pl->pcm->open(pcm_src_read, pl,
                                        (long)pl->pcm_len);
                if (h != NULL) {
                    pl->pcm_dec = h;
                    pl->duration_ms = pl->pcm->duration_ms(h);
                    alog("pcm buffered: %s %d Hz %d ch, %ld ms, %lu B"
                         " (%lu B RAM + %lu B file)\n",
                         (pl->pcm == &vita_dec_mp3) ? "mp3" :
                         (pl->pcm == &vita_dec_amr) ? "amr" :
                         (pl->pcm == &vita_dec_aac) ? "aac" :
                         (pl->pcm == &vita_dec_ogg) ? "ogg" : "?",
                         pl->pcm->rate(h), pl->pcm->channels(h),
                         pl->duration_ms, (unsigned long)pl->pcm_len,
                         (unsigned long)pl->pcm_mem,
                         (unsigned long)(pl->pcm_len - pl->pcm_mem));
                } else {
                    alog("pcm open FAILED (%lu bytes)\n",
                         (unsigned long)pl->pcm_len);
                    if (pl->wav_acc != NULL) {
                        javacall_free(pl->wav_acc);
                        pl->wav_acc = NULL;
                        pl->wav_acap = 0;
                    }
                    pcm_spill_close(pl);
                    pl->pcm = NULL;
                    return JAVACALL_INVALID_ARGUMENT;
                }
            } else {
                alog("pcm no data\n");
                pcm_spill_close(pl);
                pl->pcm = NULL;
                return JAVACALL_INVALID_ARGUMENT;
            }
        }
        if (pl->wav.valid && !pl->wav.data_off) {
            wav_info info;
            /* v02.66: parse the accumulator, not the KNI staging buffer. */
            if (pl->wav_acc != NULL &&
                vita_wav_parse_header(pl->wav_acc, pl->wav_buf_len, &info)) {
                pl->wav.data_off = info.data_off;
                pl->wav.data_len = info.data_len;
                pl->wav.rate     = info.rate;
                pl->wav.channels = info.channels;
                pl->wav.bits     = info.bits;
                pl->duration_ms  = vita_wav_duration_ms(&info);
                alog("wav buffered: %ld Hz %d ch %d bit, %ld ms, %ld bytes\n",
                     info.rate, info.channels, info.bits,
                     pl->duration_ms, info.data_len);
            } else {
                alog("wav parse FAILED (%ld bytes)\n", pl->wav_buf_len);
                pl->wav.valid = 0;
                /* v01.97: a big heap buffer is of no use now - release it
                 * so a failed stream does not pin megabytes until close(). */
                if (pl->wav_acc != NULL) {
                    javacall_free(pl->wav_acc);
                    pl->wav_acc = NULL;
                    pl->wav_acap = 0;
                }
                return JAVACALL_INVALID_ARGUMENT;
            }
        }
        return JAVACALL_OK;
    }

    /* v01.97: WAV chunks - KNI copies each sliding window to the START
     * of jts_data (see vita_wav.c), so append into our storage here.
     * v02.66: `buffer` IS pl->jts_data, so the append target is the
     * separate accumulator (appending into jts_data overwrote the bytes
     * already collected from the 2nd window on).
     * The Java loop advances by the length we report, so we must take
     * every byte: grow the buffer instead of clamping (v01.96 clamped
     * at 32 KB and truncated long BGMs). */
    /* v02.88: compressed decoder streams (MP3 / AMR-NB / AAC).
     *
     * The first PCM_MEM_KEEP bytes go into wav_acc; the rest is spilled to
     * a temp file. pcm_src_read() presents the two halves as one
     * contiguous source, so the decoders do not care where the bytes live.
     * If ux0: is not writable the remainder simply stays in RAM (the
     * pre-v02.88 behaviour) - a full card must not break playback. */
    if (pl->pcm != NULL) {
        long take = *length;
        long room, to_mem;

        if (take < 0) take = 0;
        room = (long)PCM_MEM_KEEP - (long)pl->pcm_mem;
        to_mem = (take < room) ? take : room;

        if (to_mem > 0) {
            if (wav_acc_ensure(pl, (long)pl->pcm_mem + to_mem) != 0) {
                alog("pcm buf grow FAILED at %lu+%ld bytes\n",
                     (unsigned long)pl->pcm_mem, to_mem);
                pl->pcm = NULL;
                *length = 0;
                *need_more_data = JAVACALL_FALSE;
                *next_chunk_size = 0;
                return JAVACALL_OUT_OF_MEMORY;
            }
            memcpy(pl->wav_acc + pl->pcm_mem, buffer, (size_t)to_mem);
            pl->pcm_mem += (uint32_t)to_mem;
        }
        if (to_mem < take) {
            long left = take - to_mem;
            const uint8_t *src = (const uint8_t *)buffer + to_mem;

            if (pl->pcm_fd < 0) {
                snprintf(pl->pcm_tmp, sizeof(pl->pcm_tmp), PCM_TMP_FMT,
                         pl->player_id);
                pl->pcm_fd = sceIoOpen(pl->pcm_tmp,
                                       SCE_O_WRONLY | SCE_O_CREAT |
                                       SCE_O_TRUNC, 0777);
                if (pl->pcm_fd < 0) {
                    alog("pcm spill open FAILED, keeping %ld in RAM\n", left);
                }
            }
            if (pl->pcm_fd >= 0) {
                long wrote = (long)sceIoWrite(pl->pcm_fd, src, (SceSize)left);
                if (wrote != left) {
                    /* A short write leaves a hole; the decoder will simply
                     * see EOF earlier. Log it rather than pretend. */
                    alog("pcm spill write short %ld/%ld\n", wrote, left);
                }
            } else {
                if (wav_acc_ensure(pl, (long)pl->pcm_mem + left) != 0) {
                    alog("pcm buf grow FAILED at %lu+%ld bytes\n",
                         (unsigned long)pl->pcm_mem, left);
                    pl->pcm = NULL;
                    *length = 0;
                    *need_more_data = JAVACALL_FALSE;
                    *next_chunk_size = 0;
                    return JAVACALL_OUT_OF_MEMORY;
                }
                memcpy(pl->wav_acc + pl->pcm_mem, src, (size_t)left);
                pl->pcm_mem += (uint32_t)left;
            }
        }
        pl->pcm_len += (uint32_t)take;

        *need_more_data = JAVACALL_TRUE;
        *next_chunk_size = 4096;
        *length = take;
        return JAVACALL_OK;
    }

    if (pl->wav.valid) {
        long take = *length;
        if (wav_acc_ensure(pl, (long)pl->wav_buf_len + take) != 0) {
            /* Ceiling hit or OOM: fail the stream cleanly instead of
             * silently corrupting the PCM data. */
            alog("wav buf grow FAILED at %lu+%ld bytes\n",
                 (unsigned long)pl->wav_buf_len, take);
            pl->wav.valid = 0;
            *length = 0;
            *need_more_data = JAVACALL_FALSE;
            *next_chunk_size = 0;
            return JAVACALL_OUT_OF_MEMORY;
        }
        if (take > 0) {
            memcpy(pl->wav_acc + pl->wav_buf_len, buffer, take);
            pl->wav_buf_len += (uint32_t)take;
        }
        /* Keep pulling until Java reports stream EOF (it then sends the
         * NULL end-of-buffering marker, where the RIFF parse happens).
         * whole_size is only a hint - not all sources know it. The
         * returned size becomes Java's next packetSize; a nonzero
         * regular-read value is required (0 would make KNI treat the
         * next nBuffering call as the end marker). */
        *need_more_data = JAVACALL_TRUE;
        *next_chunk_size = 4096;
        *length = take;
        return JAVACALL_OK;
    }

    /* v02.60: MIDI chunks - accumulate the whole SMF the same way as
     * WAV. The parse (tml_load_memory) happens on the player thread at
     * start(), not here, so buffering is a straight byte append. */
    if (pl->midi) {
        long take = *length;
        /* v02.64: `buffer` IS pl->jts_data (KNI staging); append into the
         * separate accumulator so the next window cannot clobber it. */
        if (midi_acc_ensure(pl, (long)pl->midi_len + take) != 0) {
            alog("midi buf grow FAILED at %lu+%ld bytes\n",
                 (unsigned long)pl->midi_len, take);
            pl->midi = 0;
            *length = 0;
            *need_more_data = JAVACALL_FALSE;
            *next_chunk_size = 0;
            return JAVACALL_OUT_OF_MEMORY;
        }
        if (take > 0) {
            memcpy(pl->midi_acc + pl->midi_len, buffer, take);
            pl->midi_len += (uint32_t)take;
        }
        *need_more_data = JAVACALL_TRUE;
        *next_chunk_size = 4096;
        *length = take;
        return JAVACALL_OK;
    }

    if (*length > JTS_MAX_SIZE) *length = JTS_MAX_SIZE;

    /* Validate + measure the sequence now so duration is known before
     * start(); invalid data fails fast with a clean error. */
    if (!jts_check_sequence(pl->jts_data, *length)) {
        pl->jts_len = 0;
        return JAVACALL_INVALID_ARGUMENT;
    }

    n = jts_expand(pl->jts_data, *length, probe, JTS_MAX_EVENTS,
                   &tempo, &resolution);
    if (n < 0) {
        pl->jts_len = 0;
        return JAVACALL_INVALID_ARGUMENT;
    }

    pl->jts_len = *length;
    pl->duration_ms = jts_total_duration(probe, n);

    *need_more_data = JAVACALL_FALSE;
    *next_chunk_size = 0;
    return JAVACALL_OK;
}

javacall_result javacall_media_clear_buffer(javacall_handle handle) {
    ALOG("clear_buffer");
    vita_player *pl = (vita_player *)handle;
    if (pl == NULL) return JAVACALL_INVALID_ARGUMENT;
    pl->jts_len = 0;
    pl->wav_buf_len = 0;
    pl->wav.data_off = 0;
    pl->wav.data_len = 0;
    pl->duration_ms = -1;
    pl->whole_size = -1;
    return JAVACALL_OK;
}

/* ---------------------------------------------------------------------------
 * VolumeControl
 * ------------------------------------------------------------------------- */

javacall_result javacall_media_get_volume(javacall_handle handle,
                                          /*OUT*/ long* volume) {
    ALOG("get_volume");
    vita_player *pl = (vita_player *)handle;
    if (pl == NULL || volume == NULL) return JAVACALL_INVALID_ARGUMENT;
    *volume = pl->volume;
    return JAVACALL_OK;
}

javacall_result javacall_media_set_volume(javacall_handle handle,
                                          /*INOUT*/ long* level) {
    ALOG_HOT("set_volume");
    vita_player *pl = (vita_player *)handle;
    if (pl == NULL || level == NULL) return JAVACALL_INVALID_ARGUMENT;
    if (*level < 0)   *level = 0;
    if (*level > 100) *level = 100;
    pl->volume = *level;
    g_master_volume = *level;
    return JAVACALL_OK;
}

javacall_result javacall_media_is_mute(javacall_handle handle,
                                       /*OUT*/ javacall_bool* mute) {
    ALOG("is_mute");
    vita_player *pl = (vita_player *)handle;
    if (pl == NULL || mute == NULL) return JAVACALL_INVALID_ARGUMENT;
    *mute = (pl->volume == 0) ? JAVACALL_TRUE : JAVACALL_FALSE;
    return JAVACALL_OK;
}

javacall_result javacall_media_set_mute(javacall_handle handle,
                                        javacall_bool mute) {
    ALOG("set_mute");
    vita_player *pl = (vita_player *)handle;
    if (pl == NULL) return JAVACALL_INVALID_ARGUMENT;
    pl->mute = mute;
    if (mute == JAVACALL_TRUE) {
        pl->volume = 0;
    } else {
        if (pl->volume == 0) pl->volume = g_master_volume ? g_master_volume : 100;
    }
    return JAVACALL_OK;
}
