#!/usr/bin/env python3
# 解析 log35 coredump: THREAD_REG_INFO 全部线程块 + 符号化死点
# 复用 log34 手工流程: phdr 扫描 -> NOTE(4) 找 THREAD_REG_INFO -> 0x178 块步进
import struct, bisect, sys

CORE = sys.argv[1] if len(sys.argv) > 1 else \
    "/home/zyb/vitasdk/samples/j2me/debug/logs/log35/psp2core-1790353011-0x0003a42231-eboot.bin.psp2dmp"
NM   = "/tmp/velf332_nm.txt"

data = open(CORE, "rb").read()
print(f"core size = {len(data)} (0x{len(data):x})")

e_phoff, = struct.unpack_from("<I", data, 0x1C)
e_phentsize, e_phnum = struct.unpack_from("<HH", data, 0x2A)
print(f"phoff=0x{e_phoff:x} phentsize={e_phentsize} phnum={e_phnum}")

loads = []
note_off = note_sz = 0
for i in range(e_phnum):
    off = e_phoff + i * e_phentsize
    p_type, p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_flags, p_align = \
        struct.unpack_from("<8I", data, off)
    tag = {1: "LOAD", 4: "NOTE"}.get(p_type, str(p_type))
    print(f"  ph[{i}] {tag:5} off=0x{p_offset:08x} vaddr=0x{p_vaddr:08x} "
          f"filesz=0x{p_filesz:x} memsz=0x{p_memsz:x} flags={p_flags:x}")
    if p_type == 1:
        loads.append((p_vaddr, p_offset, p_filesz, p_memsz))
    if p_type == 4:
        note_off, note_sz = p_offset, p_filesz

def vaddr_to_off(va):
    for v, o, fs, ms in loads:
        if v <= va < v + fs:
            return o + (va - v)
    return None

# ---- 符号表 ----
syms = []
for line in open(NM):
    parts = line.split(None, 2)
    if len(parts) == 3:
        syms.append((int(parts[0], 16), parts[2].strip()))
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

# ---- NOTE 内找 THREAD_REG_INFO ----
pos = note_off
end = note_off + note_sz
blocks = []
while pos + 8 <= end:
    namesz, descsz = struct.unpack_from("<II", data, pos)
    name = data[pos+8:pos+8+namesz].rstrip(b"\0").decode(errors="replace")
    desc = data[pos+8+namesz:pos+8+namesz+descsz]
    if name == "THREAD_REG_INFO":
        blocks.append(desc)
        print(f"THREAD_REG_INFO desc @0x{pos:x} size=0x{descsz:x}")
    pos += 8 + namesz + ((namesz % 4) and (4 - namesz % 4) or 0) + descsz
    pos = (pos + 3) & ~3

print(f"\n== {len(blocks)} THREAD_REG_INFO blocks ==")

# ---- 每块: 先 4 字节对齐扫描 tid, 再按 0x178 步进 ----
# log34 经验: ph[7]@0x1e54, 块起 0x1e7c, stride 0x178, 每块开头 tid
for bi, desc in enumerate(blocks):
    print(f"\n-- block {bi}: len=0x{len(desc):x} --")
    base = 0
    n = 0
    while base + 0x178 <= len(desc):
        tid, = struct.unpack_from("<I", desc, base)
        if tid == 0 or (tid & 0xFFFF0000) not in (0x40010000,):
            # 尝试 +4 对齐窗口
            tid2, = struct.unpack_from("<I", desc, base + 4) if base + 8 <= len(desc) else (0,)
            if tid2 & 0xFFFF0000 == 0x40010000:
                base += 4
                tid = tid2
            else:
                break
        pc, = struct.unpack_from("<I", desc, base + 0x34)
        lr, = struct.unpack_from("<I", desc, base + 0x40)
        regs = struct.unpack_from("<16I", desc, base + 0x48)
        r0, r1, r2, r3 = regs[:4]
        print(f"  [{n}] tid=0x{tid:08x} pc=0x{pc:08x} {sym(pc)}")
        print(f"      lr=0x{lr:08x} {sym(lr)}  r0=0x{r0:08x} r1=0x{r1:08x} "
              f"r2=0x{r2:08x} r3=0x{r3:08x}")
        n += 1
        base += 0x178
