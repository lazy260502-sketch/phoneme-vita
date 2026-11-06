"""Instruction semantics for armsim.CPU (monkey-patched in as CPU.step)."""
from armsim import CPU, Abort, f64, bits64


def _op2(cpu, ins, pc):
    if (ins >> 25) & 1:
        imm = ins & 0xff
        rot = ((ins >> 8) & 15) * 2
        return ((imm >> rot) | (imm << (32 - rot))) & 0xffffffff if rot else imm
    v = cpu.r[ins & 15]
    typ = (ins >> 5) & 3
    by_reg = (ins >> 4) & 1
    sh = (cpu.r[(ins >> 8) & 15] & 0xff) if by_reg else ((ins >> 7) & 31)
    if typ == 0:
        return (v << sh) & 0xffffffff
    if typ == 1:
        return (v >> sh) if (sh or by_reg) else 0
    if typ == 2:
        sv = v - (1 << 32) if v >> 31 else v
        return (sv >> (sh if (sh or by_reg) else 31)) & 0xffffffff
    raise Abort('ror/rrx @%#x' % pc)


def step(self, ins, pc):
    c = ins >> 28
    if c == 15:
        raise Abort('unconditional space %#x @%#x' % (ins, pc))
    if not self.cond(c):
        return
    if (ins & 0x0ffffff0) == 0x012fff10:                      # bx Rm
        self.r[15] = self.r[ins & 15] & ~1
        return
    if (ins & 0x0fbf0fd0) == 0x0eb80b40:                      # vcvt.f64.u32 Dd, Sm  (unsigned source: op bit = 0)
        d = ((ins >> 12) & 15) | (((ins >> 22) & 1) << 4)
        m = ((ins & 15) << 1) | ((ins >> 5) & 1)
        self.d[d] = float(self.s[m])
        return
    if (ins & 0x0fbf0fd0) == 0x0eb80bc0:                      # vcvt.f64.s32 Dd, Sm  (must precede generic VFP DP)
        d = ((ins >> 12) & 15) | (((ins >> 22) & 1) << 4)
        m = ((ins & 15) << 1) | ((ins >> 5) & 1)
        v = self.s[m]
        self.d[d] = float(v - (1 << 32) if v >> 31 else v)
        return
    if (ins & 0x0fbf0fd0) == 0x0ebd0bc0:                      # vcvt.s32.f64 Sd, Dm (toward zero)
        d = (((ins >> 12) & 15) << 1) | ((ins >> 22) & 1)
        m = (ins & 15) | (((ins >> 5) & 1) << 4)
        v = self.d[m]
        self.s[d] = (0 if v != v else max(-2 ** 31, min(2 ** 31 - 1, int(v)))) & 0xffffffff
        return
    if (ins & 0x0f000f10) == 0x0e000b00:                      # VFP double data-processing
        opc1 = (ins >> 20) & 0xf
        op = (ins >> 6) & 1
        d = ((ins >> 12) & 15) | (((ins >> 22) & 1) << 4)
        n = ((ins >> 16) & 15) | (((ins >> 7) & 1) << 4)
        m = (ins & 15) | (((ins >> 5) & 1) << 4)
        a, b = self.d[n], self.d[m]
        key = (opc1 & 0xb, op)
        if key == (0x0, 0):                                   # vmla.f64: Dd = Dd + Dn * Dm
            res = self.d[d] + a * b
        elif key == (0x2, 0):
            res = a * b
        elif key == (0x3, 0):
            res = a + b
        elif key == (0x3, 1):
            res = a - b
        elif key == (0x8, 0):
            if b == 0.0:                                      # IEEE-754: x/0 = +-inf, 0/0 = nan
                res = float('nan') if (a == 0.0 or a != a) else float('inf') * (1 if (a > 0) == (str(b)[0] != '-') else -1)
            else:
                res = a / b
        else:
            raise Abort('vfp dp %#x @%#x' % (ins, pc))
        self.d[d] = res
        return
    if (ins & 0x0fe00f7f) == 0x0e000a10:                      # vmov Sn<->Rt
        n = (((ins >> 16) & 15) << 1) | ((ins >> 7) & 1)
        rt = (ins >> 12) & 15
        if (ins >> 20) & 1:
            self.r[rt] = self.s[n]
        else:
            self.s[n] = self.r[rt] & 0xffffffff
        return
    if (ins & 0x0fbf0fd0) == 0x0eb80bc0:                      # vcvt.f64.s32 Dd, Sm
        d = ((ins >> 12) & 15) | (((ins >> 22) & 1) << 4)
        m = ((ins & 15) << 1) | ((ins >> 5) & 1)
        v = self.s[m]
        self.d[d] = float(v - (1 << 32) if v >> 31 else v)
        return
    if (ins & 0x0fbf0fd0) == 0x0ebd0bc0:                      # vcvt.s32.f64 Sd, Dm (toward zero)
        d = (((ins >> 12) & 15) << 1) | ((ins >> 22) & 1)
        m = (ins & 15) | (((ins >> 5) & 1) << 4)
        v = self.d[m]
        self.s[d] = (0 if v != v else max(-2 ** 31, min(2 ** 31 - 1, int(v)))) & 0xffffffff
        return
    if (ins & 0x0fe00fd0) == 0x0c400b10:                      # vmov Dm<->Rt,Rt2
        rt = (ins >> 12) & 15
        rt2 = (ins >> 16) & 15
        m = (ins & 15) | (((ins >> 5) & 1) << 4)
        if (ins >> 20) & 1:
            hi, lo = bits64(self.d[m])
            self.r[rt], self.r[rt2] = lo, hi
        else:
            self.d[m] = f64(self.r[rt2], self.r[rt])
        return
    if (ins & 0x0e000000) == 0x0a000000:                      # b / bl
        off = ins & 0xffffff
        off = off - (1 << 24) if off >> 23 else off
        if (ins >> 24) & 1:
            self.r[14] = pc + 4
        self.r[15] = (pc + 8 + off * 4) & 0xffffffff
        return
    if (ins & 0x0c000000) == 0:                               # data processing
        opc = (ins >> 21) & 15
        S = (ins >> 20) & 1
        a = self.r[(ins >> 16) & 15]
        rd = (ins >> 12) & 15
        b = _op2(self, ins, pc)
        if opc == 0x8:                                        # tst
            self.N = (a & b) >> 31
            self.Z = int((a & b) == 0)
            return
        if opc == 0xa:                                        # cmp
            self.flags_sub(a, b)
            return
        if opc == 0xb:                                        # cmn: flags of a + b
            full = a + b
            r32 = full & 0xffffffff
            self.N, self.Z = r32 >> 31, int(r32 == 0)
            self.C = int(full > 0xffffffff)
            self.V = int(((a ^ r32) & (b ^ r32)) >> 31 & 1)
            return
        if opc == 0x9:                                        # teq: flags of a ^ b
            self.N = (a ^ b) >> 31
            self.Z = int((a ^ b) == 0)
            return
        cin = self.C
        res = {0xd: b, 0xf: (~b) & 0xffffffff, 0x0: a & b, 0xe: a & ~b & 0xffffffff, 0xc: a | b,
               0x1: a ^ b, 0x3: (b - a) & 0xffffffff, 0x4: (a + b) & 0xffffffff,
               0x2: (a - b) & 0xffffffff,
               0x5: (a + b + cin) & 0xffffffff,                       # adc
               0x6: (a - b - (1 - cin)) & 0xffffffff,                 # sbc
               0x7: (b - a - (1 - cin)) & 0xffffffff}.get(opc)        # rsc
        if S and opc == 0x3:                                      # rsbs: flags of (b - a)
            self.flags_sub(b, a)
        if S and opc in (0x5, 0x6, 0x7):
            full = {0x5: a + b + cin, 0x6: a - b - (1 - cin), 0x7: b - a - (1 - cin)}[opc]
            r32 = full & 0xffffffff
            self.N, self.Z = r32 >> 31, int(r32 == 0)
            self.C = int(full >= 0) if opc != 0x5 else int(full > 0xffffffff)
        if res is None:
            raise Abort('dp opc %d @%#x ins %#x' % (opc, pc, ins))
        if S and opc == 0x2:
            self.flags_sub(a, b)
        elif S:
            self.N = res >> 31
            self.Z = int(res == 0)
        if rd == 15:
            self.r[15] = res
        else:
            self.r[rd] = res & 0xffffffff
        return
    raise Abort('unsupported %#010x @%#x' % (ins, pc))


CPU.step = step
