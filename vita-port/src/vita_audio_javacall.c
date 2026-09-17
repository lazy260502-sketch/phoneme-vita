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
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>

#include "javacall_multimedia.h"
#include "javacall_defs.h"
#include "javacall_memory.h"
#include "javanotify_multimedia.h"

/* ---------------------------------------------------------------------------
 * Audio diagnosis log (independent fd, not affected by VM stderr redirect)
 * ------------------------------------------------------------------------- */
#include <psp2/io/fcntl.h>
#include <stdarg.h>
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
    if (n > 0) sceIoWrite(alog_fd, buf, n);
    __sync_lock_release(&lock);
}
#define ALOG(name) alog("[IN ] %s\n", name)


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

/* One expanded playback event */
typedef struct {
    int8_t  note;      /* MIDI note 0..127, or JTS_SILENCE for a rest */
    int32_t duration;  /* milliseconds */
    int32_t volume;    /* 0..100 */
} jts_event;

/* Native player handle created by javacall_media_create */
typedef struct {
    int      app_id;
    int      player_id;
    int      state;        /* 0=idle 1=realized 2=prefetched 3=playing */
    uint8_t  jts_data[JTS_MAX_SIZE];
    long     jts_len;      /* bytes buffered */
    long     whole_size;   /* set via javacall_media_set_whole_content_size */
    long     duration_ms;  /* computed total sequence duration, -1 unknown */
    long     volume;       /* 0..100 */
    javacall_bool mute;
    /* playback progress (touched by player thread) */
    volatile int      playing;
    volatile long     cur_time_ms;
    volatile long     stop_flag;
} vita_player;

/* ---------------------------------------------------------------------------
 * State
 * ------------------------------------------------------------------------- */
static int g_audio_port = -1;
static volatile int g_tone_stop_requested = 0;

static int16_t g_tone_buffer[MAX_TONE_SAMPLES];
static int g_tone_len = 0;

static int16_t g_out_buffer[OUT_CHUNK_FRAMES * 2];

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

/* v01.72: the tone player thread's id, published for the hang
 * watchdog (vita_watchdog.c). -1 until javacall_media_initialize runs. */
volatile SceUID vita_tone_tid = -1;

/* Dedicated tone player thread: waits for requests, streams them out.
 * Handles both simple playTone requests and JTS sequence playback. */
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
                    pl->stop_flag = 0;
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

/* URI is a UTF-16 string; compare against "device://tone" (ASCII subset) */
static int uri_is_device_tone(javacall_const_utf16_string uri, long len) {
    static const char dev[] = "device://tone";
    if (uri == NULL || len < (long)sizeof(dev) - 1) return 0;
    for (long i = 0; i < (long)(sizeof(dev) - 1); i++) {
        if (uri[i] != (javacall_utf16)dev[i]) return 0;
    }
    return 1;
}

javacall_result javacall_media_create(int appID, int playerID,
                                      javacall_const_utf16_string uri,
                                      long uriLength,
                                      /*OUT*/ javacall_handle* handle) {
    ALOG("create");
    vita_player *pl;

    if (handle == NULL) return JAVACALL_INVALID_ARGUMENT;
    *handle = NULL;

    /* Only device tone players are supported; everything else (wav/midi/
     * http files) fails fast so Java gets a clean MediaException instead
     * of a half-working player. */
    if (!uri_is_device_tone(uri, uriLength)) {
        return JAVACALL_INVALID_ARGUMENT;
    }

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

    *handle = (javacall_handle)pl;
    return JAVACALL_OK;
}

javacall_result javacall_media_close(javacall_handle handle) {
    ALOG("close");
    vita_player *pl = (vita_player *)handle;
    if (pl == NULL) return JAVACALL_OK;
    /* request stop; the player thread copies what it needs per event */
    pl->stop_flag = 1;
    if (g_jts_req == pl) g_jts_req = NULL;
    javacall_free(pl);
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
    (void)pl;
    if (controls == NULL) return JAVACALL_INVALID_ARGUMENT;
    /* ToneControl + VolumeControl: makes Manager pick DirectTone */
    *controls = JAVACALL_MEDIA_CTRL_TONE | JAVACALL_MEDIA_CTRL_VOLUME;
    return JAVACALL_OK;
}

