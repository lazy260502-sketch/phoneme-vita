package com.nokia.mid.sound;

public class RingToneTest {
    static int failures = 0;
    static void ck(boolean c, String what) {
        if (!c) { failures++; System.out.println("FAIL " + what); }
        else System.out.println("ok   " + what);
    }
    static int le32(byte[] b, int o) {
        return (b[o]&0xff) | ((b[o+1]&0xff)<<8) | ((b[o+2]&0xff)<<16) | ((b[o+3]&0xff)<<24);
    }

    public static void main(String[] a) {
        /* --- RTTTL --- */
        String r = "Test:d=4,o=5,b=120:c,d,e,f,g,a,b";
        byte[] w = RingTone.toWav(r.getBytes());
        ck(w != null, "rtttl: converted");
        if (w != null) {
            ck(w[0]=='R' && w[1]=='I' && w[2]=='F' && w[3]=='F', "rtttl: RIFF");
            ck(w[8]=='W' && w[9]=='A' && w[10]=='V' && w[11]=='E', "rtttl: WAVE");
            ck(le32(w, 24) == 8000, "rtttl: 8000 Hz");
            ck((w[34] & 0xff) == 8, "rtttl: 8 bit");
            int dlen = le32(w, 40);
            ck(dlen == w.length - 44, "rtttl: data size consistent");
            /* 7 quarter notes at 120 bpm = 7 * 500 ms = 3500 ms */
            ck(Math.abs(dlen - 3500 * 8) < 800, "rtttl: ~3500 ms of samples (" + dlen + ")");
        }

        /* --- RTTTL with rests, dotted and sharps --- */
        String r2 = "T2:d=8,o=6,b=180:8p,16c#.,4d,2p";
        byte[] w2 = RingTone.toWav(r2.getBytes());
        ck(w2 != null, "rtttl2: converted");
        if (w2 != null) ck(le32(w2, 40) > 0, "rtttl2: has samples");

        /* --- OTA: tempo + scale + 4 notes, in the layout the parser reads --- */
        /* v03.19: the layout the parser really reads (checked against the three real ringtones of log118):
         *   cmdLen:8  RTP:7+filler:1  sound(0x1d):7  songType:3 (2 = no title)  sequenceLength:8
         *   per sequence: patternHeader:3  patternId:2  loop:4  instrCount:8  then the instructions.
         * The v02.88 version of this test fed instructions straight after the 3 magic bytes, which the
         * v02.89 parser (correctly) rejects - it failed before and after every later change. */
        byte[] ota = new byte[64];
        int bitpos = 0;
        bitpos = put(ota, bitpos, 8, 2);        /* command length                        */
        bitpos = put(ota, bitpos, 7, 0x25);     /* ringing tone programming              */
        bitpos = put(ota, bitpos, 1, 0);        /* filler                                */
        bitpos = put(ota, bitpos, 7, 0x1d);     /* sound                                 */
        bitpos = put(ota, bitpos, 3, 2);        /* song type 2 (no title)                */
        bitpos = put(ota, bitpos, 8, 1);        /* one sequence                          */
        bitpos = put(ota, bitpos, 3, 0);        /* pattern header id                     */
        bitpos = put(ota, bitpos, 2, 0);        /* pattern id                            */
        bitpos = put(ota, bitpos, 4, 0);        /* loop                                  */
        bitpos = put(ota, bitpos, 8, 6);        /* instruction count: tempo, scale, 4 notes */
        bitpos = put(ota, bitpos, 3, 4);        /* tempo instruction                     */
        bitpos = put(ota, bitpos, 5, 15);       /*   tempo index 15 -> tick 13714        */
        bitpos = put(ota, bitpos, 3, 2);        /* scale instruction                     */
        bitpos = put(ota, bitpos, 2, 1);        /*   octave field is 2 bits: 1 -> base 1*12+59 */
        for (int k = 0; k < 4; k++) {
            bitpos = put(ota, bitpos, 3, 1);    /* note instruction                      */
            bitpos = put(ota, bitpos, 4, 1);    /* note C                                */
            bitpos = put(ota, bitpos, 3, 2);    /* duration exponent                     */
            bitpos = put(ota, bitpos, 2, 0);    /* normal                                */
        }
        byte[] w3 = RingTone.toWav(ota);
        ck(w3 != null, "ota: converted");
        if (w3 != null) {
            ck(le32(w3, 40) > 0, "ota: has samples (" + le32(w3, 40) + ")");
        }

        /* --- v03.20: pitch is equal-tempered A4 = 440 Hz (MIDI semantics), not Nokia's +50 cents table --- */
        try {
            java.lang.reflect.Method sm = RingTone.class.getDeclaredMethod("stepForMidi", new Class[] { Integer.TYPE });
            sm.setAccessible(true);
            int[] notes = { 36, 45, 57, 60, 62, 67, 69, 72, 77, 81, 93 };
            for (int i = 0; i < notes.length; i++) {
                double want = 440.0 * Math.pow(2.0, (notes[i] - 69) / 12.0);
                int st = ((Integer) sm.invoke(null, new Object[] { new Integer(notes[i]) })).intValue();
                double hz = st * 8000.0 / 65536.0;
                double cents = 1200.0 * Math.log(hz / want) / Math.log(2.0);
                /* 16.16 phase at 8 kHz resolves 0.122 Hz: up to ~4 cents at 55 Hz, <0.1 cent at the tunes' 260 Hz+ */
                ck(Math.abs(cents) < 4.0, "pitch midi " + notes[i] + ": " + hz + " Hz vs " + want + " (" + cents + " cents)");
            }
            int hi = ((Integer) sm.invoke(null, new Object[] { new Integer(120) })).intValue();
            ck(hi == 0, "pitch midi 120 (8.4 kHz) is above Nyquist -> 0 (got " + hi + ")");
            int neg = ((Integer) sm.invoke(null, new Object[] { new Integer(-1) })).intValue();
            ck(neg == 0, "pitch midi -1 -> 0 (got " + neg + ")");
        } catch (Exception e) {
            ck(false, "pitch test: " + e);
        }

        /* --- v03.34: ringtone -> Standard MIDI File (what J2ME-Loader hands to Sonivox) --- */
        {
            byte[] m = RingTone.toMidi(ota);
            ck(m != null, "midi: ota converted");
            if (m != null) {
                ck(m[0]=='M' && m[1]=='T' && m[2]=='h' && m[3]=='d', "midi: MThd");
                ck(le32be(m, 4) == 6 && m[9] == 0 && m[11] == 1, "midi: format 0, one track");
                ck(((m[12] & 0xff) << 8 | (m[13] & 0xff)) == 500, "midi: division 500 (1 tick = 1 ms at 500000 us)");
                ck(m[14]=='M' && m[15]=='T' && m[16]=='r' && m[17]=='k', "midi: MTrk");
                ck(le32be(m, 18) == m.length - 22, "midi: track length " + le32be(m, 18) + " matches " + (m.length - 22));
                /* walk the track */
                int p = 22, t = 0, ons = 0, offs = 0, prog = -1, vol = -1, endAt = -1, firstOn = -1, lastOff = -1;
                int vel0 = -1;
                while (p < m.length) {
                    int d = 0;
                    while ((m[p] & 0x80) != 0) { d = (d << 7) | (m[p++] & 0x7f); }
                    d = (d << 7) | (m[p++] & 0x7f);
                    t += d;
                    int st = m[p++] & 0xff;
                    if (st == 0xff) {
                        int ty = m[p++] & 0xff, len = m[p++] & 0xff;
                        if (ty == 0x2f) { endAt = t; }
                        if (ty == 0x51) { ck(len == 3 && (m[p] & 0xff) == 0x07 && (m[p+1] & 0xff) == 0xa1 && (m[p+2] & 0xff) == 0x20, "midi: tempo 500000"); }
                        p += len;
                    } else if (st == 0xc0) {
                        prog = m[p++] & 0xff;
                    } else if (st == 0xb0) {
                        if ((m[p] & 0xff) == 7) { vol = m[p + 1] & 0xff; }
                        p += 2;
                    } else if (st == 0x90) {
                        ons++; if (firstOn < 0) { firstOn = t; vel0 = m[p + 1] & 0xff; }
                        ck((m[p] & 0xff) == 72, "midi: note-on key " + (m[p] & 0xff) + " (scale 1 -> base 71, note C=1 -> 72)");
                        p += 2;
                    } else if (st == 0x80) {
                        offs++; lastOff = t; p += 2;
                    } else {
                        ck(false, "midi: unexpected status " + st);
                        break;
                    }
                }
                ck(prog == 80, "midi: GM program 80 (Square Lead, as Sonivox eas_ota.c), got " + prog);
                ck(vol == 80, "midi: channel volume 80 (127 clips in TinySoundFont), got " + vol);
                ck(ons == 4 && offs == 4, "midi: 4 note-ons / 4 note-offs (" + ons + "/" + offs + ")");
                ck(vel0 == 7 * 4 + 67, "midi: velocity = vol*4+67 = 95 (default volume 7), got " + vel0);
                /* tempo index 15 -> tick 13714; exp 2 -> 32>>2 = 8 ticks; (13714*8)>>8 = 428 ms per note; style 0: last 1/16 silent */
                ck(firstOn == 0, "midi: first note at 0, got " + firstOn);
                ck(endAt == 4 * 428, "midi: ends after 4 notes = " + (4 * 428) + " ms, got " + endAt);
                ck(lastOff == 3 * 428 + (428 - (428 >> 4)), "midi: last note-off at " + (3 * 428 + 428 - (428 >> 4)) + ", got " + lastOff);
            }
            byte[] m2 = RingTone.toMidi("T2:d=8,o=6,b=180:8p,16c#.,4d,2p".getBytes());
            ck(m2 != null, "midi: rtttl converted");
            ck(RingTone.toMidi(junkBlob()) == null, "midi: garbage rejected");
            ck(RingTone.toMidi(new byte[2]) == null, "midi: tiny input rejected");
            ck(RingTone.toMidi(ota) == RingTone.toMidi(ota), "midi: repeat is a cache hit (same array)");
            if (a.length > 0) {
                try {
                    java.io.FileOutputStream fo = new java.io.FileOutputStream(a[0]);
                    fo.write(RingTone.toMidi(ota));
                    fo.close();
                } catch (java.io.IOException e) { ck(false, "write " + a[0]); }
            }
        }

        /* --- rejects garbage --- */
        byte[] junk = new byte[64];
        for (int i = 0; i < junk.length; i++) junk[i] = (byte) 0x5A;
        ck(RingTone.toWav(junk) == null, "garbage rejected");
        ck(RingTone.toWav(new byte[2]) == null, "tiny input rejected");

        System.out.println(failures == 0 ? "\nALL PASS" : "\nFAILED (" + failures + ")");
        if (failures != 0) System.exit(1);
    }

    static int le32be(byte[] b, int o) {
        return ((b[o]&0xff)<<24) | ((b[o+1]&0xff)<<16) | ((b[o+2]&0xff)<<8) | (b[o+3]&0xff);
    }

    static byte[] junkBlob() {
        byte[] j = new byte[64];
        for (int i = 0; i < j.length; i++) j[i] = (byte) 0x5A;
        return j;
    }

    static int put(byte[] b, int pos, int nbits, int val) {
        for (int i = nbits - 1; i >= 0; i--) {
            int bit = (val >> i) & 1;
            int byteIdx = pos >> 3, bitIdx = 7 - (pos & 7);
            if (bit != 0) b[byteIdx] |= (byte)(1 << bitIdx);
            pos++;
        }
        return pos;
    }
}
