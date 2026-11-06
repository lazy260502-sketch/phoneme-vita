/*
 * vita_midi.c - Standard MIDI File playback (see vita_midi.h)
 *
 * Combines TinySoundFont (SF2 synthesizer) and TinyMidiLoader (SMF parser)
 * into one translation unit. The render loop follows TinySoundFont's
 * example3.c: advance a millisecond clock, dispatch every message whose
 * time has been reached, then render one output chunk. TML already folds
 * tempo meta-events into each message's absolute millisecond time, so
 * TML_SET_TEMPO needs no special case here.
 */
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include <psp2/io/fcntl.h>
#include <psp2/kernel/processmgr.h>   /* sceKernelGetProcessTimeWide (render timing) */

#include "vita_midi.h"

#define TSF_IMPLEMENTATION
#define TSF_NO_STDIO
#include "tsf.h"

/* v02.64: tml.h's TML_ERROR/TML_WARN default to empty, so a parse failure
 * was silent (log110 only said "parse FAILED"). Route them to stderr; they
 * fire only on malformed input, never per message. */
#define TML_ERROR(msg) fprintf(stderr, "[MIDI] tml error: %s\n", (msg))
#define TML_WARN(msg)  fprintf(stderr, "[MIDI] tml warn: %s\n", (msg))
#define TML_IMPLEMENTATION
#define TML_NO_STDIO
#include "tml.h"

/* Must match vita_audio_javacall.c's SceAudioOut port configuration:
 * 48 kHz, 512-frame grain, S16 stereo interleaved. */
#define MIDI_SAMPLE_RATE   48000
#define MIDI_CHUNK_FRAMES  512

/* Extra render time after the last message so a note's release envelope
 * decays instead of being cut off at the final note_off. */
#define MIDI_RELEASE_MS    1500

/* v02.65: polyphony cap. TinySoundFont grows its voice pool without limit,
 * and every voice costs an interpolation + a low-pass filter per sample at
 * 48 kHz in plain VFP code. A dense song (log111: 11 channels, 3638 notes,
 * 1.5 s release tails) can ask for far more than one Vita core renders in
 * real time, which both underruns the audio and steals the core from the
 * VM. With a cap TSF kills the oldest voice that is already in its release
 * phase first (tsf.h, tsf_note_on), i.e. the least audible one. */
#define MIDI_MAX_VOICES    32

/* One render chunk must finish within its own duration or the audio port
 * underruns: 512 frames / 48000 Hz = 10666 us. */
#define MIDI_CHUNK_BUDGET_US ((unsigned long long)MIDI_CHUNK_FRAMES * 1000000ULL / MIDI_SAMPLE_RATE)

static tsf *g_tsf = NULL;

/* v03.41: make GM program 80 ("Square Wave") a steady, single-layer square.
 *
 * Why (log139, user: the ring tone sounds "stretched / dragging"): Nokia ring tones are played on program 80
 * (Sonivox: a flat square that stays at one level until note-off). TimGM6mb's preset 0:80 is not that. It stacks two
 * layers per key range (regions 0-6 and 7-13) with tunings a few cents apart - they beat, so the level wobbles - and
 * the layers differ in sustain level (0.977 vs 0.676) and carry a mod-envelope pitch/filter sweep. Measured on the host
 * (TinySoundFont, the same code that runs on the device; device pcm_dump == host render sample for sample):
 *   one 282 ms note, level 20 ms -> 260 ms:  unmodified -8.2 dB; pitch/filter env off -3.1 dB; + flat amp envelope
 *   -2.6 dB (still beating); + one layer only -0.1 dB.
 *   Per note, start-of-note vs end-of-note RMS over the real tunes (notes >= 150 ms):
 *     D0 (22 notes)   median -10.4 dB -> 0.0 dB, worst -16.1 -> -1.3
 *     D1 (181 notes)  median  -7.4 dB -> -1.2 dB, worst -18.0 -> -3.7 (key 65 / F5; a residual of the sample itself)
 *     D2: all notes are 88 ms, too short for this measure.
 * so the fix is: keep only the first layer (regions 0-6), zero its mod-envelope pitch/filter sweep and lowpass, make the
 * amplitude envelope a flat sustain, and give back the 6 dB the removed layer contributed. Only preset 0:80 is touched;
 * every other program (game BGM) is unchanged. Peaks of the patched render: 30 / 33 / 37 % FS, no clipping.
 * This is measured, not heard: whether it sounds right is for the user's ears. */
