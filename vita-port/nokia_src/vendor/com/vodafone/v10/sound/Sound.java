package com.vodafone.v10.sound;

import java.io.ByteArrayInputStream;
import java.io.IOException;

import javax.microedition.media.Manager;
import javax.microedition.media.Player;

/**
 * Vodafone/J-Phone v10 Sound. J2ME-Loader hands the data to MMAPI as
 * "audio/xmf"; this MMAPI only decodes MIDI and WAV, so the format is taken
 * from the data header (MThd = MIDI, RIFF = WAV) as com.nokia.mid.sound.Sound
 * does. Anything else (XMF, MFi, SMAF, ...) cannot be played here: the object
 * is created without a player and playing it does nothing - it must not throw,
 * games load these at start-up.
 */
public class Sound {
    private Player player;

    public Sound(byte[] data) throws IOException {
        if (data == null) {
            throw new NullPointerException("sound data is null!");
        }
        String mime = null;
        if (data.length > 4 && data[0] == 'M' && data[1] == 'T'
                && data[2] == 'h' && data[3] == 'd') {
            mime = "audio/midi";
        } else if (data.length > 4 && data[0] == 'R' && data[1] == 'I'
                && data[2] == 'F' && data[3] == 'F') {
            mime = "audio/x-wav";
        }
        if (mime == null) {
            System.out.println("[VENDOR] vodafone Sound: unplayable format "
                               + "(first byte " + (data.length > 0 ? data[0] : 0)
                               + ", " + data.length + " bytes)");
            return;
        }
        try {
            player = Manager.createPlayer(new ByteArrayInputStream(data), mime);
            player.realize();
        } catch (Exception e) {
            System.out.println("[VENDOR] vodafone Sound: " + e);
            player = null;
        }
    }

    /** null when the data could not be turned into a player */
    Player getPlayer() {
        return player;
    }
}
