package com.nokia.mid.sound;
// Host-test stand-in for vita-port/nokia_src/Sound.java: only the real FREQ_TABLE + freqForNote (RingTone needs nothing else).
final class Sound {
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
    static int freqForNote(int note) {
        if (note < 0) note = 0;
        if (note >= FREQ_TABLE.length) note = FREQ_TABLE.length - 1;
        return FREQ_TABLE[note];
    }
}
