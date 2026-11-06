package com.nokia.mid.sound;

import java.io.ByteArrayOutputStream;

/*
 * RingTone - Nokia ringtone (Smart Messaging "OTA") and RTTTL -> WAV.
 *
 * Why this exists: com.nokia.mid.sound.Sound() is frequently handed a raw
 * ringtone blob - neither MThd (MIDI) nor RIFF (WAV) - and before v02.88
 * such a Sound stayed SOUND_UNINITIALIZED, i.e. the game played its music
 * and sound effects in total silence. log118 showed a shipped title
 * (Xiyangyang) doing exactly that with real Nokia OTA ringtones.
 *
 * Why WAV and not a tone sequence: the MMAPI tone path needs
 * "audio/x-tone-seq" players, and getting those to work from a stream
 * depends on DirectTone's JTS validator. The WAV path is the one this
 * port has exercised since v01.96 (RIFF -> PCM -> sceAudioOut), so the
 * conversion target is a plain 8 kHz 8-bit mono WAV: 8 KB per second,
 * resampled 6x to 48 kHz by the native renderer.
 *
 * Formats understood (anything else returns null, i.e. "unsupported" -
 * never a crash, never silence-by-exception):
 *   1. OTA / Smart Messaging ringtone, the "02 4A 3A ..." blob.
 *      The command list is: length byte, then that many command bytes
 *      whose upper 7 bits are the id (0x25 = ringing tone programming,
 *      0x1d = sound/song, 0x22 = unicode text). The song itself is a
 *      MSB-first BIT stream of 3-bit instructions:
 *        0x01 note   4-bit note (0 = rest) + 3-bit duration + 2-bit
 *                    modifier; duration = tick * (32 >> exp)
 *        0x02 scale  2-bit octave, MIDI base = octave*12 + 59
 *        0x03 style  2-bit, controls how much of the note is silence
 *        0x04 tempo  5-bit index into the BPM tick table
 *        0x05 volume 4-bit
 *        0x00 pattern (repeats) - deliberately unsupported; a wrong guess
 *                    here would garble the melody, so we bail out and
 *                    behave exactly like before this file existed.
 *      Bit layout, instruction ids and the tick table follow the Nokia
 *      Smart Messaging spec as implemented by the Sonivox/EAS OTA
 *      parser (eas_ota.c), which is the reference J2ME-Loader uses.
 *   2. RTTTL text: "name:d=4,o=5,b=63:d,8p,g#5,4c." etc.
 *
 * The synth is a square wave with a short fade, which is what the tone
 * path in vita_audio_javacall.c already does for Manager.playTone().
 */
final class RingTone {

    private static final int RATE = 8000;
    /* Hard caps so a corrupt blob cannot exhaust the 32 MB Java heap. */
    private static final int MAX_MS = 120000;
    private static final int MAX_EVENTS = 2000;
    private static final int AMP = 96;

    /* Tick for a 32nd note, in 1/256 ms - Nokia's 32-entry tempo table
     * (index 0 = slowest). Copied from eas_ota.c bpmTable. */
    private static final int[] BPM_TICK = {
        76800, 68571, 61935, 54857,
        48000, 42667, 38400, 34286,
        30476, 27429, 24000, 21333,
        19200, 17143, 15360, 13714,
        12000, 10667, 9600, 8533,
        7680, 6737, 6000, 5408,
        4800, 4267, 3840, 3398,
        3024, 2685, 2400, 2133
    };

    private RingTone() {
    }

    /* ------------------------------------------------------------------ */
    /* Entry point                                                         */
    /* ------------------------------------------------------------------ */

    /* v03.05 (log123): xiyangyang re-creates its Sound for the same blob on
     * every menu transition (five "init len=326" in a row), and each one
     * synthesised ~270 KB of PCM one byte at a time in the interpreter and
     * pushed it through ~70 KNI do_buffering calls. Remember the last few
     * results, keyed by blob content, so the repeat costs a compare. */
    private static final int CACHE_SLOTS = 4;
    private static byte[][] cacheKey = new byte[CACHE_SLOTS][];
    private static byte[][] cacheWav = new byte[CACHE_SLOTS][];
    private static int cacheNext = 0;

