package com.siemens.mp.game;

import javax.microedition.media.Manager;

/** Siemens API Sound: a beep via MIDP's tone player. */
public class Sound {
    /* 2^(1/12) in 16.16 fixed point = 1.059463 * 65536 */
    private static final int SEMITONE = 69433;

    public static void playTone(int freq, int time) {
        if (time <= 0 || freq <= 0) {
            return;
        }
        /* Nearest MIDI note to freq: walk the equal-tempered scale from
         * A4 (note 69, 440 Hz) in 16.16 fixed point. CLDC has no Math.pow
         * and no log, and this avoids the floating-point library. */
        long f = 440L << 16;
        int note = 69;
        long target = ((long) freq) << 16;
        while (note < 127 && f < target) {
            long up = (f * SEMITONE) >> 16;
            if (Math.abs(target - up) >= Math.abs(target - f)) {
                break;
            }
            f = up;
            note++;
        }
        while (note > 0 && f > target) {
            long down = (f << 16) / SEMITONE;
            if (Math.abs(target - down) >= Math.abs(target - f)) {
                break;
            }
            f = down;
            note--;
        }
        try {
            Manager.playTone(note, time, 100);
        } catch (Exception e) {
            /* sound failure must not reach the game */
        }
    }
}
