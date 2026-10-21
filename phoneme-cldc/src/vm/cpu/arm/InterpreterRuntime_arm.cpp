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
}
/* v02.41: register/exit-table recorder. log85 (v02.40) showed the VM
 * thread is NOT frozen by the kernel: vita_v240_land climbs ~14M/s while
 * switch/ttk/site_seq stay constant, i.e. a pure userland loop through
 * this pad (pc=pad+0x18, lr=pad entry, fp=0). The loop is all asm, so
 * no C++ probe sees it. The pad now records a full register + exit-table
 * snapshot for entries land=1..8 (baseline: healthy handoffs) and
 * land=VITA_V241_TRIP..+7 (inside the loop). Comparing the two sets
 * shows whether r6/jsp, the exit-table words ([r6+0..16]) and the
 * current thread fields (pending_entries/stack_pointer/status) differ
 * between a healthy handoff and a looping one, and whether the loop is
 * a fixed cycle (consecutive loop records identical) or drifting.
 * Behaviour is unchanged: the pad still forwards to the trampoline.
 *
 * Record layout, 24 words (96 bytes) per record:
 *   0 land   1..9 r4..r12   10 sp   11 lr
 *   12..16 [r6+0],[r6+4],[r6+8],[r6+12],[r6+16]
 *   17 cur_thread   18 cur->stack_pointer(+0x64)  19 cur->status(+0x68)
 *   20 cur->pending_entries(+0x24)
 *   21 [gp+1040] pending exception   22 [cur.sp]   23 [cur.sp+4]   (v02.50)
 * The thread-field offsets are read off the v02.40 binary (trampoline:
 * str r2,[r0,#36]; ldr r6,[r1,#100]; ldr r1,[r0,#104]) because a naked
 * function may only contain basic asm (no operands for offsetof).
 * sp is still the primordial stack here (green-thread model), so a
 * balanced push/pop of two scratch registers is safe. The slow path is
 * only taken for 16 distinct land values; every other entry costs the
 * same handful of instructions as v02.40. */
#define VITA_V241_TRIP 1000000
extern "C" {
volatile unsigned int vita_v241_rec[16][24];
}
/* v02.44: loop detector (debug experiment, answers "does the shadow break
 * the loop on its FIRST lap, or only after ~1e6?").
 * A healthy thread enters the pad at most twice back to back with the same
 * (current thread, r6): once at birth and once at death (log88 rec[4] vs
 * rec[5] show exactly this: same r6, cur.sp moved). The log85/86/88/90 loop
 * enters it forever with that same pair. So the 3rd consecutive entry with
 * an unchanged (cur, r6) - rep >= VITA_V243_REP_TRIP - is a loop, and the
 * pad diverts to the shadow trampoline at once instead of waiting for
 * land >= VITA_V241_TRIP. The old land threshold stays as a fallback.
 * Reading (exit_probe.log / watchdog "v243" line):
 *   trip_rep small, trip_land ~ 7, land stays tiny  -> the shadow breaks
 *       the loop on its first lap: same state, different code path.
 *   trip_rep huge / land >= 1e6 anyway               -> the shadow loops
 *       too; the earlier "rescue after 1e6 laps" is not a code difference.
 * rep_trip counts diverts; trip_land is `land` at the first divert. */