    /** Convert a ringtone blob to WAV, or return null if not recognised. */
    static byte[] toWav(byte[] data) {
        if (data == null || data.length < 6) {
            return null;
        }
        synchronized (RingTone.class) {
            for (int c = 0; c < CACHE_SLOTS; c++) {
                byte[] k = cacheKey[c];
                if (k != null && k.length == data.length && sameBytes(k, data)) {
                    return cacheWav[c];
                }
            }
        }
        int[] ev = null;
        if (isOta(data)) {
            ev = parseOta(data);
        } else if (isRtttl(data)) {
            ev = parseRtttl(data);
        }
        if (ev == null || ev.length < 2) {
            return null;
        }
        byte[] wav = synth(ev);
        if (wav != null) {
            synchronized (RingTone.class) {
                byte[] key = new byte[data.length];
                System.arraycopy(data, 0, key, 0, data.length);
                cacheKey[cacheNext] = key;
                cacheWav[cacheNext] = wav;
                cacheNext = (cacheNext + 1) % CACHE_SLOTS;
            }
        }
        return wav;
    }

    /*
     * v03.34: convert a ringtone blob to a Standard MIDI File instead of an 8 kHz WAV.
     *
     * J2ME-Loader does not synthesise ringtones at all: Sound.init(FORMAT_TONE) turns them into "audio/midi"
     * and Android's Sonivox EAS plays them; EAS's own OTA parser (eas_ota.c) fixes GM program 80 (Square
     * Lead), channel volume 127 and velocity vol*4+67. The 8 kHz / 8-bit WAV this class made before is
     * the reason the output was poor whatever the resampler did (log135: steady hiss from the
     * 8 kHz -> 48 kHz upsampling, plus 8-bit quantisation), so now the same notes go through the port's
     * 48 kHz TinySoundFont + TimGM6mb.sf2 MIDI renderer (vita_midi.c), as any real MIDI file does.
     *
     * Division 500 ticks per quarter at tempo 500000 us makes one tick exactly 1 ms, so the event list
     * (milliseconds) maps 1:1 to ticks. Returns null if the blob is not a ringtone.
     */
    private static final int MIDI_PROGRAM = 80;
    /* Channel volume (CC7) - the only level control on this path: Sound.setGain(255) makes
     * tsf_set_volume(1.0), and CC7 scales everything after that.
     *
     * v03.61: this was 80, chosen when preset 0:80 still had TWO layers: the selection was made by sweeping
     * CC7 with debug/tools/wavq/render_smf.c and 127 clipped 72/3487/5563 samples. v03.41 then dropped the
     * detuned second layer, which cut the level by ~6 dB, and v03.61 removed nothing more - so the constant was
     * left 12 dB too low for the preset that actually ships.
     *
     * Re-swept v03.61 with the same tsf.h the device links, patched preset, velocity 95, 38-note monophonic tune
     * built exactly like buildMidi() (division 500, tempo 500000, rest = dur >> 4):
     *   CC7  80 -> peak 6483 = 19.8 % FS (-14.1 dBFS), 0 clipped
     *   CC7 100 -> peak 12570 = 38.4 % FS (-8.3 dBFS), 0 clipped
     *   CC7 112 -> peak 17604 = 53.7 % FS (-5.4 dBFS), 0 clipped
     *   CC7 120 -> peak 21614 = 66.0 % FS (-3.6 dBFS), 0 clipped
     *   CC7 127 -> peak 25587 = 78.1 % FS (-2.1 dBFS), 0 clipped
     *   CC7 127, old 2-layer preset -> peak 32768, 647073/1384448 = 46.7 % clipped  (the old measurement)
     * and the device pcm_dump agreed sample for sample (peak 6483 at CC7 80, 0 clipped samples, -16.1 dBFS RMS,
     * non-harmonic energy 0.013 % / -38.9 dB): the waveform was already clean, it was simply quiet.
     * 120 leaves 3.6 dB of headroom for the release tails of dense tunes while raising the level by 10.5 dB; the
     * matching equal-loudness gain for a full-scale square is +12 dB, so this recovers almost all of it. */
    private static final int MIDI_CC7 = 120;

    static byte[] toMidi(byte[] data) {
        if (data == null || data.length < 6) {
            return null;
        }
        synchronized (RingTone.class) {
            for (int c = 0; c < CACHE_SLOTS; c++) {
                byte[] k = midiKey[c];
                if (k != null && k.length == data.length && sameBytes(k, data)) {
                    return midiVal[c];
                }
            }
        }
        int[] ev = null;
        if (isOta(data)) {
            ev = parseOta(data);
        } else if (isRtttl(data)) {
            ev = parseRtttl(data);
        }
        if (ev == null || ev.length < 2) {
            return null;
        }
        byte[] mid = buildMidi(ev);
        if (mid != null) {
            synchronized (RingTone.class) {
                byte[] key = new byte[data.length];
                System.arraycopy(data, 0, key, 0, data.length);
                midiKey[midiNext] = key;
                midiVal[midiNext] = mid;
                midiNext = (midiNext + 1) % CACHE_SLOTS;
            }
        }
        return mid;
    }

