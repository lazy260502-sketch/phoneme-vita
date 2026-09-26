#!/usr/bin/env python3
"""log26: decode ALL 17 kernel-thread register sets from THREAD_INFO (0x1004).

Layout (verified): desc = [u32 count] then count entries of 0x178 bytes:
  +0x00 u32 size (0x178)
  +0x04 u32 tid
  +0x08 32B name
  +0x28 ... misc pointers
  +0x44 u32 sp      (+0x44 in entry = desc+0x48+0x44? no - see below)
Verified against entry0: pc at entry+0x48, lr at entry+0x44, sp at entry+0x40
(entry0: 0x813bffb0 = sp, 0x810eefec = lr, 0x810ef000 = pc).
Symbolize pc/lr against the .sym ELF.
"""
import struct, sys, gzip, bisect, subprocess

path, symfile = sys.argv[1], sys.argv[2]
data = open(path, 'rb').read()
if data[:2] == b'\x1f\x8b':
    data = gzip.decompress(data)

e_phoff = struct.unpack_from('<I', data, 0x1c)[0]
e_phnum = struct.unpack_from('<H', data, 0x2c)[0]

target = None
for i in range(e_phnum):
    f = struct.unpack_from('<8I', data, e_phoff + i * 32)
    if f[0] == 4 and f[4] >= 0x100:
        pos = f[1]
        while pos + 12 <= f[1] + f[4]:
            namesz, descsz, ntype = struct.unpack_from('<III', data, pos)
            desc_off = pos + 12 + ((namesz + 3) & ~3)
            if ntype == 0x1004:
                target = data[desc_off:desc_off + descsz]
            pos = desc_off + ((descsz + 3) & ~3)
        if target: break

count = struct.unpack_from('<I', target, 0)[0]
print(f'kernel threads: {count}')

# symbol table
syms = []
out = subprocess.run(['nm', '--defined-only', symfile], capture_output=True,
                     text=True).stdout
for line in out.splitlines():
    p = line.split(None, 2)
    if len(p) == 3 and p[1] in 'tTwW':
        syms.append((int(p[0], 16), p[2]))
syms.sort()
addrs = [a for a, _ in syms]

def sym(a):
    i = bisect.bisect_right(addrs, a) - 1
    if i >= 0 and a - syms[i][0] < 0x10000:
        return f'{syms[i][1]}+{a - syms[i][0]:#x}'
    return f'{a:#010x} (?)'

off = 4
for n in range(count):
    e = target[off:off + 0x178]
    if len(e) < 0x178: break
    tid = struct.unpack_from('<I', e, 4)[0]
    name = e[8:40].rstrip(b'\0').decode('latin1', 'replace')
    sp = struct.unpack_from('<I', e, 0x40)[0]
    lr = struct.unpack_from('<I', e, 0x44)[0]
    pc = struct.unpack_from('<I', e, 0x48)[0]
    print(f'[{n:2d}] tid={tid:<6d} {name:<14s} pc={sym(pc)}')
    print(f'                 lr={sym(lr)} sp={sp:#x}')
    off += 0x178
