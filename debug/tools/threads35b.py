#!/usr/bin/env python3
# log35 core: THREAD_REG_INFO 正确布局
# ph[7]@0x1e74: name(0x10)+pad, desc@0x1e8c: [count=5][size=0x178] + 5 blocks
import struct, bisect, sys

CORE = "/tmp/log35.core"
NM   = "/tmp/velf332_nm.txt"
data = open(CORE, "rb").read()
SLIDE = int(sys.argv[1], 0) if len(sys.argv) > 1 else 0

syms = []
for line in open(NM):
    p = line.split(None, 2)
    if len(p) == 3:
        a = int(p[0], 16)
        if a > 0x1000000000:
            a = a - 0x1020000000 + 0x81000000 + SLIDE
        syms.append((a, p[2].strip()))
syms.sort()
sym_addrs = [s[0] for s in syms]

def sym(addr):
    if addr == 0: return ""
    i = bisect.bisect_right(sym_addrs, addr) - 1
    if i < 0 or addr - syms[i][0] > 0x10000: return ""
    a, n = syms[i]
    return f"{n}+0x{addr-a:x}"

BASE = 0x1e8c + 8
N, STRIDE = 5, 0x178
names = {0x40010003:"vm(main)", 0x400100c3:"?", 0x4001015d:"tone",
         0x40010161:"wd", 0x400101cb:"cldc_ticker"}

for i in range(N):
    b = BASE + i * STRIDE
    tid, = struct.unpack_from("<I", data, b)
    nm = names.get(tid, "?")
    print(f"\n[{i}] tid=0x{tid:08x} {nm}")
    for off in range(0, 0x178, 4):
        v, = struct.unpack_from("<I", data, b + off)
        tag = ""
        if 0x81000000 <= v < 0x82000000:
            tag = sym(v)
            if tag: tag = f"  <- {tag}"
        if v != 0 or tag:
            print(f"  +0x{off:03x}: 0x{v:08x}{tag}")