    private static byte[][] midiKey = new byte[CACHE_SLOTS][];
    private static byte[][] midiVal = new byte[CACHE_SLOTS][];
    private static int midiNext = 0;

    /** events = (midi note, duration ms, trailing silence ms) triples + volume marker, see parseOta. */
    private static byte[] buildMidi(int[] ev) {
        int vol = -1 - ev[ev.length - 1];
        if (vol <= 0) {
            vol = 8;
        }
        int vel = vol * 4 + 67;                       /* eas_ota.c OTA_VEL_MUL / OTA_VEL_OFS */
        if (vel > 127) {
            vel = 127;
        }
        int notes = ev.length / 3;
        byte[] t = new byte[32 + notes * 14];         /* a note is at most 2 * (3 delta + 3) bytes */
        int p = 0;
        /* meta: tempo 500000 us/quarter */
        p = put(t, p, "\u0000\u00ff\u0051\u0003");
        t[p++] = (byte) 0x07;
        t[p++] = (byte) 0xA1;
        t[p++] = (byte) 0x20;
        /* program change + channel volume 127 on channel 0 (eas_ota.c does both at time 0) */
        t[p++] = 0;
        t[p++] = (byte) 0xC0;
        t[p++] = (byte) MIDI_PROGRAM;
        t[p++] = 0;
        t[p++] = (byte) 0xB0;
        t[p++] = 7;
        t[p++] = (byte) MIDI_CC7;

        int now = 0;                                  /* start of the current note, ms */
        int last = 0;                                 /* time of the last event written */
        int written = 0;                              /* notes emitted */
        int i;
        for (i = 0; i + 2 < ev.length; i += 3) {
            int key = ev[i];
            int dur = ev[i + 1];
            int rest = ev[i + 2];
            if (dur <= 0) {
                continue;
            }
            int sound = dur - rest;
            if (key > 0 && key <= 127 && sound > 0 && vel > 0) {
                p = putVlq(t, p, now - last);
                t[p++] = (byte) 0x90;
                t[p++] = (byte) key;
                t[p++] = (byte) vel;
                last = now;
                p = putVlq(t, p, sound);
                t[p++] = (byte) 0x80;
                t[p++] = (byte) key;
                t[p++] = 0;
                last = now + sound;
                written++;
            }
            now += dur;
        }
        /* end of track at the end of the last note slot (so a trailing rest keeps its length) */
        p = putVlq(t, p, now > last ? now - last : 0);
        t[p++] = (byte) 0xFF;
        t[p++] = (byte) 0x2F;
        t[p++] = 0;
        if (written == 0) {
            return null;
        }

        byte[] m = new byte[14 + 8 + p];
        int q = 0;
        q = put(m, q, "MThd");
        q = put32be(m, q, 6);
        m[q++] = 0;                                   /* format 0 */
        m[q++] = 0;
        m[q++] = 0;                                   /* one track */
        m[q++] = 1;
        m[q++] = (byte) 0x01;                         /* division 500 = 0x01F4 */
        m[q++] = (byte) 0xF4;
        q = put(m, q, "MTrk");
        q = put32be(m, q, p);
        System.arraycopy(t, 0, m, q, p);
        return m;
    }

    private static int putVlq(byte[] b, int p, int v) {
        if (v < 0) {
            v = 0;
        }
        if (v >= (1 << 21)) {
            b[p++] = (byte) (0x80 | ((v >> 21) & 0x7F));
        }
        if (v >= (1 << 14)) {
            b[p++] = (byte) (0x80 | ((v >> 14) & 0x7F));
        }
        if (v >= (1 << 7)) {
            b[p++] = (byte) (0x80 | ((v >> 7) & 0x7F));
        }
        b[p++] = (byte) (v & 0x7F);
        return p;
    }

    private static int put32be(byte[] b, int p, int v) {
        b[p++] = (byte) ((v >> 24) & 0xFF);
        b[p++] = (byte) ((v >> 16) & 0xFF);
        b[p++] = (byte) ((v >> 8) & 0xFF);
        b[p++] = (byte) (v & 0xFF);
        return p;
    }

