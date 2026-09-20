#!/usr/bin/env python3
# 解析 log18 coredump: ELF phdr + THREAD_REG_INFO note 线程寄存器
import struct, sys

CORE = "/tmp/core18/core"

data = open(CORE, "rb").read()
print(f"core size = {len(data)} (0x{len(data):x})")

# ---- ELF header (32-bit LE) ----
assert data[:4] == b"\x7fELF", "not ELF"
e_phoff, = struct.unpack_from("<I", data, 0x1C)
e_phentsize, e_phnum = struct.unpack_from("<HH", data, 0x2A)
print(f"phoff=0x{e_phoff:x} phentsize={e_phentsize} phnum={e_phnum}")

loads = []
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
        if v <= va < v + ms:
            return o + (va - v)
    return None

def read_mem(va, n):
    o = vaddr_to_off(va)
    if o is None or o + n > len(data):
        return None
    return data[o:o+n]

# ---- parse notes ----
print("\n---- notes ----")
notes = []
off = note_off
end = note_off + note_sz
while off + 12 <= end:
    namesz, descsz, ntype = struct.unpack_from("<3I", data, off)
    off += 12
    name = data[off:off+namesz].rstrip(b"\0").decode(errors="replace")
    off += (namesz + 3) & ~3
    desc = data[off:off+descsz]
    off += (descsz + 3) & ~3
    notes.append((name, ntype, desc, descsz))
    print(f"  note name={name!r} type=0x{ntype:x} descsz=0x{descsz:x}")

# ---- THREAD_REG_INFO: iterate desc as array of entries ----
# entry layout: tid(4B) + r0..r15(64B) = 68B stride (per log17 finding)
REG_NAMES = ["r0","r1","r2","r3","r4","r5","r6","r7","r8","r9","sl","fp","sp","lr","pc","cpsr"]

def dump_thread_regs(desc, label):
    n = len(desc) // 68
    print(f"  {label}: {len(desc)} bytes -> ~{n} entries (68B stride)")
    for i in range(n):
        base = i * 68
        tid, = struct.unpack_from("<I", desc, base)
        regs = struct.unpack_from("<16I", desc, base + 4)
        if tid == 0 or all(r == 0 for r in regs):
            continue
        print(f"    thread tid={tid} (0x{tid:08x})")
        for j, rn in enumerate(REG_NAMES):
            extra = ""
            if rn == "pc":
                extra = f"  (pc & 1 = thumb={regs[j]&1})"
            print(f"      {rn:5}= 0x{regs[j]:08x}{extra}")

for name, ntype, desc, descsz in notes:
    if "reg" in name.lower() or ntype in (0x200,) or descsz >= 68:
        print(f"\n---- candidate reg note: name={name!r} type=0x{ntype:x} ----")
        dump_thread_regs(desc, name)

# ---- try every note with plausible reg layout ----
print("\n---- exhaustive: any desc with stride-68 reg pattern ----")
for name, ntype, desc, descsz in notes:
    if descsz % 68 == 0 and descsz >= 68:
        tid, = struct.unpack_from("<I", desc, 0)
        r13, r14, r15 = struct.unpack_from("<3I", desc, 4 + 13*4)
        print(f"  {name!r} type=0x{ntype:x}: first tid=0x{tid:08x} sp=0x{r13:08x} lr=0x{r14:08x} pc=0x{r15:08x}")
