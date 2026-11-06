"""Full numeric check of the Math natives through the REAL interpreter entries in the FINAL ELF:
   native_math_X_entry:  ldr r0,[r6],#4 ; ldr r1,[r6],#4 ; mov r7,lr ; bl jvm_X_isoft ; mov r2,#2 ; mov pc,r7
   => the double is popped as r0 = HIGH word (top of stack), r1 = LOW word ; result in r0:r1.
Reports OK / WRONG(value) / ABORT(model cannot run an instruction: unknown, NOT a pass)."""
import sys, os, math
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
os.environ.setdefault('FPELF', os.path.join(os.path.dirname(os.path.abspath(__file__)), '../../../vita-port/build/cmake/midp_vita'))
import fplib
from fplib import *

STEPS = int(os.environ.get('STEPS', '60000'))
c = fplib.cpu


def entry(name, v):
    h, l = bits64(v)
    c.reset_vfp(); c.mem = {}
    c.wr(STACK, h, 4); c.wr(STACK + 4, l, 4)
    c.r = [0] * 16
    c.r[6] = STACK; c.r[13] = 0x2f000000; c.r[14] = 0xFFFFFFF0; c.r[15] = sym(name)
    n = 0
    while c.r[15] != 0xFFFFFFF0:
        pc = c.r[15]
        ins = c.w.get(pc)
        if ins is None:
            raise Abort('pc %#x outside image' % pc)
        c.r[15] = pc + 4
        c.step(ins, pc)
        n += 1
        if n > STEPS:
            raise Abort('step limit')
    return f64(c.r[0], c.r[1]), n


def close(g, w):
    if w != w:
        return g != g
    if math.isinf(w):
        return g == w
    return abs(g - w) <= 1e-12 * max(1.0, abs(w))


CASES = {
    'sqrt': (math.sqrt, (0.0, 1.0, 2.0, 4.0, 240.0, 0.5, 1e10, 2.25)),
    'floor': (math.floor, (0.0, 2.7, -2.7, 240.0, -0.5, 1e10 + 0.5, 3.0)),
    'ceil': (math.ceil, (0.0, 2.2, -2.2, 240.0, -0.5, 1e10 + 0.5, 3.0)),
    'sin': (math.sin, (0.0, 0.5, 1.0, -1.0, 3.14159265358979, 10.0, 100.0)),
    'cos': (math.cos, (0.0, 0.5, 1.0, -1.0, 3.14159265358979, 10.0, 100.0)),
    'tan': (math.tan, (0.0, 0.5, 1.0, -1.0, 1.5, 10.0)),
}
only = sys.argv[1:] or list(CASES)
tot = ok = ab = bad = 0
for nm in only:
    fn, vals = CASES[nm]
    for v in vals:
        tot += 1
        want = float(fn(v))
        try:
            g, n = entry('native_math_%s_entry' % nm, v)
            good = close(g, want)
            ok += good; bad += (not good)
            print('%-5s(%-14g) got=%-22r want=%-22r %-5s (%d insns)' % (nm, v, g, want, 'OK' if good else 'WRONG', n))
        except Abort as e:
            ab += 1
            print('%-5s(%-14g) ABORT %s' % (nm, v, e))
print('TOTAL %d: ok=%d wrong=%d abort=%d' % (tot, ok, bad, ab))
