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
/* v02.45: SATURATION forensics - make the ring catch itself. log68/69
 * both show the ring closes THROUGH this pad (land/pe0 climb at ring
 * speed ~10M/s) while EVERY C++ counter is frozen, so the pad is the
 * only instrumented point guaranteed to be on the ring. Real-hardware
 * rounds are expensive; instead of one probe per run, the pad now
 * carries a flood detector plus a bounce history, and on trip it
 * saves the whole register scene for the C dumper (vita_ring_trap in
 * OS_vita.cpp).
 *
 * vita_v245_hist: circular record of the last 4 crossings, written
 * EVERY crossing before the gate: [0] = write index, then word
 * triples [1+3i]=r6, [2+3i]=lr, [3+3i]=[r6-8] (i = idx&3). lr at pad
 * entry names the closing edge (jump vs call into the pad); r6's
 * per-bounce delta exposes any stack slide between consecutive
 * bounces. v02.49 adds the third word: [r6-8] is the fp slot just
 * below the sentinel - call_on_primordial_stack's prologue pushes
 * (lr,fp) at [r6-4],[r6-8] and its epilogue pops them back, so during
 * a bounce this word alternates between the static template content
 * (0 / whatever setup_stack_asm left) and the trampoline's transient
 * push. A [r6-8] that CHANGES across the 4 recorded bounces proves
 * the death-tail C++ runs inside the ring (prologue executed); a
 * frozen [r6-8] matching the pristine template proves the ring closes
 * before any push - pure asm topology. Format grew from 9 to 13
 * words; the dumper (OS_vita.cpp) reads the same layout.
 *
 * v02.50: 4th word = _current_pending_exception snapshot (cpe). The
 * log73 paradox: the trampoline head is a straight line into the
 * unconditional "bl call_on_primordial_stack(finish)" yet finish=0
 * for the WHOLE session - so the ring must leave the trampoline
 * before +0x1c, and the only branch in there whose predicate has no
 * counter is the "blne c_o_p_s(uncaught_exception)" guarded by
 * _current_pending_exception (globals gp+0x410). This word tells the
 * dump whether that predicate was live during the ring. Layout grew
 * 13 -> 17 words (4 per crossing, byte offset 16*i, write index at
 * [0]); the dumper in OS_vita.cpp reads the same layout.
 *
 * vita_v245_flood: gate state + trip scene. Layout (word index):
 *   [0] consecutive pe==NULL crossings (reset on pe!=NULL or ct==NULL)
 *   [1] tripped flag (1 = parked for dump, 2 = BRAKE mode: scene
 *       stored, thread left the ring through [r6]=lw_exit to
 *       self-heal; the watchdog dumps but does NOT kill the process).
 *       v02.48: handoff flag - the spinner stores the scene first,
 *       writes the flag LAST; the launcher watchdog polls it
 *   [2] ct  [3] r3  [4] r4  [5] r5  [6] r6  [7] r7
 *   [8] r8  [9] r9 [10] r10 [11] fp(r11) [12] sp [13] lr
 *  [14] cpe snapshot (_current_pending_exception at trip)
 *  [15] parked-site cookie (&vita_ring_parked)
 *  [16..19] reserved (0) [20] brake lap count (v02.50)
 * 256 consecutive empty bounces cannot happen in legitimate
 * operation: normal births cross with pe!=NULL (pe1 stays ~thread
 * count, see log69), so reaching the threshold IS the livelock,
 * caught in milliseconds with the full scene instead of a coredump
 * lottery (and the coredump never maps the Java stack pages - this
 * dump does, from inside the spinning thread).
 *
 * v02.50 BRAKE: on trip the scene is stored as before, but instead
 * of parking on wfe (which freezes the game forever) the thread now
 * EXITS THE RING through the designed death door: ldr r0,[r6] (the
 * birth template's lightweight_thread_exit slot) + bx r0. lw_exit
 * runs terminate (op-2 REM - breaks the q5 CYCLE), then yield()
 * switches to a live thread; the ghost never returns. The watchdog,
 * on seeing tripped==2, dumps the scene to ring_trap.log with a
 * BRAKE line and then MONITORS instead of exiting: if the VM recovers
 * (heartbeat CE climbs) the process lives on - the livelock healed
 * itself; if CE stays flat for the usual window the watchdog exits
 * as before. tripped==1 (legacy park) is kept as the fallback if the
 * brake lap budget (flood[20]) is exhausted, so a brake bug can at
 * worst degrade to the v02.48 behavior, never worse. */