    private static boolean sameBytes(byte[] a, byte[] b) {
        for (int i = 0; i < a.length; i++) {
            if (a[i] != b[i]) {
                return false;
            }
        }
        return true;
    }

    /* ------------------------------------------------------------------ */
    /* OTA (Smart Messaging)                                               */
    /* ------------------------------------------------------------------ */

    private static boolean isOta(byte[] d) {
        int n = d[0] & 0xFF;
        return n >= 2 && n <= 32 && d.length > n + 2
                && ((d[1] & 0xFF) >> 1) == 0x25;
    }

    private static int[] parseOta(byte[] d) {
        int cmdLen = d[0] & 0xFF;
        int songAt = 1 + cmdLen;
        int i;
        boolean hasSound = false;

        for (i = 1; i <= cmdLen && i < d.length; i++) {
            int id = (d[i] & 0xFF) >> 1;
            if (id == 0x1d) {
                hasSound = true;
            }
        }
        if (!hasSound || songAt >= d.length) {
            return null;
        }

        /* The whole blob is one bit stream, read from bit 0. Layout (Nokia
         * Smart Messaging, as documented by J2ME-Loader's ToneVerifier and
         * checked against the three real ringtones of log118 - all three
         * consume exactly up to zero padding this way):
         *   cmdLen:8  RTP:7+filler:1  sound:7  songType:3
         *   [titleLen:4 title:8*titleLen]  (songType 1 only)
         *   sequenceLength:8
         *   repeated sequenceLength times:
         *     patternHeader:3  patternId:2  loop:4  instrCount:8  instr...
         * The v02.88 version skipped all of that and read instructions at
         * the first song byte, so the first "instruction" was the song type
         * and every real ringtone was rejected (log120: "ringtone->wav
         * FAILED (null)"). */
        Bits b = new Bits(d, 0);
        b.get(8);                      /* command length */
        b.get(8);                      /* ringing-tone-programming + filler */
        int soundId = b.get(7);        /* 0x1d = sound */
        if (soundId != 0x1d) {
            return null;
        }
        int songType = b.get(3);
        if (songType == 1) {
            int titleLen = b.get(4);
            for (int t = 0; t < titleLen; t++) {
                if (b.get(8) < 0) {
                    return null;
                }
            }
        } else if (songType != 2) {
            return null;               /* unsupported song type */
        }
        int sequences = b.get(8);
        if (sequences <= 0) {
            return null;
        }

        int[] out = new int[MAX_EVENTS * 3];
        int n = 0;
        int totalMs = 0;

        int tick = BPM_TICK[15];       /* 140 BPM-ish default */
        int octave = 4 * 12 + 59;      /* MIDI note of the default octave */
        /* v03.14: the default style is 0 ("natural": the last 1/16 of every note is silent).
         * The reference parser (Sonivox eas_ota.c:895, which this file says it follows) starts
         * with style = 0. This code started with 1 (continuous, no gap at all), and the three
         * Xiyangyang tunes never send a style instruction, so every note ran straight into the
         * next: 180 of 181 note boundaries in the 34 s tune had a 0 ms gap, 46 of those pairs are
         * the SAME pitch twice, which became one long tone and lost the rhythm (log125: "hard to
         * make out"). With 0 the median gap is 11 ms (34 s tune) / 18 ms (9 s tune). */
        int style = 0;
        int volume = 7;

        seqLoop:
        for (int seq = 0; seq < sequences; seq++) {
            if (b.get(3) < 0 || b.get(2) < 0 || b.get(4) < 0) {
                break;                 /* pattern header id, id, loop */
            }
            int count = b.get(8);      /* instructions in this pattern */
            if (count < 0) {
                break;
            }
            for (int ins = 0; ins < count; ins++) {
            int id = b.get(3);
            if (id < 0) {
                break seqLoop;         /* end of data */
            }
            if (id == 0x01) {
                int note = b.get(4);
                int exp = b.get(3);
                int mod = b.get(2);
                if (note < 0 || exp < 0 || mod < 0) {
                    break seqLoop;
                }
                int dur = (tick * (0x20 >> exp)) >> 8;   /* ms */
                if (mod == 1) {
                    dur += dur >> 1;                     /* dotted */
                } else if (mod == 2) {
                    dur += (dur >> 1) + (dur >> 2);      /* double dotted */
                } else if (mod == 3) {
                    dur = (dur * 2) / 3;                 /* triplet */
                }
                if (dur <= 0) {
                    dur = 1;
                }

                int midi = 0;
                int rest = 0;
                if (note != 0) {
                    midi = note + octave;
                    if (style == 0) {
                        rest = dur >> 4;
                    } else if (style == 1) {
                        rest = 0;
                    } else {
                        rest = dur >> 1;
                    }
                }
                if (n + 3 > out.length) {
                    break seqLoop;
                }
                out[n++] = midi;
                out[n++] = dur;
                out[n++] = rest;
                totalMs += dur;
                if (totalMs >= MAX_MS) {
                    break seqLoop;
                }
            } else if (id == 0x02) {
                int v = b.get(2);
                if (v < 0) break seqLoop;
                octave = v * 12 + 59;
            } else if (id == 0x03) {
                int v = b.get(2);
                if (v < 0) break seqLoop;
                style = v;
            } else if (id == 0x04) {
                int v = b.get(5);
                if (v < 0) break seqLoop;
                tick = BPM_TICK[v & 0x1F];
            } else if (id == 0x05) {
                int v = b.get(4);
                if (v < 0) break seqLoop;
                volume = v;
            } else {
                /* Unknown instruction id (0, 6, 7): the data is not what
                 * we think it is, stop and play what was decoded so far. */
                break seqLoop;
            }
            }   /* for ins */
        }       /* for seq */
        if (n == 0) {
            return null;
        }
        int[] ev = new int[n];
        System.arraycopy(out, 0, ev, 0, n);
        /* volume lives in every 3rd slot as 0..15 -> fold into the
         * synthesis stage through a sentinel note below 0. Simplest: keep
         * it in the last event triple's rest field is wrong, so append it
         * as a separate trailing entry the synthesiser reads. */
        ev = appendVolume(ev, volume);
        return ev;
    }

