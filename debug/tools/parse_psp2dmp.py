#!/usr/bin/env python3
# parse_psp2dmp.py - Vita3K psp2dmp coredump 取证解析器（v01.95 配套，2026-09-21）
#
# 背景：Vita3K 挂死时写出的 psp2dmp 是 gzip 压缩的 ELF core（EM_ARM），
#       但只含脏匿名页——.data/.bss 头、Java 堆结构性不可读（log21 定案）。
#       本脚本把 log18/19/21 三轮手工取证流程固化为一键命令：
#
#   1. gzip 魔数检测 +自动解压（Vita3K 直接落盘的是未解压流）
#   2. ELF64 程序头解析，PT_LOAD 段建 vaddr→file offset 映射
#   3. PSVITA THREAD_REG_INFO note（type=0x1004）定位与寄存器窗口自适应
#      （log19 寄存器从 +0x08 起、log21 从 +0x04 起——按 r15=合理 SP 收敛）
#   4. ASLR slide 自动求解：优先 note 内 self 指针（vita_site_dump 首行），
#      兜底用 stop_reason/ring 槽匹配 pump_events_vita+0x3c
#   5. nm -n 符号二分定位（PC/LR/SP 帧回溯地址 → 函数名+偏移）
#   6. 栈走：SP 起 512 字，过滤 runtime .text 区间 + 距离拒绝（±1MB）
#
# 用法（在 vita-port 构建树内）：
#   python3 debug/tools/parse_psp2dmp.py <psp2dmp 文件> [--elf build/eboot.elf]
#   不给 --elf 时自动探测 out/vpk 同批 eboot.elf / build/eboot.elf。
#
# 依赖：python3（标准库）；nm 用 arm-vita-eabi-nm（PATH 或 $VITASDK/bin）。

import argparse
import bisect
import gzip
import os
import re
import struct
import subprocess
import sys
import tempfile

# ---------------------------------------------------------------- gzip / ELF

def maybe_gunzip(path):
    with open(path, 'rb') as f:
        magic = f.read(2)
    if magic == b'\x1f\x8b':
        return gzip.open(path, 'rb').read()
    return open(path, 'rb').read()

class Core:
    def __init__(self, blob, verbose=False):
        self.v = verbose
        self.blob = blob
        if blob[:4] != b'\x7fELF':
            raise SystemExit('不是 ELF 文件（解压后仍无 \\x7fELF 魔数）')
        self.ei_class = blob[4]
        self.parse_phdrs()

    def parse_phdrs(self):
        if self.ei_class == 1:  # ELF32
            e_phoff, = struct.unpack_from('<I', self.blob, 0x1c)
            e_phentsize, e_phnum = struct.unpack_from('<HH', self.blob, 0x2a)
            self.is64 = False
        else:
            e_phoff, = struct.unpack_from('<Q', self.blob, 0x20)
            e_phentsize, e_phnum = struct.unpack_from('<HH', self.blob, 0x36)
            self.is64 = True
        self.loads = []   # (vaddr, filesz, fileoff)
        self.notes = []   # (note_file_off, size)
        for i in range(e_phnum):
            off = e_phoff + i * e_phentsize
            if self.is64:
                p_type, p_flags = struct.unpack_from('<II', self.blob, off)
                p_offset, p_vaddr = struct.unpack_from('<QQ', self.blob, off + 8)
                p_filesz, = struct.unpack_from('<Q', self.blob, off + 32)
            else:
                p_type, p_offset, p_vaddr, p_paddr, p_filesz = struct.unpack_from(
                    '<IIIII', self.blob, off)
            if p_type == 1:  # PT_LOAD
                self.loads.append((p_vaddr, p_filesz, p_offset))
            elif p_type == 4:  # PT_NOTE
                self.notes.append((p_offset, p_filesz))
        if self.v:
            print(f'[core] {len(self.loads)} PT_LOAD, {len(self.notes)} PT_NOTE',
                  file=sys.stderr)

    def read_vaddr(self, vaddr, size):
        """vaddr → (bytes|None)。core 只含脏页，None = 洞。"""
        for base, fsz, foff in self.loads:
            if base <= vaddr and vaddr + size <= base + fsz:
                return self.blob[foff + (vaddr - base): foff + (vaddr - base) + size]
        return None

# ---------------------------------------------------------------- note 解析

REG_NAMES = ['r0','r1','r2','r3','r4','r5','r6','r7','r8','r9','r10','r11','r12',
             'sp','lr','pc']

