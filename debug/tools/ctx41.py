#!/usr/bin/env python3
"""读 t0 的线程上下文块（r6 指向的保存上下文），符号化其中的代码指针。

primordial_to_current_thread:
   ldr r6,[r1,#100]      ; r6 = 当前线程的保存上下文
   ldr lr,[r6,#4]        ; [r6+4] = 恢复后要跳到的入口 pc
   ldr fp,[r6],#8        ; [r6+0] = 保存的 fp, 然后 r6 += 8
用法: python3 ctx41.py <psp2dmp> <elf> <r6_hex> <slide_hex>
"""
import gzip, glob, bisect, struct, subprocess, sys

dmp = sys.argv[1]; elf = sys.argv[2]
r6 = int(sys.argv[3], 16); slide = int(sys.argv[4], 16)

d = open(dmp, 'rb').read()
if d[:2] == b'\x1f\x8b':
    d = gzip.decompress(d)
phoff = struct.unpack_from('<I', d, 0x1c)[0]
phnum = struct.unpack_from('<H', d, 0x2c)[0]
segs = []
for i in range(phnum):
    f = struct.unpack_from('<8I', d, phoff + i * 32)
    if f[0] == 1:
        segs.append((f[1], f[2], f[4]))


def rd(va, n):
    for off, vaddr, sz in segs:
        if vaddr <= va < vaddr + sz:
            o = off + (va - vaddr)
            return d[o:min(o + n, off + sz)]
    return None


syms = []
for line in subprocess.run(['arm-vita-eabi-nm', '-n', elf],
                           capture_output=True, text=True).stdout.splitlines():
    p = line.split(None, 2)
    if len(p) == 3:
        try:
            syms.append((int(p[0], 16), p[2].strip()))
        except ValueError:
            pass
syms.sort()
addrs = [s[0] for s in syms]


def sym(a):
    i = bisect.bisect_right(addrs, a) - 1
    if i < 0:
        return None
    b, n = syms[i]
    return n, a - b


base = r6 - 8
print(f'上下文块 base=0x{base:x} (r6-8)，slide=0x{slide:x}')
b = rd(base, 0x60)
if b is None:
    sys.exit('未映射')
for i in range(0, len(b), 4):
    v = struct.unpack_from('<I', b, i)[0]
    note = ''
    s = sym(v - slide) if 0x81000000 + slide <= v < 0x81200000 + slide else None
    if s:
        note = f'  -> {s[0]}+0x{s[1]:x}'
    print(f'  [base+0x{i:02x}] 0x{base+i:08x} = 0x{v:08x}{note}')