    private static int[] appendVolume(int[] ev, int vol) {
        int[] r = new int[ev.length + 1];
        System.arraycopy(ev, 0, r, 0, ev.length);
        r[ev.length] = -1 - vol;      /* negative = volume marker */
        return r;
    }

    /** MSB-first bit reader (the OTA song stream is bit packed). */
    private static final class Bits {
        private final byte[] d;
        private int pos;
        private int cur;
        private int left;

        Bits(byte[] d, int off) {
            this.d = d;
            this.pos = off;
        }

        int get(int n) {
            int v = 0;
            while (n > 0) {
                if (left == 0) {
                    if (pos >= d.length) {
                        return -1;
                    }
                    cur = d[pos++] & 0xFF;
                    left = 8;
                }
                int take = n < left ? n : left;
                int shift = left - take;
                v = (v << take) | ((cur >>> shift) & ((1 << take) - 1));
                left -= take;
                n -= take;
            }
            return v;
        }
    }

    /* ------------------------------------------------------------------ */
    /* RTTTL                                                               */
    /* ------------------------------------------------------------------ */

    private static boolean isRtttl(byte[] d) {
        int i;
        /* "name:" within the first bytes and at least two ':' overall. */
        int colons = 0;
        int firstColon = -1;
        for (i = 0; i < d.length && i < 64; i++) {
            int c = d[i] & 0xFF;
            if (c == ':') {
                colons++;
                if (firstColon < 0) {
                    firstColon = i;
                }
            } else if (!(c == ' ' || c == '=' || c == ',' || c == '.' ||
                         c == '#' || (c >= '0' && c <= '9') ||
                         (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))) {
                return false;
            }
        }
        return colons >= 2 && firstColon > 0;
    }