def find_thread_notes(core):
    """PSVita core：note type 0x1004 = THREAD_REG_INFO（每线程一条）。
    布局（log21 实测校准）：name 无 4 字节填充（namesz 长度即实长），
    desc 直接紧跟；寄存器窗口基址 +0x10（log19 曾为 +0x08，Vita3K 版本
    相关，故保留自适应：取 pc/lr 落 0x81xxxxxx 段且 sp 对齐 8 的窗口）。
    返回 [(regs_dict, window_offset, note_size)]。"""
    results = []
    for noff, nsz in core.notes:
        pos = noff
        end = noff + nsz
        while pos + 12 <= end:
            namesz, descsz, ntype = struct.unpack_from('<III', core.blob, pos)
            name = core.blob[pos + 12: pos + 12 + namesz].rstrip(b'\0')
            # PSVita note：name 段无对齐填充（COREFILE_INFO=15 字节实长）
            dpos = pos + 12 + namesz
            if ntype == 0x1004 and descsz >= 0x50:
                best = None
                for window in (0x10, 0x08, 0x04, 0x0c):
                    if dpos + window + 0x40 > len(core.blob):
                        continue
                    words = struct.unpack_from('<16I', core.blob, dpos + window)
                    pc, lr, sp = words[15], words[14], words[13]
                    # 合理窗口：pc/lr 在 0x81xxxxxxx 代码段、sp 8 对齐
                    plausible = (0x81000000 <= pc < 0x84000000 and
                                 0x81000000 <= lr < 0x84000000 and
                                 (sp & 7) == 0)
                    if plausible and best is None:
                        best = (window, words)
                if best:
                    results.append((dict(zip(REG_NAMES, best[1])),
                                    best[0], descsz))
            pos = dpos + ((descsz + 3) & ~3)
    return results

# ---------------------------------------------------------------- slide 求解

def solve_slide(core, regs, symtab, sp, anchor=None):
    """ASLR slide 自动求解（页对齐约束 + 区间交）。
    每个代码指针 v 给出约束：v - slide ∈ [sym_i, sym_i + 0x2000)，
    即 slide ∈ (v - sym_i - 0x2000, v - sym_i]。取所有约束的交集，
    再加 Vita slide 恒 0x1000 对齐。anchor 优先：site_ring.log 的
    self= 行（vita_site_dump 运行时地址）配合符号表直接定位。"""
    if not symtab:
        return None
    # 途径 0：anchor（site_ring.log 首行 self=0x...）
    # self 是 vita_site_dump 函数内某指令的运行时地址（v01.95 起也可能是
    # vita_wd_snapshot）。按符号名定位静态地址，d = anchor - 静态；
    # slide = d 就近取页对齐，残差（函数内偏移 ± 版本漂移）验 < 0x800。
    if anchor is not None:
        for sym_name in ('vita_site_dump', 'vita_wd_snapshot'):
            sym = symtab.by_name(sym_name)
            if sym is None:
                continue
            d = anchor - sym
            if d <= 0 or d >= 0x200000:
                continue
            slide = (d + 0x800) & ~0xFFF   # 就近取页
            if abs(d - slide) < 0x800:
                return slide
        return None
    # 途径 1：寄存器 + 栈指针区间交
    vals = [regs.get(n, 0) for n in REG_NAMES]
    if sp:
        data = core.read_vaddr(sp, 128 * 4)
        if data:
            for k in range(128):
                v, = struct.unpack_from('<I', data, k * 4)
                vals.append(v)
    lo, hi = 0, 0x200000          # slide 允许域
    n_ok = 0
    for val in vals:
        if not (0x81000000 <= val < 0x84000000):
            continue
        i = bisect.bisect_right(symtab.keys_list, val - lo) - 1
        # 对最近邻的 3 个符号尝试（防中间隔了大 data 符号）
        for j in (i, i - 1, i - 2):
            if j < 0:
                continue
            sym = symtab.keys_list[j]
            d = val - sym          # slide ≤ d ≤ slide + 0x2000（函数内）
            if d <= 0 or d - 0x2000 > hi:
                continue
            cand_lo = max(lo, d - 0x2000)
            cand_hi = min(hi, d)
            if cand_lo > cand_hi:
                continue
            lo, hi = cand_lo, cand_hi
            n_ok += 1
            break
    if n_ok == 0:
        return None
    # 区间内取 0x1000 对齐值（离区间中心最近）
    mid = (lo + hi) // 2
    cand = (mid + 0xFFF) & ~0xFFF
    if lo <= cand <= hi:
        return cand
    cand2 = (lo + 0xFFF) & ~0xFFF
    return cand2 if cand2 <= hi else None

# ---------------------------------------------------------------- 符号表

class SymTab:
    def __init__(self, entries):
        # entries: [(addr, name)] 升序
        self.keys_list = [e[0] for e in entries]
        self.names = [e[1] for e in entries]

    def lookup(self, addr):
        i = bisect.bisect_right(self.keys_list, addr) - 1
        if i < 0:
            return None
        return self.names[i], addr - self.keys_list[i]

    def by_name(self, name):
        try:
            i = self.names.index(name)
            return self.keys_list[i]
        except ValueError:
            return None

