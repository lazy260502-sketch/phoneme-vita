#!/usr/bin/env python3
"""静态检查：找出"装着绝对地址、却没有重定位覆盖"的字。

背景（log41 取证）：
  真机上 .text 搬移 +0x37000，.data 搬移 +0x120000（两个段独立搬）。
  装载器只会修**重定位表覆盖到**的字。任何漏掉的绝对地址字在真机上就是错的，
  而 Vita3K 固定按链接地址 0x81000000 加载，恰好是对的 -> "模拟器不挂、真机挂"。

用法: python3 abscheck.py <elf>
"""
import re
import struct
import subprocess
import sys

elf = sys.argv[1]
RD = 'arm-vita-eabi-readelf'


def run(*a):
    for cmd in ([RD] + list(a), ['/home/zyb/.local/vitasdk/bin/' + RD] + list(a)):
        try:
            out = subprocess.run(cmd, capture_output=True, text=True).stdout
            if out:
                return out
        except FileNotFoundError:
            continue
    return ''


# ---- 段表 ----
secs = {}
for line in run('-S', '-W', elf).splitlines():
    m = re.match(r'\s*\[\s*\d+\]\s+(\S+)\s+(\S+)\s+([0-9a-f]+)\s+([0-9a-f]+)\s+([0-9a-f]+)', line)
    if m:
        name, typ, addr, off, size = m.group(1), m.group(2), int(m.group(3), 16), \
            int(m.group(4), 16), int(m.group(5), 16)
        secs[name] = (typ, addr, off, size)

# ---- 重定位：收集被覆盖的 vaddr ----
relocs = {}          # 目标段名 -> set(vaddr)
cur = None
for line in run('-r', '-W', elf).splitlines():
    m = re.match(r"Relocation section '(\S+)'", line)
    if m:
        cur = m.group(1)
        continue
    m = re.match(r'([0-9a-f]{8,16})\s+([0-9a-f]{8,16})\s+(\S+)', line.strip())
    if m and cur:
        relocs.setdefault(cur, set()).add(int(m.group(1), 16))

print('重定位覆盖到的段:')
for k, v in sorted(relocs.items()):
    print(f'  {k:20s} {len(v):6d} 条')

LO, HI = 0x81000000, 0x81500000     # 疑似链接期绝对地址范围
for name in ('.data', '.rodata'):
    if name not in secs:
        continue
    typ, vaddr, off, size = secs[name]
    covered = relocs.get('.rel' + name, set())
    data = open(elf, 'rb').read()
    body = data[off:off + size]
    print(f'\n===== {name}  vaddr=0x{vaddr:x} size=0x{size:x} '
          f'重定位覆盖 {len(covered)} 字 =====')
    bad = []
    for i in range(0, len(body) - 3, 4):
        v = struct.unpack_from('<I', body, i)[0]
        if LO <= v < HI:
            va = vaddr + i
            if va not in covered:
                bad.append((va, v))
    print(f'  疑似绝对地址共 {sum(1 for i in range(0, len(body) - 3, 4) if LO <= struct.unpack_from("<I", body, i)[0] < HI)} 字，'
          f'其中**未被重定位** {len(bad)} 字')
    for va, v in bad[:60]:
        print(f'    unrelocated  0x{va:08x} = 0x{v:08x}')
    if len(bad) > 60:
        print(f'    ...(还有 {len(bad) - 60} 条)')
