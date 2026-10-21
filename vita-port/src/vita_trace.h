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
    /* v02.29: close the EVEN-slot holes. log51 verdict left two
     * candidates: (B) VM wedged inside a syscall the v02.27/28 set
     * never traced, or (D) scheduler spin after the last switch_thread
     * mark. The VM slot's final traced call was io_pwrite RETURNED,
     * and the very next natural call for a file manager after writing
     * is fsync() -> sceIoSyncByFd - untraced until now. Census of the
     * v02.28 ELF (blx sites): SyncByFd 1, Lseek32 1, Rename 5, Rmdir
     * 5, Chstat 2, ChstatByFd 2, GetstatByFd 3, Devctl 1,
     * AudioOutOutput 9, NetEpollWait 1, ExitDeleteThread 1,
     * ExitProcess 1. sceKernelExitThread has ZERO direct sites (all
     * calls go through the __sceKernelExitThread_from_thumb local
     * veneer, which --wrap cannot redirect) - excluded. The _-prefixed
     * stub-only family (_sceIoChstat etc) binds __real_ to the plain
     * fstubs table entry, same proven mechanism as v02.27. */
    TRACE_OP_IO_SYNCBYFD,
    TRACE_OP_IO_LSEEK32,
    TRACE_OP_IO_RENAME,
    TRACE_OP_IO_RMDIR,
    TRACE_OP_IO_CHSTAT,
    TRACE_OP_IO_CHSTAT_BYFD,
    TRACE_OP_IO_GETSTAT_BYFD,
    TRACE_OP_IO_DEVCTL,
    TRACE_OP_AUDIO_OUT,
    TRACE_OP_NET_EPOLL_WAIT,
    TRACE_OP_THREAD_EXITDEL,
    TRACE_OP_PROCESS_EXIT,
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

/* v02.47: VM-thread syscall accounting + zero-I/O peek for the watchdog's
 * early stall sampling. Only the VM thread (vita_wd_vm_tid) is accounted,
 * so the counters have a single writer. */
unsigned int vita_trace_op_count(void);              /* TRACE_OP_N         */
const char  *vita_trace_op_name(unsigned int op);
/* cumulative calls and microseconds spent INSIDE op, VM thread only */
void vita_trace_op_stats(unsigned int op, unsigned int *cnt,
                         unsigned long long *us);

/* v02.50: where do the VM thread's sceIoWrite calls go? fd -> path tail
 * (recorded at sceIoOpen) and per-fd call count / time, VM thread only. */
const char *vita_trace_fd_path(int fd);
int vita_trace_fdw_get(unsigned int idx, int *fd, unsigned int *cnt,
                       unsigned long long *us, unsigned long long *bytes);

typedef struct {
    unsigned int arg;       /* v02.50: arg of the last op (fd for I/O)   */
    unsigned int found;     /* 0 = tid not registered in the tracer      */
    unsigned int op;        /* last traced op entered (0xFF = none)      */
    unsigned int inside;    /* 1 = that call has not returned yet        */
    unsigned int ts_lo;     /* sceKernelGetSystemTimeWide low 32 bits    */
    unsigned int total;     /* all traced calls by this thread so far    */
    unsigned int rd, lseek, lseek32, lwmutex, sema;
} vita_trace_peek_t;
/* Read-only copy of one thread's slot and a few op counters. */
void vita_trace_peek(unsigned int tid, vita_trace_peek_t *out);

#endif /* VITA_TRACE_H */