javacall_result javacall_media_get_format(javacall_handle handle,
        /*OUT*/ javacall_media_format_type* format) {
    ALOG("get_format");
    vita_player *pl = (vita_player *)handle;
    (void)pl;
    if (format == NULL) return JAVACALL_INVALID_ARGUMENT;
    /* UNKNOWN lets HighLevelPlayer map device://tone to DEVICE_TONE
     * itself (handledByDevice=true, no source.connect/download). */
    *format = JAVACALL_MEDIA_FORMAT_UNKNOWN;
    return JAVACALL_OK;
}

javacall_result javacall_media_acquire_device(javacall_handle handle) {
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

javacall_result javacall_media_realize(javacall_handle handle,
                                       javacall_const_utf16_string mime,
                                       long mimeLength) {
    ALOG("realize");
    vita_player *pl = (vita_player *)handle;
    (void)mime; (void)mimeLength;
    if (pl == NULL) return JAVACALL_INVALID_ARGUMENT;
    if (pl->state == 0) pl->state = 1;
    return JAVACALL_OK;
}

javacall_result javacall_media_prefetch(javacall_handle handle) {
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

    pl->state = 3;

    if (pl->jts_len >= 2) {
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
    if (pl->playing) pl->stop_flag = 1;
    if (pl->state == 3) pl->state = 2;
    return JAVACALL_OK;
}

javacall_result javacall_media_pause(javacall_handle handle) {
    ALOG("pause");
    vita_player *pl = (vita_player *)handle;
    if (pl == NULL) return JAVACALL_INVALID_ARGUMENT;
    if (pl->playing) pl->stop_flag = 1;
    if (pl->state == 3) pl->state = 2;
    return JAVACALL_OK;
}

javacall_result javacall_media_resume(javacall_handle handle) {
    ALOG("resume");
    /* Restart from the beginning (simplest correct behavior) */
    return javacall_media_start(handle);
}

javacall_result javacall_media_get_time(javacall_handle handle,
                                        /*OUT*/ long* ms) {
    ALOG("get_time");
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
    /* Seeking within a tone sequence is not supported; clamp to 0. */
    *ms = 0;
    pl->cur_time_ms = 0;
    return JAVACALL_OK;
}

javacall_result javacall_media_get_duration(javacall_handle handle,
                                            /*OUT*/ long* ms) {
    ALOG("get_duration");
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
    *first_chunk_size = JTS_FIRST_CHUNK;
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
    ALOG("get_buffer_address");
    vita_player *pl = (vita_player *)handle;
    if (pl == NULL || buffer == NULL || max_size == NULL)
        return JAVACALL_INVALID_ARGUMENT;
    *buffer = pl->jts_data;
    *max_size = JTS_MAX_SIZE;
    return JAVACALL_OK;
}

javacall_result javacall_media_do_buffering(javacall_handle handle,
                                 const void* buffer,
                                 /*INOUT*/ long* length,
                                 /*OUT*/ javacall_bool* need_more_data,
                                 /*OUT*/ long* next_chunk_size) {
    ALOG("do_buffering");
    vita_player *pl = (vita_player *)handle;
    int tempo = 120, resolution = 64;
    jts_event probe[JTS_MAX_EVENTS];
    long n;

    if (pl == NULL || length == NULL || need_more_data == NULL ||
        next_chunk_size == NULL) {
        return JAVACALL_INVALID_ARGUMENT;
    }

    if (buffer == NULL || *length <= 0) {
        /* End of buffering marker (Java sends NULL/-1). JTS always
         * arrives in one chunk, so nothing to do here. */
        *length = 0;
        *need_more_data = JAVACALL_FALSE;
        *next_chunk_size = 0;
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
    ALOG("set_volume");
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