    private static int[] parseRtttl(byte[] data) {
        String s = new String(data, 0, data.length);
        int c1 = s.indexOf(':');
        int c2 = s.indexOf(':', c1 + 1);
        if (c1 <= 0 || c2 <= c1) {
            return null;
        }
        String defaults = s.substring(c1 + 1, c2).toLowerCase();
        String notes = s.substring(c2 + 1);

        int defDur = 4;
        int defOct = 5;
        int bpm = 63;
        int p = 0;
        while (p < defaults.length()) {
            int comma = defaults.indexOf(',', p);
            String item = (comma < 0) ? defaults.substring(p)
                                      : defaults.substring(p, comma);
            p = (comma < 0) ? defaults.length() : comma + 1;
            item = item.trim();
            if (item.length() >= 3 && item.charAt(1) == '=') {
                int v;
                try {
                    v = Integer.parseInt(item.substring(2));
                } catch (NumberFormatException e) {
                    continue;
                }
                char k = item.charAt(0);
                if (k == 'd') {
                    defDur = v;
                } else if (k == 'o') {
                    defOct = v;
                } else if (k == 'b') {
                    bpm = v;
                }
            }
        }
        if (defDur <= 0 || bpm <= 0) {
            return null;
        }

        int[] out = new int[MAX_EVENTS * 3];
        int n = 0;
        int totalMs = 0;
        int p2 = 0;

        while (p2 < notes.length()) {
            int comma = notes.indexOf(',', p2);
            String tok = (comma < 0) ? notes.substring(p2)
                                     : notes.substring(p2, comma);
            p2 = (comma < 0) ? notes.length() : comma + 1;
            tok = tok.trim();
            if (tok.length() == 0) {
                continue;
            }

            int dur = defDur;
            int oct = defOct;
            boolean dotted = false;
            int i = 0;

            /* optional leading duration digits (e.g. "8d" or "16p") */
            if (i < tok.length() && tok.charAt(i) >= '0'
                    && tok.charAt(i) <= '9') {
                int st = i;
                while (i < tok.length() && tok.charAt(i) >= '0'
                        && tok.charAt(i) <= '9') {
                    i++;
                }
                try {
                    dur = Integer.parseInt(tok.substring(st, i));
                } catch (NumberFormatException e) {
                    continue;
                }
            }
            if (i >= tok.length()) {
                continue;
            }

            char c = Character.toLowerCase(tok.charAt(i));
            if (c == 'p') {
                i++;
            } else if (c >= 'a' && c <= 'g') {
                i++;
            } else {
                continue;
            }
            boolean sharp = false;
            if (c != 'p' && i < tok.length() && tok.charAt(i) == '#') {
                sharp = true;
                i++;
            }
            /* trailing octave digit */
            if (i < tok.length() && tok.charAt(i) >= '0'
                    && tok.charAt(i) <= '9') {
                oct = tok.charAt(i) - '0';
                i++;
            }
            if (i < tok.length() && tok.charAt(i) == '.') {
                dotted = true;
                i++;
            }

            int ms = (60000 / bpm) * 4 / (dur <= 0 ? 4 : dur);
            if (dotted) {
                ms += ms >> 1;
            }
            if (ms <= 0) {
                ms = 1;
            }

            int midi = 0;
            if (c != 'p') {
                midi = midiOf(c, sharp, oct);
                if (midi < 0) {
                    continue;
                }
            }
            if (n + 3 > out.length) {
                break;
            }
            out[n++] = midi;
            out[n++] = ms;
            out[n++] = midi == 0 ? 0 : (ms >> 5);   /* tiny gap */
            totalMs += ms;
            if (totalMs >= MAX_MS) {
                break;
            }
        }
        if (n == 0) {
            return null;
        }
        int[] ev = new int[n];
        System.arraycopy(out, 0, ev, 0, n);
        return appendVolume(ev, 8);
    }

    /** MIDI note of an RTTTL note letter in the given octave. */
    private static int midiOf(char c, boolean sharp, int oct) {
        int semitone;
        if (c == 'c') {
            semitone = 0;
        } else if (c == 'd') {
            semitone = 2;
        } else if (c == 'e') {
            semitone = 4;
        } else if (c == 'f') {
            semitone = 5;
        } else if (c == 'g') {
            semitone = 7;
        } else if (c == 'a') {
            semitone = 9;
        } else if (c == 'b') {
            semitone = 11;
        } else {
            return -1;
        }
        if (sharp) {
            semitone++;
        }
        int midi = (oct + 1) * 12 + semitone;
        if (midi < 0) {
            midi = 0;
        }
        if (midi > 127) {
            midi = 127;
        }
        return midi;
    }

    /* ------------------------------------------------------------------ */
    /* Synthesis                                                           */
    /* ------------------------------------------------------------------ */