static void midi_patch_square_wave(tsf *t) {
    int idx = tsf_get_presetindex(t, 0, 80);
    struct tsf_preset *p;
    struct tsf_region *donor = NULL;
    int i, kept = 0, dropped = 0, remapped = 0;

    if (idx < 0) {
        return;
    }
    p = &t->presets[idx];
    for (i = 0; i < p->regionNum; i++) {
        struct tsf_region *r = &p->regions[i];
        if (r->ampenv.sustain < 0.9f) {
            /* second, detuned layer: remove it from every key */
            r->lokey = 127;
            r->hikey = 0;
            dropped++;
            continue;
        }
        r->ampenv.decay = 0.0f;
        r->ampenv.sustain = 1.0f;
        /* v03.42: TimGM6mb releases over 0.385 s (-23 dB still audible 120 ms after note-off), longer than a whole
         * 88 ms ring-tone note, so every note smeared into the next ("stretched"). Sonivox's square stops at once. */
        r->ampenv.release = 0.02f;
        r->modEnvToPitch = 0;
        r->modEnvToFilterFc = 0;
        r->modLfoToPitch = 0;
        r->vibLfoToPitch = 0;
        r->modLfoToFilterFc = 0;
        /* v03.61: 13500 was intended as "bypass", but TSF tests `f <= 13500` before building the biquad
         * (tsf.h: tsf_voice_start__ and tsf_voice_render both use `<= 13500 ? cents2Hertz(...) : 1.0f`), so
         * 13500 actually ENABLED a 19.9 kHz lowpass with QInv = 1 (region filterQ = 0 dB). Anything above
         * 13500 is the real bypass. Measured on the host: < 1 dB difference on the square, so this is a
         * correctness fix, not a sound change. */
        r->initialFilterFc = 14000;
        r->attenuation -= 6.0f;          /* dB: replace the level of the removed layer */
        if (r->lokey == 0) {
            /* the 336-sample single-period square, region keys 0-42: the only region in this preset whose
             * loop is a whole number of periods AND long enough to survive interpolation. */
            donor = r;
        }
        kept++;
    }
    /* v03.61: the two top regions are broken in TimGM6mb itself. Region keys 91-102 loops 12 samples
     * (3675 Hz at 44.1 kHz) and 103-108 loops 9 samples (4900 Hz) - far too few to hold a square, so the
     * waveform is mangled. Measured with the device's own tsf.h on the host (steady key, 0.39 volume, CC7 120):
     *   original : pitch error +3.0..+6.5 cents, crest factor 1.42..1.55
     *   donor C1 : pitch error -0.8..+2.0 cents, crest factor 1.26..1.44
     *   donor C4 : pitch error +2.4..+5.2 cents, crest factor 1.48..1.90  (41-sample loop, worse)
     * A ideal square has a crest factor near 1.0 (1.09 with Gibbs ringing), so the C1 donor is the closest to a
     * real square while also being the most in tune. Nothing below key 91 is touched, so game BGM and the
     * ring tones used by shipped titles (keys 55-78) are unaffected. */
    if (donor != NULL) {
        for (i = 0; i < p->regionNum; i++) {
            struct tsf_region *r = &p->regions[i];
            if (r->hikey < r->lokey) {   /* dropped layer */
                continue;
            }
            if (r->lokey <= 90) {
                continue;
            }
            r->sample_rate = donor->sample_rate;
            r->offset = donor->offset;
            r->end = donor->end;
            r->loop_start = donor->loop_start;
            r->loop_end = donor->loop_end;
            r->loop_mode = donor->loop_mode;
            r->pitch_keycenter = donor->pitch_keycenter;
            r->tune = donor->tune;
            r->transpose = donor->transpose;
            remapped++;
        }
    }
    fprintf(stderr, "[MIDI] preset 0:80 patched to a steady single layer: %d regions kept, %d dropped, "
            "%d high regions remapped to the %u-sample donor\n",
            kept, dropped, remapped, donor != NULL ? (donor->loop_end - donor->loop_start) : 0u);
}

