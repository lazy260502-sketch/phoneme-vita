#!/usr/bin/env python3
"""log27: decode the psp2core dump's THREAD_INFO + THREAD_REG_INFO notes
into a named per-thread register table (VM thread PC verdict machine).

Usage: threads27.py <core.psp2dmp[.gz]> [<symfile>] [pc...]

Layouts (log27-verified, fw3.65 coredumper):
  THREAD_INFO  desc: [u32 0x12][u32 count] + fixed 200B entries:
      +0x00 esz(=0xc8)  +0x04 tid  +0x08 name[32]
      +0x2c initPriority +0x30 currentPriority
  THREAD_REG_INFO desc: u32 count, then [u32 tid][u32 esz=376] entries:
      +0x0c..+0x3c r0..r12  +0x40 sp  +0x44 lr  +0x48 pc  +0x4c cpsr
  REG_INFO carries MORE rows than THREAD_INFO (kernel-internal ones
  with tid=0) and its tid field is unreliable for user threads (log27:
  VM row carried 5).  Match names by exact tid; the VM main thread is
  the row whose pc/lr sit in user .text and sp is inside its stack.
  Kernel text (0xe0......) PCs are syscall traps - normal for WAITING;
  a RUNNING thread parked there = frozen inside a syscall (log27
  watchdog verdict).
Symbolize user PCs with addr2line or the fallback nearest-symbol walk
when the symfile carries no line info.
"""
import struct, sys, gzip, bisect, subprocess

path = sys.argv[1]
symfile = sys.argv[2] if len(sys.argv) > 2 else None
data = open(path, 'rb').read()
if data[:2] == b'\x1f\x8b':
    data = gzip.decompress(data)

e_phoff = struct.unpack_from('<I', data, 0x1c)[0]
e_phnum = struct.unpack_from('<H', data, 0x2c)[0]

def walk_notes(buf, base):
    pos = 0
    while pos + 12 <= len(buf):
        namesz, descsz, ntype = struct.unpack_from('<III', buf, pos)
        pos += 12
        name = buf[pos:pos+namesz].rstrip(b'\0').decode('latin1')
        pos += (namesz + 3) & ~3
        yield ntype, name, base + pos, descsz
        pos += (descsz + 3) & ~3

notes = {}
for i in range(e_phnum):
    off = e_phoff + i * 32
    t, o, v, p, fs = struct.unpack_from('<IIIII', data, off)[:5]
    if t != 4:
        continue
    for ntype, name, d_off, d_sz in walk_notes(data[o:o+fs], o):
        notes.setdefault(name, []).append(data[d_off:d_off+d_sz])

# THREAD_INFO desc: [u32 0x12][u32 count], then fixed 200B entries:
#   +0x00 esz(=0xc8)  +0x04 tid  +0x08 name[32]  +0x2c initPrio
#   +0x30 currPrio    +0xa0 lastPC (RUNNING only!)
# desc size = 8 + count*200.  REG_INFO has MORE rows (kernel-internal
# ones with tid=0); its tid field is unreliable for user threads
# (log27: VM row carried 5).  Match by exact tid, else '?' - the rest
# needs manual reasoning (see PROJECT_MEMORY log27 section).
names = {}
if 'THREAD_INFO' in notes:
    d = notes['THREAD_INFO'][0]
    cnt = struct.unpack_from('<I', d, 4)[0]
    print(f"# THREAD_INFO: {cnt} process threads")
    pos = 8
    while pos + 200 <= len(d):
        e = d[pos:pos+200]
        tid = struct.unpack_from('<I', e, 4)[0]
        nm = e[8:40].rstrip(b'\0').decode('latin1')
        ip, cp = struct.unpack_from('<II', e, 0x2c)
        pc = struct.unpack_from('<I', e, 0xa0)[0]
        print(f"#   tid={tid:#010x} '{nm}' prio {ip:#x}/{cp:#x} pcField={pc:#010x}")
        names[tid] = nm
        pos += 200

# symbols
syms = []
if symfile:
    try:
        out = subprocess.run(['arm-vita-eabi-nm', '-n', symfile],
                             capture_output=True, text=True).stdout
        for line in out.splitlines():
            p = line.split()
            if len(p) == 3:
                try:
                    syms.append((int(p[0], 16), p[2]))
                except ValueError:
                    pass
    except FileNotFoundError:
        pass
addrs = [s[0] for s in syms]

def sym(a):
    if a >= 0xe0000000:
        return "kernel(syscall)"
    if not syms:
        return "?"
    i = bisect.bisect_right(addrs, a) - 1
    if i < 0:
        return "?"
    b, n = syms[i]
    return f"{n}+0x{a-b:x}"

d = notes['THREAD_REG_INFO'][0]
cnt = struct.unpack_from('<I', d, 0)[0]
print(f"# {cnt} threads with register sets")
pos = 4

while pos + 8 <= len(d):
    tid, esz = struct.unpack_from('<II', d, pos)
    if esz == 0 or pos + esz > len(d):
        break
    e = d[pos:pos+esz]
    regs = struct.unpack_from('<13I', e, 0x0c)
    sp, lr, pc, cpsr = struct.unpack_from('<4I', e, 0x40)
    nm = names.get(tid, f"? (regtid={tid})")
    print(f"tid={tid:<11d} '{nm}'")
    print(f"    sp ={sp:#010x}  lr ={lr:#010x} {sym(lr)}")
    print(f"    pc ={pc:#010x} {sym(pc)}  cpsr={cpsr:#010x}")
    print("    " + " ".join(f"r{i}={v:08x}" for i, v in enumerate(regs)))
    pos += esz