volatile unsigned int vita_v245_hist[17] = {0};
volatile unsigned int vita_v245_flood[24] = {0};
/* v02.50: total brake engagements (pad -> [r6] door). Written before
 * the bx; the watchdog prints it in the v250 line. */
volatile unsigned int vita_v250_brakes = 0;
/* v02.48: the trap path no longer switches stacks (log71: the C
 * dumper's sceIoOpen froze in-kernel when called FROM a lightweight
 * thread - not a real OS thread). The spinner parks on wfe; the
 * watchdog thread performs the dump. This symbol's ADDRESS is the
 * parked-site cookie stored into flood[15] - resolve it against a
 * coredump to name the spin loop. */
volatile unsigned int vita_ring_parked = 0;
}
extern "C" void vita_landing_pad(void) __attribute__((naked));
extern "C" void vita_ring_trap(void);
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
    /* v02.45: bounce history - r0/r1 are dead after the pe store,
     * r2 is reloaded by the gate below. v02.50: layout is now 4 words
     * per crossing (r6, lr, [r6-8], cpe - see the vita_v245_hist
     * comment above); the byte offset is 16*idx: idx -> 2*idx ->
     * 4*idx -> 8*idx -> 16*idx (four doublings, all single-cycle).
     * r3 is the scratch (dead at the stub edge - the stub writes
     * r2/r3 itself before use; it is reloaded from [r6,#-8] below
     * before its sample store). ~22 instructions per crossing,
     * irrelevant next to a ~10M/s spin. */
    "movw  r1, #:lower16:vita_v245_hist\n"
    "movt  r1, #:upper16:vita_v245_hist\n"
    "ldr   r2, [r1]\n"
    "and   r2, r2, #3\n"
    "mov   r3, r2\n"
    "add   r3, r3, r3\n"
    "add   r3, r3, r3\n"
    "add   r3, r3, r3\n"
    "add   r3, r3, r3\n"
    "add   r2, r3, r1\n"
    "str   r6, [r2, #4]\n"
    "str   lr, [r2, #8]\n"
    "ldr   r3, [r6, #-8]\n"
    "str   r3, [r2, #12]\n"
    "movw  r3, #:lower16:_current_pending_exception\n"
    "movt  r3, #:upper16:_current_pending_exception\n"
    "ldr   r3, [r3]\n"
    "str   r3, [r2, #16]\n"
    "ldr   r2, [r1]\n"
    "add   r2, r2, #1\n"
    "str   r2, [r1]\n"
    /* v02.45: flood gate - see vita_v245_flood above. Uses only
     * r0/r1/r2/ip, all dead at the stub edge (stub reloads r0/r1
     * and writes r2/r3 itself; ip is written before read). */
    "movw  r2, #:lower16:vita_v245_flood\n"
    "movt  r2, #:upper16:vita_v245_flood\n"
    "ldr   r1, [r2, #4]\n"              /* already tripped? */
    "cmp   r1, #0\n"
    "bne   8f\n"                         /* armed once, plain bounce */
    "movw  r0, #:lower16:_current_thread\n"
    "movt  r0, #:upper16:_current_thread\n"
    "ldr   r0, [r0]\n"
    "cmp   r0, #0\n"
    "beq   7f\n"                         /* no ct: not a spin */
    "ldr   r1, [r0, #36]\n"              /* pe */
    "cmp   r1, #0\n"
    "bne   7f\n"                         /* pe!=0: reset */
    "ldr   r1, [r2, #0]\n"              /* consec count */
    "add   r1, r1, #1\n"
    "str   r1, [r2, #0]\n"
    "cmp   r1, #256\n"
    "blo   8f\n"
    /* Trip: save the scene, then park. v02.48: the v02.45 design
     * switched sp to a private stack and called the C dumper
     * (vita_ring_trap) which starts with sceIoOpen - and log71
     * proved the kernel freezes that I/O forever when the caller
     * is a lightweight thread (not a real OS thread; psp2core:
     * pc in SceLibKernel, r12=0xb2a sceIo* slot, r0 -> ring_trap.log
     * path, ring_trap.log never appeared). So the trip path now
     * does MEMORY ONLY: registers into the flood array (word layout
     * unchanged, [2..13]), tripped=1 LAST as the handoff flag, then
     * spin right here on a word the watchdog owns. The watchdog
     * (real OS thread, 250ms poll) sees tripped, dumps the scene to
     * ring_trap.log via the existing vita_ring_trap, and exits the
     * process. The spin keeps the cpu exactly where the ring was
     * caught; a dead core would take the watchdog with it (log40
     * proved both stay alive when the VM thread spins). */
    "mov   ip, r2\n"                     /* ip = &flood */
    "mov   r1, #1\n"
    "str   r0, [ip, #8]\n"              /* [2] ct */
    "str   r3, [ip, #12]\n"             /* [3] r3 - NOTE v02.49: r3
     * here holds the [r6-8] hist sample (the hist block above used
     * it as scratch), not the caller's r3 at pad entry - r3 is
     * caller-saved and dead at this edge either way */
    "str   r4, [ip, #16]\n"             /* [4] r4 */
    "str   r5, [ip, #20]\n"             /* [5] r5 */
    "str   r6, [ip, #24]\n"             /* [6] r6 - exit-table base */
    "str   r7, [ip, #28]\n"             /* [7] r7 */
    "str   r8, [ip, #32]\n"             /* [8] r8 */
    "str   r9, [ip, #36]\n"             /* [9] r9 */
    "str   r10, [ip, #40]\n"            /* [10] r10 */
    "str   r11, [ip, #44]\n"            /* [11] fp */
    "mov   r1, sp\n"
    "str   r1, [ip, #48]\n"             /* [12] sp */
    "str   lr, [ip, #52]\n"             /* [13] lr */
    "movw  r0, #:lower16:_current_pending_exception\n"
    "movt  r0, #:upper16:_current_pending_exception\n"
    "ldr   r0, [r0]\n"
    "str   r0, [ip, #56]\n"             /* [14] cpe at trip */
    "movw  r0, #:lower16:vita_ring_parked\n"
    "movt  r0, #:upper16:vita_ring_parked\n"
    "str   r0, [ip, #60]\n"             /* [15] parked-site cookie:
     * the ADDRESS of vita_ring_parked - nm-resolvable in any later
     * coredump to name this spin loop */
    "movw  r0, #:lower16:vita_v250_brakes\n"
    "movt  r0, #:upper16:vita_v250_brakes\n"
    "ldr   r1, [r0]\n"
    "add   r1, r1, #1\n"
    "str   r1, [r0]\n"
    "str   r1, [ip, #80]\n"             /* [20] brake lap count */
    "cmp   r1, #8\n"
    "bhs   9f\n"                         /* brake budget exhausted:
     * fall back to the v02.48 park (tripped=1) below */
    "mov   r1, #2\n"
    "str   r1, [ip, #4]\n"              /* [1] tripped=2 BRAKE - HANDOFF
     * LAST: scene complete; watchdog dumps + monitors, no kill */
    "ldr   r0, [r6]\n"                 /* the designed death door:
     * birth template [0] = lightweight_thread_exit */
    "bx    r0\n"                        /* leave the ring; lw_exit does
     * terminate (op-2 REM, breaks q5 CYCLE) + yield -> live thread.
     * NOT returning: lw_exit ends in current_thread_to_primordial */
    "9:\n"
    "mov   r1, #1\n"
    "str   r1, [ip, #4]\n"              /* [1] tripped=1 - HANDOFF
     * LAST: the scene + cookie were stored BEFORE the brake-budget
     * branch, so nothing else is needed here before the handoff */
    "wfe\n"                             /* park. WFE is a hint: if it
     * degrades to a nop this is a plain spin - still correct, only
     * hotter; the watchdog runs on the other core either way */
    "wfe\n"                             /* second one: first may return
     * immediately off a stale event register bit */
    "b     9b\n"
    "7:\n"
    "mov   r1, #0\n"
    "str   r1, [r2, #0]\n"              /* reset consec count */
    "8:\n"
    "movw  r2, #:lower16:vita_v244_ct\n"   /* v02.44 record (unchanged):
     * ct + r6 for watchdog rd7 when no trip happens. */
    "movt  r2, #:upper16:vita_v244_ct\n"  /* v02.47: restore the movt lost
     * in the v02.45 edit — without it r2 holds only the lower 16 bits
     * (0x9790) and the store below faults on every thread birth
     * (log70 psp2core: pc=str r1,[r2], r2=0x9790). */
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
