/*
 *   
 *
 * Copyright  1990-2007 Sun Microsystems, Inc. All Rights Reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER
 * 
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License version
 * 2 only, as published by the Free Software Foundation.
 * 
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License version 2 for more details (a copy is
 * included at /legal/license.txt).
 * 
 * You should have received a copy of the GNU General Public License
 * version 2 along with this work; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA
 * 02110-1301 USA
 * 
 * Please contact Sun Microsystems, Inc., 4150 Network Circle, Santa
 * Clara, CA 95054 or visit www.sun.com if you need additional
 * information or have any questions.
 */

#include "incls/_precompiled.incl"
#include "incls/_InterpreterRuntime_arm.cpp.incl"

extern "C" {

OopDesc* _newobject(Thread*thread, OopDesc* raw_klass JVM_TRAPS) {
  (void)thread;
  UsingFastOops fast_oops;
  InstanceClass::Fast klass = raw_klass;
  return klass().new_instance(ErrorOnFailure JVM_NO_CHECK_AT_BOTTOM_0);
}

jint _instanceof(Thread*thread, OopDesc* raw_object_class,      
                 OopDesc* raw_test_class) {
  (void)thread;
  JavaClass::Raw object_class = raw_object_class;
  JavaClass::Raw test_class = raw_test_class;

  return (object_class().is_subtype_of(&test_class) ? 1 : 0);
}


OopDesc* _anewarray(Thread *thread, OopDesc* raw_base_klass, int length
                    JVM_TRAPS) {
  UsingFastOops fast_oops;
  (void)thread;
  // Called from compiled code when it can't do the allocation
#ifdef AZZERT
  JavaFrame frame(thread);
  frame.verify();
#endif
  JavaClass::Fast base_klass = raw_base_klass;
  return Universe::new_obj_array(&base_klass, length JVM_NO_CHECK_AT_BOTTOM_0);
}

#if ENABLE_THUMB_COMPILER
  jlong jvm_lmul(jlong op1, jlong op2) {
    return op1 * op2;
  }

  jlong jvm_ladd(jlong op1, jlong op2) {
    return op1 + op2;
  }

  jlong jvm_lsub(jlong op1, jlong op2) {
    return op1 - op2;
  }

  jlong jvm_land(jlong op1, jlong op2) {
    return op1 & op2;
  }

  jlong jvm_lor(jlong op1, jlong op2) {
    return (julong)op1 | (julong)op2;
  }

  jlong jvm_lxor(jlong op1, jlong op2) {
    return (julong)op1 ^ (julong)op2;
  }

  jlong jvm_lshl(jlong op1, jint op2) {
    return ((julong)op1) << op2;
  }

  jlong jvm_lshr(jlong op1, jint op2) {
    return op1 >> op2;
  }

  jlong jvm_lushr(jlong op1, jint op2) {
    return ((julong)op1) >> op2;
  }

  jint jvm_lcmp(jlong op1, jlong op2) {
    return (op1 < op2 ?  -1 : op1 == op2 ? 0 : 1);
  }

  jlong jvm_lmin(jlong op1, jlong op2) {
    return ((op1 <= op2) ? op1 : op2);
  }

  jlong jvm_lmax(jlong op1, jlong op2) {
    return ((op1 >= op2) ? op1 : op2);
  }
#endif /*#if ENABLE_THUMB_COMPILER*/

#if defined(VITA)
/* v02.40: C landing pad for the green->green sentinel handoff.
 *
 * Six freeze rounds (log61..66, version-corrected deathpoint table in
 * PROJECT_MEMORY.md) froze the VM thread at pc=trampoline+0/+8 with
 * lr=trampoline entry, half-executed (log66: r5/r0 loaded by instr 1/2
 * but r1=0 => instr 3 never ran). The trampoline lives in the
 * loopgen-frozen Interpreter_arm.o and can never be instrumented, so
 * this pad moves the observable boundary into fresh-compiled code:
 * setup_stack_asm below now seeds THIS function's address as the
 * bootstrap sentinel word instead of start_lightweight_thread_asm.
 * Every path that used to enter the trampoline "from nothing" - the
 * birth tail of primordial_to_current_thread (ldr lr,[jsp,-4];
 * ldr fp,[jsp,-8]; mov pc,lr) and the interpreter unwind at run()
 * return - now lands here first.
 *
 * ABI at entry (birth tail verified in the v02.39 binary at 0x810c26e0):
 *   lr = pad entry address (the sentinel word itself)
 *   fp = 0 (bootstrap frame), jsp/r6 = lightweight_thread_exit word,
 *   gp/r5 = UNDEFINED (the trampoline self-initializes it), and the
 *   ARM sp/r13 is still the primordial stack (green-thread model).
 * The trampoline reads [r6,#16] at its 7th instruction, so the pad
 * MUST NOT clobber r4-r11: a compiled C body would legally use r6 as
 * callee-saved scratch and only restore it in an epilogue that never
 * runs (the trampoline does not return). Hence naked + hand asm,
 * touching only r0/r1 (dead at this point), movw/movt absolute
 * addressing (non-PIE build; avoids literal-pool range issues in a
 * merged translation unit).
 *
 * Probes (watchdog v240 line in OS_vita.cpp):
 *   land  >=1 -> sentinel handoff completed at least once
 *   land  ==0 at freeze -> freeze sits BEFORE any sentinel handoff
 *   fp/lr snapshot -> template confirmation (fp==0, lr==pad expected)
 */
extern "C" {
volatile unsigned int vita_v240_land = 0;
volatile unsigned int vita_v240_fp   = 0;
volatile unsigned int vita_v240_lr   = 0;
/* v02.42: pe liveness at the sentinel crossing. log67's livelock
 * dispatched id=3 with pe=0 sixteen times while land spun at 14.5M/s:
 * if the pad sees pe==0 on every crossing the spinning thread has NO
 * Java body at all (empty trampoline bounce); if pe!=0 the body is
 * there and the invoke_pending_entries stub must be consuming it -
 * the distinction the frozen binary's pe probes could not make.
 * Slot 0 counts pe==NULL crossings, slot 1 counts pe!=NULL.
 * Offset 0x24 is Thread::pending_entries_offset(), verified against
 * the shipped v02.41 invoke_pending_entries stub (ldr r1,[r0,#36]). */
volatile unsigned int vita_v242_pe[2] = {0, 0};
/* v02.44: bouncer anatomy. log69 froze the VM thread at
 * start_lightweight_thread_asm+0x8 (3rd instruction, the status load)
 * with lr=pad, fp=0, r6=0x81ed0804 - which is 0xBC ABOVE id4's saved
 * stack_pointer (0x81ed0748): the exit-table base the stub reads
 * ([r6]=lightweight_thread_exit .. [r6+16]=TestCompiler) is NOT where
 * setup_stack_asm left the birth frame. Every forward path past the
 * stub's +0x1c goes through C++ whose counters are ALL frozen at 0,
 * so if the spin is real it must close via a CORRUPTED slot jumping
 * back (L444 -> call_on_primordial_stack([r6]) -> pad if [r6] holds
 * the pad address; note call_on_primordial_stack also PUSHES lr/fp
 * at [r6-8]/[r6-4] every iteration - a fingerprint rd7 can read).
 * The coredump never contains the Java heap, so the pad records the
 * two words the next freeze needs: ct = the _current_thread object
 * the bouncer sees (is it really the dispatch ring's id?), r6 = the
 * exit-table base (watchdog rd7 dereferences 7 words around it).
 * r2 is dead at pad entry (stub writes it at +0xc before any use). */
volatile unsigned int vita_v244_ct = 0;
volatile unsigned int vita_v244_r6 = 0;
}
extern "C" void vita_landing_pad(void) __attribute__((naked));
extern "C" void vita_landing_pad(void) {
  asm volatile(
    "movw  r0, #:lower16:vita_v240_land\n"
    "movt  r0, #:upper16:vita_v240_land\n"
    "ldr   r1, [r0]\n"
    "add   r1, r1, #1\n"
    "str   r1, [r0]\n"
    "movw  r0, #:lower16:vita_v240_fp\n"
    "movt  r0, #:upper16:vita_v240_fp\n"
    "str   r11, [r0]\n"
    "movw  r0, #:lower16:vita_v240_lr\n"
    "movt  r0, #:upper16:vita_v240_lr\n"
    "str   lr, [r0]\n"
    "movw  r0, #:lower16:_current_thread\n"
    "movt  r0, #:upper16:_current_thread\n"
    "ldr   r0, [r0]\n"
    "ldr   r1, [r0, #36]\n"
    "movw  r0, #:lower16:vita_v242_pe\n"
    "movt  r0, #:upper16:vita_v242_pe\n"
    "cmp   r1, #0\n"
    "addne r0, r0, #4\n"
    "ldr   r1, [r0]\n"
    "add   r1, r1, #1\n"
    "str   r1, [r0]\n"
    "movw  r2, #:lower16:vita_v244_ct\n"   /* v02.44: r1 dead after the
     * counter store, r2 dead across pad->stub edge (stub does
     * mov r2,#0 at +0xc before any use). Reload ct: r0 now holds
     * &vita_v242_pe, not the thread. */
    "movt  r2, #:upper16:vita_v244_ct\n"
    "movw  r1, #:lower16:_current_thread\n"
    "movt  r1, #:upper16:_current_thread\n"
    "ldr   r1, [r1]\n"
    "str   r1, [r2]\n"
    "movw  r2, #:lower16:vita_v244_r6\n"
    "movt  r2, #:upper16:vita_v244_r6\n"
    "str   r6, [r2]\n"
    "movw  r0, #:lower16:start_lightweight_thread_asm\n"
    "movt  r0, #:upper16:start_lightweight_thread_asm\n"
    "bx    r0\n"
  );
}
#endif /* VITA */

  address setup_stack_asm(address sp) {
    jint* xsp = (jint*)sp;
    *xsp = TestCompiler;
      xsp += JavaStackDirection;
    *xsp = (jint)(address_word)force_terminated;
      xsp += JavaStackDirection;
    *xsp = (jint)(address_word)Thread::finish;
      xsp += JavaStackDirection;
    *xsp = (jint)(address_word)Thread::lightweight_thread_uncaught_exception;
      xsp += JavaStackDirection;
    *xsp = (jint)(address_word)Thread::lightweight_thread_exit;
      xsp += JavaStackDirection;
#if defined(VITA)
    /* v02.40: sentinel word now lands in vita_landing_pad (see above)
     * instead of the trampoline directly; the pad forwards to the
     * trampoline after recording the crossing. */
    *xsp = (jint)(address_word)vita_landing_pad;
#else
    *xsp = (jint)(address_word)start_lightweight_thread_asm;
#endif
      xsp += JavaStackDirection;
    *xsp = 0;                   // value of frame pointer
    return (address)xsp;
  }
} // extern "C"
