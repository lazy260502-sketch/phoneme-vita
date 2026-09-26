#!/usr/bin/env python3
"""log41: 用栈回溯给 text slide 定标（页对齐扫描 + 符号命中评分）。"""
import bisect
import gzip
import struct
import subprocess
import sys

dmp, symfile = sys.argv[1], sys.argv[2]
data = open(dmp, 'rb').read()
if data[:2] == b'\x1f\x8b':
    data = gzip.decompress(data)

e_phoff = struct.unpack_from('<I', data, 0x1c)[0]
e_phnum = struct.unpack_from('<H', data, 0x2c)[0]
segs, notes = [], []
for i in range(e_phnum):
    f = struct.unpack_from('<8I', data, e_phoff + i * 32)
    if f[0] == 1:
        segs.append((f[1], f[2], f[5]))
    if f[0] == 4:
        notes.append((f[1], f[4]))

trinfo = None
for off, sz in notes:
    pos = off
    while pos + 12 <= off + sz:
        namesz, descsz, ntype = struct.unpack_from('<III', data, pos)
        doff = pos + 12 + ((namesz + 3) & ~3)
        if ntype == 0x1004 and trinfo is None:
            trinfo = data[doff:doff + descsz]
        pos = doff + ((descsz + 3) & ~3)


def rd(vaddr, n):
    for off, va, fsz in segs:
        if va <= vaddr and vaddr + n <= va + fsz:
            return data[off + (vaddr - va):off + (vaddr - va) + n]
    return None


N = (len(trinfo) - 8) // 0x178
blocks = []
for i in range(N):
    b = 8 + i * 0x178
    blocks.append(struct.unpack_from('<16I', trinfo, b + 8))

nm = subprocess.run(['nm', '-n', symfile], capture_output=True, text=True).stdout
syms = []
for line in nm.splitlines():
    p = line.split(None, 2)
    if len(p) == 3:
        try:
            syms.append((int(p[0], 16), p[2].strip()))
        except ValueError:
            pass
syms.sort()
addrs = [a for a, _ in syms]

# .text 范围（从 nm 推）
text = [a for a, n in syms if 0x81000000 <= a < 0x81300000]
TLO, THI = min(text), max(text)
print(f'nm .text 近似范围: {TLO:#x} .. {THI:#x}  ({len(text)} 符号)')

# 收集所有候选值：各线程 pc/lr + 各线程栈内 .text 区间字
vals = set()
for regs in blocks:
    for v in (regs[15], regs[14]):
        if v >= 0x81000000:
            vals.add(v)
    sp = regs[13]
    blob = rd(sp, 0x600)
    if blob:
        for k in range(0, len(blob) - 3, 4):
            w, = struct.unpack_from('<I', blob, k)
            if 0x81000000 <= w < 0x81300000:
                vals.add(w)
print(f'候选地址数: {len(vals)}')

best = []
for sl in range(0, 0x300000, 0x1000):
    hits = 0
    for v in vals:
        nv = v - sl
        if not (TLO <= nv <= THI):
            continue
        i = bisect.bisect_right(addrs, nv) - 1
        if i >= 0 and nv - syms[i][0] < 0x20000:
            hits += 1
    best.append((hits, sl))
best.sort(reverse=True)
print('\n==== slide 评分前 8 ====')
for h, s in best[:8]:
    print(f'  slide={s:#x}  命中 {h}/{len(vals)}')

SL = best[0][1]


def sy(a):
    if not a:
        return '(null)'
    nv = a - SL
    i = bisect.bisect_right(addrs, nv) - 1
    if i >= 0 and nv - syms[i][0] < 0x4000:
        return f'{syms[i][1]}+{nv - syms[i][0]:#x}'
    return f'{a:#010x} (?)'


print(f'\n==== 选定 slide={SL:#x} ====')
for i, regs in enumerate(blocks):
    print(f'[{i}] sp={regs[13]:#010x} pc={regs[15]:#010x} {sy(regs[15])}')
    print(f'       lr={regs[14]:#010x} {sy(regs[14])}')

print(f'\n==== 线程[0]（vm）栈回溯 ====')
sp = blocks[0][13]
blob = rd(sp, 0x800)
if blob is None:
    print('  栈未保存')
else:
    print(f'  sp={sp:#010x} 栈内存 {len(blob)} 字节')
    for k in range(0, len(blob) - 3, 4):
        w, = struct.unpack_from('<I', blob, k)
        if 0x81000000 <= w < 0x81300000:
            nv = w - SL
            i = bisect.bisect_right(addrs, nv) - 1
            if i >= 0 and nv - syms[i][0] < 0x8000:
                print(f'  sp+{k:#05x}  {w:#010x}  {syms[i][1]}+{nv - syms[i][0]:#x}')