#define VITA_V243_REP_TRIP 2
#define V243_STR2(x) #x
#define V243_STR(x)  V243_STR2(x)   /* expand the macro, then stringify */
extern "C" {
volatile unsigned int vita_v243_last_cur = 0;
volatile unsigned int vita_v243_last_r6  = 0;
volatile unsigned int vita_v243_rep      = 0;
volatile unsigned int vita_v243_trip_rep = 0;
volatile unsigned int vita_v243_trip_land = 0;
}
/* v02.47: defined in OS_vita.cpp (declared in Globals_vita.hpp). */
extern "C" volatile int vita_v247_nodivert;
extern "C" void vita_landing_pad(void) __attribute__((naked));
extern "C" void vita_landing_pad(void) {
  asm volatile(
    "movw  r0, #:lower16:vita_v240_land\n"
    "movt  r0, #:upper16:vita_v240_land\n"
    "ldr   r1, [r0]\n"
    "add   r1, r1, #1\n"
    "str   r1, [r0]\n"
    /* record slot selection: land 1..8 -> 0..7 */
    "sub   r0, r1, #1\n"
    "cmp   r0, #8\n"
    "blo   1f\n"
    /* land TRIP..TRIP+7 -> 8..15 */
    "movw  r0, #0x4240\n"       /* VITA_V241_TRIP = 1000000 = 0x000F4240 */
    "movt  r0, #0x000f\n"
    "sub   r0, r1, r0\n"
    "cmp   r0, #8\n"
    "bhs   3f\n"
    "add   r0, r0, #8\n"
    "1:\n"
    "push  {r2, r3}\n"
    "movw  r2, #:lower16:vita_v241_rec\n"
    "movt  r2, #:upper16:vita_v241_rec\n"
    "mov   r3, #96\n"
    "mla   r2, r0, r3, r2\n"
    "str   r1, [r2, #0]\n"
    "str   r4, [r2, #4]\n"
    "str   r5, [r2, #8]\n"
    "str   r6, [r2, #12]\n"
    "str   r7, [r2, #16]\n"
    "str   r8, [r2, #20]\n"
    "str   r9, [r2, #24]\n"
    "str   r10, [r2, #28]\n"
    "str   r11, [r2, #32]\n"
    "str   r12, [r2, #36]\n"
    "add   r3, sp, #8\n"
    "str   r3, [r2, #40]\n"
    "str   lr, [r2, #44]\n"
    "ldr   r3, [r6]\n"
    "str   r3, [r2, #48]\n"
    "ldr   r3, [r6, #4]\n"
    "str   r3, [r2, #52]\n"
    "ldr   r3, [r6, #8]\n"
    "str   r3, [r2, #56]\n"
    "ldr   r3, [r6, #12]\n"
    "str   r3, [r2, #60]\n"
    "ldr   r3, [r6, #16]\n"
    "str   r3, [r2, #64]\n"
    "movw  r3, #:lower16:_current_thread\n"
    "movt  r3, #:upper16:_current_thread\n"
    "ldr   r3, [r3]\n"
    "str   r3, [r2, #68]\n"
    /* v02.50: word 21 = [gp+1040], the pending exception the real
     * trampoline tests (cmp r1,#0 -> uncaught_exception call). It was the
     * one trampoline input NOT in this record: log101 shows the loop state
     * is a fixed point with every recorded input healthy, yet Thread::finish
     * is never called. gp comes from the symbol, NOT r5: r5 is documented
     * undefined on the birth path. r0 is scratch here. */
    "movw  r0, #:lower16:gp_base_label\n"
    "movt  r0, #:upper16:gp_base_label\n"
    "ldr   r0, [r0, #1040]\n"
    "str   r0, [r2, #84]\n"
    "cmp   r3, #0\n"
    "beq   2f\n"
    "ldr   r0, [r3, #104]\n"
    "str   r0, [r2, #76]\n"
    "ldr   r0, [r3, #36]\n"
    "str   r0, [r2, #80]\n"
    "ldr   r0, [r3, #100]\n"
    "str   r0, [r2, #72]\n"
    /* v02.50: words 22/23 = [cur.sp], [cur.sp+4]. call_on_primordial_stack's
     * tail does ldr r6,[cur.sp]; ldr lr,[r6,#4]; mov pc,lr, i.e. it "returns"
     * to whatever sits in that slot. If [cur.sp+4] is the pad sentinel, any
     * such return lands back here - the hypothesis these two words test.
     * Guarded: non-null and word aligned (cur.sp is VM-written, in the
     * green-thread stack). r3 (cur) is dead after this point. */
    "cmp   r0, #0\n"
    "beq   2f\n"
    "tst   r0, #3\n"
    "bne   2f\n"
    "ldr   r3, [r0]\n"
    "str   r3, [r2, #88]\n"
    "ldr   r3, [r0, #4]\n"
    "str   r3, [r2, #92]\n"
    "2:\n"
    "pop   {r2, r3}\n"
    "3:\n"
    "movw  r0, #:lower16:vita_v240_fp\n"
    "movt  r0, #:upper16:vita_v240_fp\n"
    "str   r11, [r0]\n"
    "movw  r0, #:lower16:vita_v240_lr\n"
    "movt  r0, #:upper16:vita_v240_lr\n"
    "str   lr, [r0]\n"
    /* v02.44 loop detector: rep = number of consecutive entries whose
     * (current thread, r6) equal the previous entry's. r2/r3 are saved
     * around it; r1 (land) is untouched; r0 leaves holding rep. */
    "push  {r2, r3}\n"
    "movw  r2, #:lower16:_current_thread\n"
    "movt  r2, #:upper16:_current_thread\n"
    "ldr   r2, [r2]\n"                  /* r2 = current thread */
    "movw  r3, #:lower16:vita_v243_last_cur\n"
    "movt  r3, #:upper16:vita_v243_last_cur\n"
    "ldr   r0, [r3]\n"
    "cmp   r0, r2\n"
    "bne   5f\n"
    "movw  r3, #:lower16:vita_v243_last_r6\n"
    "movt  r3, #:upper16:vita_v243_last_r6\n"
    "ldr   r0, [r3]\n"
    "cmp   r0, r6\n"
    "bne   5f\n"
    "movw  r3, #:lower16:vita_v243_rep\n"
    "movt  r3, #:upper16:vita_v243_rep\n"
    "ldr   r0, [r3]\n"
    "add   r0, r0, #1\n"
    "str   r0, [r3]\n"
    "b     6f\n"
    "5:\n"                              /* new (cur, r6): restart count */
    "movw  r3, #:lower16:vita_v243_last_cur\n"
    "movt  r3, #:upper16:vita_v243_last_cur\n"
    "str   r2, [r3]\n"
    "movw  r3, #:lower16:vita_v243_last_r6\n"
    "movt  r3, #:upper16:vita_v243_last_r6\n"
    "str   r6, [r3]\n"
    "movw  r3, #:lower16:vita_v243_rep\n"
    "movt  r3, #:upper16:vita_v243_rep\n"
    "mov   r0, #0\n"
    "str   r0, [r3]\n"
    "6:\n"
    "pop   {r2, r3}\n"
    /* v02.47: nodivert switch (pad_nodivert.cfg). When set, skip BOTH
     * divert rules and run the REAL trampoline every time, so the original
     * FileManager exit loop can be reproduced on purpose and caught by the
     * watchdog's early dump. r2 is saved around the load; pop does not
     * touch the flags, so the cmp below still decides the branch. */
    "push  {r2}\n"
    "movw  r2, #:lower16:vita_v247_nodivert\n"
    "movt  r2, #:upper16:vita_v247_nodivert\n"
    "ldr   r2, [r2]\n"
    "cmp   r2, #0\n"
    "pop   {r2}\n"
    "bne   12f\n"                   /* v02.54: nodivert -> sentinels first */
    /* v02.58 ROOT-CAUSE FIX. log107 (v02.57 read-back of the loaded code):
     * the real trampoline's "bleq invoke_pending_entries" is 0x0affff42 in
     * memory but 0x0bffff42 in the file - the loader dropped the L (link)
     * bit of this one instruction (SCE reloc code 29 = R_ARM_JUMP24; the
     * sibling blne 0x1bffff48 and the unconditional bl 0xebffff46 are
     * intact). With L lost, lr stays = this pad, so invoke_pending_entries'
     * "moveq pc,lr" (pe==0) returns HERE instead of to trampoline+0x1c: pad
     * -> trampoline -> beq ipe -> pad ... forever, call_on_primordial_stack
     * (-> Thread::finish) is never reached. That is the FileManager exit
     * loop; every earlier detector/fallback only ever escaped it by running
     * the shadow, whose branches are bne + UNCONDITIONAL bl (R_ARM_CALL).
     * So run the shadow always. The real trampoline stays reachable only
     * via pad_nodivert.cfg (diagnostics). The loop detector and the land
     * threshold below are now dead code on this path and kept only so the
     * pad can be flipped back by deleting this one branch. */
    "b     4f\n"
    /* r0 = rep, r1 = land. Loop detected -> shadow trampoline now. */
    "cmp   r0, #" V243_STR(VITA_V243_REP_TRIP) "\n"
    "bhs   7f\n"
    /* v02.42 fallback: r1 still holds land here on every path. Once the
     * land threshold is crossed, divert to the instrumented shadow
     * trampoline instead of the real one (see below). */
    "movw  r0, #0x4240\n"       /* VITA_V241_TRIP = 0x000F4240 */
    "movt  r0, #0x000f\n"
    "cmp   r1, r0\n"
    "bhs   4f\n"
    "9:\n"                          /* v02.47: nodivert lands here */
    "movw  r0, #:lower16:start_lightweight_thread_asm\n"
    "movt  r0, #:upper16:start_lightweight_thread_asm\n"
    "bx    r0\n"
    "4:\n"
    "b     vita_tramp_shadow\n"
    /* v02.54 SENTINEL EXPERIMENT (nodivert mode only; every other path
     * reaches 9: without touching these registers). log101/log102: the loop
     * is a fixed point, Thread::finish never runs (its counter is the first
     * statement of the function), yet the trampoline's code says that with
     * pe==0, [r6+16]==0 and pendexc==0 it must reach
     * call_on_primordial_stack(Thread::finish). Pad records can't say how
     * far the real trampoline gets, so mark two registers it overwrites on
     * the way and read them back at the next pad entry (rec[i] r5 / r12):
     *   r12 = 0xdead0c12 : call_on_primordial_stack's "mov ip,#12; add
     *         ip,ip,#0x400" never ran -> r12 stays the sentinel;
     *         0x40c if it did run (finish may then have clobbered ip, so a
     *         value other than the sentinel means "ran or something else
     *         wrote r12").
     *   r5  = 0xdead0505 : the trampoline's first insn is ldr r5,[pc,#120]:
     *         gp 0x81xx18f4 (slid) = ran and the literal is relocated;
     *         0x813018f4 (unslid) = ran but the literal is NOT relocated;
     *         sentinel = it never even started.
     * r5 is documented undefined at pad entry (the trampoline initialises
     * it itself) and r12/ip is a scratch register, so neither is an input. */
    "12:\n"
    "movw  r5, #0x0505\n"
    "movt  r5, #0xdead\n"
    "movw  r12, #0x0c12\n"
    "movt  r12, #0xdead\n"
    "b     9b\n"
    "7:\n"                              /* loop-detector divert */
    "push  {r2, r3}\n"
    "movw  r2, #:lower16:vita_v243_trip_rep\n"
    "movt  r2, #:upper16:vita_v243_trip_rep\n"
    "ldr   r3, [r2]\n"
    "add   r3, r3, #1\n"
    "str   r3, [r2]\n"
    "cmp   r3, #1\n"
    "bne   8f\n"
    "movw  r2, #:lower16:vita_v243_trip_land\n"
    "movt  r2, #:upper16:vita_v243_trip_land\n"
    "str   r1, [r2]\n"                  /* land at the first divert */
    "8:\n"
    "pop   {r2, r3}\n"
    "b     vita_tramp_shadow\n"
  );
}

