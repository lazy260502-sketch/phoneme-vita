"""Driver: run a real interpreter bytecode handler from the ELF snapshot through the ARM/VFP model.
Java stack layout (from the handler disassembly): TOS cached in r0; a double operand = HIGH word nearer the top,
so for double b: r0 = b.hi, next pop = b.lo.  A handler leaves its result in r0 (+ r1 pushed for 2-word results)."""
import os, sys, struct
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from armsim import load_text, sym, f64, bits64, Abort
import armsim3

W = load_text()
cpu = armsim3.Sim(W)
STACK = 0x30000000


def f32bits(x):
    return struct.unpack('<I', struct.pack('<f', x))[0]


def f32val(b):
    return struct.unpack('<f', struct.pack('<I', b & 0xffffffff))[0]


def s32(x):
    x &= 0xffffffff
    return x - (1 << 32) if x >> 31 else x


def run(name, below, tos, steps=600):
    """Return (r0, r1, sp_after, word_pushed_below_tos)."""
    cpu.reset_vfp()
    cpu.mem = {}
    for i, w in enumerate(below):
        cpu.wr(STACK + 4 * i, w & 0xffffffff, 4)
    cpu.r = [0] * 16
    cpu.r[0] = tos & 0xffffffff
    cpu.r[6] = STACK
    cpu.r[5] = 0x40000000
    cpu.r[13] = 0x2f000000
    cpu.r[14] = 0x50000000
    cpu.wr(0x50000001, 0, 1)
    cpu.wr(0x40000000, 0xFFFFFFF0, 4)
    cpu.r[15] = sym(name)
    for _ in range(steps):
        pc = cpu.r[15]
        if pc == 0xFFFFFFF0:
            return cpu.r[0], cpu.r[1], cpu.r[6], cpu.rd(cpu.r[6], 4)
        ins = cpu.w.get(pc)
        if ins is None:
            raise Abort('pc %#x' % pc)
        cpu.r[15] = pc + 4
        cpu.step(ins, pc)
    raise Abort('no finish')


ROWS = []


def rec(op, args, got, want, ok):
    ROWS.append((op, args, got, want, bool(ok)))


def report(path):
    ok = sum(1 for r in ROWS if r[4])
    lines = ['%-6s %-12s got=%-30s want=%-12s %s' % (r[0], r[1], r[2], r[3], 'OK' if r[4] else 'WRONG') for r in ROWS]
    lines += ['', 'SUMMARY: %d/%d correct' % (ok, len(ROWS))]
    open(path, 'w').write('\n'.join(lines) + '\n')
    return '\n'.join(lines)