    private static byte[] synth(int[] ev) {
        /* Last entry is the volume marker (-1-vol). */
        int vol = -1 - ev[ev.length - 1];
        if (vol <= 0) {
            vol = 8;
        }
        int amp = (AMP * vol) / 15;
        if (amp > 127) {
            amp = 127;
        }
        if (amp < 1) {
            amp = 1;
        }

        /* v03.05: write straight into a pre-sized array. The old
         * ByteArrayOutputStream.write(int) is a synchronized method call per
         * SAMPLE (270,776 of them for the 34 s tune), which is the slow part
         * when the bytecode is interpreted. Size = sum of the note lengths,
         * clamped the same way the loop below clamps them. */
        int cap = MAX_MS * RATE / 1000;
        int total = 0;
        int i;
        for (i = 0; i + 2 < ev.length; i += 3) {
            int nn = (ev[i + 1] * RATE) / 1000;
            if (nn > 0) {
                total += (nn > cap) ? cap : nn;
                if (total >= cap) {
                    total = cap;
                    break;
                }
            }
        }
        byte[] pcm = new byte[total];
        int pos = 0;
        int guard = 0;

        for (i = 0; i + 2 < ev.length; i += 3) {
            int midi = ev[i];
            int dur = ev[i + 1];
            int rest = ev[i + 2];
            int n = (dur * RATE) / 1000;
            if (n <= 0) {
                continue;
            }
            if (n > MAX_MS * RATE / 1000) {
                n = MAX_MS * RATE / 1000;
            }
            int sound = n - (rest * RATE) / 1000;
            if (sound < 0) {
                sound = 0;
            }

            /* v03.05: the old code used an INTEGER half period,
             * RATE / (2 * freq). At 8 kHz that is 14 samples for 285 Hz
             * (wanted 270), 11 for 363 (wanted 340), 9 for 444 (wanted 404)
             * - measured on the real xiyangyang melodies, 14 of 22 notes in
             * the 58-byte tune and 151 of 181 in the 326-byte one were more
             * than half a semitone sharp, up to 284 cents. A fixed-point
             * phase accumulator (16.16) gives the exact frequency. */
            int step = 0;                      /* phase increment per sample */
            if (midi > 0) {
                /* v03.20: equal temperament, A4 = 440 Hz. This used Sound.freqForNote(midi), Nokia's
                 * table, which sits +50..+54 cents (half a semitone) above equal temperament at every
                 * note (host check: midi 60 -> 270 Hz instead of 261.6, midi 69 -> 453 instead of 440).
                 * That table is the right one for the Sound(freq, duration) constructor, whose input is a
                 * frequency taken from the same table; an OTA/RTTTL note is a MIDI note number
                 * (nibble + scale * 12 + 59, Sonivox eas_ota.c) and every OTA player (Android's
                 * MediaPlayer = Sonivox, which J2ME-Loader uses) sounds it at equal temperament. */
                step = stepForMidi(midi);
            }
            int phaseAcc = 0;                  /* 0..65535 = one period */
            int fade = RATE / 500;            /* 2 ms */
            if (fade > sound / 2) {
                fade = sound / 2;
            }

            for (int k = 0; k < n; k++, guard++) {
                int v = 128;
                if (k < sound && step > 0) {
                    phaseAcc = (phaseAcc + step) & 0xFFFF;
                    int a = blepSquare(phaseAcc, step, amp);
                    if (k < fade) {
                        a = (a * k) / (fade == 0 ? 1 : fade);
                    } else if (k >= sound - fade) {
                        a = (a * (sound - k)) / (fade == 0 ? 1 : fade);
                    }
                    v = 128 + a;
                }
                if (v < 0) {
                    v = 0;
                }
                if (v > 255) {
                    v = 255;
                }
                if (pos >= pcm.length) {
                    guard = cap;
                    break;
                }
                pcm[pos++] = (byte) v;
                if (guard >= cap) {
                    break;
                }
            }
            if (guard >= cap) {
                break;
            }
        }

        if (pos < 2) {
            return null;
        }
        byte[] s = pcm;
        if (pos != pcm.length) {
            s = new byte[pos];
            System.arraycopy(pcm, 0, s, 0, pos);
        }
        return wrapWav(s);
    }

