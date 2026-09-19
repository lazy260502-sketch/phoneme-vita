# Minimal AsmStubs for x86_64 host build
# phoneME CLDC uses these only for x86 strictfp; x86_64 uses SSE2 natively
.section .text
.align 16
.globl JFP_lib_dmul_x86
JFP_lib_dmul_x86:
    ret
.globl JFP_lib_ddiv_x86
JFP_lib_ddiv_x86:
    ret
.globl end_of_the_world
end_of_the_world:
