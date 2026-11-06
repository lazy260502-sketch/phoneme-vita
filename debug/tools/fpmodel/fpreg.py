"""Regression: apply a Plan overlay to the model and run the FP bytecode cases against it.
usage: fpreg.py <overlay.elf|none> <out>.  Also counts executed instructions per case (performance proxy)."""
import os, sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fplib
from fplib import *
import overlay

ovl = sys.argv[1]
out = sys.argv[2]
if ovl != 'none':
    n, names = overlay.apply(fplib.cpu.w, ovl)
    print('overlay %s: %d interpreter call sites re-pointed to %d shims' % (ovl, n, len(names)))

# instrument: count steps
steps = [0]
_orig = fplib.cpu.step


def counting(ins, pc, _o=_orig):
    steps[0] += 1
    return _o(ins, pc)


fplib.cpu.step = counting
sys.argv = ['fpcases.py', out]
exec(open(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'fpcases.py')).read())
print('instructions executed over all cases: %d' % steps[0])
open(out, 'a').write('instructions executed over all cases: %d\n' % steps[0])
