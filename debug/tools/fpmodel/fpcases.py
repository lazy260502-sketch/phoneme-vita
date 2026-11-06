"""Test cases: every FP bytecode handler of the interpreter, expected = IEEE/Java semantics."""
import os, sys, struct
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from fplib import *

OUT = sys.argv[1] if len(sys.argv) > 1 else '/tmp/fpcases.out'


def dbin(op, h, f):
    for a, b in ((240.0, 0.95), (3.0, 4.0), (-1.5, 8.25)):
        ah, al = bits64(a)
        bh, bl = bits64(b)
        try:
            r0, r1, sp, pushed = run(h, [bl, ah, al], bh)
            got = f64(r0, pushed)
            want = f(a, b)
            rec(op, '%g,%g' % (a, b), got, want, abs(got - want) <= 1e-9 * max(1, abs(want)))
        except Abort as e:
            rec(op, '%g,%g' % (a, b), 'ABORT ' + str(e), None, False)


def fbin(op, h, f):
    for a, b in ((240.0, 0.95), (3.0, 4.0)):
        try:
            r0, r1, sp, pushed = run(h, [f32bits(a)], f32bits(b))
            got = f32val(r0)
            want = struct.unpack('<f', struct.pack('<f', f(a, b)))[0]
            rec(op, '%g,%g' % (a, b), got, want, abs(got - want) <= 1e-6 * max(1, abs(want)))
        except Abort as e:
            rec(op, '%g,%g' % (a, b), 'ABORT ' + str(e), None, False)


def case(op, args, fn):
    try:
        got, want, ok = fn()
        rec(op, args, got, want, ok)
    except Abort as e:
        rec(op, args, 'ABORT ' + str(e), None, False)


for op, f in (('dadd', lambda a, b: a + b), ('dsub', lambda a, b: a - b),
              ('dmul', lambda a, b: a * b), ('ddiv', lambda a, b: a / b)):
    dbin(op, 'bc_impl_%s_internal' % op, f)
for op, f in (('fadd', lambda a, b: a + b), ('fsub', lambda a, b: a - b),
              ('fmul', lambda a, b: a * b), ('fdiv', lambda a, b: a / b)):
    fbin(op, 'bc_impl_%s_internal' % op, f)

for v in (240, -5):
    def i2d(v=v):
        r0, r1, sp, p = run('bc_impl_i2d_internal', [], v)
        g = f64(r0, p)
        return g, float(v), g == float(v)
    case('i2d', str(v), i2d)

    def i2f(v=v):
        r0, r1, sp, p = run('bc_impl_i2f_internal', [], v)
        g = f32val(r0)
        return g, float(v), g == float(v)
    case('i2f', str(v), i2f)

for v in (228.0, -7.5):
    h, l = bits64(v)

    def d2i(v=v, h=h, l=l):
        r0, r1, sp, p = run('bc_impl_d2i_internal', [l], h)
        return s32(r0), int(v), s32(r0) == int(v)
    case('d2i', str(v), d2i)

    def d2l(v=v, h=h, l=l):
        r0, r1, sp, p = run('bc_impl_d2l_internal', [l], h)
        hi, lo = s32(r0), s32(p)          # 2-word long: one in r0, the other pushed
        want = int(v)
        ok = (lo == want and hi == (0 if v >= 0 else -1)) or (hi == want and lo == (0 if v >= 0 else -1))
        return 'r0=%d pushed=%d' % (hi, lo), want, ok
    case('d2l', str(v), d2l)

for v in (228.0, 0.5):
    h, l = bits64(v)

    def f2d(v=v):
        r0, r1, sp, p = run('bc_impl_f2d_internal', [], f32bits(v))
        g = f64(r0, p)
        return g, v, g == v
    case('f2d', str(v), f2d)

    def d2f(v=v, h=h, l=l):
        r0, r1, sp, p = run('bc_impl_d2f_internal', [l], h)
        g = f32val(r0)
        return g, v, g == v
    case('d2f', str(v), d2f)

    def f2i(v=v):
        r0, r1, sp, p = run('bc_impl_f2i_internal', [], f32bits(v))
        return s32(r0), int(v), s32(r0) == int(v)
    case('f2i', str(v), f2i)

for a, b, exp in ((1.0, 2.0, -1), (2.0, 1.0, 1), (2.0, 2.0, 0)):
    ah, al = bits64(a)
    bh, bl = bits64(b)

    def dc(a=a, b=b, ah=ah, al=al, bh=bh, bl=bl, exp=exp):
        r0, r1, sp, p = run('bc_impl_dcmpl_internal', [bl, ah, al], bh)
        return s32(r0), exp, s32(r0) == exp
    case('dcmpl', '%g,%g' % (a, b), dc)

    def fc(a=a, b=b, exp=exp):
        r0, r1, sp, p = run('bc_impl_fcmpl_internal', [f32bits(a)], f32bits(b))
        return s32(r0), exp, s32(r0) == exp
    case('fcmpl', '%g,%g' % (a, b), fc)

print(report(OUT))
