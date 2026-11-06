"""Float/Double bit-conversion natives through the REAL entry points in the final ELF.

The interpreter (bc_impl_fast_invokenative -> shared_call_vm -> simple_c_store_result) takes a native's result from
INTEGER registers: int/float in r0, long/double in r0:r1 (r0 = HIGH word of a double, same as the Math entries).
Java_java_lang_Float_intBitsToFloat / Double_longBitsToDouble are hard-float C functions and answer in s0 / d0, so
r0 still holds whatever the caller left there (log143: every float read through M3gReader.f32() was 0x813f3258, the
address of _current_thread).  This checks what a CALLER would observe, i.e. the integer registers after return.

Parameters are read by the natives through the global _kni_parameter_base (its address is taken from the ELF symbol
table, not assumed): value = *(base - 4) for an int/float, and the two words at base-8 / base-4 for a long/double.
The model seeds that pointer.
"""
import sys, os, struct, math
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
os.environ.setdefault('FPELF', os.path.join(os.path.dirname(os.path.abspath(__file__)), '../../../vita-port/build/cmake/midp_vita'))
import fplib
from fplib import *

c = fplib.cpu
GP = sym('_kni_parameter_base')   # the natives load this pointer and index it with negative offsets
PARAM = 0x31000000                # fake KNI parameter area; base points just past the words
STEPS = 4000
RET = 0xFFFFFFF0
JUNK = 0x813f3258        # the value log143 saw


def call(name, words):
    """Run native `name` with `words` as its parameter words; return (r0, r1, s0, d0)."""
    c.reset_vfp(); c.mem = {}
    base = PARAM + 4 * len(words)
    for i, w in enumerate(words):
        c.wr(PARAM + 4 * i, w & 0xffffffff, 4)
    c.wr(GP, base, 4)
    c.r = [0] * 16
    c.r[0] = JUNK
    c.r[1] = JUNK
    c.r[13] = 0x2f000000
    c.r[14] = RET
    c.r[15] = sym(name)
    n = 0
    while c.r[15] != RET:
        pc = c.r[15]
        ins = c.w.get(pc)
        if ins is None:
            raise Abort('pc %#x outside image' % pc)
        c.r[15] = pc + 4
        c.step(ins, pc)
        n += 1
        if n > STEPS:
            raise Abort('step limit')
    return c.r[0], c.r[1], n


def fbits(x):
    return struct.unpack('<I', struct.pack('<f', x))[0]


def dbits(x):
    return struct.unpack('<Q', struct.pack('<d', x))[0]


F_CASES = [0.0, 1.0, -1.0, 0.0276, 628.1, 3.5e-38, 1.0e30, -8198.0, 0.67, float('inf'), float('-inf')]
D_CASES = [0.0, 1.0, -1.0, 0.0276, 628.1, 1.0e300, -8198.0, 0.67, float('inf')]

tot = ok = ab = bad = 0


def run_case(label, fn):
    global tot, ok, ab, bad
    tot += 1
    try:
        good, msg = fn()
        ok += good
        bad += (not good)
        print('%-44s %s %s' % (label, 'OK   ' if good else 'WRONG', msg))
    except Abort as e:
        ab += 1
        print('%-44s ABORT %s' % (label, e))


# ---- Float.floatToIntBits (F)I : result in r0 -----------------------------------------------------------------
for x in F_CASES + [float('nan')]:
    def t(x=x):
        r0, r1, n = call('Java_java_lang_Float_floatToIntBits', [fbits(x)])
        want = 0x7fc00000 if x != x else fbits(x)
        return r0 == want, 'got=%#010x want=%#010x' % (r0, want)
    run_case('floatToIntBits(%r)' % x, t)

# ---- Double.doubleToLongBits (D)J : result r0 = LOW, r1 = HIGH (AAPCS jlong) -----------------------------------
for x in D_CASES + [float('nan')]:
    def t(x=x):
        b = dbits(x)
        hi, lo = b >> 32, b & 0xffffffff
        # JavaStackDirection < 0: KNI_GetParameterAsLong takes the word at base-4 as HIGH and base-8 as LOW
        r0, r1, n = call('Java_java_lang_Double_doubleToLongBits', [lo, hi])
        want = 0x7ff8000000000000 if x != x else b
        got = (r1 << 32) | r0
        return got == want, 'got=%#018x want=%#018x' % (got, want)
    run_case('doubleToLongBits(%r)' % x, t)

# ---- Float.intBitsToFloat (I)F : caller reads r0 -----------------------------------------------------------------
for x in F_CASES:
    def t(x=x):
        r0, r1, n = call('Java_java_lang_Float_intBitsToFloat', [fbits(x)])
        return r0 == fbits(x), 'r0=%#010x want=%#010x' % (r0, fbits(x))
    run_case('intBitsToFloat(%#010x = %r)' % (fbits(x), x), t)

# ---- Double.longBitsToDouble (J)D : caller reads r0 = HIGH, r1 = LOW (same as the Math entries) ----------------
for x in D_CASES:
    def t(x=x):
        b = dbits(x)
        hi, lo = b >> 32, b & 0xffffffff
        r0, r1, n = call('Java_java_lang_Double_longBitsToDouble', [lo, hi])
        return (r0 == hi and r1 == lo), 'r0:r1=%#010x:%#010x want=%#010x:%#010x' % (r0, r1, hi, lo)
    run_case('longBitsToDouble(%#018x = %r)' % (dbits(x), x), t)

print('TOTAL %d: ok=%d wrong=%d abort=%d' % (tot, ok, bad, ab))
