package com.nokia.mid.sound;

import java.io.ByteArrayInputStream;

import javax.microedition.media.Manager;
import javax.microedition.media.Player;
import javax.microedition.media.PlayerListener;
import javax.microedition.media.control.VolumeControl;

/**
 * Nokia Sound API on top of MMAPI. Behaviour follows J2ME-Loader's Sound
 * (Apache-2.0, Copyright 2017-2020 Nikita Shakarun, 2021-2023 Yury
 * Kharchenko), re-implemented on this tree's MMAPI (CLDC 1.x: no generics,
 * no string switch).
 *
 * Differences from Loader, on purpose:
 *  - the format is taken from the DATA (MThd = MIDI, RIFF = WAV), not from
 *    the type argument, because a game may pass standard MIDI with
 *    FORMAT_TONE and Loader's ToneVerifier would try to "repair" it as a
 *    Nokia ringtone. (An earlier version of this comment said the Xiyangyang
 *    game does that; log118 analysis showed it does NOT - it passes real
 *    Nokia ringtones, see PROJECT_MEMORY v02.81.);
 *  - a real Nokia ringtone (neither MThd nor RIFF) is converted to a small
 *    8 kHz 8-bit WAV by RingTone (v02.88) instead of being rejected. Before
 *    that, such a Sound stayed SOUND_UNINITIALIZED, so any game whose music
 *    is an OTA/RTTTL blob played in silence. Anything RingTone cannot parse
 *    still leaves the object SOUND_UNINITIALIZED instead of throwing;
 *  - Sound(freq, duration) goes through Manager.playTone.
 */
public class Sound {
    public static final int FORMAT_TONE = 1;
    public static final int FORMAT_WAV = 5;
    public static final int SOUND_PLAYING = 0;
    public static final int SOUND_STOPPED = 1;
    public static final int SOUND_UNINITIALIZED = 3;

    /* Nokia's own frequency table (Hz) indexed by MIDI note; ~3% above equal
     * temperament, so it is looked up, not computed. 128 entries.
     * v02.88: package-private so RingTone can reuse it (CLDC has no
     * Math.pow, and this table is the tuning Nokia games expect). */
    static final short[] FREQ_TABLE = {
        0, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 21, 22, 23,
        24, 26, 27, 29, 30, 32, 34, 36, 38, 41, 43, 45, 48, 51, 54, 57, 60,
        64, 68, 72, 76, 81, 85, 90, 96, 101, 107, 114, 120, 128, 135, 143,
        152, 161, 170, 180, 191, 202, 214, 227, 240, 255, 270, 286, 303,
        321, 340, 360, 381, 404, 428, 453, 480, 509, 539, 571, 605, 641,
        679, 719, 762, 807, 855, 906, 960, 1017, 1078, 1142, 1210, 1282,
        1358, 1438, 1524, 1614, 1710, 1812, 1920, 2034, 2155, 2283, 2419,
        2563, 2715, 2876, 3047, 3228, 3420, 3624, 3839, 4067, 4309, 4565,
        4837, 5125, 5429, 5752, 6094, 6456, 6840, 7247, 7678, 8134, 8618,
        9130, 9673, 10249, 10858, 11504, 12188, 12912
    };

    private Player player;
    private SoundListener listener;
    private int state = SOUND_UNINITIALIZED;
    private int gain = 255;
    /* frequency-constructor sound: replayed through Manager.playTone */
    private int toneNote = -1;
    private int toneMs;

    private final PlayerListener pl = new PlayerListener() {
        public void playerUpdate(Player p, String event, Object data) {
            if (event.equals(PlayerListener.END_OF_MEDIA)
                    || event.equals(PlayerListener.STOPPED)) {
                post(SOUND_STOPPED);
            } else if (event.equals(PlayerListener.STARTED)) {
                post(SOUND_PLAYING);
            } else if (event.equals(PlayerListener.CLOSED)) {
                post(SOUND_UNINITIALIZED);
            }
        }
    };

    public Sound(byte[] data, int type) {
        init(data, type);
    }

    /** Frequency in Hz of a MIDI note, from Nokia's own table. */
    static int freqForNote(int note) {
        if (note < 0) {
            note = 0;
        }
        if (note >= FREQ_TABLE.length) {
            note = FREQ_TABLE.length - 1;
        }
        return FREQ_TABLE[note];
    }

    public Sound(int freq, long duration) {
        init(freq, duration);
    }

