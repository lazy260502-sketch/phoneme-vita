#ifndef VITA_MENU_H
#define VITA_MENU_H

#include <setjmp.h>

typedef struct {
    char jar[256];   /* absolute path of the game jar */
    char cls[128];   /* MIDlet class name */
    char orient[32]; /* "portrait" or "landscape" */
} VitaGameSel;

/* v01.78h: the menu does NOT return normally. Forensics on the v01.78g
 * quit crash proved the crash PC was vita_menu_run's epilogue
 * `ldmia sp!, {r4,r5,r7,r8,r9,sl,fp,pc}` with every popped register
 * reading 0: the saved-register block at entrySP-32 gets zeroed by
 * something during the session and is only READ by that pop. So the
 * menu now leaves via longjmp(vita_menu_escape, 1) and the caller arms
 * it with setjmp. The jmp_buf lives in .bss, out of reach of whatever
 * walks the stack. Read the menu's answer from vita_menu_result after
 * longjmp returns. */
extern jmp_buf vita_menu_escape;
extern int vita_menu_result;

/* Runs the native game menu. Returns 1 when a game was picked (fills
 * *out), 0 when the user quit without a selection - via vita_menu_result
 * when the longjmp path is taken (the C return value is unreachable). */
int vita_menu_run(VitaGameSel *out);

#endif
