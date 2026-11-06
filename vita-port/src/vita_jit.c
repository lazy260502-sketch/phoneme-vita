/*
 * vita_jit.c - per-round JIT policy and a JIT probe.
 *
 * Why this exists (log119 / PROJECT_MEMORY "JIT 实际没有生效"):
 * runMidlet.c injects "-int" for policy 0. That sets UseCompiler = false
 * AND, in ObjectHeap::initialize(), forces CompilerAreaPercentage and
 * CompilerAreaSlack to 0 (ObjectHeap.cpp:1177). Both are process globals,
 * and the only code that sets UseCompiler back to true is the "-comp"
 * option, which nobody passes. So once ANY round ran with policy 0, every
 * later round - including those configured jit=2 - stayed an interpreter.
 *
 * vita_jit_round_begin() restores the three values captured at start-up
 * before each round whose policy is not 0 (and forces them off for policy
 * 0, so the default behaviour is exactly what it was). It only restores
 * what the VM itself had as defaults: nothing here makes the JIT any more
 * or less safe - see the NOTE below about executable memory.
 *
 * NOTE (not solved here): the JIT writes code into the Java heap
 * (compiler_area) which is plain malloc memory, and OsMisc_flush_icache is
 * a "bx lr". Whether code generated there can run on a real Vita is the
 * open question the probe below is meant to answer.
 */
#include <stdio.h>
#include <string.h>

#include <psp2/kernel/sysmem.h>

#include "vita_jit.h"
#include "vita_crumb.h"

/* VM globals (phoneme-cldc Globals.hpp, plain C++ globals, unmangled) */
extern unsigned char UseCompiler __attribute__((weak));
extern unsigned char MixedMode __attribute__((weak));
extern int CompilerAreaPercentage __attribute__((weak));
extern int CompilerAreaSlack __attribute__((weak));

/* probe counters (Compiler.cpp, VITA block) */
extern volatile unsigned int vita_jit_try_count __attribute__((weak));
extern volatile unsigned int vita_jit_ok_count __attribute__((weak));
extern volatile unsigned int vita_jit_first_code __attribute__((weak));
extern int vita_jit_area_size(void) __attribute__((weak));
extern int vita_jit_area_used(void) __attribute__((weak));

static int g_captured = 0;
static unsigned char g_use_compiler0;
static unsigned char g_mixed_mode0;
static int g_area_pct0;
static int g_area_slack0;

static int vm_has_compiler_globals(void) {
    return &UseCompiler != NULL && &CompilerAreaPercentage != NULL &&
           &CompilerAreaSlack != NULL && &MixedMode != NULL;
}

void vita_jit_round_begin(int round, int policy) {
    if (!vm_has_compiler_globals()) {
        crumb_printf("jit: VM compiler globals not linked, nothing to do");
        return;
    }
    if (!g_captured) {
        /* First call happens before any round, i.e. before "-int" could
         * have touched anything: these are the VM's own defaults. */
        g_use_compiler0 = UseCompiler;
        g_mixed_mode0 = MixedMode;
        g_area_pct0 = CompilerAreaPercentage;
        g_area_slack0 = CompilerAreaSlack;
        g_captured = 1;
        crumb_printf("jit: captured defaults UseCompiler=%d MixedMode=%d "
                     "CompilerAreaPercentage=%d CompilerAreaSlack=%d",
                     (int)g_use_compiler0, (int)g_mixed_mode0,
                     g_area_pct0, g_area_slack0);
    }
    if (policy != 0) {
        UseCompiler = g_use_compiler0;
        MixedMode = g_mixed_mode0;
        CompilerAreaPercentage = g_area_pct0;
        CompilerAreaSlack = g_area_slack0;
        crumb_printf("jit: round %d policy %d -> restored UseCompiler=%d "
                     "area=%d%%", round, policy, (int)UseCompiler,
                     CompilerAreaPercentage);
    }
    /* policy 0: runMidlet.c passes "-int" itself; nothing to do. */
}

void vita_jit_round_end(int round) {
    SceKernelMemBlockInfo bi;
    unsigned int tries, oks, first;
    if (&vita_jit_try_count == NULL) {
        return;
    }
    tries = vita_jit_try_count;
    oks = vita_jit_ok_count;
    first = vita_jit_first_code;
    crumb_printf("jit: round %d done: UseCompiler=%d compile-requests=%u "
                 "produced-code=%u", round,
                 (&UseCompiler == NULL) ? -1 : (int)UseCompiler, tries, oks);
    if (&vita_jit_area_size != NULL && &vita_jit_area_used != NULL) {
        /* The Java heap is torn down at the end of a round, so the area
         * numbers describe the state just after it; they are mostly
         * useful with jit=2 to see that the area was ever sized. */
        crumb_printf("jit: compiler area size=%d used=%d",
                     vita_jit_area_size(), vita_jit_area_used());
    }
    if (first != 0) {
        memset(&bi, 0, sizeof(bi));
        bi.size = sizeof(bi);
        /* access: 1=X 2=W 4=R (SceKernelMemoryAccessType). If the block
         * that holds the first generated code has no X bit, that code can
         * not run on this system - the key fact for the JIT question. */
        int rc = sceKernelGetMemBlockInfoByAddr((void *)(first & ~0xFFFu),
                                                &bi);
        crumb_printf("jit: first code @0x%08x block-info rc=0x%08x "
                     "access=0x%x type=0x%08x base=%p size=%u",
                     first, (unsigned)rc, (unsigned)bi.access,
                     (unsigned)bi.type, bi.mappedBase,
                     (unsigned)bi.mappedSize);
    }
}
