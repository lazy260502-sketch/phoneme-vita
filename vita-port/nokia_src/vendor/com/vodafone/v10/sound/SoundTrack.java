package com.vodafone.v10.sound;

import javax.microedition.media.Player;

/**
 * Vodafone/J-Phone v10 SoundTrack. Behaviour follows J2ME-Loader's class
 * (Apache-2.0); differences: a Sound without a player (unplayable format) is
 * tolerated, and the volume is applied to the player when it has a
 * VolumeControl (Loader stores it and never applies it).
 */
public class SoundTrack {
    public static final int NO_DATA = 0;
    public static final int PAUSED = 3;
    public static final int PLAYING = 2;
    public static final int READY = 1;
    private static final int MAX_VOLUME = 127;

    private Sound snd;
    private Player player;
    private int state;
    private int loopCount;
    private int volume = MAX_VOLUME;
    private SoundTrackListener listener;

    public Sound getSound() {
        return snd;
    }

    public void setSound(Sound p) {
        this.snd = p;
        this.player = p.getPlayer();
        /* Loader leaves state at NO_DATA; READY is what the Vodafone API
         * documents once a sound is attached. UNVERIFIED on a device. */
        state = READY;
        applyVolume();
    }

    public int getState() {
        return state;
    }

    public void setVolume(int value) {
        this.volume = value;
        if (volume < 0) {
            volume = 0;
        }
        if (volume > MAX_VOLUME) {
            volume = MAX_VOLUME;
        }
        applyVolume();
    }

    private void applyVolume() {
        if (player == null) {
            return;
        }
        try {
            javax.microedition.media.control.VolumeControl vc =
                (javax.microedition.media.control.VolumeControl)
                    player.getControl("VolumeControl");
            if (vc != null) {
                vc.setLevel(volume * 100 / MAX_VOLUME);
            }
        } catch (Exception e) {
            /* volume is cosmetic */
        }
    }

    public void stop() {
        if (player == null) {
            return;
        }
        try {
            player.stop();
            state = READY;
        } catch (Exception e) {
            e.printStackTrace();
        }
    }

    public void play(int loop) {
        if (player == null) {
            return;
        }
        try {
            if (state != PLAYING) {
                loopCount = loop;
                if (loop == 0) {
                    loop = -1;
                }
                player.setLoopCount(loop);
                player.start();
                state = PLAYING;
            }
        } catch (Exception e) {
            e.printStackTrace();
        }
    }

    public void removeSound() {
        this.snd = null;
        this.player = null;
        state = NO_DATA;
    }

    public void setEventListener(SoundTrackListener l) {
        this.listener = l;
    }

    private void postEvent(int event) {
        if (listener != null) {
            listener.eventOccurred(event);
        }
    }
}