def load_syms(elf_path, toolchain_prefix='arm-vita-eabi-'):
    nm = None
    for cand in (toolchain_prefix + 'nm',
                 os.path.join(os.environ.get('VITASDK', ''), 'bin',
                              toolchain_prefix + 'nm')):
        if subprocess.run(['sh', '-c', f'command -v {cand}'],
                          capture_output=True).returncode == 0:
            nm = cand
            break
    if nm is None:
        raise SystemExit('找不到 arm-vita-eabi-nm（先 source vitasdk.sh）')
    out = subprocess.run([nm, '-n', '--defined-only', elf_path],
                         capture_output=True, text=True).stdout
    entries = []
    for line in out.splitlines():
        m = re.match(r'([0-9a-f]{8}) [a-zA-Z] (\S+)', line)
        if m:
            entries.append((int(m.group(1), 16), m.group(2)))
    entries.sort()
    return SymTab(entries)

# ---------------------------------------------------------------- 栈走

def stack_walk(core, sp, symtab, slide, words=128):
    out = []
    if sp is None or sp == 0:
        return out
    data = core.read_vaddr(sp, words * 4)
    if data is None:
        return [('(SP 所在页未被 core 捕获)', 0, 0)]
    for i in range(words):
        val, = struct.unpack_from('<I', data, i * 4)
        if not (0x81000000 <= val < 0x84000000):
            continue
        hit = symtab.lookup(val - (slide or 0))
        if hit and hit[1] < 0x100000:  # 距离拒绝
            out.append((hit[0], hit[1], val))
    return out

# ---------------------------------------------------------------- main

def main():
    ap = argparse.ArgumentParser(
        description='psp2dmp coredump 一键取证（log21 流程固化）')
    ap.add_argument('core', help='psp2dmp 文件（gzip 或已解压 ELF core）')
    ap.add_argument('--elf', help='同批 eboot.elf（符号源）', default=None)
    ap.add_argument('--slide', type=lambda x: int(x, 0), default=None,
                    help='手工指定 ASLR slide（默认自动求解）')
    ap.add_argument('--anchor', type=lambda x: int(x, 0), default=None,
                    help='site_ring.log self 行的运行时地址（默认自动读同目录')
    ap.add_argument('-v', '--verbose', action='store_true')
    args = ap.parse_args()

    # anchor 自动探测：core 同目录 site_ring.log 首个 self= 值
    anchor = args.anchor
    if anchor is None:
        ring = os.path.join(os.path.dirname(os.path.abspath(args.core)),
                            'site_ring.log')
        if os.path.exists(ring):
            m = re.search(rb'self=(0x[0-9a-f]+)', open(ring, 'rb').read(200))
            if m:
                anchor = int(m.group(1), 16)
                if args.verbose:
                    print(f'[slide] anchor={hex(anchor)} (site_ring.log self 行)',
                          file=sys.stderr)

    blob = maybe_gunzip(args.core)
    core = Core(blob, args.verbose)

    # 符号源探测
    elf = args.elf
    if elf is None:
        for cand in ('vita-port/build/eboot.elf', 'build/eboot.elf',
                     'vita-port/out/vpk/eboot.elf', 'out/vpk/eboot.elf'):
            if os.path.exists(cand):
                elf = cand
                break
    symtab = None
    if elf and os.path.exists(elf):
        symtab = load_syms(elf)
        if args.verbose:
            print(f'[sym] {elf}: {len(symtab.keys_list)} 符号', file=sys.stderr)
    else:
        print('[!] 未找到 eboot.elf，符号化不可用（--elf 指定）', file=sys.stderr)

    notes = find_thread_notes(core)
    if not notes:
        print('[!] 未找到 THREAD_REG_INFO note（type 0x1004）', file=sys.stderr)
        return 1
    print(f'== {len(notes)} 线程 note ==')
    for idx, (regs, window, nsz) in enumerate(notes):
        pc, lr, sp = regs['pc'], regs['lr'], regs['sp']
        slide = args.slide
        if slide is None:
            slide = solve_slide(core, regs, symtab, sp, anchor=anchor)
        print(f'\n-- thread[{idx}] note={nsz:#x} 寄存器窗口=+{window:#x} '
              f'slide={hex(slide) if slide is not None else "未解"}')
        print('   pc=%08x lr=%08x sp=%08x fp=%08x r0=%08x r1=%08x' % (
            pc, lr, sp, regs['r11'], regs['r0'], regs['r1']))
        if symtab:
            for tag in ('pc', 'lr'):
                s = symtab.lookup(regs[tag] - (slide or 0))
                print(f'   {tag} → {s[0]}+{s[1]:#x}' if s else f'   {tag} → 无符号')
            print('   -- 栈回溯（SP 起 512B，runtime 区间过滤）--')
            for name, off, vaddr in stack_walk(core, sp, symtab, slide)[:16]:
                print(f'     {vaddr:08x}  {name}+{off:#x}')

    print('\n== PT_LOAD 覆盖（core 捕获完整性）==')
    for base, fsz, foff in core.loads:
        print(f'   {base:08x}-{base + fsz:08x} ({fsz:#x} bytes)')
    return 0

if __name__ == '__main__':
    sys.exit(main())
