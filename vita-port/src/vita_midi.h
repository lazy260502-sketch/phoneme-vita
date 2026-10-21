/*
 * vita_midi.h - Standard MIDI File (SMF) playback for phoneME MIDP
 *
 * This port has no MIDI synthesizer (phoneME's open-source build ships
 * without SUN's proprietary MIDI renderer). Games whose BGM is a .mid /
 * .midi file fail in Manager.createPlayer("audio/midi") with a clean
 * MediaException - see log97 (fengshenbang). This module adds a software
 * General-MIDI synthesizer:
 *
 *   - TinySoundFont (tsf.h, MIT)     : SF2 SoundFont synthesizer
 *   - TinyMidiLoader (tml.h, ZLIB)   : SMF (Format 0/1/2) parser
 *   - TimGM6mb.sf2 (GPL-2)           : compact 5.7 MB GM sound bank
 *
 * Both libraries are single-header; their implementations are compiled
 * here (TSF_IMPLEMENTATION / TML_IMPLEMENTATION) so no other translation
 * unit needs them. Rendering is 48 kHz stereo interleaved S16 to match
 * vita_audio_javacall.c's SceAudioOut port (OUT_SAMPLE_RATE /
 * OUT_CHUNK_FRAMES).
 */
#ifndef VITA_MIDI_H
#define VITA_MIDI_H

#include <stdint.h>

/* Load the GM sound bank (idempotent, lazy). Returns 0 on success.
 * Tries the writable ux0: copy first, then the read-only VPK copy. */
int vita_midi_init(void);

/* Release the sound bank (process exit; optional). */
void vita_midi_shutdown(void);

/* PCM output callback: `stereo` is interleaved S16 (L,R,L,R...),
 * `frames` frames (always a full output chunk). */
typedef void (*vita_midi_out_fn)(const int16_t *stereo, int frames, void *ctx);

/* Stop callback: return non-zero to abort playback early. */
typedef int (*vita_midi_stop_fn)(void *ctx);

/* Render a whole MIDI byte stream to 48 kHz stereo, blocking.
 *   midi_data/midi_len : the complete SMF file bytes
 *   volume             : 0..100 (maps to the synthesizer master gain)
 *   out / stop / ctx   : output + early-abort (either may be NULL)
 *   duration_ms_out    : if non-NULL, receives the estimated length (ms)
 * Returns 1 = played to completion, 0 = stopped early, -1 = failure. */
int vita_midi_play(const uint8_t *midi_data, long midi_len, long volume,
                   vita_midi_out_fn out, vita_midi_stop_fn stop, void *ctx,
                   long *duration_ms_out);

#endif /* VITA_MIDI_H */
