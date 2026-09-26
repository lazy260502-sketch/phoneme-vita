#!/usr/bin/env python3
# log35 core: THREAD_REG_INFO 手工解析（log34 流程）
# ph[7]@0x1e74: namesz=0x10 descsz=0x760, 名字后 descsz=0x760, 块起 0x1e94
# 块格式: tid(4) ... pc@+0x34 lr@+0x40 regs@+0x48, stride 0x178
import struct, bisect, sys

CORE = "/tmp/log35.core"
NM   = "/tmp/velf332_nm.txt"

data = open(CORE, "rb").read()

syms = []
for line in open(NM):
    p = line.split(None, 2)
    if len(p) == 3:
        syms.append((int(p[0], 16), p[2].strip()))
syms.sort()
sym_addrs = [s[0] for s in syms]

def sym(addr):
    if addr == 0:
        return "(null)"
    i = bisect.bisect_right(sym_addrs, addr) - 1
    if i < 0:
        return f"0x{addr:08x}(?)"
    a, n = syms[i]
    return f"{n}+0x{addr-a:x}"

BASE = 0x1e94      # ph[7] desc 起点（"THREAD_REG_INFO\0" 后）
N = 7              # 0x760 / 0x178 = 7.03 -> 7 块（log34 同为 7）
STRIDE = 0x178

for i in range(N):
    b = BASE + i * STRIDE
    tid, = struct.unpack_from("<I", data, b)
    pc,  = struct.unpack_from("<I", data, b + 0x34)
    lr,  = struct.unpack_from("<I", data, b + 0x40)
    r = struct.unpack_from("<16I", data, b + 0x48)
    sp4, = struct.unpack_from("<I", data, b + 0x10)  # +0x10: log34 是 0xcccccccc 处
    print(f"[{i}] tid=0x{tid:08x}")
    print(f"    pc=0x{pc:08x}  {sym(pc)}")
    print(f"    lr=0x{lr:08x}  {sym(lr)}")
    print(f"    +0x10=0x{sp4:08x}  r0=0x{r[0]:08x} r1=0x{r[1]:08x} "
          f"r2=0x{r[2]:08x} r3=0x{r[3]:08x}")
    print(f"    r4=0x{r[4]:08x} r5=0x{r[5]:08x} r6=0x{r[6]:08x} "
          f"r7=0x{r[7]:08x} r12=0x{r[12]:08x}")
