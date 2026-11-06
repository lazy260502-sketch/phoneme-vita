@ vita_fp_overlay.S - FP ABI overlay for the Vita CLDC VM (integer-register adapters + hard-float d2i/f2i entries).
@
@ WHY THIS FILE EXISTS (v03.08, found via log123: (int)(.95 * 240) evaluated to 0 on the VM)
@
@ The VM interpreter (Interpreter_arm.o, generated assembly) calls every FP helper with the operands in INTEGER
@ registers and expects the result in integer registers.  The helpers it calls - jvm_dadd/dsub/dmul/ddiv, jvm_fadd..,
@ jvm_i2d, jvm_f2d, jvm_d2f, jvm_dcmpl.., jvm_sin.. and so on - are C functions in libcldc_vm.a that
@ rebuild_vm.sh compiles with -mfloat-abi=hard, i.e. they read d0/d1 (s0/s1) and return in d0 (s0).  The two sides
@ never agreed: the interpreter's dmul left (b, a) in r0-r3 and the helper multiplied whatever happened to be in
@ d0/d1, so almost every Java double/float operation returned one of its operands or garbage.  The only helpers that
@ worked were jvm_d2i / jvm_f2i, which Interpreter_arm.o defines itself with the integer protocol.
@
@ Measured by running the real bc_impl_* handlers from the ELF through an ARM/VFP model (40 cases): 6/40 correct before
@ (only d2i/f2i + dneg/fneg), 40/40 with these shims.  See PROJECT_MEMORY "v03.08".
@
@ HOW IT IS WIRED (build/vita_arm/patch_interp.sh, run by rebuild_vm.sh pack_lib; nothing at link time):
@   1. The generated Interpreter_arm.o is extracted, and objcopy renames every call to an FP helper:
@        jvm_X -> jvm_X_isoft   (28 helpers; the adapters below)
@      The interpreter's OWN hand-written integer-protocol d2i/f2i (defined inside that object) are renamed too:
@        jvm_d2i -> jvm_d2i_isoft,  jvm_f2i -> jvm_f2i_isoft
@      (their address is also stored in the GP table inside the object, so the table follows the rename).
@   2. This file is assembled and merged into the same object (ld -r), so libcldc_vm.a ends up with ONE member that
@      defines every *_isoft symbol.  The C++ callers (sin/cos/tan kernels, JIT constant folding) keep calling the
@      plain hard-float C helpers, which stay untouched.
@   3. jvm_d2i / jvm_f2i themselves are now defined HERE with the hard-float C ABI (double in d0 / float in s0, int
@      result in r0).  Before this change the integer-protocol copy answered the C++ callers too, and read r0:r1 /
@      r0 instead of d0 / s0: ieee754_rem_pio2 got n = 0 for every argument and sin/cos/tan were wrong whenever
@      |x| > pi/4 (sin(10) = 532.9).  The entries only move the argument into integer registers and tail-call the
@      verified integer-protocol code, so no conversion logic is duplicated.
@
@ Protocol (derived from the disassembly of the handlers; each case is checked by the model):
@   double = two words, r(N) = HIGH word, r(N+1) = LOW word       (MSW_FIRST_FOR_DOUBLE; same as jvm_d2i's input)
@   binary double op (a op b): a = r0:r1, b = r2:r3, result r0:r1
@   binary float  op (a op b): a = r0,    b = r1,    result r0
@   compare: same operand layout, result (-1/0/1) in r0
@   long = r0 = LOW, r1 = HIGH   (AAPCS jlong, unchanged)
        .syntax unified
        .arm
        .fpu    vfpv3
        .text

        .macro  DBL_BIN name
        .global \name\()_isoft
        .type   \name\()_isoft, %function
\name\()_isoft:
        vmov    d0, r1, r0          @ a  (Dd = Rt(low word), Rt2(high word))
        vmov    d1, r3, r2          @ b
        push    {r4, lr}
        bl      \name
        pop     {r4, lr}
        vmov    r1, r0, d0          @ r0 = high, r1 = low
        bx      lr
        .size   \name\()_isoft, .-\name\()_isoft
        .endm

        .macro  FLT_BIN name
        .global \name\()_isoft
        .type   \name\()_isoft, %function
\name\()_isoft:
        vmov    s0, r0
        vmov    s1, r1
        push    {r4, lr}
        bl      \name
        pop     {r4, lr}
        vmov    r0, s0
        bx      lr
        .size   \name\()_isoft, .-\name\()_isoft
        .endm

        .macro  DBL_CMP name
        .global \name\()_isoft
        .type   \name\()_isoft, %function
\name\()_isoft:
        vmov    d0, r1, r0
        vmov    d1, r3, r2
        push    {r4, lr}
        bl      \name
        pop     {r4, pc}
        .size   \name\()_isoft, .-\name\()_isoft
        .endm

        .macro  FLT_CMP name
        .global \name\()_isoft
        .type   \name\()_isoft, %function
\name\()_isoft:
        vmov    s0, r0
        vmov    s1, r1
        push    {r4, lr}
        bl      \name
        pop     {r4, pc}
        .size   \name\()_isoft, .-\name\()_isoft
        .endm

        .macro  MATH1 name
        .global \name\()_isoft
        .type   \name\()_isoft, %function
\name\()_isoft:
        vmov    d0, r1, r0
        push    {r4, lr}
        bl      \name
        pop     {r4, lr}
        vmov    r1, r0, d0
        bx      lr
        .size   \name\()_isoft, .-\name\()_isoft
        .endm

        DBL_BIN jvm_dadd
        DBL_BIN jvm_dsub
        DBL_BIN jvm_dmul
        DBL_BIN jvm_ddiv
        DBL_BIN jvm_drem
        FLT_BIN jvm_fadd
        FLT_BIN jvm_fsub
        FLT_BIN jvm_fmul
        FLT_BIN jvm_fdiv
        FLT_BIN jvm_frem
        DBL_CMP jvm_dcmpl
        DBL_CMP jvm_dcmpg
        FLT_CMP jvm_fcmpl
        FLT_CMP jvm_fcmpg
        MATH1   jvm_sin
        MATH1   jvm_cos
        MATH1   jvm_tan
        MATH1   jvm_sqrt
        MATH1   jvm_ceil
        MATH1   jvm_floor

        @ ---- conversions -------------------------------------------------------------------------
        .global jvm_i2d_isoft
        .type   jvm_i2d_isoft, %function
jvm_i2d_isoft:
        vmov    s0, r0
        vcvt.f64.s32 d0, s0
        vmov    r1, r0, d0          @ r0 = high, r1 = low
        bx      lr

        .global jvm_i2f_isoft
        .type   jvm_i2f_isoft, %function
jvm_i2f_isoft:
        vmov    s0, r0
        vcvt.f32.s32 s0, s0
        vmov    r0, s0
        bx      lr

        .global jvm_f2d_isoft
        .type   jvm_f2d_isoft, %function
jvm_f2d_isoft:
        vmov    s0, r0
        vcvt.f64.f32 d0, s0
        vmov    r1, r0, d0
        bx      lr

        .global jvm_d2f_isoft
        .type   jvm_d2f_isoft, %function
jvm_d2f_isoft:
        vmov    d0, r1, r0
        vcvt.f32.f64 s0, d0
        vmov    r0, s0
        bx      lr

        @ long <-> fp: jlong is (r0 = low, r1 = high) both in the interpreter and in the AAPCS, so the long side needs
        @ no marshalling; only the fp side moves between s0/d0 and the integer registers.
        .global jvm_l2d_isoft
        .type   jvm_l2d_isoft, %function
jvm_l2d_isoft:
        push    {r4, lr}
        bl      jvm_l2d             @ jlong in r0:r1 -> d0
        pop     {r4, lr}
        vmov    r1, r0, d0
        bx      lr

        .global jvm_l2f_isoft
        .type   jvm_l2f_isoft, %function
jvm_l2f_isoft:
        push    {r4, lr}
        bl      jvm_l2f
        pop     {r4, lr}
        vmov    r0, s0
        bx      lr

        .global jvm_f2l_isoft
        .type   jvm_f2l_isoft, %function
jvm_f2l_isoft:
        vmov    s0, r0
        push    {r4, lr}
        bl      jvm_f2l             @ result r0:r1 = low:high
        pop     {r4, pc}

        .global jvm_d2l_isoft
        .type   jvm_d2l_isoft, %function
jvm_d2l_isoft:
        vmov    d0, r1, r0
        push    {r4, lr}
        bl      jvm_d2l
        pop     {r4, pc}

        @ ---- hard-float d2i / f2i for the C++ callers --------------------------------------------------------
        @ The integer-protocol bodies live in Interpreter_arm.o as jvm_d2i_isoft / jvm_f2i_isoft (renamed by
        @ patch_interp.sh).  They take the double as r0 = HIGH, r1 = LOW (the float in r0) and return in r0 with the
        @ Java saturating semantics, using only r0-r2.
        .global jvm_d2i
        .type   jvm_d2i, %function
jvm_d2i:
        vmov    r1, r0, d0          @ r0 = high word, r1 = low word
        b       jvm_d2i_isoft
        .size   jvm_d2i, .-jvm_d2i

        .global jvm_f2i
        .type   jvm_f2i, %function
jvm_f2i:
        vmov    r0, s0
        b       jvm_f2i_isoft
        .size   jvm_f2i, .-jvm_f2i
