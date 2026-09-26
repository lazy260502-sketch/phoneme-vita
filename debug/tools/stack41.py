#!/usr/bin/env python3
"""log41 core dump 取证：把某个线程的"栈"扫出来并符号化。

目的：coredump 只给寄存器，不给调用栈。但 psp2dmp 的 PT_LOAD 里带着进程内存，
所以可以直接读该线程 sp 附近的字节，把其中的**返回地址**挑出来符号化，
还原出它卡死时的调用链。

关键：runtime 地址 = link 地址 + slide(0x37000)，符号化要减回去。
判别：只认落在 [text_lo+slide, text_hi+slide] 的词，其它一律忽略。

用法:
  python3 stack41.py <psp2dmp> <sym/elf> [tid_hex] [slide_hex]
"""
import bisect
import gzip
import struct
import subprocess
import sys

dmp, symfile = sys.argv[1], sys.argv[2]
want_tid = int(sys.argv[3], 16) if len(sys.argv) > 3 else None
slide = int(sys.argv[4], 16) if len(sys.argv) > 4 else 0x37000

data = open(dmp, 'rb').read()
if data[:2] == b'\x1f\x8b':
    data = gzip.decompress(data)

# ---- ELF 程序头 ----
e_phoff = struct.unpack_from('<I', data, 0x1c)[0]
e_phnum = struct.unpack_from('<H', data, 0x2c)[0]
loads, notes = [], []
for i in range(e_phnum):
    f = struct.unpack_from('<8I', data, e_phoff + i * 32)
    if f[0] == 1:
        loads.append({'off': f[1], 'vaddr': f[2], 'filesz': f[4], 'memsz': f[5]})
    elif f[0] == 4:
        notes.append((f[1], f[4]))

# ---- 线程寄存器 ----
trinfo = None
for off, sz in notes:
    pos = off
    while pos + 12 <= off + sz:
        namesz, descsz, ntype = struct.unpack_from('<III', data, pos)
        doff = pos + 12 + ((namesz + 3) & ~3)
        if ntype == 0x1004 and trinfo is None:
            trinfo = data[doff:doff + descsz]
        pos = doff + ((descsz + 3) & ~3)

threads = []
n = (len(trinfo) - 8) // 0x178
for i in range(n):
    b = 8 + i * 0x178
    tid = struct.unpack_from('<I', trinfo, b + 4)[0]
    r = list(struct.unpack_from('<16I', trinfo, b + 8))
    cpsr = struct.unpack_from('<I', trinfo, b + 0x48)[0]
    threads.append({'tid': tid, 'r': r, 'cpsr': cpsr})

# ---- 符号表 ----
out = subprocess.run(['arm-vita-eabi-nm', '-n', symfile],
                     capture_output=True, text=True).stdout
if not out:
    out = subprocess.run(['/home/zyb/.local/vitasdk/bin/arm-vita-eabi-nm',
                          '-n', symfile], capture_output=True, text=True).stdout
syms = []
for line in out.splitlines():
    p = line.split(None, 2)
    if len(p) == 3:
        try:
            syms.append((int(p[0], 16), p[2].strip()))
        except ValueError:
            pass
syms.sort()
addrs = [s[0] for s in syms]


def sym_of(a):
    i = bisect.bisect_right(addrs, a) - 1
    if i < 0:
        return None
    base, name = syms[i]
    return name, a - base


# text 链接期范围（只取 0x810xxxxx 段的代码符号）
code = [a for a, nm in syms if 0x81000000 <= a < 0x81200000]
text_lo, text_hi = min(code), max(code)
rt_lo, rt_hi = text_lo + slide, text_hi + slide
print(f'slide=0x{slide:x}  link text=[0x{text_lo:x},0x{text_hi:x}]  '
      f'runtime text=[0x{rt_lo:x},0x{rt_hi:x}]')
print(f'PT_LOAD={len(loads)}  threads={len(threads)}')

if want_tid is not None:
    threads = [t for t in threads if t['tid'] == want_tid]
    if not threads:
        sys.exit(f'no thread tid=0x{want_tid:x}')


def seg_of(va):
    for l in loads:
        if l['vaddr'] <= va < l['vaddr'] + l['filesz']:
            return l
    return None


def dump(t):
    r = t['r']
    sp, fp, lr, pc = r[13], r[11], r[14], r[15]
    thumb = 'Thumb' if (t['cpsr'] & 0x20) else 'ARM  '
    print(f"\n===== tid=0x{t['tid']:x} {thumb} sp=0x{sp:x} fp=0x{fp:x} "
          f"lr=0x{lr:x} pc=0x{pc:x} =====")
    for tag, a in (('pc', pc), ('lr', lr), ('fp', fp), ('r7', r[7]),
                   ('r5', r[5]), ('r4', r[4])):
        if a == 0:
            continue
        s = sym_of(a - slide) if rt_lo <= a < rt_hi else None
        print(f'  {tag}=0x{a:08x}  ' +
              (f'{s[0]}+0x{s[1]:x}' if s else '(非代码/无符号)'))
    # 帧链（ARM AAPCS: [fp-4]=保存的 lr, [fp] = 上一帧 fp）
    print('  --- fp 链 ---')
    cur, seen = fp, set()
    for _ in range(40):
        if cur == 0 or cur in seen or cur % 4:
            break
        seen.add(cur)
        seg = seg_of(cur - 4)
        if not seg:
            print(f'    fp=0x{cur:x} 越界/未映射')
            break
        o = seg['off'] + (cur - seg['vaddr'])
        prev, ret = struct.unpack_from('<II', data, o - 4)
        s = sym_of(ret - slide) if rt_lo <= ret < rt_hi else None
        print(f'    fp=0x{cur:08x} ret=0x{ret:08x} ' +
              (f'{s[0]}+0x{s[1]:x}' if s else '(非代码)'))
        cur = prev
    # 裸扫栈：把栈上所有像返回地址的词挖出来
    print('  --- 栈扫（sp..sp+0x4000 里的代码指针）---')
    seg = seg_of(sp)
    if not seg:
        print('    sp 未映射')
        return
    top = min(seg['vaddr'] + seg['filesz'], sp + 0x4000)
    curo = seg['off'] + (sp - seg['vaddr'])
    prev_name = None
    for va in range(sp & ~3, top, 4):
        v = struct.unpack_from('<I', data, curo + (va - sp))[0]
        if rt_lo <= v < rt_hi:
            s = sym_of(v - slide)
            if s:
                txt = f'  [0x{va:08x}] 0x{v:08x}  {s[0]}+0x{s[1]:x}'
                if txt[21:] == prev_name:
                    prev_name = txt[21:]
                    continue
                prev_name = txt[21:]
                print(txt)


for t in threads:
    dump(t)
