#!/usr/bin/env python3
"""log41 coredump slide solver - STRONG method.

Instead of scoring "addresses that fall near a symbol" (degenerate, see
slide41.py), we exploit the ONE hard constraint a return address must
satisfy:

    lr - 4   must decode as a BL / BLX / BLX-immediate (ARM or Thumb)
             (i.e. the call site that produced lr)

For the correct slide ALL thread lr values must satisfy this.  A wrong
slide satisfies it with probability ~1/4 per address, so with 5 threads
x (lr) + 3 audit creators = 8 constraints the true slide stands out
uniquely.

Bonus: the same trick applied to `pc` is weaker, but we also report the
symbol each candidate resolves to so the answer is human-checkable.

Usage: slide42.py <core.dmp> <elf>
"""
import gzip, struct, sys, bisect, subprocess, os

core_path, elf_path = sys.argv[1], sys.argv[2]

open_bytes = open(core_path, 'rb').read()
if open_bytes[:2] == b'\x1f\x8b':
    open_bytes = gzip.decompress(open_bytes)
assert open_bytes[:4] == b'\x7fELF', "not an ELF core"

# ---- parse ELF32 program headers ----
(e_phoff,) = struct.unpack_from('<I', open_bytes, 0x1c)
(e_phentsize,) = struct.unpack_from('<H', open_bytes, 0x2a)
(e_phnum,) = struct.unpack_from('<H', open_bytes, 0x2c)
loads = []   # (vaddr, filesz, offset)
notes = []   # (offset, filesz)
for i in range(e_phnum):
    b = e_phoff + i * e_phentsize
    p_type, p_offset, p_vaddr, p_paddr, p_filesz, p_memsz = \
        struct.unpack_from('<IIIIII', open_bytes, b)
    if p_type == 1:
        loads.append((p_vaddr, p_filesz, p_offset))
    elif p_type == 4:
        notes.append((p_offset, p_filesz))
loads.sort()
print("PT_LOAD:", [("0x%08x" % v, "0x%x" % s, "0x%x" % o) for v, s, o in loads])


def mem_read(va, n):
    for v, size, off in loads:
        if v <= va < v + size:
            k = min(n, v + size - va)
            return open_bytes[off + (va - v): off + (va - v) + k]
    return b''


# ---- parse NT_FILE / thread note ----
NOTE_THREAD_REG = 0x1004
threads = []
for off, size in notes:
    p = off
    while p < off + size:
        nsz, dsz, ntype = struct.unpack_from('<III', open_bytes, p)
        name = open_bytes[p + 12:p + 12 + nsz].rstrip(b'\0')
        dstart = (p + 12 + nsz + 3) & ~3
        if ntype == NOTE_THREAD_REG:
            d = open_bytes[dstart:dstart + dsz]
            hdr = struct.unpack_from('<II', d, 0)
            n = hdr[0] // 0x178
            for k in range(n):
                b = 8 + k * 0x178
                regs = list(struct.unpack_from('<16I', d, b + 0x08))
                cpsr = struct.unpack_from('<I', d, b + 0x48)[0]
                tid = struct.unpack_from('<I', d, b + 0x04)[0]
                threads.append((tid, regs, cpsr))
        p = dstart + ((dsz + 3) & ~3)

print("=== threads (tid, r13/sp, r14/lr, r15/pc, cpsr) ===")
for tid, r, cpsr in threads:
    print("  tid=0x%08x sp=0x%08x lr=0x%08x pc=0x%08x cpsr=0x%08x %s"
          % (tid, r[13], r[14], r[15], cpsr, "THUMB" if cpsr & 0x20 else "ARM"))

# ---- nm symbol table ----
nm_out = subprocess.run(['arm-vita-eabi-nm', '-n', elf_path],
                        capture_output=True, text=True).stdout
sym = []   # (addr, name)
for line in nm_out.splitlines():
    f = line.split()
    if len(f) >= 3 and f[2] not in ('U', 'u'):
        try:
            sym.append((int(f[0], 16), f[2]))
        except ValueError:
            pass
sym.sort()
saddr = [a for a, _ in sym]


def sym_of(va):
    i = bisect.bisect_right(saddr, va) - 1
    if i < 0:
        return "?"
    a, n = sym[i]
    return "%s+0x%x" % (n, va - a)


# ---- ELF text window for instruction fetch ----
TEXT_VA, TEXT_FILESZ = loads[0][0], loads[0][1]


def is_call_site(va):
    """True if the instruction at va is BL/BLX (ARM) or BL (Thumb)."""
    w = mem_read(va, 2)
    if len(w) < 2:
        return None
    h = struct.unpack_from('<H', w)[0]
    # Thumb BL / BLX: 1111 0xxx xxxx xxxx  (first halfword)
    if (h & 0xF800) == 0xF000:
        return 'THUMB-BL'
    w = mem_read(va, 4)
    if len(w) < 4:
        return None
    iw = struct.unpack_from('<I', w)[0]
    top = iw >> 24
    if (top & 0x0F) == 0x0B:
        return 'ARM-BL'
    if top in (0xFA, 0xFB):
        return 'ARM-BLX'
    return False


# ---- collect runtime code addresses ----
lr_addrs, pc_addrs = [], []
for tid, r, cpsr in threads:
    th = bool(cpsr & 0x20)
    if not th:
        lr_addrs.append((r[14], "tid0x%x.lr" % tid))
        pc_addrs.append((r[15], "tid0x%x.pc" % tid))
# audit "creator=" values seen in site_ring.log (runtime lr of the creator)
for a, tag in ((0x81043fd9, 'audit.tone.creator'),
               (0x81045af9, 'audit.wd.creator'),
               (0x8108bcdc, 'audit.ticker.creator')):
    lr_addrs.append((a, tag))
# vita_site ring entries + &site_mark/&site_dump (runtime code addrs)
for a, tag in ((0x810bb300, 'ring[0]'), (0x810b90f4, 'ring[2]'),
               (0x8108ba38, 'site_dump_self'), (0x8108ba10, 'site_mark_self')):
    pc_addrs.append((a, tag))

print()
print("=== slide scan: lr-4 must BE a call site ===")
best = []
for S in range(0, 0x300000 + 1, 0x1000):
    hits = 0
    for a, tag in lr_addrs:
        if is_call_site(a - S - 4):
            hits += 1
    best.append((hits, S))
best.sort(reverse=True)
top = best[0][0]
print("best hits=%d/%d, candidates:" % (top, len(lr_addrs)))
for h, S in best[:10]:
    if h < max(1, top - 1):
        break
    print("  slide=0x%-6x hits=%d" % (S, h))

# ---- resolve for the top slide(s) ----
print()
for h, S in best[:3]:
    print("=== slide 0x%x (hits %d/%d) ===" % (S, h, len(lr_addrs)))
    for a, tag in lr_addrs:
        cs = is_call_site(a - S - 4)
        print("  %-22s 0x%08x -> %-40s lr-4=%s" %
              (tag, a, sym_of(a - S), cs or 'not-a-call'))
    for a, tag in pc_addrs:
        print("  %-22s 0x%08x -> %s" % (tag, a, sym_of(a - S)))
    print()
