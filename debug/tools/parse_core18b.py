#!/usr/bin/env python3
# 解析 log18 coredump THREAD_REG_INFO: ver=17 count=5 esz=0x178
import struct, subprocess

CORE = "/tmp/core18/core"
ELF  = "/home/zyb/vitasdk/samples/j2me/vita-port/midp_vita"

data = open(CORE, "rb").read()
# THREAD_REG_INFO note at file offset 0x1e74, desc len 0x77c
# note header: namesz(4) descsz(4) ntype(4) name("...
NOTE_OFF = 0x1e74
namesz, descsz, ntype = struct.unpack_from("<3I", data, NOTE_OFF)
name = data[NOTE_OFF+12:NOTE_OFF+12+namesz].rstrip(b"\0").decode()
desc_off = NOTE_OFF + 12 + ((namesz + 3) & ~3)
print(f"note {name!r} type=0x{ntype:x} descsz=0x{descsz:x} desc@0x{desc_off:x}")

ver, count, esz = struct.unpack_from("<3I", data, desc_off)
print(f"ver={ver} count={count} esz=0x{esz:x}")

# thread names from THREAD_INFO order (log17/THREAD_INFO): 
NAMES = ["J2ME00001[VM]", "SceCommonDialogWorker", "j2me_tone", "j2me_watchdog", "cldc_ticker"]

# find CPSR signature 0x800f0010 / pattern: within each entry locate
# a run: sp(0x8..), lr, pc, cpsr(0x800f0010-like)
def sym(va):
    if not (0x81000000 <= va < 0x83000000):
        return f"0x{va:08x}"
    try:
        out = subprocess.run(["arm-vita-eabi-addr2line", "-f", "-C", "-e", ELF, f"0x{va:08x}"],
                             capture_output=True, text=True, timeout=10).stdout.strip().split("\n")
        fn = out[0] if out else "?"
        loc = out[1] if len(out) > 1 else "?"
        if fn in ("??", "?") or "??" in loc:
            return f"0x{va:08x}"
        return f"0x{va:08x} {fn} ({loc})"
    except Exception:
        return f"0x{va:08x}"

for i in range(count):
    base = desc_off + 12 + i * esz
    ent = data[base:base+esz]
    tid_hi, tid_lo = struct.unpack_from("<2I", ent, 0)
    tid = tid_hi  # first word looked like 0x40010003 style uid
    nm = NAMES[i] if i < len(NAMES) else f"t{i}"
    # locate CPSR-like word (0x?00f0010 pattern: mode bits 0x10, high 0x800f0000)
    regs = None
    for off in range(0, esz - 4, 4):
        w, = struct.unpack_from("<I", ent, off)
        if (w & 0xffff00ff) == 0x800f0010 or (w & 0xfffffff0) == 0x60000010 and False:
            # candidate cpsr at off; assume r0..r15 precede: r0 at off-15*4
            r0off = off - 15 * 4
            if r0off < 0: continue
            cand = struct.unpack_from("<16I", ent, r0off)
            sp, lr, pc, cpsr = cand[12], cand[13], cand[14], cand[15]
            # sanity: pc/lr in code domain
            if (0x81000000 <= lr < 0x83000000 or lr == 0) and (0x81000000 <= pc < 0x83000000 or pc == 0):
                regs = cand
                break
    print(f"\n=== thread[{i}] uid=0x{tid_hi:08x}/{tid_lo:08x} {nm} ===")
    if regs is None:
        # dump nonzero words for manual inspection
        words = struct.unpack_from(f"<{esz//4}I", ent, 0)
        print("  no CPSR sig; nonzero words:")
        for j, w in enumerate(words):
            if w != 0x7f80dead and w != 0x7ff8dead and w != 0:
                print(f"    +0x{j*4:03x}: 0x{w:08x}")
        continue
    rn = ["r0","r1","r2","r3","r4","r5","r6","r7","r8","r9","sl","fp","sp","lr","pc","cpsr"]
    for j, n in enumerate(rn):
        mark = ""
        if n == "pc": mark = "  << PC " + sym(regs[j])
        if n == "lr": mark = "  << LR " + sym(regs[j])
        print(f"  {n:5}= 0x{regs[j]:08x}{mark}")
