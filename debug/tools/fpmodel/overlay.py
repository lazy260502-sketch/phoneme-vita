"""Overlay = model of 'the final link resolved the interpreter's calls to the *_isoft shims'.
  1. build W from the ELF snapshot (real interpreter handlers + real hard-float helpers);
  2. load the shim image (linked at OVL_BASE) into W;
  3. re-point every `bl jvm_X` that sits INSIDE the interpreter (bc_impl_* / native_math_* callers) to the shim of the
     same name + '_isoft' - exactly what objcopy --redefine-sym on Interpreter_arm.o does at link time.
C++ callers (math kernels etc.) keep calling the hard-float originals, so they are NOT re-pointed."""
import os, re, struct, subprocess

NM = os.path.join(os.environ.get('VITASDK_BIN', '/usr/local/vitasdk/bin'),
                 'arm-vita-eabi-nm')
OD = os.path.join(os.environ.get('VITASDK_BIN', '/usr/local/vitasdk/bin'),
                 'arm-vita-eabi-objdump')
DIS = os.environ.get('FPDIS', '/tmp/elf.dis')


def image_words(elf):
    raw = subprocess.check_output([OD, '-s', '-j', '.text', elf]).decode()
    w = {}
    for line in raw.splitlines():
        m = re.match(r'\s([0-9a-f]{8}) ((?:[0-9a-f]{8} ?){1,4})', line)
        if m:
            a = int(m.group(1), 16)
            for i, x in enumerate(m.group(2).split()):
                w[a + 4 * i] = struct.unpack('<I', bytes.fromhex(x))[0]
    return w


def image_syms(elf):
    s = {}
    for l in subprocess.check_output([NM, elf]).decode().splitlines():
        p = l.split()
        if len(p) == 3:
            s[p[2]] = int(p[0], 16)
    return s


def apply(W, ovl_elf):
    W.update(image_words(ovl_elf))
    syms = image_syms(ovl_elf)
    patched = 0
    enclosing = None
    for line in open(DIS):
        m = re.match(r'^[0-9a-f]+ <(.+)>:$', line)
        if m:
            enclosing = m.group(1)
            continue
        m = re.match(r'\s*([0-9a-f]+):\s+[0-9a-f]{8}\s+bl\t[0-9a-f]+ <(jvm_[a-z0-9_]+)>', line)
        if m and enclosing and (enclosing.startswith('bc_impl_') or enclosing.startswith('native_math_')):
            pc, tgt = int(m.group(1), 16), m.group(2) + '_isoft'
            if tgt in syms:
                off = (syms[tgt] - (pc + 8)) >> 2
                W[pc] = (W[pc] & 0xff000000) | (off & 0x00ffffff)
                patched += 1
    return patched, sorted(k for k in syms if k.endswith('_isoft'))
