package com.nokia.mid.sound;

/** Nokia Sound API: state-change callback. */
public interface SoundListener {
    void soundStateChanged(Sound sound, int event);
}
