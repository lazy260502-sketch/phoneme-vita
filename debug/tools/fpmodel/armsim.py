"""Minimal ARM/VFP interpreter: runs the VM's FP helper routines straight from the ELF bytes.
It implements only the instructions that occur in jvm_dadd/dsub/dmul/ddiv/i2d/d2i and ABORTS on anything else,
so a wrong answer can never come from a silently ignored instruction."""
import struct, subprocess, re

import os
ELF = os.environ.get('FPELF', 'vita-port/build/cmake/midp_vita')
# v04.54: do not hard-code the SDK location. build.sh exports VITASDK_BIN from
# build_env.sh, so a tree whose vitasdk lives elsewhere works too; the default
# keeps the old behaviour for a hand-run of run.sh.
VBIN = os.environ.get('VITASDK_BIN', '/usr/local/vitasdk/bin')
NM = os.path.join(VBIN, 'arm-vita-eabi-nm')
OD = os.path.join(VBIN, 'arm-vita-eabi-objdump')


def load_text():
    # .rodata too: remainder_pio2_kernel reads init_jk[] / two_over_pi[] / PIo2[] from it. Before this, those reads came
    # back as 0 and every |x| > 2^19*pi/2 argument ran forever in the model ("step limit"), which hid the exact path that
    # crashed in log189. Instructions are still looked up by address, so extra words do not matter for the decoder.
    raw = subprocess.check_output([OD, '-s', '-j', '.text', '-j', '.rodata', ELF]).decode()
    words = {}
    for line in raw.splitlines():
        m = re.match(r'\s([0-9a-f]{8}) ((?:[0-9a-f]{8} ?){1,4})', line)
        if not m:
            continue
        a = int(m.group(1), 16)
        for i, w in enumerate(m.group(2).split()):
            words[a + 4 * i] = struct.unpack('<I', bytes.fromhex(w))[0]
    return words


def sym(name):
    for l in subprocess.check_output([NM, ELF]).decode().splitlines():
        p = l.split()
        if len(p) == 3 and p[2] == name:
            return int(p[0], 16)
    raise KeyError(name)


def f64(hi, lo):
    return struct.unpack('<d', struct.pack('<II', lo & 0xffffffff, hi & 0xffffffff))[0]


def bits64(d):
    lo, hi = struct.unpack('<II', struct.pack('<d', d))
    return hi, lo


class Abort(Exception):
    pass


class CPU:
    def __init__(self, words):
        self.w = words
        self.r = [0] * 16
        self.d = [0.0] * 32
        self.s = [0] * 64
        self.N = self.Z = self.C = self.V = 0

    def cond(self, c):
        t = [self.Z, not self.Z, self.C, not self.C, self.N, not self.N, self.V, not self.V,
             self.C and not self.Z, (not self.C) or self.Z, self.N == self.V, self.N != self.V,
             (not self.Z) and self.N == self.V, self.Z or self.N != self.V, True]
        return bool(t[c])

    def flags_sub(self, a, b):
        res = (a - b) & 0xffffffff
        self.N = res >> 31
        self.Z = int(res == 0)
        self.C = int(a >= b)
        self.V = ((a ^ b) & (a ^ res)) >> 31
        return res

    def run(self, entry, args, maxsteps=500):
        self.r = [0] * 16
        for i, a in enumerate(args):
            self.r[i] = a & 0xffffffff
        self.r[14] = 0xFFFFFFF0
        self.r[15] = entry
        for _ in range(maxsteps):
            pc = self.r[15]
            if pc == 0xFFFFFFF0:
                return
            ins = self.w.get(pc)
            if ins is None:
                raise Abort('pc %#x outside .text' % pc)
            self.r[15] = pc + 4
            self.step(ins, pc)
        raise Abort('step limit')
