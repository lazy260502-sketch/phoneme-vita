@ Conditional-BL load-time rewrite test (ARM state).
@
@ Background (see PROJECT_MEMORY.md, v02.58): on a real Vita the loaded image
@ had the trampoline's "bleq invoke_pending_entries" as 0x0affff42 (L bit
@ cleared, i.e. a plain beq) although the file / ld / vita-elf-create /
@ vita-make-fself output all say 0x0bffff42. "blne" and an unconditional "bl"
@ in the same function were intact. This file puts one "bl<cond>" for EVERY
@ ARM condition code (eq ne cs cc mi pl vs vc hi ls ge lt gt le + al) into a
@ real executable, so the loader's treatment of each can be read back.
@
@ Every site is *executed* with the flags set so the condition is TRUE, and
@ lr is preset (adr lr, fail) to a failure label before the call:
@   - link bit intact : callee sees lr = the instruction after the bl,
@                       returns there -> result 1 (PASS)
@   - link bit lost   : callee sees lr = our preset fail label, returns
@                       there -> result 2 (FAIL)
@ so a lost L bit cannot hang the test.
@
@ Three variants, to separate "which condition" from "where the target is":
@   A : callee is a global symbol in THIS file, placed BEFORE the sites
@       (backward branch, ~0x300 bytes+ away)  <- same shape as the real bug
@   F : callee is a global symbol in THIS file, placed AFTER the sites
@       (forward branch)
@   E : callee is in another object file (ext_callee.S)
@ A branch to a *global* symbol is left to the linker, so it carries a
@ relocation (R_ARM_JUMP24 for conditional, R_ARM_CALL for unconditional) -
@ exactly the type pair seen in the real image. Local labels would be
@ resolved by the assembler with no relocation and would test nothing.
@
@ Registers: r9 = result[], r10 = expected_lr[], r11 = observed_lr[]
@ (indexed by site number), r8 = &observed_lr[i] for the current site.

    .syntax unified
    .arm

@ { site address, callee address } per site, appended by SITE in order.
@ The symbol sits first in the section; later .pushsection blocks follow it.
    .pushsection .data.cond_bl_tab, "aw"
    .global cond_bl_tab
    .align  2
cond_bl_tab:
    .popsection

    .text

    .global callee_A
    .type   callee_A, %function
callee_A:
    str     lr, [r8]
    bx      lr
    .size   callee_A, .-callee_A

    @ ~0x300 bytes of padding, same order of magnitude as the real
    @ "bleq" -> invoke_pending_entries distance (0x2e8 bytes back).
    .space  0x2f0

@ SITE tag, sfx, name, flags
@   tag   : A / F / E (variant, only used for labels)
@   sfx   : condition suffix ("" for the unconditional control)
@   name  : label suffix
@   flags : NZCV nibble (N=8 Z=4 C=2 V=1) that makes the condition true
@   target: symbol to branch to
    .macro SITE tag, sfx, name, flags, target
    mov     r4, #((\flags) << 28)
    msr     APSR_nzcvq, r4
    add     r8, r11, #(cnt * 4)
    adr     lr, 9f
site_\tag\()_\name:
    bl\sfx  \target
ret_\tag\()_\name:
    mov     r5, #1
    b       8f
9:  mov     r5, #2
8:  str     r5, [r9, #(cnt * 4)]
    adr     r5, ret_\tag\()_\name
    str     r5, [r10, #(cnt * 4)]
    .pushsection .data.cond_bl_tab, "aw"
    .word   site_\tag\()_\name
    .word   \target
    .popsection
    .set    cnt, cnt + 1
    .endm

    .macro ALL tag, target
    SITE \tag, eq, eq, 0x4, \target
    SITE \tag, ne, ne, 0x0, \target
    SITE \tag, cs, cs, 0x2, \target
    SITE \tag, cc, cc, 0x0, \target
    SITE \tag, mi, mi, 0x8, \target
    SITE \tag, pl, pl, 0x0, \target
    SITE \tag, vs, vs, 0x1, \target
    SITE \tag, vc, vc, 0x0, \target
    SITE \tag, hi, hi, 0x2, \target
    SITE \tag, ls, ls, 0x0, \target
    SITE \tag, ge, ge, 0x0, \target
    SITE \tag, lt, lt, 0x8, \target
    SITE \tag, gt, gt, 0x0, \target
    SITE \tag, le, le, 0x4, \target
    SITE \tag, , al, 0x0, \target
    .endm

    .set    cnt, 0

@ void run_cond_bl_tests(int *result, unsigned *expected_lr, unsigned *observed_lr);
    .global run_cond_bl_tests
    .type   run_cond_bl_tests, %function
run_cond_bl_tests:
    push    {r4-r11, lr}
    mov     r9, r0
    mov     r10, r1
    mov     r11, r2
    ALL     A, callee_A
    ALL     F, callee_F
    ALL     E, ext_callee
    pop     {r4-r11, pc}
    .size   run_cond_bl_tests, .-run_cond_bl_tests

    .space  0x2f0

    .global callee_F
    .type   callee_F, %function
callee_F:
    str     lr, [r8]
    bx      lr
    .size   callee_F, .-callee_F

    .global cond_bl_site_count
    .section .rodata
    .align  2
cond_bl_site_count:
    .word   cnt