/* v02.42: instrumented shadow of start_lightweight_thread_asm.
 *
 * log86 (v02.41) proved the loop is a fixed cycle (8 consecutive records
 * at land=1000000.. identical in every field) with healthy exit table
 * ([r6+0..16] = lightweight_thread_exit / uncaught_exception / finish /
 * force_terminated / 0), cur.st=0 and cur.pe=0. By the trampoline's own
 * code that state must reach Thread::finish, yet finish=0 / lw_exit=0
 * after 2.6e8 laps. The trampoline lives in the loopgen-frozen
 * Interpreter_arm.o and cannot carry probes, so this function replays it
 * instruction for instruction (v02.41 binary, start_lightweight_thread_asm
 * .. L444) with a counter bump between stages.
 *
 * DESIGN NOTE (corrected in v02.43; the v02.42 wording was too strong):
 * call_on_primordial_stack pushes lr/fp on the Java stack, stores that
 * sp in cur->stack_pointer, calls the target, and on return reloads
 * jsp from the CURRENT thread's stack_pointer and jumps to [jsp+4].
 * For a target that does not switch threads (finish, force_terminated)
 * that is the caller's next instruction. Only when the target switches
 * threads (lightweight_thread_exit) does it resume another thread's
 * saved point. Probes sit BEFORE the step they describe, which stays
 * valid either way. Reading: stage[0] = entries; a "before X" count
 * equal to stage[0] means every lap reached X; the first stage whose
 * count falls below stage[0] marks where laps leave the trampoline body
 * (a taken branch, or a preceding call that switched threads / never
 * came back).
 *
 * Replaced literals: ldr r5,=0x813018ec -> &gp_base_label,
 * 0x810abc94 -> thread_task_cleanup, 0x814d4470 -> &_next_runnable_thread
 * (verified against the literal pool in the v02.41 binary).
 * vita_v242_stage word index:
 *   0 entry          1 before ipe#1     2 before TestCompiler test
 *   3 before uncaught-call (pending exc only)   4 before finish call
 *   5 before force_terminated call      6 before task_cleanup call
 *   7 before ipe#3   8 L444 / tail join  9 before lw_exit call
 *  10 r6 at last entry   11 lr at last entry
 *  13 [gp+1040] (pending exception) at last entry */
