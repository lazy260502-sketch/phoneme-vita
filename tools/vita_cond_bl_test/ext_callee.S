@ Callee for variant E of cond_bl.S: a global function in a different object
@ file, so the branch to it can only be resolved by the linker.
    .syntax unified
    .arm
    .text
    .global ext_callee
    .type   ext_callee, %function
ext_callee:
    str     lr, [r8]
    bx      lr
    .size   ext_callee, .-ext_callee