    public static int getConcurrentSoundCount(int type) {
        return 1;
    }

    public static int[] getSupportedFormats() {
        return new int[] { FORMAT_TONE, FORMAT_WAV };
    }

    public int getGain() {
        return gain;
    }

    public int getState() {
        return state;
    }

    public void setSoundListener(SoundListener l) {
        listener = l;
    }

    private void post(int s) {
        state = s;
        SoundListener l = listener;
        if (l != null) {
            l.soundStateChanged(this, s);
        }
    }

    private void closePlayer() {
        if (player != null) {
            try {
                player.close();
            } catch (Exception e) {
                /* ignore */
            }
            player = null;
        }
    }

    private void applyGain() {
        if (player == null) {
            return;
        }
        try {
            VolumeControl vc =
                (VolumeControl) player.getControl("VolumeControl");
            if (vc != null) {
                vc.setLevel(gain * 100 / 255);
            }
        } catch (Exception e) {
            /* no volume control: keep default level */
        }
    }

    public void init(int freq, long duration) {
        if (duration <= 0) {
            throw new IllegalArgumentException("duration = " + duration);
        }
        if (freq < 0 || freq > FREQ_TABLE[FREQ_TABLE.length - 1]) {
            throw new IllegalArgumentException("freq = " + freq);
        }
        closePlayer();
        int best = 0;
        for (int i = 1; i < FREQ_TABLE.length; i++) {
            if (Math.abs(FREQ_TABLE[i] - freq)
                    < Math.abs(FREQ_TABLE[best] - freq)) {
                best = i;
            }
        }
        toneNote = best;
        toneMs = (int) Math.min(duration, 3600000L);
        state = SOUND_STOPPED;
    }

    public void init(byte[] data, int type) {
        /* v02.90: unconditional entry trace. log119 (v02.87) showed ten
         * "startmusic" lines from Xiyangyang but NOT ONE "[VENDOR] ...
         * unplayable data" line, which the code below should have printed for
         * a Nokia OTA blob; this line says whether init() is reached at all,
         * with what, and which branch it takes. */
        System.out.println("[SOUND] t=" + System.currentTimeMillis() + " init len="
                           + (data == null ? -1 : data.length) + " type=" + type
                           + " head="
                           + (data != null && data.length > 0 ? data[0] & 0xff : -1) + ","
                           + (data != null && data.length > 1 ? data[1] & 0xff : -1) + ","
                           + (data != null && data.length > 2 ? data[2] & 0xff : -1)
                           + " this=" + hashCode());
        if (data == null) {
            throw new NullPointerException();
        }
        if (type != FORMAT_TONE && type != FORMAT_WAV) {
            throw new IllegalArgumentException();
        }
        closePlayer();
        toneNote = -1;
        String mime = null;
        if (data.length > 4 && data[0] == 'M' && data[1] == 'T'
                && data[2] == 'h' && data[3] == 'd') {
            mime = "audio/midi";
        } else if (data.length > 4 && data[0] == 'R' && data[1] == 'I'
                && data[2] == 'F' && data[3] == 'F') {
            mime = "audio/x-wav";
        }
        if (mime == null) {
            /* v02.88: a Nokia ringtone (OTA / Smart Messaging) or RTTTL
             * text. RingTone converts it to a small 8 kHz 8-bit WAV, which
             * this port has been able to play since v01.96. Real games do
             * ship these: log118 showed an OTA blob that used to leave the
             * Sound uninitialised, i.e. silent. */
            long t0 = System.currentTimeMillis();
            /* v03.34: try a Standard MIDI File first (J2ME-Loader does the same: "audio/midi" -> Sonivox,
             * GM program 80). It goes through the 48 kHz TinySoundFont renderer; the 8 kHz WAV below is
             * only the fallback if the MIDI player cannot be created. */
            byte[] smf = RingTone.toMidi(data);
            if (smf != null) {
                try {
                    player = Manager.createPlayer(new ByteArrayInputStream(smf), "audio/midi");
                    player.addPlayerListener(pl);
                    state = SOUND_STOPPED;
                    applyGain();
                    System.out.println("[SOUND] t=" + System.currentTimeMillis() + " ringtone->midi ok "
                                       + smf.length + " bytes in " + (System.currentTimeMillis() - t0) + " ms");
                    return;
                } catch (Exception e) {
                    System.out.println("[SOUND] ringtone midi createPlayer failed: " + e);
                    player = null;
                }
            }
            byte[] wav = RingTone.toWav(data);
            /* v03.14: how long the conversion took on the device (first call of a blob = synth,
             * repeat = cache hit). Needed before the synth can be made richer: more samples per
             * second means more interpreted bytecode, and the load stall must not come back. */
            long took = System.currentTimeMillis() - t0;
            System.out.println("[SOUND] t=" + System.currentTimeMillis() + " ringtone->wav "
                               + (wav == null ? "FAILED (null)"
                                  : "ok " + wav.length + " bytes")
                               + " in " + took + " ms");
            if (wav != null) {
                try {
                    /* v03.15: split the new-Sound cost. log126: pause -> next start is ~2.7-3.3 s, but
                     * toWav is a cache hit (0 ms) and only 17 VM-thread log lines are written, so the
                     * time is in createPlayer (realize + 270 KB through MediaDownload) or in play()
                     * (prefetch / setMediaTime / start). These two numbers say which. */
                    long c0 = System.currentTimeMillis();
                    player = Manager.createPlayer(
                            new ByteArrayInputStream(wav), "audio/x-wav");
                    long c1 = System.currentTimeMillis();
                    player.addPlayerListener(pl);
                    state = SOUND_STOPPED;
                    applyGain();
                    System.out.println("[SOUND] t=" + System.currentTimeMillis() + " wav player created createPlayer="
                                       + (c1 - c0) + " ms, listener+gain="
                                       + (System.currentTimeMillis() - c1) + " ms");
                    return;
                } catch (Exception e) {
                    System.out.println("[SOUND] wav createPlayer failed: " + e);
                    player = null;
                    state = SOUND_UNINITIALIZED;
                    return;
                }
            }
            /* Unknown blob: not playable here */
            System.out.println("[VENDOR] nokia Sound: unplayable data, "
                               + data.length + " bytes, type=" + type
                               + ", first bytes="
                               + (data.length > 0 ? data[0] & 0xff : -1) + ","
                               + (data.length > 1 ? data[1] & 0xff : -1) + ","
                               + (data.length > 2 ? data[2] & 0xff : -1));
            state = SOUND_UNINITIALIZED;
            return;
        }
        try {
            player = Manager.createPlayer(new ByteArrayInputStream(data),
                                          mime);
            player.addPlayerListener(pl);
            state = SOUND_STOPPED;
            applyGain();
        } catch (Exception e) {
            player = null;
            state = SOUND_UNINITIALIZED;
        }
    }

