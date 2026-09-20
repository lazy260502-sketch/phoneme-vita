#!/usr/bin/env python3
# 解析 Vita coredump THREAD_REG_INFO note（log18: ver=17 count=5 esz=0x178）
# 用法: parse_core18b.py <core> <elf> [note_off_hex] [slide_hex]
#   slide: 运行时地址 − slide = 静态地址（log19 实测 0x15000；不传则不减）
#   注意 ELF 版本必须与 coredump 现场一致！log19 是 v01.92 现场，而
#   build/midp_vita 会被 v01.93 重建覆盖——用错 ELF 会静默给出错误符号
#   （build 里旧版可从 vita-port/*.bak 或 out/vpk 里的 velf 还原）
#   log19 教训 1: 路径不得硬编码（曾因 argv 被忽略而解析出旧 log18 寄存器）
#   log19 教训 2: CPSR 签名 0x800f0010 不通用（log19 VM 线程 cpsr=0x60070010），
#     改用寄存器窗口偏移法：note entry 内固定布局 r0@+0x08 起，
#     sp=+0x38 lr=+0x3c pc=+0x40 cpsr=+0x44（log19 thread[0] 实测）
import struct, subprocess, sys

if len(sys.argv) < 3:
    sys.exit(f"usage: {sys.argv[0]} <core> <elf> [note_off_hex]\n"
             f"note_off_hex 默认自动扫描（原来是 0x1e74，log19 同为 0x1e74）")
CORE = sys.argv[1]
ELF  = sys.argv[2]

data = open(CORE, "rb").read()
# THREAD_REG_INFO note：默认 log18/19 实测偏移；也可用第三个参数指定
NOTE_OFF = int(sys.argv[3], 16) if len(sys.argv) > 3 else 0x1e74
SLIDE    = int(sys.argv[4], 16) if len(sys.argv) > 4 else 0
namesz, descsz, ntype = struct.unpack_from("<3I", data, NOTE_OFF)
name = data[NOTE_OFF+12:NOTE_OFF+12+namesz].rstrip(b"\0").decode()
desc_off = NOTE_OFF + 12 + ((namesz + 3) & ~3)
print(f"note {name!r} type=0x{ntype:x} descsz=0x{descsz:x} desc@0x{desc_off:x}")

ver, count, esz = struct.unpack_from("<3I", data, desc_off)
print(f"ver={ver} count={count} esz=0x{esz:x}")

# thread names from THREAD_INFO order (log17/THREAD_INFO): 
NAMES = ["J2ME00001[VM]", "SceCommonDialogWorker", "j2me_tone", "j2me_watchdog", "cldc_ticker"]

# 符号化：用 nm 符号表 + 二分（addr2line 在无调试信息的 velf 上只回 ??，
# 会静默丢符号——log19 教训之一；且取"最近低地址符号+偏移"与取证流程一致）
def _load_syms():
    syms = []
    try:
        nm = subprocess.run(["arm-vita-eabi-nm", "-n", ELF],
                            capture_output=True, text=True, timeout=30).stdout
        for line in nm.splitlines():
            parts = line.split(" ", 2)
            if len(parts) == 3 and len(parts[0]) == 8:
                try:
                    syms.append((int(parts[0], 16), parts[2]))
                except ValueError:
                    pass
    except Exception:
        pass
    return syms

SYMS = _load_syms()

def sym(va):
    sa = va - SLIDE  # 运行时地址 → 静态地址
    if not (0x81000000 <= sa < 0x83000000) or not SYMS:
        return f"0x{va:08x}"
    # 二分找 <= sa 的最近符号
    lo, hi = 0, len(SYMS)
    while lo < hi:
        mid = (lo + hi) // 2
        if SYMS[mid][0] <= sa:
            lo = mid + 1
        else:
            hi = mid
    if lo == 0:
        return f"0x{va:08x}"
    saddr, sname = SYMS[lo - 1]
    nxt = SYMS[lo][0] if lo < len(SYMS) else saddr + 0x10000
    if saddr <= sa < nxt:  # 落在本函数体 [符号, 下一符号) 内
        off = sa - saddr
        return f"0x{va:08x} {sname}+0x{off:x}"
    # 恰在下一符号前边界（对齐 padding）：按下一符号 −offset 报告
    nname = SYMS[lo][1]
    return f"0x{va:08x} <{nname}-0x{nxt-sa:x}>"

def find_regs(ent, esz):
    """定位 r0..r15/cpsr 寄存器窗口。
    优先窗口偏移法（+0x08 起 17 字：r0..r12,sp,lr,pc,cpsr），
    校验失败再退回旧 CPSR 签名扫描。"""
    # 方法 1: 固定窗口（log19 thread[0] 实测: sp=+0x38 lr=+0x3c pc=+0x40 cpsr=+0x44）
    r0off = 0x08
    cand = struct.unpack_from("<17I", ent, r0off)
    sp, lr, pc, cpsr = cand[13], cand[14], cand[15], cand[16]
    if (0x81000000 <= lr < 0x83000000 or lr == 0) and \
       (0x81000000 <= pc < 0x83000000 or pc == 0) and pc != 0:
        return cand[:16], cpsr
    # 方法 2（回退）: 旧签名扫描，签名放宽为低位模式 0x10（ARM 用户态）
    for off in range(0, esz - 4, 4):
        w, = struct.unpack_from("<I", ent, off)
        if (w & 0xffff00ff) == 0x800f0010 or (w & 0xf00000ff) == 0x60000010:
            r0off = off - 15 * 4
            if r0off < 0: continue
            cand = struct.unpack_from("<16I", ent, r0off)
            sp, lr, pc, cpsr = cand[12], cand[13], cand[14], cand[15]
            if (0x81000000 <= lr < 0x83000000 or lr == 0) and \
               (0x81000000 <= pc < 0x83000000 or pc == 0):
                return cand, cpsr
    return None, None

for i in range(count):
    base = desc_off + 12 + i * esz
    ent = data[base:base+esz]
    tid_hi, tid_lo = struct.unpack_from("<2I", ent, 0)
    tid = tid_hi  # first word looked like 0x40010003 style uid
    nm = NAMES[i] if i < len(NAMES) else f"t{i}"
    regs, cpsr_sig = find_regs(ent, esz)
    print(f"\n=== thread[{i}] uid=0x{tid_hi:08x}/{tid_lo:08x} {nm} ===")
    if regs is None:
        # dump nonzero words for manual inspection
        words = struct.unpack_from(f"<{esz//4}I", ent, 0)
        print("  no reg window; nonzero words:")
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
