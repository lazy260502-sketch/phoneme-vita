#!/usr/bin/env python3
"""psp2dmp.py - Vita psp2core coredump triage for the J2ME port.

Self-contained (stdlib only). Decodes the gzip'd ELF32 ARM core file that
psp2core drops next to the crashed app, then prints a verdict-ready summary:

  threads   : name, tid, pc/lr/sp, sleeping-vs-crashed classification,
              module-base-corrected symbolization for OUR module
  base      : module load base discovered from a sleeper thread landing in
              a SceLibKernel delay stub (r12 = stub slot) - addr2line on a
              raw pc is garbage until the delta is subtracted
  notes     : PT_NOTE inventory (THREAD_REG_INFO, STACK_INFO, TTY_INFO...)
  pages     : PT_LOAD "extra pages" psp2core dumps around the fault
              (registers pointing into these pages = fault-relevant state)
  strings   : probe outputs (rd7/jstack/ring_trap) found in raw TTY

Usage:
  psp2dmp.py list <file.psp2dmp>                  # notes + pages inventory
  psp2dmp.py threads <file.psp2dmp> [--elf PATH]  # register dump + symbols
  psp2dmp.py base <file.psp2dmp> [--elf PATH]     # only the base verdict
  psp2dmp.py mem <file.psp2dmp> <hexaddr> [len]   # read fault pages
  psp2dmp.py elfstr <file.psp2dmp> --elf PATH <hexaddr> [len]
                                                  # read our ELF (.rodata
                                                  # string at a runtime addr)

Exit status 0 on success, 1 on parse failure.

Facts encoded here (learned the hard way, see PROJECT_MEMORY.md log70/71):
  - psp2dmp = gzip -> ELF32 LSB core, notes are PT_NOTE, regs 5x376B.
  - Module base is NOT fixed: 0x8101a000 (log70), 0x81024000 (log71).
    A sleeper pc like 0x81127338 with r12=0x9da lands in a kernel stub;
    its lr lands right after the R_ARM_THM_CALL inside our
    __wrap_sceKernelDelayThread - subtracting (lr_elf_site - lr_dump)
    yields the delta. Symbolizing without the delta points into .rodata
    ("executing data" = wrong base tell).
  - cpsr masks 0x1f: 0x10 ARM/0x11 user etc; i/f/a bits (0x80/0x40/0x20
    set) mean faulted-in-kernel context.
  - r12 in a kernel-pc thread = stub slot id (0x9da delay, 0xb2a io...).
"""
import gzip
import struct
import subprocess
import sys
import os

REG_NAMES = ['r0', 'r1', 'r2', 'r3', 'r4', 'r5', 'r6', 'r7',
             'r8', 'r9', 'r10', 'r11', 'r12', 'sp', 'lr', 'pc', 'cpsr']

# our module's ELF (vita-port build) - overridable with --elf
DEFAULT_ELF = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                           '..', '..', 'vita-port', 'build', 'midp_vita')

# ELF vaddr of __wrap_sceKernelDelayThread's thumb call return site
# (readelf -r: 81011b46 R_ARM_THM_CALL sceKernelDelayThread -> ret 81011b4b),
# and the ELF vaddr of the sceKernelDelayThread stub's 3rd instruction where
# a sleeper's pc parks (stub+8; r12 = slot 0x9da is loaded by the kernel side).
# Verified against log70 (delta 0x1a000) and log71 (delta 0x24000).
DELAY_RET_SITE = 0x81011b4b
DELAY_STUB_PC = 0x8110d338

KNOWN_R12 = {
    0x9d7: 'sceKernelWaitEventFlag ??',
    0x9da: 'sceKernelDelayThread',
    0xb2a: 'sceIo* (ioctrl family)',
}


def die(msg):
    sys.stderr.write('psp2dmp: %s\n' % msg)
    sys.exit(1)


def load_core(path):
    raw = open(path, 'rb').read()
    if raw[:2] == b'\x1f\x8b':
        raw = gzip.decompress(raw)
    if raw[:4] != b'\x7fELF':
        die('not an ELF (even after gunzip): %s' % path)
    if raw[4] != 1 or raw[5] != 1:
        die('not ELF32 LE')
    return raw


