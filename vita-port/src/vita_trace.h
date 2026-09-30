/*
 * vita_trace.h - v02.27 syscall entry/exit tracer (see vita_trace.c).
 *
 * Zero-I/O ring: wrappers call vita_trace_enter() before __real_* and
 * vita_trace_exit() after. The watchdog dumps slots on HANG; a slot
 * whose seq bit0 is 1 marks a thread INSIDE that call - the deadpoint
 * candidate when the process is frozen.
 */
#ifndef VITA_TRACE_H
#define VITA_TRACE_H

#include <psp2/types.h>

/* Traced ops. Order MUST match g_opname[] in vita_trace.c. */
enum {
    TRACE_OP_IO_OPEN = 0,
    TRACE_OP_IO_CLOSE,
    TRACE_OP_IO_READ,
    TRACE_OP_IO_WRITE,
    TRACE_OP_IO_PWRITE,
    TRACE_OP_IO_LSEEK,
    TRACE_OP_IO_SYNC,
    TRACE_OP_IO_REMOVE,
    TRACE_OP_IO_MKDIR,
    TRACE_OP_IO_DOPEN,
    TRACE_OP_IO_DREAD,
    TRACE_OP_IO_DCLOSE,
    TRACE_OP_IO_GETSTAT,
    TRACE_OP_DELAY,
    TRACE_OP_DISP_SETFB,
    TRACE_OP_DISP_VBLANK,
    TRACE_OP_CTRL_PEEK,
    TRACE_OP_TOUCH_PEEK,
    /* v02.28: sync family. CENSUS of the v02.27 ELF showed the calls
     * actually made: LwMutex 23 sites, Mutex 12, PollEventFlag 5,
     * WaitSema 4, ThreadEndCB/SignalSema 2 each, MsgPipe 4. The pure
     * waits WaitEventFlag/WaitCond/WaitSignal are NEVER called (and
     * have no plain-name stub) - wrapping them would only produce
     * undefined __real_ refs. Release-side ops (unlock/signal/set)
     * never block but their per-thread COUNTERS give lock/unlock
     * balance: whose slot shows lock>unlock holds what the VM wants. */
    TRACE_OP_MUTEX_LOCK,
    TRACE_OP_MUTEX_UNLOCK,
    TRACE_OP_MUTEX_TRYLOCK,
    TRACE_OP_LWMUTEX_LOCK,
    TRACE_OP_LWMUTEX_UNLOCK,
    TRACE_OP_LWMUTEX_TRYLOCK,
    TRACE_OP_SEMA_WAIT,
    TRACE_OP_SEMA_POLL,
    TRACE_OP_SEMA_SIGNAL,
    TRACE_OP_EVTFLAG_POLL,
    TRACE_OP_EVTFLAG_SET,
    TRACE_OP_EVTFLAG_CLEAR,
    TRACE_OP_COND_SIGNAL,
    TRACE_OP_SIGNAL_SEND,
    TRACE_OP_THREAD_END_WAIT,
    TRACE_OP_MSGPIPE_SEND,
    TRACE_OP_MSGPIPE_RECV,
    TRACE_OP_DELAY_CB,
    TRACE_OP_THREAD_SLEEP,
    TRACE_OP_N
};

/* Registry size: main + VM + tone + watchdog + ticker + CDW + ANI pool
 * headroom (VITA_T_MAX threads get per-thread dedicated slots). */
#ifndef VITA_T_MAX
#define VITA_T_MAX 12
#endif

void vita_trace_register(void);   /* optional explicit registration */
void vita_trace_enter(unsigned int op, unsigned int arg);
void vita_trace_exit(void);
void vita_trace_dump(const char *path);

#endif /* VITA_TRACE_H */
