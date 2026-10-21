/* Conditional-BL load-time rewrite test - see cond_bl.S for the idea.
 *
 * Reads back, from the LOADED image, the instruction word of every
 * "bl<cond>" site and checks two things:
 *   1. static : cond field, L bit and branch target of the loaded word
 *   2. dynamic: the site was executed with its condition true; the callee
 *               must have been entered with lr = the following instruction
 * Output goes to the screen and to ux0:data/cond_bl_test.log.
 * Press any button to exit.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#include <psp2/ctrl.h>
#include <psp2/kernel/modulemgr.h>
#include <psp2/kernel/processmgr.h>

#include "debugScreen.h"

#define NCOND 15
#define NVAR  3
#define NSITE (NCOND * NVAR)

extern void run_cond_bl_tests(int *result, unsigned *expected_lr, unsigned *observed_lr);
extern const unsigned cond_bl_site_count;
/* { site address, callee address } per site, in execution order */
extern unsigned cond_bl_tab[][2];

static const char *const cond_name[NCOND] = {
    "eq", "ne", "cs", "cc", "mi", "pl", "vs", "vc",
    "hi", "ls", "ge", "lt", "gt", "le", "al"
};
/* ARM condition field value for each entry above */
static const unsigned cond_code[NCOND] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14 };
static const char *const var_name[NVAR] = {
    "A back/same-file", "F fwd/same-file", "E extern-file"
};

static FILE *g_log;

static void out(const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    psvDebugScreenPuts(buf);
    if (g_log) {
        fputs(buf, g_log);
        fflush(g_log);
    }
}

int main(void)
{
    static int      result[NSITE];
    static unsigned expected[NSITE];
    static unsigned observed[NSITE];
    int i, bad_word = 0, bad_run = 0, bad_target = 0, bad_l = 0;
    SceKernelSystemSwVersion ver;
    SceCtrlData pad;

    psvDebugScreenInit();
    g_log = fopen("ux0:data/cond_bl_test.log", "w");

    memset(&ver, 0, sizeof(ver));
    ver.size = sizeof(ver);
    if (sceKernelGetSystemSwVersion(&ver) < 0)
        strcpy(ver.versionString, "?");

    out("cond_bl_test: bl<cond> as loaded on this device\n");
    out("firmware %s   sites %u (expected %d)\n", ver.versionString,
        cond_bl_site_count, NSITE);

    if (cond_bl_site_count != NSITE) {
        out("site count mismatch - test image is inconsistent\n");
    } else {
        run_cond_bl_tests(result, expected, observed);

        out("\n var  cond  loaded-word   L cond tgt  run  lr(obs vs exp)\n");
        for (i = 0; i < NSITE; i++) {
            const unsigned site   = cond_bl_tab[i][0];
            const unsigned callee = cond_bl_tab[i][1];
            const int v = i / NCOND, c = i % NCOND;
            const uint32_t w = *(volatile uint32_t *)site;
            const int32_t imm = ((int32_t)(w << 8)) >> 6;  /* sext(imm24) << 2 */
            const unsigned target = site + 8 + (unsigned)imm;
            const unsigned wcond = w >> 28;
            const unsigned wL = (w >> 24) & 1;
            const unsigned wop = (w >> 25) & 7;            /* 101 = B/BL */
            int w_ok   = (wop == 5) && (wcond == cond_code[c]);
            int l_ok   = (wL == 1);
            int tgt_ok = (target == callee);
            int run_ok = (result[i] == 1) && (observed[i] == expected[i]);

            if (!w_ok)   bad_word++;
            if (!l_ok)   bad_l++;
            if (!tgt_ok) bad_target++;
            if (!run_ok) bad_run++;

            out(" %c    %s    %08x    %u  %x   %s  %s  %08x/%08x%s\n",
                "AFE"[v], cond_name[c], (unsigned)w, wL, wcond,
                tgt_ok ? "ok" : "BAD",
                result[i] == 1 ? "PASS" : (result[i] == 2 ? "FAIL" : "??? "),
                observed[i], expected[i],
                (w_ok && l_ok && tgt_ok && run_ok) ? "" : "   <<<");
        }

        out("\nsummary: L-bit cleared %d, wrong cond/opcode %d, wrong target %d, "
            "call did not link %d  (of %d)\n",
            bad_l, bad_word, bad_target, bad_run, NSITE);
        /* per-condition verdict, summed over the three variants */
        out("per cond (bad sites out of 3):");
        for (i = 0; i < NCOND; i++) {
            int n = 0, v;
            for (v = 0; v < NVAR; v++) {
                const int k = v * NCOND + i;
                const uint32_t w = *(volatile uint32_t *)cond_bl_tab[k][0];
                if (((w >> 24) & 1) == 0 || result[k] != 1) n++;
            }
            if (n) out("  %s=%d", cond_name[i], n);
        }
        out("%s\n", (bad_l || bad_run || bad_word || bad_target) ? "" : "  none");
        (void)var_name;
    }

    out("\npress any button to exit (log: ux0:data/cond_bl_test.log)\n");
    if (g_log) {
        fclose(g_log);
        g_log = NULL;
    }
    for (;;) {
        memset(&pad, 0, sizeof(pad));
        sceCtrlPeekBufferPositive(0, &pad, 1);
        if (pad.buttons)
            break;
        sceKernelDelayThread(50 * 1000);
    }
    sceKernelExitProcess(0);
    return 0;
}
