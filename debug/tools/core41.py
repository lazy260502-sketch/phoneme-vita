#!/usr/bin/env python3
"""log41 core dump 取证：线程寄存器 + slide 求解 + 符号化。

按已核实布局解析: THREAD_REG_INFO(note 0x1004) = 8 字节头 + N x 0x178 块
  块 +0x00 size_field(=0x178)
  块 +0x04 SceUID
  块 +0x08..0x44 r0..r15
  块 +0x48 cpsr   (bit5=1 -> Thumb)

用法: python3 core41.py <psp2dmp> <sym/elf> [text_slide_hex]
"""
import bisect
import gzip
import struct
import subprocess
import sys

dmp, symfile = sys.argv[1], sys.argv[2]
forced = int(sys.argv[3], 16) if len(sys.argv) > 3 else None

data = open(dmp, 'rb').read()
if data[:2] == b'\x1f\x8b':
    data = gzip.decompress(data)

# ---- ELF 程序头 / PT_NOTE 扫描 ----
e_phoff = struct.unpack_from('<I', data, 0x1c)[0]
e_phnum = struct.unpack_from('<H', data, 0x2c)[0]
notes = []
for i in range(e_phnum):
    f = struct.unpack_from('<8I', data, e_phoff + i * 32)
    if f[0] == 4:  # PT_NOTE
        notes.append((f[1], f[4]))
print(f'phnum={e_phnum} PT_NOTE={len(notes)}')

trinfo = None
summ = None
for off, sz in notes:
    pos = off
    while pos + 12 <= off + sz:
        namesz, descsz, ntype = struct.unpack_from('<III', data, pos)
        doff = pos + 12 + ((namesz + 3) & ~3)
        if ntype == 0x1004 and trinfo is None:
            trinfo = data[doff:doff + descsz]
        if ntype == 0x102e:
            summ = (doff, descsz)
        pos = doff + ((descsz + 3) & ~3)

# ---- 符号表 ----
syms = []
out = subprocess.run(['arm-vita-eabi-nm', '-n', symfile],
                     capture_output=True, text=True).stdout
if not out:
    out = subprocess.run(['/home/zyb/.local/vitasdk/bin/arm-vita-eabi-nm',
                          '-n', symfile], capture_output=True, text=True).stdout
for line in out.splitlines():
    p = line.split(None, 2)
    if len(p) == 3:
        try:
            syms.append((int(p[0], 16), p[2].strip()))
        except ValueError:
            pass
syms.sort()
addrs = [a for a, _ in syms]


def sym(a):
    if a == 0:
        return '(null)'
    i = bisect.bisect_right(addrs, a) - 1
    if i < 0:
        return f'{a:#010x} (?)'
    return f'{syms[i][1]}+{a - syms[i][0]:#x}'


def symat(a, slide):
    return sym(a - slide) if a else '(null)'


# ---- 线程块（自描述 size 字段扫描，抗头部长度差异） ----
N = (len(trinfo) - 8) // 0x178
print(f'THREAD_REG_INFO descsz={len(trinfo)} -> N={N}\n')
print('==== 原始寄存器（未加 slide）====')
blocks = []
for i in range(N):
    b = 8 + i * 0x178
    sz, = struct.unpack_from('<I', trinfo, b)
    tid, = struct.unpack_from('<I', trinfo, b + 4)
    regs = struct.unpack_from('<16I', trinfo, b + 8)
    cpsr, = struct.unpack_from('<I', trinfo, b + 0x48)
    blocks.append((sz, tid, regs, cpsr))
    print(f'[{i:2d}] sz={sz:#x} tid={tid:#010x} cpsr={cpsr:#010x}'
          f'{" T" if cpsr & 0x20 else " A"}')
    print(f'     r0={regs[0]:#010x} r1={regs[1]:#010x} r2={regs[2]:#010x} '
          f'r3={regs[3]:#010x}')
    print(f'     r4={regs[4]:#010x} r5={regs[5]:#010x} r6={regs[6]:#010x} '
          f'r7={regs[7]:#010x}')
    print(f'     r8={regs[8]:#010x} r9={regs[9]:#010x} r10={regs[10]:#010x} '
          f'r11={regs[11]:#010x}')
    print(f'     r12={regs[12]:#010x} sp={regs[13]:#010x} lr={regs[14]:#010x} '
          f'pc={regs[15]:#010x}')

# ---- slide 求解：对每个候选 slide，最大化 pc/lr 落进 .text 且能符号化 ----
# 用 nm 里 .text 的地址范围做锚点
taddrs = [a for a, n in syms if 0x81000000 <= a < 0x81300000]
lo, hi = min(taddrs), max(taddrs)

cand = {}
for i, (sz, tid, regs, cpsr) in enumerate(blocks):
    for name, v in (('pc', regs[15]), ('lr', regs[14])):
        if v < 0x81000000 or v > 0x82000000:
            continue
        # 假设 v 是 text_slide 后的地址 -> slide = v - (base + k*0x1000)
        for k in range(0, 0x200):
            s = v - (lo + k * 0x1000)
            if s < 0:
                continue
            cand[s] = cand.get(s, 0) + 1

best = sorted(cand.items(), key=lambda x: (-x[1], x[0]))[:6]
print('\n==== slide 候选（pc/lr 落入 .text 的计数）====')
for s, c in best:
    print(f'  slide={s:#x}  hits={c}')

if forced is not None:
    slides = [forced]
else:
    slides = [best[0][0]] if best else [0]

for slide in slides:
    print(f'\n==== 符号化（text slide={slide:#x}）====')
    for i, (sz, tid, regs, cpsr) in enumerate(blocks):
        pc, lr, sp = regs[15], regs[14], regs[13]
        print(f'[{i:2d}] tid={tid:#010x} sp={sp:#010x}'
              f'{" Thumb" if cpsr & 0x20 else ""}')
        print(f'     pc={pc:#010x} {symat(pc, slide)}')
        print(f'     lr={lr:#010x} {symat(lr, slide)}')

# ---- 栈回溯：用 PT_LOAD 段映射读栈内存 ----
segs = []
for i in range(e_phnum):
    f = struct.unpack_from('<8I', data, e_phoff + i * 32)
    if f[0] == 1 and f[4] > 0:  # PT_LOAD
        segs.append((f[1], f[2], f[5]))  # off, vaddr, filesz


def rd(vaddr, n):
    for off, va, fsz in segs:
        if va <= vaddr and vaddr + n <= va + fsz:
            return data[off + (vaddr - va):off + (vaddr - va) + n]
    return None


slide = slides[0]
for idx in (0,):
    regs = blocks[idx][2]
    sp = regs[13]
    print(f'\n==== 线程[{idx}] 栈回溯  sp={sp:#010x} ====')
    blob = rd(sp, 0x400)
    if blob is None:
        print('  栈不在 core 中（filesz < memsz，未保存）')
        continue
    for k in range(0, len(blob) - 3, 4):
        w, = struct.unpack_from('<I', blob, k)
        if 0x81000000 <= w < 0x81300000:
            nmv = w - slide
            i2 = bisect.bisect_right(addrs, nmv) - 1
            if i2 >= 0 and nmv - syms[i2][0] < 0x4000:
                print(f'  sp+{k:#05x} {w:#010x}  {syms[i2][1]}+{nmv - syms[i2][0]:#x}')