extern "C" {
volatile unsigned int vita_v242_stage[16];
}
/* Bump stage[idx]. Clobbers r2/r3 only. r0/r1 (live arguments) are
 * untouched, and the flags survive (add without S, ldr/str). */
#define V242_PROBE(idx) \
    "movw  r2, #:lower16:vita_v242_stage+" #idx "*4\n" \
    "movt  r2, #:upper16:vita_v242_stage+" #idx "*4\n" \
    "ldr   r3, [r2]\n" \
    "add   r3, r3, #1\n" \
    "str   r3, [r2]\n"
extern "C" void vita_tramp_shadow(void) __attribute__((naked));
extern "C" void vita_tramp_shadow(void) {
  asm volatile(
    V242_PROBE(0)
    "str   r6, [r2, #40]\n"
    "str   lr, [r2, #44]\n"
    "movw  r5, #:lower16:gp_base_label\n"
    "movt  r5, #:upper16:gp_base_label\n"
    "ldr   r3, [r5, #1040]\n"
    "str   r3, [r2, #52]\n"
    "ldr   r0, [r5, #1036]\n"
    "ldr   r1, [r0, #104]\n"
    "mov   r2, #0\n"
    "tst   r1, #0x40000000\n"
    "strne r2, [r0, #36]\n"
    "bne   8f\n"
    V242_PROBE(1)
    "ldr   r0, [r5, #1036]\n"
    "bl    invoke_pending_entries\n"
    "8:\n"
    V242_PROBE(2)
    "ldr   r0, [r6, #16]\n"
    "ldr   r1, [r5, #1040]\n"
    "cmp   r0, #0\n"
    "bne   9f\n"
    "cmp   r1, #0\n"
    "beq   7f\n"
    V242_PROBE(3)
    "ldr   r0, [r6, #4]\n"
    "bl    call_on_primordial_stack\n"
    "7:\n"
    V242_PROBE(4)
    "ldr   r0, [r6, #8]\n"
    "bl    call_on_primordial_stack\n"
    "ldr   r0, [r5, #1036]\n"
    "bl    invoke_pending_entries\n"
    V242_PROBE(5)
    "ldr   r0, [r6, #12]\n"
    "bl    call_on_primordial_stack\n"
    V242_PROBE(6)
    "movw  r0, #:lower16:thread_task_cleanup\n"
    "movt  r0, #:upper16:thread_task_cleanup\n"
    "bl    call_on_primordial_stack\n"
    V242_PROBE(7)
    "ldr   r0, [r5, #1036]\n"
    "bl    invoke_pending_entries\n"
    "9:\n"
    V242_PROBE(8)
    V242_PROBE(9)
    "ldr   r0, [r6]\n"
    "bl    call_on_primordial_stack\n"
    "movw  r0, #:lower16:_next_runnable_thread\n"
    "movt  r0, #:upper16:_next_runnable_thread\n"
    "ldr   r0, [r0]\n"
    "cmp   r0, #0\n"
    "svcne 0x9f0001\n"
    "bl    current_thread_to_primordial_fast\n"
    "svc   0x9f0001\n"
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
