package com.sun.m3g;

/**
 * One-shot floating point self test, run the first time Graphics3D is used (v04.51, diagnostic).
 *
 * Why it exists: log191 showed the SAME Java code (Matrix4.invert via SkinnedMesh.bind, Transform.rotation) giving
 * different floating point results with the JIT on and off, and the JIT's float/double code never had a regression
 * test (the host-side fpmodel only runs the interpreter and the C++ library). Each probe below has a known answer
 * and exercises ONE kind of operation, so the log says which kind is wrong once the JIT has compiled it:
 *   pass 1 runs before anything is hot (interpreter);
 *   then the probes are called in a loop for a while so the timer-driven JIT compiles them;
 *   pass 2 and 3 run again. A probe that is right in pass 1 and wrong later is a JIT bug.
 * calls/s per pass shows whether compilation happened at all: if pass 3 is not clearly faster than pass 1, an
 * all-OK result proves nothing about the JIT. Inputs come from NON-final static fields so javac cannot fold the
 * probes into compile-time constants. Remove this class once the JIT float path is confirmed.
 */
public final class JitSelfTest {

    private JitSelfTest() {
    }

    static double dIn1 = 3.0, dIn2 = 4.0, dHalf = 0.5;
    static float fIn1 = 1.25f, fIn2 = 2.0f;
    static long lIn = 123456789L;
    static double dField = 2.25;
    double iField = 6.5;

    private static final int N = 20;
    private static final String[] NAME = {
        "dconst", "dcallarg", "cos0", "sin90", "sqrt", "f2d", "d2f", "dlocal", "l2d", "d2l",
        "frem", "drem", "darray", "dstatic", "dinst", "dneg", "dabs", "i2d", "d2i", "dcmp"
    };
    /* expected results, all scaled so they are ints */
    private static final int[] WANT = {
        1570796, 750000, 1000000, 1000000, 1500000, 1250000, 2500000, 5000000, 123456, 10000,
        1500000, 1250000, 3500000, 2250000, 6500000, -1500000, 1500000, 7000000, 3, 1
    };

    private static int s(double v) {
        return (int)(v * 1000000.0);
    }

    private static double id(double v) {
        return v;
    }

    static int probe(int k) {
        switch (k) {
        case 0:  return s(0.017453292519943295 * (dIn1 * 30.0));           /* ldc2_w + dmul: 1.5707963 */
        case 1:  return s(id(dHalf + 0.25));                                 /* double through a call */
        case 2:  return s(Math.cos(dIn1 - 3.0));                             /* native, arg 0 */
        case 3:  return s(Math.sin(dHalf * 3.141592653589793));              /* sin(pi/2) */
        case 4:  return s(Math.sqrt(dIn1 * 0.75));                           /* sqrt(2.25) */
        case 5:  return s((double)fIn1);                                     /* f2d */
        case 6:  return (int)((float)(dIn1 - 0.5) * 1000000.0f);             /* d2f */
        case 7: {
            double a = dIn1, b = dIn2;                                       /* locals + dmul/dadd + sqrt */
            return s(Math.sqrt(a * a + b * b));
        }
        case 8:  return (int)((double)lIn / 1000.0);                         /* l2d */
        case 9:  return (int)((long)(dIn2 * 2.5e9) / 1000000L);              /* d2l: 1e10 */
        case 10: return s((double)(fIn1 + 4.25f) % (fIn2)) ;                 /* frem via double: 5.5 % 2 */
        case 11: return s((dIn1 + 4.25) % fIn2);                             /* drem: 7.25 % 2 */
        case 12: {
            double[] arr = new double[4];                                    /* double array store/load */
            arr[2] = dIn1;
            arr[3] = dHalf;
            return s(arr[2] + arr[3]);
        }
        case 13: return s(dField);                                           /* static double field */
        case 14: return s(new JitSelfTest().iField);                         /* instance double field */
        case 15: {
            double x = 1.5;                                                  /* dneg (constant-folded) */
            return s(-x);
        }
        case 16: {
            double x = -1.5;                                                 /* abs */
            return s(Math.abs(x) > 0.0 ? 1.5 : 0.0);
        }
        case 17: return s((double)(int)(dIn1 + dIn2));                       /* i2d + d2i */
        case 18: return (int)(dIn1 + 0.99);                                  /* d2i: 3 */
        default: return dIn1 < dIn2 ? 1 : 0;                                 /* dcmp */
        }
    }

    private static int pass(String label, long ms) {
        StringBuffer bad = new StringBuffer();
        int ok = 0;
        long t0 = System.currentTimeMillis();
        int calls = 0;
        do {
            for (int k = 0; k < N; k++) {
                int r = probe(k);
                calls++;
                if (calls <= N) {
                    if (r == WANT[k]) {
                        ok++;
                    } else {
                        bad.append(NAME[k]).append('=').append(r).append("(want ").append(WANT[k]).append(") ");
                    }
                }
            }
        } while (System.currentTimeMillis() - t0 < ms);
        long dt = System.currentTimeMillis() - t0;
        M3gTrace.log("jitfp " + label + ": ok=" + ok + "/" + N + " calls/s=" + (dt > 0 ? (calls * 1000L / dt) : -1)
            + (bad.length() > 0 ? " BAD: " + bad.toString() : ""));
        return ok;
    }

    private static boolean done = false;

    /** Safe to call more than once; runs the sequence only the first time. Never throws. */
    public static void runOnce() {
        if (done) {
            return;
        }
        done = true;
        try {
            pass("pass1(interp)", 50);
            pass("pass2(warm)", 700);
            pass("pass3(hot)", 300);
        } catch (Throwable t) {
            M3gTrace.log("jitfp aborted: " + t);
        }
    }
}