    /*
     * v03.19: band-limited square wave (integer PolyBLEP).
     *
     * The plain square used here until v03.18 (acc < 0x8000 ? amp : -amp) has edges that fall BETWEEN
     * samples but are rounded to the nearest one, and its odd harmonics above 4 kHz (RATE/2) fold back
     * into the audible band at frequencies that are not multiples of the note: measured on the real
     * tunes (host JVM, Hann-windowed DFT, power outside the note's own harmonic lines) that is
     * -15.5 / -13.9 / -12.8 / -13.8 dB relative to the note at 270 / 404 / 539 / 800 Hz - a rough,
     * metallic buzz present on every note ("sound has crackle all the way through", log127).
     * PolyBLEP puts a 2-sample polynomial correction on each edge so the step is band-limited:
     * -29.9 / -30.1 / -31.0 / -31.7 dB with the same peak (44/127) and the same pitch, using only
     * integer arithmetic (no sin(), no float: this runs interpreted on the device).
     *
     * acc  = phase 0..65535 AFTER this sample's increment, step = increment per sample, amp = peak.
     * Edges: rising at acc == 0, falling at acc == 0x8000. The correction is applied on the sample just
     * after an edge (t = how far past it, 0..1) and on the sample just before one (t = -1..0).
     */
    private static int blepSquare(int acc, int step, int amp) {
        int v = (acc < 0x8000) ? amp : -amp;
        int a2 = (acc - 0x8000) & 0xFFFF;
        int t;
        int c;
        if (acc < step) {                                   /* just after the rising edge */
            t = (acc << 15) / step;
            c = (t << 1) - ((t * t) >> 15) - 32768;
            v += (int) (((long) amp * c) >> 15);
        } else if (acc > 0x10000 - step) {                  /* just before the rising edge */
            t = ((acc - 0x10000) << 15) / step;
            c = ((t * t) >> 15) + (t << 1) + 32768;
            v += (int) (((long) amp * c) >> 15);
        }
        if (a2 < step) {                                    /* just after the falling edge */
            t = (a2 << 15) / step;
            c = (t << 1) - ((t * t) >> 15) - 32768;
            v -= (int) (((long) amp * c) >> 15);
        } else if (a2 > 0x10000 - step) {                   /* just before the falling edge */
            t = ((a2 - 0x10000) << 15) / step;
            c = ((t * t) >> 15) + (t << 1) + 32768;
            v -= (int) (((long) amp * c) >> 15);
        }
        return v;
    }

    /* Equal-tempered frequency of MIDI notes 60..71 (C4..B4, A4 = 440 Hz) in milli-Hz. Other octaves are this
     * shifted by whole octaves, which keeps it exact and needs no Math.pow (CLDC has none). */
    private static final int[] C4_MHZ = {
        261626, 277183, 293665, 311127, 329628, 349228,
        369994, 391995, 415305, 440000, 466164, 493883
    };

    /**
     * Phase increment per sample (16.16 phase, one period = 65536) of MIDI note m at RATE, or 0 when the note
     * is not representable (at or above the Nyquist frequency, or negative).
     */
    private static int stepForMidi(int m) {
        if (m < 0) {
            return 0;
        }
        long mhz = C4_MHZ[m % 12];
        int shift = m / 12 - 5;
        if (shift > 0) {
            mhz <<= shift;
        } else if (shift < 0) {
            mhz >>= -shift;
        }
        if (mhz >= (long) RATE * 500L) {              /* >= RATE/2 Hz (in milli-Hz) */
            return 0;
        }
        int step = (int) ((mhz << 16) / ((long) RATE * 1000L));
        return (step < 1) ? 1 : step;
    }

    /** RIFF/WAVE header for 8-bit unsigned mono PCM at RATE. */
    private static byte[] wrapWav(byte[] pcm) {
        int n = pcm.length;
        byte[] w = new byte[44 + n];
        int p = 0;

        p = put(w, p, "RIFF");
        p = put32(w, p, 36 + n);
        p = put(w, p, "WAVE");
        p = put(w, p, "fmt ");
        p = put32(w, p, 16);          /* PCM chunk size     */
        p = put16(w, p, 1);           /* format = PCM       */
        p = put16(w, p, 1);           /* channels = mono    */
        p = put32(w, p, RATE);
        p = put32(w, p, RATE);        /* byte rate (8-bit mono) */
        p = put16(w, p, 1);           /* block align        */
        p = put16(w, p, 8);           /* bits per sample    */
        p = put(w, p, "data");
        p = put32(w, p, n);
        System.arraycopy(pcm, 0, w, 44, n);
        return w;
    }

    private static int put(byte[] b, int p, String s) {
        for (int i = 0; i < s.length(); i++) {
            b[p++] = (byte) s.charAt(i);
        }
        return p;
    }

    private static int put32(byte[] b, int p, int v) {
        b[p++] = (byte) (v & 0xFF);
        b[p++] = (byte) ((v >> 8) & 0xFF);
        b[p++] = (byte) ((v >> 16) & 0xFF);
        b[p++] = (byte) ((v >> 24) & 0xFF);
        return p;
    }

    private static int put16(byte[] b, int p, int v) {
        b[p++] = (byte) (v & 0xFF);
        b[p++] = (byte) ((v >> 8) & 0xFF);
        return p;
    }
}
