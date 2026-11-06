"""Calibration: the simulator must reproduce what a correct soft-float helper does, otherwise its verdict on the
ELF's jvm_dmul means nothing. Run the pcs("aapcs") reference functions compiled by GCC from soft_ref.o
(these DO take doubles in r0-r3 and return in r0/r1) through the same simulator and compare to Python."""
import os, sys, struct, subprocess, re
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import armsim, armsim2
from armsim import CPU, bits64, f64

VBIN = os.environ.get('VITASDK_BIN', '/usr/local/vitasdk/bin')
OD = os.path.join(VBIN, 'arm-vita-eabi-objdump')
NM = os.path.join(VBIN, 'arm-vita-eabi-nm')
# v04.54: soft_ref.o is a GCC build product that nothing used to create, so a
# fresh clone hit FileNotFoundError here and the whole FP gate failed. Build it
# on demand; it is ARM code (the model interprets it), not a host binary.
SOFT_REF = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'soft_ref.o')
_SRC = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'soft_ref.c')
if (not os.path.exists(SOFT_REF)) or os.path.getmtime(_SRC) > os.path.getmtime(SOFT_REF):
    subprocess.check_call([os.path.join(VBIN, 'arm-vita-eabi-gcc'),
                           '-marm', '-march=armv7-a', '-mtune=cortex-a9',
                           '-mfloat-abi=hard', '-mfpu=vfpv3', '-O2',
                           '-c', _SRC, '-o', SOFT_REF])
# load soft.o's .text at address 0x1000 (relative to symbol offsets)
raw = subprocess.check_output([OD, '-s', '-j', '.text', SOFT_REF]).decode()
words = {}
for line in raw.splitlines():
    m = re.match(r'\s([0-9a-f]{4}) ((?:[0-9a-f]{8} ?){1,4})', line)
    if m:
        a = int(m.group(1), 16)
        for i, w in enumerate(m.group(2).split()):
            words[0x1000 + a + 4 * i] = struct.unpack('<I', bytes.fromhex(w))[0]
syms = {}
for l in subprocess.check_output([NM, SOFT_REF]).decode().splitlines():
    p = l.split()
    if len(p) == 3 and p[1] == 'T':
        syms[p[2]] = 0x1000 + int(p[0], 16)
cpu = CPU(words)
bad = 0
out = ['calibration against GCC-compiled soft-float references: %s' % sorted(syms)]
for a, b in ((240.0, 0.95), (240.0, 2.0), (3.0, 4.0), (-1.5, 8.25), (1e10, 1e-3)):
    ahi, alo = bits64(a); bhi, blo = bits64(b)
    cpu.d = [0.0] * 32
    cpu.run(syms['dmul_s'], [alo, ahi, blo, bhi])      # AAPCS soft: x in r0:r1 (lo,hi), y in r2:r3
    got = f64(cpu.r[1], cpu.r[0])
    ok = abs(got - a * b) < 1e-9 * max(1, abs(a * b))
    bad += (not ok)
    out.append('  dmul_s(%g,%g) = %g  expected %g  %s' % (a, b, got, a * b, 'OK' if ok else 'MISMATCH'))
for v in (228.0, 480.0, -7.5, 0.5, 123456.0):
    hi, lo = bits64(v)
    cpu.run(syms['d2i_s'], [lo, hi])
    got = cpu.r[0] if cpu.r[0] < 2**31 else cpu.r[0] - 2**32
    ok = got == int(v)
    bad += (not ok)
    out.append('  d2i_s(%g) = %d  expected %d  %s' % (v, got, int(v), 'OK' if ok else 'MISMATCH'))
for v in (240, -5, 0, 100000):
    cpu.d = [0.0] * 32
    cpu.run(syms['i2d_s'], [v])
    got = f64(cpu.r[1], cpu.r[0])
    ok = got == float(v)
    bad += (not ok)
    out.append('  i2d_s(%d) = %g  expected %g  %s' % (v, got, float(v), 'OK' if ok else 'MISMATCH'))
out.append('CALIBRATION %s (%d mismatches)' % ('PASSED' if not bad else 'FAILED', bad))
open('/tmp/armcal.out', 'w').write('\n'.join(out) + '\n')
print('\n'.join(out))