int vita_midi_init(void) {
    static const char *paths[2] = {
        "ux0:/data/J2ME00001/TimGM6mb.sf2",
        "app0:/data/J2ME00001/TimGM6mb.sf2",
    };
    int p;

    if (g_tsf != NULL) {
        return 0;
    }

    for (p = 0; p < 2; p++) {
        SceUID fd = sceIoOpen(paths[p], SCE_O_RDONLY, 0);
        int size;
        int rd;
        unsigned char *buf;
        if (fd < 0) {
            continue;
        }
        size = (int)sceIoLseek(fd, 0, SCE_SEEK_END);
        sceIoLseek(fd, 0, SCE_SEEK_SET);
        if (size <= 0) {
            sceIoClose(fd);
            continue;
        }
        buf = (unsigned char *)malloc((size_t)size);
        if (buf == NULL) {
            sceIoClose(fd);
            continue;
        }
        rd = (int)sceIoRead(fd, buf, size);
        sceIoClose(fd);
        if (rd != size) {
            free(buf);
            continue;
        }
        g_tsf = tsf_load_memory(buf, size);
        free(buf);
        if (g_tsf != NULL) {
            tsf_set_output(g_tsf, TSF_STEREO_INTERLEAVED, MIDI_SAMPLE_RATE, 0.0f);
            if (!tsf_set_max_voices(g_tsf, MIDI_MAX_VOICES)) {
                fprintf(stderr, "[MIDI] tsf_set_max_voices(%d) FAILED\n",
                        MIDI_MAX_VOICES);
            }
            midi_patch_square_wave(g_tsf);
            fprintf(stderr, "[MIDI] soundfont loaded: %d bytes, %d presets, "
                    "max %d voices\n",
                    size, tsf_get_presetcount(g_tsf), MIDI_MAX_VOICES);
            return 0;
        }
        fprintf(stderr, "[MIDI] soundfont parse FAILED (%d bytes)\n", size);
    }
    fprintf(stderr, "[MIDI] soundfont not found\n");
    return -1;
}

void vita_midi_shutdown(void) {
    if (g_tsf != NULL) {
        tsf_close(g_tsf);
        g_tsf = NULL;
    }
}

/* v02.65: one summary line per song. avg/max are CPU microseconds spent in
 * message dispatch + tsf_render_short per 512-frame chunk, to be read against
 * the budget (10666 us): avg close to the budget or "over" > 0 means the
 * synth cannot keep real time on one core, and every one of those us is taken
 * from the VM when both land on the same core. */
static void midi_perf_report(const char *why, unsigned long chunks,
                             unsigned long long tot_us,
                             unsigned long long max_us, unsigned long over,
                             unsigned long vsum, int vmax) {
    if (chunks == 0) {
        return;
    }
    fprintf(stderr, "[MIDI] %s: %lu chunks, render avg %llu us max %llu us "
            "(budget %llu us, %lu over), voices avg %lu max %d (cap %d)\n",
            why, chunks, tot_us / chunks, max_us,
            (unsigned long long)MIDI_CHUNK_BUDGET_US, over,
            vsum / chunks, vmax, MIDI_MAX_VOICES);
}


