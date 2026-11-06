"""Analyse a PCM dump captured ON THE DEVICE (what is actually handed to sceAudioOutOutput) and compare it with the host reference.

Input  : a raw s16le stereo 48 kHz file (the port's dump) and optionally the host reference rendered from the same WAV.
Outputs: per-chunk statistics that point at a specific kind of fault:
    - clipping           : fraction of samples at +-32767/-32768
    - discontinuities    : sample-to-sample jumps larger than the square swing -> clicks
    - L/R mismatch       : L != R (the renderer duplicates mono; a difference means memory corruption / wrong channel math)
    - zero runs inside a sounding note : dropped data
    - chunk-boundary jump: |last sample of chunk n - first sample of chunk n+1| compared with the in-chunk step -> a seam every 10.7 ms (= 93.75 Hz buzz!)
    - difference to the host reference sample by sample
"""
import struct, sys, math

def load(p):
    b = open(p, 'rb').read()
    n = len(b) // 4
    s = struct.unpack('<%dh' % (n * 2), b[:n * 4])
    return list(s[0::2]), list(s[1::2])

def report(path, ref=None):
    L, R = load(path)
    n = len(L)
    print('%s: %d frames = %.2f s' % (path, n, n / 48000.0))
    clip = sum(1 for v in L if v >= 32767 or v <= -32768)
    print('  clipped samples: %d (%.4f %%)' % (clip, 100.0 * clip / n))
    mm = sum(1 for a, b in zip(L, R) if a != b)
    print('  L != R frames: %d' % mm)
    peak = max(abs(v) for v in L)
    jumps = [abs(L[i] - L[i - 1]) for i in range(1, n)]
    big = [i for i, j in enumerate(jumps, 1) if j > 2 * peak * 0.6 and peak > 0]
    print('  peak %d ; jumps larger than 1.2*peak: %d' % (peak, len(big)))
    CH = 512
    seam = []
    inchunk = []
    for c in range(CH, n - 1, CH):
        seam.append(abs(L[c] - L[c - 1]))
    for i in range(1, n):
        if i % CH:
            inchunk.append(abs(L[i] - L[i - 1]))
    ms = sum(seam) / max(1, len(seam)); mi = sum(inchunk) / max(1, len(inchunk))
    print('  mean |step| at a 512-frame seam: %.1f ; inside a chunk: %.1f ; ratio %.2f (about 1.0 when there is no seam artefact)' % (ms, mi, ms / max(mi, 1e-9)))
    zr = 0; run = 0; inside = 0
    for i in range(n):
        if L[i] == 0: run += 1
        else:
            if run >= 48 and 0 < i - run and L[i - run - 1] != 0: zr += 1
            run = 0
    print('  zero runs >= 48 frames that are surrounded by sound: %d' % zr)
    if ref:
        rl, rr = load(ref)
        m = min(len(rl), n)
        d = [abs(L[i] - rl[i]) for i in range(m)]
        bad = sum(1 for v in d if v > 2)
        print('  vs host reference %s: %d frames compared, %d differ by more than 2 LSB (%.3f %%), max diff %d' % (ref, m, bad, 100.0 * bad / m, max(d)))
        first = next((i for i, v in enumerate(d) if v > 2), None)
        if first is not None:
            print('  first mismatch at frame %d (%.3f s), device %d host %d' % (first, first / 48000.0, L[first], rl[first]))

if __name__ == '__main__':
    report(sys.argv[1], sys.argv[2] if len(sys.argv) > 2 else None)
