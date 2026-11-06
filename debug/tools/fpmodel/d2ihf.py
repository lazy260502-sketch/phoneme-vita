"""Hard-float jvm_d2i / jvm_f2i (the C++ callers' entries, v03.11) against Java (int) cast semantics:
truncate toward zero, saturate at INT_MIN/INT_MAX, NaN -> 0. Called exactly the way GCC hard-float calls them
(argument in d0 / s0, result in r0, r1-r3 deliberately poisoned)."""
import sys, os, math, struct
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
os.environ.setdefault('FPELF', os.path.join(os.path.dirname(os.path.abspath(__file__)), '../../../vita-port/build/cmake/midp_vita'))
import fplib
from fplib import *
c = fplib.cpu

def java_int(x):
    if x != x: return 0
    if x >= 2147483647.0: return 2147483647
    if x <= -2147483648.0: return -2147483648
    return int(x)

def run(name, setup):
    c.reset_vfp(); c.mem = {}
    c.r = [0x1234] * 16
    c.r[13] = 0x2f000000; c.r[14] = 0xFFFFFFF0; c.r[15] = sym(name)
    setup()
    n = 0
    while c.r[15] != 0xFFFFFFF0:
        pc = c.r[15]; ins = c.w[pc]; c.r[15] = pc + 4; c.step(ins, pc); n += 1
        if n > 500: raise Abort('step limit')
    v = c.r[0]
    return v - (1 << 32) if v >> 31 else v

def f32(x):
    """Round to binary32; overflow becomes +-inf like a C (float) cast."""
    try:
        return struct.unpack('<f', struct.pack('<f', x))[0]
    except OverflowError:
        return math.copysign(float('inf'), x)

DV = (0.0, -0.0, 0.5, -0.5, 0.99, 1.0, -1.0, 3.0, 6.0, 15.9, -15.9, 240.0, 65535.5, 1e9, 2147483646.5,
      2147483647.0, 2147483648.0, 1e10, 1e300, -2147483648.0, -2147483649.0, -1e10, float('inf'), float('-inf'), float('nan'))
bad = tot = 0
for v in DV:
    fx = f32(v)
    for nm, fv, fn in (('jvm_d2i', v, lambda v=v: c.d.__setitem__(0, v)),
                       ('jvm_f2i', fx, lambda fx=fx: c.s.__setitem__(0, f32bits(fx)))):
        tot += 1
        try:
            got = run(nm, fn); want = java_int(fv)
            ok = got == want; bad += (not ok)
            if not ok: print('WRONG %s(%r) got=%d want=%d' % (nm, v, got, want))
        except Abort as e:
            bad += 1; print('ABORT %s(%r) %s' % (nm, v, e))
print('d2i/f2i hard-float: %d cases, %d bad' % (tot, bad))
