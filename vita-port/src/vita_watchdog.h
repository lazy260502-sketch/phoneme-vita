/*
 * vita_watchdog.h - hang forensics watchdog (v01.72)
 *
 * The watchdog samples the event-pump counter; when it stalls >3 s
 * during a MIDlet round it snapshots the VM/tone threads' kernel wait
 * state into ux0:/data/J2ME00001/watchdog.log. See vita_watchdog.c.
 */
#ifndef VITA_WATCHDOG_H
#define VITA_WATCHDOG_H

#include <psp2/kernel/threadmgr.h>

/* 1 while a MIDlet round runs (menu = 0). vita_main.c sets this. */
extern volatile int vita_wd_round_active;
/* VM thread id, recorded by vita_main.c when the round starts. */
extern volatile SceUID vita_wd_vm_tid;

/* Start the watchdog thread (call once after media init). */
void vita_watchdog_start(void);

#endif /* VITA_WATCHDOG_H */
