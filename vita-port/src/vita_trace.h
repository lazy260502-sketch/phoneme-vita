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
