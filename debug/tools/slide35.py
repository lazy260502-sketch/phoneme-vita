#!/usr/bin/env python3
# log35 slide 定标: site[0]=0x810fad0c site[1]=0x810f8c7c
# rt -> nm: nm = rt + 0x1020000000 - 0x81000000 - slide
# 在 T/t 源码域 (0x1020xxxxxx) 找覆盖, 过滤 stub 域
import bisect

syms = []
for line in open('/tmp/velf332_nm.txt'):
    p = line.split(None, 2)
    if len(p) == 3:
        syms.append((int(p[0], 16), p[2].strip()))
syms.sort()
addrs = [s[0] for s in syms]

for slide in range(0, 0x60000, 0x800):
    hits = 0
    for probe_rt in (0x810fad0c, 0x810f8c7c):
        nm = probe_rt + 0x1020000000 - 0x81000000 - slide
        i = bisect.bisect_right(addrs, nm) - 1
        if i >= 0 and 0x1020000000 < syms[i][0] < 0x1021000000 \
           and nm - syms[i][0] < 0x8000:
            hits += 1
    if hits == 2:
        for probe_rt in (0x810fad0c, 0x810f8c7c):
            nm = probe_rt + 0x1020000000 - 0x81000000 - slide
            i = bisect.bisect_right(addrs, nm) - 1
            print(f'slide=0x{slide:x}: 0x{probe_rt:08x} -> '
                  f'{syms[i][1]}+0x{nm-syms[i][0]:x}')
