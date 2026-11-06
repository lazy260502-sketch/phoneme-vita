@ vita_kni_fp_overlay.S - integer-register result adapters for the two KNI natives that RETURN a float/double
@ (overlay, v03.48; applied by patch_kni_fp.sh from rebuild_vm.sh pack_lib).
@
@ WHY (log143, Vita3K, BioMain): M3gReader.f32() = Float.intBitsToFloat(u32()) returned 0x813f3258 for EVERY call
@ - that is the address of _current_thread / jvm_fast_globals, i.e. a stale r0.  positionScale and the three positionBias
@ values all decoded to -3.5e-38, so 472 vertices of every mesh landed on one NDC point (x[4,4] y[-40,-40]) and the
@ characters and scene never appeared.  Float.toString on those values also threw a bare RuntimeException
@ ("Assertion botch" in FloatingDecimal), which is why the v03.42 "ndc src" probe failed.
@
@ ROOT CAUSE: the interpreter takes a native's result from INTEGER registers (bc_impl_fast_invokenative: float -> r0,
@ double -> r0 = HIGH / r1 = LOW, the same convention as native_math_*_entry).  FloatNatives.cpp is compiled
@ -mfloat-abi=hard, so Float.intBitsToFloat answers in s0 and Double.longBitsToDouble in d0 and r0/r1 keep whatever the
@ caller left there.  floatToIntBits / doubleToLongBits return integers and were always fine (checked by bitsnat.py).
@
@ HOW: patch_kni_fp.sh renames the two C bodies in the archive member that defines them
@        Java_java_lang_Float_intBitsToFloat     -> Java_java_lang_Float_intBitsToFloat_c
@        Java_java_lang_Double_longBitsToDouble  -> Java_java_lang_Double_longBitsToDouble_c
@ and adds THIS object as its own member, defining the original names.  Callers (ROMImage.o) are unchanged.
@ VERIFY: nm shows each of the four symbols exactly once in the archive; debug/tools/fpmodel/bitsnat.py runs the real
@ entries from the final ELF (42 cases).  IDEMPOTENT: ar r replaces the member, the rename is skipped once done.
        .syntax unified
        .arm
        .fpu    vfpv3
        .text

        .global Java_java_lang_Float_intBitsToFloat
        .type   Java_java_lang_Float_intBitsToFloat, %function
Java_java_lang_Float_intBitsToFloat:
        push    {r4, lr}
        bl      Java_java_lang_Float_intBitsToFloat_c
        vmov    r0, s0
        pop     {r4, pc}
        .size   Java_java_lang_Float_intBitsToFloat, .-Java_java_lang_Float_intBitsToFloat

        .global Java_java_lang_Double_longBitsToDouble
        .type   Java_java_lang_Double_longBitsToDouble, %function
Java_java_lang_Double_longBitsToDouble:
        push    {r4, lr}
        bl      Java_java_lang_Double_longBitsToDouble_c
        vmov    r1, r0, d0          @ r0 = high word, r1 = low word
        pop     {r4, pc}
        .size   Java_java_lang_Double_longBitsToDouble, .-Java_java_lang_Double_longBitsToDouble
