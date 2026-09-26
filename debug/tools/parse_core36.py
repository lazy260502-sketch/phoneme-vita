#!/usr/bin/env python3
# 解析 log36 coredump: THREAD_REG_INFO 全部线程块 + 符号化死点
# slide=0x1a000 (mark=0x8106e6f8 - vita_site_mark VMA 0x810546f8)
# 符号化: nm 值 = VMA + 0x81000040, 故 runtime -> nm = runtime - slide + 0x81000040
import struct, bisect, sys

CORE = sys.argv[1] if len(sys.argv) > 1 else \
    "/tmp/log36.core"
NM   = "/tmp/velf336_nm.txt"
SLIDE = 0x1a000
NM_BASE = 0x81000040   # nm value = VMA + 0x81000040

data = open(CORE, "rb").read()
print(f"core size = {len(data)} (0x{len(data):x})")

e_phoff, = struct.unpack_from("<I", data, 0x1C)
e_phentsize, e_phnum = struct.unpack_from("<HH", data, 0x2A)
print(f"phoff=0x{e_phoff:x} phentsize={e_phentsize} phnum={e_phnum}")

loads = []
notes = []
for i in range(e_phnum):
    off = e_phoff + i * e_phentsize
    p_type, p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_flags, p_align = \
        struct.unpack_from("<8I", data, off)
    tag = {1: "LOAD", 4: "NOTE"}.get(p_type, str(p_type))
    if p_type == 1:
        loads.append((p_vaddr, p_offset, p_filesz, p_memsz))
    if p_type == 4:
        notes.append((p_offset, p_filesz))

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
        try:
            syms.append((int(parts[0], 16), parts[2].strip()))
        except ValueError:
            pass
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

# ---- 遍历所有 NOTE 找 THREAD_REG_INFO ----
blocks = []
for noff, nsz in notes:
    pos = noff
    end = noff + nsz
    while pos + 8 <= end:
        namesz, descsz = struct.unpack_from("<II", data, pos)
        if namesz > 0x1000 or descsz > 0x100000:
            break
        name = data[pos+8:pos+8+namesz]
        desc = data[pos+8+namesz:pos+8+namesz+descsz]
        if b"THREAD_REG_I" in name:
            blocks.append((noff, desc))
            print(f"THREAD_REG_INFO name={name!r} desc @0x{pos:x} size=0x{descsz:x}")
            break
        pos += 8 + namesz + ((namesz % 4) and (4 - namesz % 4) or 0) + descsz
        pos = (pos + 3) & ~3

print(f"\n== {len(blocks)} THREAD_REG_INFO blocks ==")

# ---- 每块: 按 0x178 步进, 每块开头 tid ----
for bi, (noff, desc) in enumerate(blocks):
    print(f"\n-- block {bi}: len=0x{len(desc):x} --")
    # desc 头 (16 字节): "NFO\0" | u32 type | u32 count | u32 stride
    typ, count, stride = struct.unpack_from("<III", desc, 4)
    print(f"  type=0x{typ:x} count={count} stride=0x{stride:x}")
    base = 0x10
    for n in range(count):
        if base + 0x48 > len(desc):
            break
        tid   = struct.unpack_from("<I", desc, base + 0x00)[0]
        r0_12 = struct.unpack_from("<13I", desc, base + 0x04)
        sp    = struct.unpack_from("<I", desc, base + 0x38)[0]
        lr    = struct.unpack_from("<I", desc, base + 0x3c)[0]
        pc    = struct.unpack_from("<I", desc, base + 0x40)[0]
        cpsr  = struct.unpack_from("<I", desc, base + 0x44)[0]
        def S(va):
            nm = va - SLIDE + NM_BASE
            return f"0x{va:08x} {sym(nm)}"
        print(f"  [{n}] tid=0x{tid:08x}")
        print(f"      r0={S(r0_12[0])}  r1={S(r0_12[1])}  r2={S(r0_12[2])}  r3={S(r0_12[3])}")
        print(f"      r4={S(r0_12[4])}  r5={S(r0_12[5])}  r6={S(r0_12[6])}")
        print(f"      r7={S(r0_12[7])}  r8={S(r0_12[8])}  r9={S(r0_12[9])}")
        print(f"      r10={S(r0_12[10])}  r11={S(r0_12[11])}  r12={S(r0_12[12])}")
        print(f"      sp={S(sp)}")
        print(f"      lr={S(lr)}")
        print(f"      pc={S(pc)}")
        print(f"      cpsr=0x{cpsr:08x}")
        base += stride