class Core:
    def __init__(self, path):
        self.d = load_core(path)
        d = self.d
        e_phoff, = struct.unpack_from('<I', d, 0x1c)
        e_phentsize, e_phnum = struct.unpack_from('<HH', d, 0x2a)
        self.notes = []   # (name, desc_off, desc_size)
        self.loads = []   # (vaddr, off, filesz)
        for i in range(e_phnum):
            o = e_phoff + i * e_phentsize
            p_type, p_offset, p_vaddr, p_paddr, p_filesz = struct.unpack_from('<5I', d, o)
            if p_type == 4:
                self._walk_note(p_offset, p_filesz)
            elif p_type == 1 and p_vaddr:
                self.loads.append((p_vaddr, p_offset, p_filesz))
        self.thread_names = self._thread_names()
        self.regs = self._thread_regs()

    def _walk_note(self, off, size):
        d = self.d
        p, end = off, off + size
        while p < end:
            nsz, dsz, ntype = struct.unpack_from('<III', d, p)
            # psp2core pads short names with SPACES, not NULs
            # ("THREAD_INFO" vs "THREAD_REG_INFO" which fits in 15+NUL)
            name = d[p + 12:p + 12 + nsz - 1].decode('ascii', 'replace').rstrip('\x00 ')
            desc = p + 12 + ((nsz + 3) // 4) * 4
            self.notes.append((name, desc, dsz))
            p = desc + ((dsz + 3) // 4) * 4

    def note(self, wanted):
        for name, desc, dsz in self.notes:
            if name == wanted:
                return self.d[desc:desc + dsz]
        return None

    def _thread_names(self):
        b = self.note('THREAD_INFO')
        names = []
        if b is None:
            return names
        # header: ver, count, entsize; entries from @12: tid u32, name[32]
        _, count, entsize = struct.unpack_from('<III', b, 0)
        for i in range(count):
            o = 12 + i * entsize
            if o + 36 > len(b):
                break
            names.append(b[o + 4:o + 36].split(b'\x00')[0].decode('utf-8', 'replace'))
        return names

    def _thread_regs(self):
        b = self.note('THREAD_REG_INFO')
        if b is None:
            die('no THREAD_REG_INFO note')
        ver, nthr = struct.unpack_from('<II', b, 0)
        out = []
        p = 8
        for t in range(nthr):
            size, tid = struct.unpack_from('<II', b, p)
            regs = struct.unpack_from('<17I', b, p + 8)
            out.append((tid, regs))
            p += size
        return out

    def read_mem(self, vaddr, n):
        """Read from PT_LOAD segments (thread stacks + fault pages)."""
        for base, off, sz in self.loads:
            if base <= vaddr and vaddr + n <= base + sz:
                o = off + (vaddr - base)
                return self.d[o:o + n]
        return None

    def fault_pages(self):
        """PT_LOAD pages that are not stack regions: psp2core fault evidence."""
        out = []
        for base, off, sz in self.loads:
            if 0x81000000 <= base < 0x82000000 and not (0x81300000 <= base < 0x81420000):
                out.append((base, off, sz))
        return out

    # ---- module base discovery ------------------------------------------
    def find_base(self):
        """Use a sleeper parked in our delay stub to pin the module base.

        A sleeper's pc sits INSIDE our module (the stub, not kernel space:
        pc_runtime - delta == DELAY_STUB_PC), r12 = 0x9da, and its lr lands
        right after the R_ARM_THM_CALL inside __wrap_sceKernelDelayThread
        (lr_runtime - delta == DELAY_RET_SITE). Two independent anchors that
        must agree, so no page-alignment guesswork is needed.

        Returns (delta, evidence) where delta = runtime_addr - elf_addr, or
        (None, reason)."""
        for k, (tid, r) in enumerate(self.regs):
            pc, lr, r12 = r[15], r[14], r[12]
            name = self.thread_names[k] if k < len(self.thread_names) else '?'
            in_mod = lambda v: 0x81000000 <= v < 0x81400000
            if r12 == 0x9DA and in_mod(pc) and in_mod(lr):
                d1, d2 = lr - DELAY_RET_SITE, pc - DELAY_STUB_PC
                if d1 == d2 and 0 <= d1 <= 0x400000 and (d1 & 0xFFF) == 0:
                    return d1, ('thread %d (%s) sleeper pc=%08x lr=%08x: '
                                'lr-anchor and stub-anchor both give +%#x'
                                % (k, name, pc, lr, d1))
        return None, 'no sleeper anchor found (threads not parked in delay stub?)'

    def elf_read(self, elf_path, vaddr, n):
        """Read our module's ELF image at a RUNTIME address (delta-aware)."""
        delta, _ = self.find_base()
        if delta is None:
            die('cannot map runtime addr without a base - run `base` first')
        elf = open(elf_path, 'rb').read()
        # ELF LOADs of the app: [text 81000000.., data 812e0000..]
        for seg_va, seg_off, seg_sz, seg_fsz in self._elf_loads(elf):
            ev = vaddr - delta
            if seg_va <= ev < seg_va + seg_fsz:
                o = seg_off + (ev - seg_va)
                return elf[o:o + n]
        return None

    @staticmethod
    def _elf_loads(elf):
        e_phoff, = struct.unpack_from('<I', elf, 0x1c)
        e_phentsize, e_phnum = struct.unpack_from('<HH', elf, 0x2a)
        for i in range(e_phnum):
            o = e_phoff + i * e_phentsize
            p_type, p_offset, p_vaddr, _, p_filesz = struct.unpack_from('<5I', elf, o)
            if p_type == 1:
                yield p_vaddr, p_offset, 0, p_filesz

    def symbolize(self, elf_path, addr_delta_applied):
        try:
            out = subprocess.run(
                ['addr2line', '-f', '-e', elf_path,
                 hex(addr_delta_applied)],
                capture_output=True, text=True, timeout=10).stdout.splitlines()
            if len(out) >= 2:
                return '%s %s' % (out[0], out[1])
        except Exception:
            pass
        return None


def classify(regs):
    """Heuristic thread state classification from the register set."""
    pc, r12 = regs[15], regs[12]
    if 0xE0000000 <= pc < 0xE1000000:
        slot = KNOWN_R12.get(r12)
        return 'KERNEL pc (blocked in %s)' % (slot or 'syscall slot %#x' % r12)
    if r12 == 0x9DA and 0x81000000 <= pc < 0x81400000:
        return 'sleeping in delay stub'
    if all(w == 0xdeadbeef or w == 0xdeadf8de or w == 0xdead80f8 for w in regs):
        return 'untouched (never ran?)'
    return 'user'


def cmd_list(core):
    print('notes:')
    for name, desc, dsz in core.notes:
        print('  %-18s desc@%06x size %d' % (name, desc, dsz))
    print('thread names:', ', '.join(core.thread_names) or '(none)')
    print('PT_LOAD fault pages:')
    for base, off, sz in core.fault_pages():
        print('  vaddr=%08x off=%06x sz=%#x' % (base, off, sz))


def cmd_threads(core, elf):
    delta, ev = core.find_base()
    print('module base: ', ('delta +%#x (base %#x) via %s' % (delta, 0x81000000 + delta, ev))
          if delta is not None else 'UNKNOWN - %s' % ev)
    for k, (tid, r) in enumerate(core.regs):
        name = core.thread_names[k] if k < len(core.thread_names) else '?'
        print('--- thr%d %-24s tid=%08x  [%s]' % (k, name, tid, classify(r)))
        # kernel-pc thread: syscall args are live in r0-r3; if r0 points into
        # our module it is usually a .rodata path/format string - fetch it
        if 0xE0000000 <= r[15] < 0xE1000000 and delta is not None \
                and 0x81000000 <= r[0] < 0x81400000 and os.access(elf, os.R_OK):
            try:
                s = core.elf_read(elf, r[0], 96).split(b'\x00')[0]
                txt = s.decode('utf-8', 'replace')
                if txt and all(32 <= ord(ch) < 127 for ch in txt[:16]):
                    print('   r0 -> %r  (probable syscall arg string)' % txt)
            except Exception:
                pass
        for i, nm in enumerate(REG_NAMES):
            v = r[i]
            ann = ''
            if delta is not None and nm in ('pc', 'lr') and 0x81000000 <= v < 0x81400000:
                sym = core.symbolize(elf, v - delta)
                if sym:
                    ann = '  -> %s' % sym
            if nm == 'r12' and v in KNOWN_R12:
                ann = '  (%s)' % KNOWN_R12[v]
            print('   %-5s %08x%s' % (nm, v, ann))


def cmd_base(core):
    delta, ev = core.find_base()
    if delta is None:
        print('FAIL: %s' % ev)
        sys.exit(1)
    print('delta = +%#x  (module loaded at %#x)' % (delta, 0x81000000 + delta))
    print('anchor: %s' % ev)
    print('usage: addr2line -e <elf> $(( runtime_pc - %#x ))' % delta)


def cmd_mem(core, vaddr, n):
    b = core.read_mem(vaddr, n)
    if b is None:
        die('address %#x not inside any dumped PT_LOAD' % vaddr)
    for off in range(0, len(b), 16):
        row = b[off:off + 16]
        print('%08x  %-47s  %s' % (vaddr + off,
              ' '.join('%02x' % c for c in row),
              ''.join(chr(c) if 32 <= c < 127 else '.' for c in row)))


def cmd_elfstr(core, elf, vaddr, n=96):
    b = core.elf_read(elf, vaddr, n)
    if b is None:
        die('runtime addr %#x does not map into the ELF image' % vaddr)
    s = b.split(b'\x00')[0]
    print('string at runtime %#x: %r' % (vaddr, s.decode('utf-8', 'replace')))
    for off in range(0, min(len(b), 64), 16):
        row = b[off:off + 16]
        print('%08x  %-47s  %s' % (vaddr + off,
              ' '.join('%02x' % c for c in row),
              ''.join(chr(c) if 32 <= c < 127 else '.' for c in row)))


def main():
    args = sys.argv[1:]
    if not args:
        print(__doc__)
        sys.exit(1)
    cmd, path = args[0], args[1]
    elf = os.path.abspath(DEFAULT_ELF)
    rest = args[2:]
    if '--elf' in rest:
        i = rest.index('--elf')
        elf = os.path.abspath(rest[i + 1])
        del rest[i:i + 2]
    core = Core(path)
    if cmd == 'list':
        cmd_list(core)
    elif cmd == 'threads':
        cmd_threads(core, elf)
    elif cmd == 'base':
        cmd_base(core)
    elif cmd == 'mem':
        if not rest:
            die('mem needs <hexaddr> [len]')
        cmd_mem(core, int(rest[0], 16), int(rest[1], 0) if len(rest) > 1 else 64)
    elif cmd == 'elfstr':
        if not rest:
            die('elfstr needs <hexaddr> [len]')
        cmd_elfstr(core, elf, int(rest[0], 16),
                   int(rest[1], 0) if len(rest) > 1 else 96)
    else:
        die('unknown command %r' % cmd)


if __name__ == '__main__':
    main()
