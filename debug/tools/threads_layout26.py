#!/usr/bin/env python3
"""log26: decode THREAD_INFO note (ntype 0x1004) - per-thread register sets.

desc head: 11000000 05000000 78010000 03000140 ...
  +0x00 u32 count? = 0x11 (17) -- but only 5 VM threads expected...
  Actually 0x11=17: matches "17 kernel threads" from log24 analysis.
  +0x04 u32 entry_size? = 5?? no. Try: entries start +0x04, each
  [u32 size=0x178][u32 tid][32B name][reg block...]. 1888 = 0x760.
  0x760 / 0x11 = 108.8 -- not integer, so header differs.
Alternative: entry layout = [u32 tid][u32 size][...]. head shows
  05 00 00 00 | 78 01 00 00 | 03 00 01 40 -> tid=5? size=0x178? 0x40010003=stack 0x810...
Take: count=0x11(17) at +0, then per entry: u32 tid, u32 entry_size(0x178=376),
  name 32B at +8 of entry, then registers. 376-8-32=336. If regs r0-r15+cpsr
  (17*4=68) + fp regs ... pc usually near start of reg block. We will print
  candidate fields and the name; pc identification via symbolization.
"""
import struct, sys, gzip

path = sys.argv[1]
data = open(path, 'rb').read()
if data[:2] == b'\x1f\x8b':
    data = gzip.decompress(data)

e_phoff = struct.unpack_from('<I', data, 0x1c)[0]
e_phnum = struct.unpack_from('<H', data, 0x2c)[0]

target = None
for i in range(e_phnum):
    off = e_phoff + i * 32
    f = struct.unpack_from('<8I', data, off)
    if f[0] == 4 and f[4] >= 0x100:
        p_off, p_sz = f[1], f[4]
        pos = p_off
        while pos + 12 <= p_off + p_sz:
            namesz, descsz, ntype = struct.unpack_from('<III', data, pos)
            desc_off = pos + 12 + ((namesz + 3) & ~3)
            if ntype == 0x1004:
                target = data[desc_off:desc_off + descsz]
            pos = desc_off + ((descsz + 3) & ~3)
        if target: break

if not target:
    sys.exit('no 0x1004 note')

cnt = struct.unpack_from('<I', target, 0)[0]
print(f'count field = {cnt}')
# dump first 3 entries as hex words to identify layout
for w in range(0, 0x60, 4):
    v = struct.unpack_from('<I', target, w)[0]
    print(f'  +{w:#04x}: {v:#010x}  {v}')
