#!/usr/bin/env python3
"""log26 forensics: reconstruct the scheduler structures from coredump memory.

Anchors (from wd_snapshot dump): the 5 ThreadDesc addresses. We scan each
object's first 0x100 bytes for pointer fields that point at OTHER known
threads (= link fields: _next/_previous), plus fields matching the known
wait_obj values (0x81eb6ee4 x2, 0x81eda000) to locate _wait_obj, and
8-byte fields holding plausible wakeup timestamps for _wakeup_time.
"""
import struct, sys, gzip

path = sys.argv[1]
data = open(path, 'rb').read()
if data[:2] == b'\x1f\x8b':
    data = gzip.decompress(data)

# parse ELF32 program headers -> (vaddr, file_off, filesz) for PT_LOAD
e_phoff = struct.unpack_from('<I', data, 0x1c)[0]
e_phnum = struct.unpack_from('<H', data, 0x2c)[0]
segs = []
for i in range(e_phnum):
    off = e_phoff + i * 32
    p_type, p_offset, p_vaddr, _, p_filesz = struct.unpack_from('<IIIII', data, off)
    if p_type == 1:  # PT_LOAD
        segs.append((p_vaddr, p_offset, p_filesz))

def read(vaddr, size):
    for va, off, sz in segs:
        if va <= vaddr < va + sz:
            o = off + (vaddr - va)
            if o + size <= off + sz:
                return data[o:o+size]
    return None

def rd32(a):
    b = read(a, 4)
    return struct.unpack('<I', b)[0] if b else None

THREADS = {0x81eddfb4: 'id4', 0x81edc2ec: 'id3', 0x81eb8128: 'id2',
           0x81eb700c: 'id1', 0x818a58f0: 'id0'}
KNOWN_OBJ = {0x81eb6ee4: 'waitobjA', 0x81eda000: 'waitobjB'}

print("== per-object pointer fields (offset: value -> identity) ==")
for taddr, name in THREADS.items():
    print(f"\n--- {name} @ {taddr:#x} ---")
    for off in range(0, 0x100, 4):
        v = rd32(taddr + off)
        if v is None: continue
        tag = THREADS.get(v) or KNOWN_OBJ.get(v)
        if tag:
            print(f"  +{off:#04x}: {v:#010x} -> {tag}")
        elif 0x81000000 <= v < 0x81600000:
            pass  # code ptr, skip
    # wakeup_time is jlong, likely 8-aligned; print plausible timestamps
    for off in range(0, 0x100, 4):
        b = read(taddr + off, 8)
        if b:
            lo, hi = struct.unpack('<II', b)
            if hi == 0 and 0x1A0000000000 < lo:  # ~2026 epoch ms
                print(f"  +{off:#04x}: jlong {lo} (epoch ms {lo/1000:.0f})")

print("\n== global_threadlist walk check (head=first active-list entry) ==")
# cur_thread reported by snapshot = 0x81401d10 - inspect it
cur = 0x81401d10
b = read(cur, 0x40)
print(f"cur_thread @ {cur:#x} first 0x40 bytes:",
      b.hex() if b else "NOT IN DUMP")