int vita_midi_play(const uint8_t *midi_data, long midi_len, long volume,
                   vita_midi_out_fn out, vita_midi_stop_fn stop, void *ctx,
                   long *duration_ms_out) {
    tml_message *msg;
    tml_message *cur;
    double msec;
    long release_frames;
    int16_t buf[MIDI_CHUNK_FRAMES * 2];
    int ch;
    /* v02.65 render statistics (see midi_perf_report) */
    unsigned long long t0 = 0, tot_us = 0, max_us = 0;
    unsigned long chunks = 0, over = 0, vsum = 0;
    int vmax = 0;

    if (vita_midi_init() != 0) {
        return -1;
    }
    if (midi_data == NULL || midi_len <= 0 || midi_len > 0x7fffffffL) {
        return -1;
    }

    msg = tml_load_memory(midi_data, (int)midi_len);
    if (msg == NULL) {
        fprintf(stderr, "[MIDI] parse FAILED (%ld bytes) head=%02x %02x %02x %02x"
                " %02x %02x %02x %02x\n", midi_len,
                midi_len > 0 ? midi_data[0] : 0, midi_len > 1 ? midi_data[1] : 0,
                midi_len > 2 ? midi_data[2] : 0, midi_len > 3 ? midi_data[3] : 0,
                midi_len > 4 ? midi_data[4] : 0, midi_len > 5 ? midi_data[5] : 0,
                midi_len > 6 ? midi_data[6] : 0, midi_len > 7 ? midi_data[7] : 0);
        return -1;
    }

    {
        int used_channels = 0, used_programs = 0, total_notes = 0;
        unsigned int time_first_note = 0, time_length = 0;
        tml_get_info(msg, &used_channels, &used_programs, &total_notes,
                     &time_first_note, &time_length);
        if (duration_ms_out != NULL) {
            *duration_ms_out = (long)time_length;
        }
        fprintf(stderr, "[MIDI] play: %d ch, %d prog, %d notes, %u ms\n",
                used_channels, used_programs, total_notes, time_length);
    }

    /* Master gain from the player volume (0..100). */
    if (volume < 0) volume = 0;
    if (volume > 100) volume = 100;
    tsf_set_volume(g_tsf, (float)volume / 100.0f);

    /* v02.65: start from a clean synth. A song that was cut short (pause,
     * next player) used to leave its voices and channel state behind; they
     * kept ringing - and costing CPU - under the next song. */
    tsf_reset(g_tsf);

    /* MIDI channel 10 (index 9) is percussion: preset from bank 128. */
    tsf_channel_set_bank_preset(g_tsf, 9, 128, 0);

    msec = 0.0;
    cur = msg;
    while (cur != NULL) {
        if (stop != NULL && stop(ctx)) {
            midi_perf_report("stopped", chunks, tot_us, max_us, over,
                             vsum, vmax);
            tsf_reset(g_tsf);
            tml_free(msg);
            return 0;
        }
        t0 = sceKernelGetProcessTimeWide();
        /* Dispatch every message whose time has been reached. */
        while (cur != NULL && msec >= (double)cur->time) {
            switch (cur->type) {
            case TML_PROGRAM_CHANGE:
                tsf_channel_set_presetnumber(g_tsf, cur->channel, cur->program,
                                             (cur->channel == 9));
                break;
            case TML_NOTE_ON:
                tsf_channel_note_on(g_tsf, cur->channel, cur->key,
                                    cur->velocity / 127.0f);
                break;
            case TML_NOTE_OFF:
                tsf_channel_note_off(g_tsf, cur->channel, cur->key);
                break;
            case TML_PITCH_BEND:
                tsf_channel_set_pitchwheel(g_tsf, cur->channel, cur->pitch_bend);
                break;
            case TML_CONTROL_CHANGE:
                tsf_channel_midi_control(g_tsf, cur->channel, cur->control,
                                         cur->control_value);
                break;
            default:
                /* TML_SET_TEMPO etc. already folded into the time stamps. */
                break;
            }
            cur = cur->next;
        }
        tsf_render_short(g_tsf, buf, MIDI_CHUNK_FRAMES, 0);
        /* v02.65: CPU time of dispatch + render for this chunk, taken BEFORE
         * out() (which blocks until the port wants more). Nothing is printed
         * while playing - a stderr write costs 12-22 ms, more than the 10.7 ms
         * chunk budget - the summary goes out when the song ends or stops. */
        {
            unsigned long long dt = sceKernelGetProcessTimeWide() - t0;
            int nv = tsf_active_voice_count(g_tsf);
            chunks++;
            tot_us += dt;
            if (dt > max_us) max_us = dt;
            if (dt > MIDI_CHUNK_BUDGET_US) over++;
            vsum += (unsigned long)nv;
            if (nv > vmax) vmax = nv;
        }
        if (out != NULL) {
            out(buf, MIDI_CHUNK_FRAMES, ctx);
        }
        msec += MIDI_CHUNK_FRAMES * (1000.0 / MIDI_SAMPLE_RATE);
    }

    /* Tail: release every still-sounding note and render the decay. */
    for (ch = 0; ch < 16; ch++) {
        tsf_channel_note_off_all(g_tsf, ch);
    }
    release_frames = (long)(MIDI_RELEASE_MS * (MIDI_SAMPLE_RATE / 1000.0));
    while (release_frames > 0) {
        if (stop != NULL && stop(ctx)) {
            break;
        }
        /* v03.38 (log138): stop as soon as every voice has finished. The fixed 1500 ms tail was ~1200 ms of digital
         * silence after a ring tone (device pcm_dump == host render, last 1.2 s all zero), which made each loop of a
         * 9.0 s tune take 10.5 s. Voices still in their release phase keep the count above 0, so no decay is cut. */
        if (tsf_active_voice_count(g_tsf) == 0) {
            break;
        }
        tsf_render_short(g_tsf, buf, MIDI_CHUNK_FRAMES, 0);
        if (out != NULL) {
            out(buf, MIDI_CHUNK_FRAMES, ctx);
        }
        release_frames -= MIDI_CHUNK_FRAMES;
    }

    midi_perf_report("ended", chunks, tot_us, max_us, over, vsum, vmax);
    tsf_reset(g_tsf);
    tml_free(msg);
    return 1;
}