    public void setGain(int g) {
        gain = g < 0 ? 0 : (g > 255 ? 255 : g);
        applyGain();
    }

    /** loop == 0 means forever (Nokia API), otherwise that many times. */
    public void play(int loop) {
        System.out.println("[SOUND] t=" + System.currentTimeMillis() + " play loop=" + loop + " player="
                           + (player != null) + " tone=" + toneNote
                           + " state=" + state);
        try {
            if (toneNote >= 0) {
                Manager.playTone(toneNote, toneMs, gain * 100 / 255);
                return;
            }
            if (player == null) {
                return;
            }
            long p0 = System.currentTimeMillis();
            player.stop();
            long p1 = System.currentTimeMillis();
            player.setLoopCount(loop == 0 ? -1 : loop);
            player.prefetch();
            long p2 = System.currentTimeMillis();
            player.setMediaTime(0);
            long p3 = System.currentTimeMillis();
            player.start();
            long p4 = System.currentTimeMillis();
            /* v03.15: where play() spends its time (see init()) */
            System.out.println("[SOUND] t=" + System.currentTimeMillis() + " play steps stop=" + (p1 - p0) + " prefetch=" + (p2 - p1)
                               + " setMediaTime=" + (p3 - p2) + " start=" + (p4 - p3)
                               + " total=" + (p4 - p0) + " ms");
        } catch (Exception e) {
            /* playback failure must not reach the game, but must be visible */
            System.out.println("[VENDOR] nokia Sound.play failed: " + e);
        }
    }

    public void stop() {
        try {
            if (player != null) {
                player.stop();
            }
        } catch (Exception e) {
            /* ignore */
        }
    }

    public void resume() {
        try {
            if (player != null) {
                player.start();
            }
        } catch (Exception e) {
            /* ignore */
        }
    }

    public void release() {
        closePlayer();
        state = SOUND_UNINITIALIZED;
    }
}
