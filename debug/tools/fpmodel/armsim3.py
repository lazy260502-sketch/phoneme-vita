"""armsim extension: byte memory, LDR/STR/LDRB, true s<->d register aliasing, VFP .f32 ops.
Everything unknown still raises Abort."""
import struct
import armsim, armsim2
from armsim import CPU, Abort, f64, bits64

_base_step = armsim2.step


class DView(object):
    def __init__(self, cpu):
        self.c = cpu

    def __getitem__(self, k):
        return f64(self.c.s[2 * k + 1], self.c.s[2 * k])

    def __setitem__(self, k, v):
        hi, lo = bits64(v)
        self.c.s[2 * k], self.c.s[2 * k + 1] = lo, hi


def f32(raw):
    return struct.unpack('<f', struct.pack('<I', raw & 0xffffffff))[0]


def raw32(x):
    try:
        return struct.unpack('<I', struct.pack('<f', x))[0]
    except OverflowError:
        return 0x7f800000 if x > 0 else 0xff800000


class Sim(CPU):
    def __init__(self, words):
        CPU.__init__(self, words)
        self.s = [0] * 64
        self.d = DView(self)
        self.mem = {}

    fN = fZ = fC = fV = 0

    def reset_vfp(self):
        self.s = [0] * 64
        self.fN = self.fZ = self.fC = self.fV = 0

    def rd(self, a, n):
        """Read memory. Anything not written by the program falls back to the loaded image (literal pools and
        constants sit in .text next to the code: vldr/ldr [pc,#x] must see them)."""
        v = 0
        for i in range(n):
            b = (a + i) & 0xffffffff
            if b in self.mem:
                byte = self.mem[b]
            else:
                word = self.w.get(b & ~3)
                byte = ((word >> (8 * (b & 3))) & 0xff) if word is not None else 0
            v |= byte << (8 * i)
        return v

    def wr(self, a, v, n):
        for i in range(n):
            self.mem[(a + i) & 0xffffffff] = (v >> (8 * i)) & 0xff

    def step(self, ins, pc):
        c = ins >> 28
        if c != 15 and not self.cond(c):
            return
        if (ins & 0x0fe00070) == 0x07e00050:                       # ubfx Rd, Rn, #lsb, #width   (ARMv6T2+)
            rd, rn = (ins >> 12) & 15, ins & 15
            lsb, widthm1 = (ins >> 7) & 31, (ins >> 16) & 31
            self.r[rd] = (self.r[rn] >> lsb) & ((1 << (widthm1 + 1)) - 1)
            return
        if (ins & 0x0fe00070) == 0x07a00050:                       # sbfx
            rd, rn = (ins >> 12) & 15, ins & 15
            lsb, widthm1 = (ins >> 7) & 31, (ins >> 16) & 31
            w = widthm1 + 1
            v = (self.r[rn] >> lsb) & ((1 << w) - 1)
            if v >> (w - 1):
                v -= 1 << w
            self.r[rd] = v & 0xffffffff
            return
        if (ins & 0x0ff00000) in (0x03000000, 0x03400000):         # movw / movt  (ARMv6T2+)
            imm16 = ((ins >> 4) & 0xf000) | (ins & 0xfff)
            rd = (ins >> 12) & 15
            if (ins & 0x0ff00000) == 0x03000000:
                self.r[rd] = imm16
            else:
                self.r[rd] = (self.r[rd] & 0xffff) | (imm16 << 16)
            return
        _b = (ins >> 24) & 15
        _w = (ins >> 21) & 1
        _is_vfp_mem = (ins & 0x00000e00) == 0x00000a00 and (ins & 0x00000100) in (0, 0x100)
        if _b == 0xd and _w == 0 and (ins & 0x0e00) == 0x0a00:     # vldr / vstr  (P=1, W=0)
            dbl = (ins & 0x0f00) == 0x0b00
            U, L = (ins >> 23) & 1, (ins >> 20) & 1
            rn = (ins >> 16) & 15
            off = (ins & 0xff) * 4
            base = ((pc + 8) & ~3) if rn == 15 else self.r[rn]
            addr = (base + off if U else base - off) & 0xffffffff
            if dbl:
                d = ((ins >> 12) & 15) | (((ins >> 22) & 1) << 4)
                if L:
                    self.s[2 * d], self.s[2 * d + 1] = self.rd(addr, 4), self.rd(addr + 4, 4)
                else:
                    self.wr(addr, self.s[2 * d], 4)
                    self.wr(addr + 4, self.s[2 * d + 1], 4)
            else:
                d = (((ins >> 12) & 15) << 1) | ((ins >> 22) & 1)
                if L:
                    self.s[d] = self.rd(addr, 4)
                else:
                    self.wr(addr, self.s[d], 4)
            return
        if (_b == 0xd and _w == 1 and ((ins >> 16) & 15) == 13 and not (ins >> 23) & 1 and (ins & 0x0f00) == 0x0b00) or \
           (_b == 0xc and _w == 1 and ((ins >> 16) & 15) == 13 and (ins >> 23) & 1 and (ins & 0x0f00) == 0x0b00):
            # vpush / vpop {Dd..}  (FSTMDBX / FLDMIAX forms: imm8 = 2 * number of doubles)
            pop = (ins >> 20) & 1
            d0 = ((ins >> 12) & 15) | (((ins >> 22) & 1) << 4)
            n = (ins & 0xff) // 2
            sp = self.r[13]
            if pop:
                for i in range(n):
                    self.s[2 * (d0 + i)], self.s[2 * (d0 + i) + 1] = self.rd(sp, 4), self.rd(sp + 4, 4)
                    sp += 8
            else:
                sp -= 8 * n
                a = sp
                for i in range(n):
                    self.wr(a, self.s[2 * (d0 + i)], 4)
                    self.wr(a + 4, self.s[2 * (d0 + i) + 1], 4)
                    a += 8
            self.r[13] = sp & 0xffffffff
            return
        # VLDM/VSTM = cond 110 PUDWL Rn Vd 1011 imm8. `vmov Dm,Rt,Rt2` is cond 1100 010x ... 1011 00M1 (bits 27:21 = 1100010):
        # it shares the 0x0b00 field, so exclude it by that exact prefix (an earlier version of this test swallowed it).
        if (ins & 0x0e000f00) == 0x0c000b00 and (ins & 0x0fe00000) != 0x0c400000 and ((ins >> 16) & 15) != 13:
            # vldmia/vstmia Rn{!}, {Dd..}  and vldmdb/vstmdb Rn!, {Dd..} with a general base register
            # (remainder_pio2_kernel's `vstmia r3!, {d17}`). Only the plain IA (no writeback decrement) and DB forms.
            P, U, W_, L = (ins >> 24) & 1, (ins >> 23) & 1, (ins >> 21) & 1, (ins >> 20) & 1
            rn = (ins >> 16) & 15
            d0 = ((ins >> 12) & 15) | (((ins >> 22) & 1) << 4)
            n = (ins & 0xff) // 2
            if (ins & 0xff) % 2 or n == 0 or (P, U) not in ((0, 1), (1, 0)) or rn == 15:
                raise Abort('unsupported vldm/vstm form 0x%08x @%#x' % (ins, pc))
            base = self.r[rn]
            a = base if U else base - 8 * n
            for i in range(n):
                if L:
                    self.s[2 * (d0 + i)], self.s[2 * (d0 + i) + 1] = self.rd(a, 4), self.rd(a + 4, 4)
                else:
                    self.wr(a, self.s[2 * (d0 + i)], 4)
                    self.wr(a + 4, self.s[2 * (d0 + i) + 1], 4)
                a += 8
            if W_:
                self.r[rn] = (base + 8 * n if U else base - 8 * n) & 0xffffffff
            return
        if (ins & 0x0e000000) == 0x08000000:                       # LDM/STM (push/pop are the SP forms)
            P, U, S_, W_, L = [(ins >> k) & 1 for k in (24, 23, 22, 21, 20)]
            rn = (ins >> 16) & 15
            regs = [i for i in range(16) if (ins >> i) & 1]
            n = len(regs)
            base = self.r[rn]
            start = base + (4 if P else 0) if U else base - 4 * n + (0 if P else 4)
            if U:
                start = base + (4 if P else 0)
            else:
                start = base - 4 * n + (0 if P else 4)
            a = start
            for r_ in regs:
                if L:
                    v = self.rd(a, 4)
                    if r_ == 15:
                        self.r[15] = v & ~1
                    else:
                        self.r[r_] = v
                else:
                    self.wr(a, self.r[r_] if r_ != 15 else pc + 8, 4)
                a += 4
            if W_:
                self.r[rn] = (base + 4 * n) & 0xffffffff if U else (base - 4 * n) & 0xffffffff
            return
        if (ins & 0x0c000000) == 0x04000000:                       # LDR/STR/LDRB/STRB
            P, U, B, W, L = [(ins >> k) & 1 for k in (24, 23, 22, 21, 20)]
            rn, rd = (ins >> 16) & 15, (ins >> 12) & 15
            base = (pc + 8) if rn == 15 else self.r[rn]
            if (ins >> 25) & 1:
                off = (self.r[ins & 15] << ((ins >> 7) & 31)) & 0xffffffff
            else:
                off = ins & 0xfff
            nb = (base + off if U else base - off) & 0xffffffff
            addr = nb if P else base
            if L:
                val = self.rd(addr, 1 if B else 4)
            else:
                val = self.r[rd] if rd != 15 else pc + 8
                self.wr(addr, val, 1 if B else 4)
            if (not P) or W:
                self.r[rn] = nb
            if L:
                if rd == 15:
                    self.r[15] = val
                else:
                    self.r[rd] = val
            return
        # --- VFP extension space (opc1 = x1x1): vcmp/vcmpe, vneg, vmrs. Must precede the generic DP handlers.
        if (ins & 0x0fbf0f50) in (0x0eb40b40, 0x0eb40a40):         # vcmp(e).f64/.f32 Rd, Rm
            dbl = (ins & 0x0f00) == 0x0b00
            if dbl:
                d = ((ins >> 12) & 15) | (((ins >> 22) & 1) << 4)
                m = (ins & 15) | (((ins >> 5) & 1) << 4)
                a, b = self.d[d], self.d[m]
            else:
                d = (((ins >> 12) & 15) << 1) | ((ins >> 22) & 1)
                m = ((ins & 15) << 1) | ((ins >> 5) & 1)
                a, b = f32(self.s[d]), f32(self.s[m])
            if a != a or b != b:
                self.fN, self.fZ, self.fC, self.fV = 0, 0, 1, 1
            elif a < b:
                self.fN, self.fZ, self.fC, self.fV = 1, 0, 0, 0
            elif a == b:
                self.fN, self.fZ, self.fC, self.fV = 0, 1, 1, 0
            else:
                self.fN, self.fZ, self.fC, self.fV = 0, 0, 1, 0
            return
        if (ins & 0x0fffffff) == 0x0ef1fa10:                       # vmrs APSR_nzcv, fpscr (Rt = 15)
            self.N, self.Z, self.C, self.V = self.fN, self.fZ, self.fC, self.fV
            return
        if (ins & 0x0fb00ff0) == 0x0eb00b00:                       # vmov.f64 Dd, #imm8   (VFPv3)
            import struct as _st
            imm8 = ((ins >> 12) & 0xf0) | (ins & 0xf)
            s_, b_, f_ = imm8 >> 7, (imm8 >> 6) & 1, imm8 & 0xf
            expo = ((0 if b_ else 1) << 10) | ((0xff if b_ else 0) << 2) | ((imm8 >> 4) & 3)
            bits = (s_ << 63) | (expo << 52) | (f_ << 48)
            d = ((ins >> 12) & 15) | (((ins >> 22) & 1) << 4)
            self.d[d] = _st.unpack('<d', _st.pack('<Q', bits))[0]
            return
        if (ins & 0x0e1000f0) in (0x000000f0, 0x000000d0) and (ins & 0x0e400000) == 0x00400000 - 0x00400000 + (ins & 0x00400000) \
                and (ins & 0x0e000000) == 0x00000000 and ((ins >> 4) & 0xf) in (0xd, 0xf):
            # ldrd (0xd) / strd (0xf) : register pair Rt, Rt+1  (immediate offset form, bit22 = 1)
            if (ins >> 22) & 1:
                P, U, W_ = (ins >> 24) & 1, (ins >> 23) & 1, (ins >> 21) & 1
                rn, rt = (ins >> 16) & 15, (ins >> 12) & 15
                off = ((ins >> 4) & 0xf0) | (ins & 0xf)
                base = self.r[rn]
                nb = (base + off if U else base - off) & 0xffffffff
                addr = nb if P else base
                if ((ins >> 4) & 0xf) == 0xd:                      # ldrd
                    self.r[rt], self.r[rt + 1] = self.rd(addr, 4), self.rd(addr + 4, 4)
                else:                                              # strd
                    self.wr(addr, self.r[rt], 4)
                    self.wr(addr + 4, self.r[rt + 1], 4)
                if (not P) or W_:
                    self.r[rn] = nb
                return
        if (ins & 0x0fbf0fd0) == 0x0eb00b40:                       # vmov.f64 Dd, Dm   (opc2=0000, op=01)
            d = ((ins >> 12) & 15) | (((ins >> 22) & 1) << 4)
            m = (ins & 15) | (((ins >> 5) & 1) << 4)
            self.d[d] = self.d[m]
            return
        if (ins & 0x0fbf0fd0) == 0x0eb00bc0:                       # vabs.f64
            d = ((ins >> 12) & 15) | (((ins >> 22) & 1) << 4)
            m = (ins & 15) | (((ins >> 5) & 1) << 4)
            self.d[d] = abs(self.d[m])
            return
        if (ins & 0x0fbf0fd0) == 0x0eb10bc0:                       # vsqrt.f64
            d = ((ins >> 12) & 15) | (((ins >> 22) & 1) << 4)
            m = (ins & 15) | (((ins >> 5) & 1) << 4)
            import math as _m
            x = self.d[m]
            self.d[d] = _m.sqrt(x) if x >= 0 else float('nan')
            return
        if (ins & 0x0fbf0fd0) == 0x0eb00a40:                       # vmov.f32 Sd, Sm
            d = (((ins >> 12) & 15) << 1) | ((ins >> 22) & 1)
            m = ((ins & 15) << 1) | ((ins >> 5) & 1)
            self.s[d] = self.s[m]
            return
        if (ins & 0x0fbf0fd0) == 0x0eb10b40:                       # vneg.f64
            d = ((ins >> 12) & 15) | (((ins >> 22) & 1) << 4)
            m = (ins & 15) | (((ins >> 5) & 1) << 4)
            self.d[d] = -self.d[m]
            return
        if (ins & 0x0fbf0fd0) == 0x0eb10a40:                       # vneg.f32
            d = (((ins >> 12) & 15) << 1) | ((ins >> 22) & 1)
            m = ((ins & 15) << 1) | ((ins >> 5) & 1)
            self.s[d] = raw32(-f32(self.s[m]))
            return
        # VFP single-precision data processing (vadd/vsub/vmul/vdiv .f32)
        if (ins & 0x0f000f10) == 0x0e000b00 and (ins & 0x00b00040) == 0x00000000 and ((ins >> 20) & 0xf) == 0x0:
            # vmla.f64 Dd, Dn, Dm : Dd += Dn * Dm   (opc1 = 0000, op = 0)
            d = ((ins >> 12) & 15) | (((ins >> 22) & 1) << 4)
            n = ((ins >> 16) & 15) | (((ins >> 7) & 1) << 4)
            m = (ins & 15) | (((ins >> 5) & 1) << 4)
            self.d[d] = self.d[d] + self.d[n] * self.d[m]
            return
        if (ins & 0x0f000f10) == 0x0e000a00 and ((ins >> 20) & 0xb) != 0xb:
            opc1 = (ins >> 20) & 0xf
            op = (ins >> 6) & 1
            d = (((ins >> 12) & 15) << 1) | ((ins >> 22) & 1)
            n = (((ins >> 16) & 15) << 1) | ((ins >> 7) & 1)
            m = ((ins & 15) << 1) | ((ins >> 5) & 1)
            a, b = f32(self.s[n]), f32(self.s[m])
            key = (opc1 & 0xb, op)
            if key == (0x2, 0):
                res = a * b
            elif key == (0x3, 0):
                res = a + b
            elif key == (0x3, 1):
                res = a - b
            elif key == (0x8, 0):
                if b == 0.0:                                   # IEEE-754: x/0 = +-inf, 0/0 = nan
                    res = float('nan') if (a == 0.0 or a != a) else float('inf')
                else:
                    res = a / b
            else:
                raise Abort('vfp sp %#x @%#x' % (ins, pc))
            self.s[d] = raw32(res)
            return
        if (ins & 0x0fbf0fd0) == 0x0eb70ac0:                       # vcvt.f64.f32 Dd, Sm
            d = ((ins >> 12) & 15) | (((ins >> 22) & 1) << 4)
            m = ((ins & 15) << 1) | ((ins >> 5) & 1)
            self.d[d] = f32(self.s[m])
            return
        if (ins & 0x0fbf0fd0) == 0x0eb70bc0:                       # vcvt.f32.f64 Sd, Dm
            d = (((ins >> 12) & 15) << 1) | ((ins >> 22) & 1)
            m = (ins & 15) | (((ins >> 5) & 1) << 4)
            self.s[d] = raw32(self.d[m])
            return
        if (ins & 0x0fbf0fd0) == 0x0eb80a40 or (ins & 0x0fbf0fd0) == 0x0eb80ac0:   # vcvt.f32.s32 (signed int source)
            d = (((ins >> 12) & 15) << 1) | ((ins >> 22) & 1)
            m = ((ins & 15) << 1) | ((ins >> 5) & 1)
            v = self.s[m]
            self.s[d] = raw32(float(v - (1 << 32) if v >> 31 else v))
            return
        if (ins & 0x0fbf0fd0) == 0x0eb80ac0:                       # vcvt.f32.s32 Sd, Sm
            d = (((ins >> 12) & 15) << 1) | ((ins >> 22) & 1)
            m = ((ins & 15) << 1) | ((ins >> 5) & 1)
            v = self.s[m]
            self.s[d] = raw32(float(v - (1 << 32) if v >> 31 else v))
            return
        _base_step(self, ins, pc)
