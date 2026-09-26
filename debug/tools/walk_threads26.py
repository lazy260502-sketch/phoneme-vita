#!/usr/bin/env python3
"""log26 forensics part 2: decode ALL THREAD_INFO notes - name + saved PC.

log24 method: PT_NOTE segments contain THREAD_INFO (type 0x3f0) notes,
one per kernel thread: entry = [u32 size][u32 tid][32B name]... with the
saved PC at +0x9c (verified against log24's coredump PC). If the VM
thread is STARVED by a spinning hog, the hog's saved PC names it.
"""
import struct, sys, gzip

path = sys.argv[1]
data = open(path, 'rb').read()
if data[:2] == b'\x1f\x8b':
    data = gzip.decompress(data)

e_phoff = struct.unpack_from('<I', data, 0x1c)[0]
e_phnum = struct.unpack_from('<H', data, 0x2c)[0]
if data[:2] == b'\x1f\x8b':
    raise SystemExit("BUG: still compressed at parse time")

notes = []
for i in range(e_phnum):
    off = e_phoff + i * 32
    p_type, p_offset, p_vaddr, p_paddr, p_filesz = \
        struct.unpack_from('<IIIII', data, off)[:5]
    if p_type == 4:  # PT_NOTE
        notes.append((p_offset, p_filesz))

def walk_notes(buf, base=0):
    """yield (namesz, descsz, name, desc_off)"""
    pos = 0
    out = []
    while pos + 12 <= len(buf):
        namesz, descsz, ntype = struct.unpack_from('<III', buf, pos)
        pos += 12
        name = buf[pos:pos+namesz].rstrip(b'\0').decode('latin1')
        pos += (namesz + 3) & ~3
        out.append((ntype, name, base + pos, descsz))
        pos += (descsz + 3) & ~3
    return out

for p_off, p_sz in notes:
    for ntype, name, d_off, d_sz in walk_notes(data[p_off:p_off+p_sz], p_off):
        if ntype != 0x3f0:  # THREAD_INFO
            continue
        d = data[d_off:d_off+d_sz]
        # log24 layout: u32 size, u32 tid, then name at +8 (32B)
        size, tid = struct.unpack_from('<II', d, 0)
        tname = d[8:40].rstrip(b'\0').decode('latin1')
        # saved PC at +0x9c (log24-verified field)
        pc = struct.unpack_from('<I', d, 0x9c)[0] if len(d) >= 0xa0 else None
        # also dump a few other candidate fields for cross-checking
        f_a0 = struct.unpack_from('<I', d, 0xa0)[0] if len(d) >= 0xa4 else 0
        f_98 = struct.unpack_from('<I', d, 0x98)[0] if len(d) >= 0x9c else 0
        print(f"tid={tid:<11d} name={tname:<28s} pc@0x9c={pc:#010x} "
              f"lr?@0x98={f_98:#010x} +0xa0={f_a0:#010x}")
