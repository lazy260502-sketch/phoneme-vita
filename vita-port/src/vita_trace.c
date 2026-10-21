/*
 * vita_trace.c - v02.27 syscall entry/exit tracer (ZERO file I/O).
 *
 * WHY THIS EXISTS (13 HANG rounds, log37..log50)
 * ==============================================
 * The frozen signature converged 8 rounds: cpu0 idle/switch/runClocks
 * all frozen, VM thread RUNNING/wait=none/runClocks d=0 on cpu0, KICK0-5
 * (signal/affinity/prio/suspend) all ineffective, tone/ticker/watchdog
 * healthy. Unified model: the VM thread entered a kernel call on cpu0
 * and never came back, and the core is wedged with it (idle counter
 * frozen = no context-switch accounting at all).
 *
 * Offline forensics is DEAD (log50 verdict): SYSTEM_INFO2 SECA-encrypted,
 * no kernel LOAD segments in the dump, MODULE_INFO lists user modules
 * only, THREAD_REG_INFO pc/sp for non-faulting threads are dump
 * ARTIFACTS (log48/50 "mla deadpoint" retracted - see PROJECT_MEMORY).
 * So the only way left to learn "which call never returns" is to make
 * the process itself remember, with tracing cheap enough to run
 * permanently without perturbing the hang.
 *
 * DESIGN (lock-free, malloc-free, stdio-free)
 * ===========================================
 *  - PER-THREAD DEDICATED SLOT (the deadpoint record). While a thread
 *    is INSIDE a traced call, nothing else can touch its slot, so an
 *    entered-never-returned call survives arbitrary churn from other
 *    threads (tone+ticker+pump produce ~800 entries/s; a shared ring
 *    of any sane size would clobber the evidence within ~50 ms).
 *      slot layout: { seq, op, arg, ts }
 *      seq bit0: 1 = INSIDE the call, 0 = returned. Upper bits are a
 *      per-slot version bumped on every entry (seqlock-flavoured).
 *  - Global history ring (32 entries) for CONTEXT ONLY: what the whole
 *    process did in the last moments. Clobbering is acceptable there.
 *  - Lazy self-registration: the first traced call on an unknown tid
 *    claims a slot. No init order dependencies, no extra boot steps.
 *  - All traced ops are per-frame or per-fileop calls (<= ~200/s per
 *    thread): a handful of stores each, no measurable cost.
 *
 * DISCRIMINATOR this buys at the next HANG:
 *   VM slot ODD  (inside op X)      -> deadpoint FOUND (syscall X)
 *   VM slot EVEN (nothing in flight)-> VM died OUTSIDE any traced
 *                                      syscall: pure interpreter or
 *                                      an untraced path.
 *
 * The watchdog dumps slots + history into trace_ring.log on HANG
 * (vita_trace_dump): plain sceIo from the healthy watchdog thread,
 * same proven pattern as site_ring.log. The dump's own sceIo calls go
 * through the wrappers too - harmless (they mark the WD's own slot,
 * which always ends EVEN on success).
 *
 * Wrappers add ZERO semantic change: trace-enter, __real_*(), trace-exit,
 * return. No blocking, no allocation inside the tracer itself.
 */

#include <string.h>          /* v02.50: fd -> path tail (strlen/strncpy) */
#include <psp2/types.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/io/dirent.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/display.h>
#include <psp2/ctrl.h>
#include <psp2/touch.h>
#include <psp2/audioout.h>   /* v02.29: sceAudioOutOutput          */
#include <psp2/net/net.h>    /* v02.29: SceNetEpollEvent           */

#include "vita_trace.h"

/* ---- per-thread slots -------------------------------------------- */

#define TR_HIST_N 32             /* history entries; power of two    */

typedef struct {
    volatile unsigned int seq;   /* bit0: 1=INSIDE 0=returned; upper
                                    bits: per-slot version           */
    volatile unsigned int op;    /* TRACE_OP_* (0xFF = never used)   */
    volatile unsigned int arg;   /* fd / path ptr / usec             */
    volatile unsigned int ts_lo; /* sceKernelGetSystemTimeWide low   */
} tr_slot;

static tr_slot g_slot[VITA_T_MAX];

typedef struct {
    volatile unsigned int tid;
    unsigned int opcnt[TRACE_OP_N];
} tr_thr;

static tr_thr g_thr[VITA_T_MAX];
static volatile unsigned int g_thr_guard;

/* global history ring (context only) */
static tr_slot g_hist[TR_HIST_N];
static volatile unsigned int g_hist_head;

static const char *const g_opname[TRACE_OP_N] = {
    "io_open", "io_close", "io_read", "io_write", "io_pwrite",
    "io_lseek", "io_sync", "io_remove", "io_mkdir", "io_dopen",
    "io_dread", "io_dclose", "io_getstat",
    "delay_thread", "disp_setframebuf", "disp_waitvblank",
    "ctrl_peek", "touch_peek",
    "mutex_lock", "mutex_unlock", "mutex_trylock",
    "lwmutex_lock", "lwmutex_unlock", "lwmutex_trylock",
    "sema_wait", "sema_poll", "sema_signal",
    "evtflag_poll", "evtflag_set", "evtflag_clear",
    "cond_signal", "signal_send", "thread_end_wait",
    "msgpipe_send", "msgpipe_recv", "delay_cb",
    "thread_sleep",
    /* v02.29 */
    "io_syncbyfd", "io_lseek32", "io_rename", "io_rmdir",
    "io_chstat", "io_chstat_byfd", "io_getstat_byfd", "io_devctl",
    "audio_out", "net_epoll_wait", "thread_exitdel", "process_exit"
};

/* ---- lazy registration ------------------------------------------- */

static int tr_register(unsigned int tid)
{
    int i, idx = -1;

    while (__atomic_exchange_n(&g_thr_guard, 1, __ATOMIC_ACQUIRE) != 0) {
    }
    for (i = 0; i < VITA_T_MAX; i++) {
        if (g_thr[i].tid == tid) { idx = i; break; }
        if (g_thr[i].tid == 0 && idx < 0)
            idx = i; /* first free */
    }
    if (idx >= 0 && g_thr[idx].tid == 0) {
        g_thr[idx].tid = tid;
        g_slot[idx].op = 0xFF;
    }
    g_thr_guard = 0;
    return idx;
}

static int tr_self(unsigned int tid)
{
    int i;
    for (i = 0; i < VITA_T_MAX; i++)
        if (g_thr[i].tid == tid)
            return i;
    return tr_register(tid);
}

void vita_trace_register(void)
{
    (void)tr_register((unsigned int)sceKernelGetThreadId());
}

/* ---- entry/exit --------------------------------------------------- */

void vita_trace_enter(unsigned int op, unsigned int arg)
{
    unsigned int tid = (unsigned int)sceKernelGetThreadId();
    int idx = tr_self(tid);
    tr_slot *s;
    unsigned int hist;

    if (idx < 0 || op >= TRACE_OP_N)
        return; /* registry full or bad op: run untraced */
    g_thr[idx].opcnt[op]++;

    s = &g_slot[idx];
    s->arg = arg;
    s->ts_lo = (unsigned int)sceKernelGetSystemTimeWide();
    s->op = op;
    /* version++ and mark INSIDE - written LAST so a reader never sees
     * INSIDE with a stale op/arg pair from the previous call */
    s->seq = (s->seq & ~1u) + 2u + 1u;

    /* history copy (context only; races merely lose a sample) */
    hist = (g_hist_head + 1u) & (TR_HIST_N - 1u);
    g_hist_head = hist;
    g_hist[hist] = *s;
}

/* v02.47: time spent inside each traced op, VM thread only (single
 * writer, so no atomics). ts_lo is the 32-bit low word of the system time
 * stamped at entry; the unsigned subtraction handles wrap-around. */
extern volatile SceUID vita_wd_vm_tid;
static unsigned int g_ucnt[TRACE_OP_N];
static unsigned long long g_usum[TRACE_OP_N];

/* v02.50: fd -> path tail, filled at sceIoOpen. Diagnostic only: entries
 * may tear if two threads open at once, and a closed fd keeps its old name
 * until reused, so the watchdog treats the name as a hint. */
#define TR_FD_N 32
#define TR_FD_PATH 40
static struct {
    volatile int fd;
    char path[TR_FD_PATH];
} g_fdp[TR_FD_N];
static volatile unsigned int g_fdp_next;

/* v02.54: log102 showed "io_write fd=1073807631 (?)": SceUIDs are never
 * reused and the startup opens dozens of short-lived files (every
 * vita_boot_log line, every heartbeat), so a 16-slot round-robin table
 * overwrote the one LONG-LIVED fd we care about. Slots are now freed at
 * sceIoClose and a free slot is preferred over evicting one. */
static void tr_forget_fd(int fd)
{
    unsigned int i;
    if (fd <= 0)
        return;
    for (i = 0; i < TR_FD_N; i++)
        if (g_fdp[i].fd == fd)
            g_fdp[i].fd = 0;
}

static void tr_note_fd(int fd, const char *file)
{
    unsigned int i, n;
    int slot = -1;
    size_t len;

    if (fd < 0 || file == NULL)
        return;
    for (i = 0; i < TR_FD_N; i++)
        if (g_fdp[i].fd == fd) { slot = (int)i; break; }
    if (slot < 0)
        for (i = 0; i < TR_FD_N; i++)
            if (g_fdp[i].fd == 0 || g_fdp[i].fd == -1) { slot = (int)i; break; }
    if (slot < 0)
        slot = (int)(g_fdp_next++ % TR_FD_N);
    len = strlen(file);
    n = (len > TR_FD_PATH - 1) ? (unsigned int)(len - (TR_FD_PATH - 1)) : 0;
    g_fdp[slot].fd = -1;                       /* hide while rewriting */
    strncpy(g_fdp[slot].path, file + n, TR_FD_PATH - 1);
    g_fdp[slot].path[TR_FD_PATH - 1] = '\0';
    g_fdp[slot].fd = fd;
}

const char *vita_trace_fd_path(int fd)
{
    unsigned int i;
    for (i = 0; i < TR_FD_N; i++)
        if (g_fdp[i].fd == fd)
            return g_fdp[i].path;
    return "?";
}

/* VM-thread sceIoWrite calls per fd (count, microseconds inside the call) */
#define TR_FDW_N 8
static struct {
    int fd;
    unsigned int cnt;
    unsigned long long us;
    unsigned long long bytes;   /* v02.54: bytes actually written */
} g_fdw[TR_FDW_N];

int vita_trace_fdw_get(unsigned int idx, int *fd, unsigned int *cnt,
                       unsigned long long *us, unsigned long long *bytes)
{
    if (idx >= TR_FDW_N || g_fdw[idx].cnt == 0)
        return 0;
    *fd = g_fdw[idx].fd;
    *cnt = g_fdw[idx].cnt;
    *us = g_fdw[idx].us;
    *bytes = g_fdw[idx].bytes;
    return 1;
}

static void tr_fdw_bytes(int fd, int n)
{
    unsigned int k;
    if (n <= 0)
        return;
    for (k = 0; k < TR_FDW_N; k++)
        if (g_fdw[k].cnt != 0 && g_fdw[k].fd == fd) {
            g_fdw[k].bytes += (unsigned int)n;
            return;
        }
}

void vita_trace_exit(void)
{
    unsigned int tid = (unsigned int)sceKernelGetThreadId();
    int i;

    for (i = 0; i < VITA_T_MAX; i++) {
        if (g_thr[i].tid == tid) {
            if (tid == (unsigned int)vita_wd_vm_tid &&
                g_slot[i].op < TRACE_OP_N) {
                unsigned int op = g_slot[i].op;
                unsigned int d = (unsigned int)sceKernelGetSystemTimeWide()
                                 - g_slot[i].ts_lo;
                g_ucnt[op]++;
                g_usum[op] += d;
                if (op == TRACE_OP_IO_WRITE) {   /* v02.50: per-fd split */
                    int wfd = (int)g_slot[i].arg;
                    unsigned int k;
                    for (k = 0; k < TR_FDW_N; k++)
                        if (g_fdw[k].cnt == 0 || g_fdw[k].fd == wfd)
                            break;
                    if (k < TR_FDW_N) {
                        g_fdw[k].fd = wfd;
                        g_fdw[k].cnt++;
                        g_fdw[k].us += d;
                    }
                }
            }
            g_slot[i].seq &= ~1u; /* even = returned */
            return;
        }
    }
}

unsigned int vita_trace_op_count(void) { return TRACE_OP_N; }

const char *vita_trace_op_name(unsigned int op)
{
    return op < TRACE_OP_N ? g_opname[op] : "?";
}

void vita_trace_op_stats(unsigned int op, unsigned int *cnt,
                         unsigned long long *us)
{
    if (op >= TRACE_OP_N) { *cnt = 0; *us = 0; return; }
    *cnt = g_ucnt[op];
    *us = g_usum[op];
}

void vita_trace_peek(unsigned int tid, vita_trace_peek_t *out)
{
    int i;
    unsigned int j;

    out->found = 0; out->op = 0xFF; out->inside = 0; out->ts_lo = 0;
    out->arg = 0;
    out->total = 0; out->rd = 0; out->lseek = 0; out->lseek32 = 0;
    out->lwmutex = 0; out->sema = 0;
    for (i = 0; i < VITA_T_MAX; i++) {
        if (g_thr[i].tid != tid) continue;
        out->found = 1;
        out->op = g_slot[i].op;
        out->inside = g_slot[i].seq & 1u;
        out->ts_lo = g_slot[i].ts_lo;
        out->arg = g_slot[i].arg;
        for (j = 0; j < TRACE_OP_N; j++) out->total += g_thr[i].opcnt[j];
        out->rd = g_thr[i].opcnt[TRACE_OP_IO_READ];
        out->lseek = g_thr[i].opcnt[TRACE_OP_IO_LSEEK];
        out->lseek32 = g_thr[i].opcnt[TRACE_OP_IO_LSEEK32];
        out->lwmutex = g_thr[i].opcnt[TRACE_OP_LWMUTEX_LOCK];
        out->sema = g_thr[i].opcnt[TRACE_OP_SEMA_WAIT];
        return;
    }
}

/* ---- watchdog dump ------------------------------------------------- */

static int tr_hex(char *dst, unsigned int v)
{
    static const char hc[] = "0123456789abcdef";
    int i, n = 0;
    for (i = 28; i >= 0; i -= 4)
        dst[n++] = hc[(v >> i) & 0xF];
    return n;
}

static int tr_str(char *dst, const char *s)
{
    int n = 0;
    while (s && s[n]) { dst[n] = s[n]; n++; }
    return n;
}

void vita_trace_dump(const char *path)
{
    SceUID fd;
    char line[160];
    int pos, i, j, n;

    fd = sceIoOpen(path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0777);
    if (fd < 0)
        return;

    pos = 0;
    pos += tr_str(line + pos,
                  "\n===== v02.27 syscall trace (per-thread slots) =====\n");
    pos += tr_str(line + pos,
                  "slot: ODD seq = INSIDE (never returned) = deadpoint\n");
    sceIoWrite(fd, line, pos);

    for (i = 0; i < VITA_T_MAX; i++) {
        unsigned int seq, op, arg, ts;
        if (g_thr[i].tid == 0)
            continue;
        seq = g_slot[i].seq;
        op = g_slot[i].op;
        arg = g_slot[i].arg;
        ts = g_slot[i].ts_lo;
        if (op == 0xFF)
            continue;
        pos = 0;
        pos += tr_str(line + pos, "tid ");
        pos += tr_hex(line + pos, g_thr[i].tid);
        pos += tr_str(line + pos, ": ");
        pos += tr_str(line + pos, op < TRACE_OP_N ? g_opname[op] : "?");
        line[pos++] = ' ';
        pos += tr_hex(line + pos, arg);
        line[pos++] = ' ';
        pos += tr_hex(line + pos, ts);
        pos += tr_str(line + pos, (seq & 1u)
                        ? "  <== INSIDE (NEVER RETURNED)"
                        : "  (returned)");
        line[pos++] = '\n';
        sceIoWrite(fd, line, pos);
    }

    pos = 0;
    pos += tr_str(line + pos, "\nhistory (newest first):\n");
    sceIoWrite(fd, line, pos);
    for (n = 0; n < TR_HIST_N; n++) {
        unsigned int idx =
            (g_hist_head - (unsigned int)n) & (TR_HIST_N - 1u);
        unsigned int op = g_hist[idx].op;
        if (op >= TRACE_OP_N)
            continue;
        pos = 0;
        pos += tr_str(line + pos, " ");
        pos += tr_str(line + pos, g_opname[op]);
        line[pos++] = ' ';
        pos += tr_hex(line + pos, g_hist[idx].arg);
        line[pos++] = ' ';
        pos += tr_hex(line + pos, g_hist[idx].ts_lo);
        if (g_hist[idx].seq & 1u)
            pos += tr_str(line + pos, " I");
        line[pos++] = '\n';
        sceIoWrite(fd, line, pos);
    }

    pos = 0;
    pos += tr_str(line + pos, "\nper-thread op counters:\n");
    sceIoWrite(fd, line, pos);
    for (i = 0; i < VITA_T_MAX; i++) {
        if (g_thr[i].tid == 0)
            continue;
        pos = 0;
        pos += tr_str(line + pos, "tid ");
        pos += tr_hex(line + pos, g_thr[i].tid);
        line[pos++] = ':';
        line[pos++] = '\n';
        sceIoWrite(fd, line, pos);
        for (j = 0; j < TRACE_OP_N; j++) {
            unsigned int v;
            char dec[12];
            int k = 0;
            if (!g_thr[i].opcnt[j])
                continue;
            v = g_thr[i].opcnt[j];
            pos = 0;
            line[pos++] = '\t';
            pos += tr_str(line + pos, g_opname[j]);
            line[pos++] = '=';
            if (v == 0) dec[k++] = '0';
            while (v && k < 11) { dec[k++] = (char)('0' + v % 10); v /= 10; }
            while (k > 0) line[pos++] = dec[--k];
            line[pos++] = '\n';
            sceIoWrite(fd, line, pos);
        }
    }

    pos = 0;
    pos += tr_str(line + pos, "===== end syscall trace =====\n");
    sceIoWrite(fd, line, pos);
    sceIoClose(fd);
}

/* ====================================================================
 * -Wl,--wrap hooks (CMakeLists v02.27 block). Each wrapper: trace the
 * entry, run the REAL call, trace the exit, return.
 * ==================================================================== */

extern SceUID __real_sceIoOpen(const char *, int, SceMode);
SceUID __wrap_sceIoOpen(const char *file, int flags, SceMode mode)
{
    SceUID r;
    vita_trace_enter(TRACE_OP_IO_OPEN, (unsigned int)file);
    r = __real_sceIoOpen(file, flags, mode);
    vita_trace_exit();
    tr_note_fd((int)r, file);   /* v02.50: remember fd -> path tail */
    return r;
}

extern int __real_sceIoClose(SceUID);
int __wrap_sceIoClose(SceUID fd)
{
    int r;
    vita_trace_enter(TRACE_OP_IO_CLOSE, (unsigned int)fd);
    r = __real_sceIoClose(fd);
    vita_trace_exit();
    tr_forget_fd((int)fd);          /* v02.54: free the fd -> path slot */
    return r;
}

extern int __real_sceIoRead(SceUID, void *, SceSize);
int __wrap_sceIoRead(SceUID fd, void *data, SceSize size)
{
    int r;
    vita_trace_enter(TRACE_OP_IO_READ, (unsigned int)fd);
    r = __real_sceIoRead(fd, data, size);
    vita_trace_exit();
    return r;
}

extern int __real_sceIoWrite(SceUID, const void *, SceSize);
int __wrap_sceIoWrite(SceUID fd, const void *data, SceSize size)
{
    int r;
    vita_trace_enter(TRACE_OP_IO_WRITE, (unsigned int)fd);
    r = __real_sceIoWrite(fd, data, size);
    vita_trace_exit();
    if (sceKernelGetThreadId() == vita_wd_vm_tid)   /* v02.54: bytes per fd */
        tr_fdw_bytes((int)fd, r);
    return r;
}

extern int __real_sceIoPwrite(SceUID, const void *, SceSize, SceOff);
int __wrap_sceIoPwrite(SceUID fd, const void *data, SceSize size,
                       SceOff offset)
{
    int r;
    vita_trace_enter(TRACE_OP_IO_PWRITE, (unsigned int)fd);
    r = __real_sceIoPwrite(fd, data, size, offset);
    vita_trace_exit();
    return r;
}

extern SceOff __real_sceIoLseek(SceUID, SceOff, int);
SceOff __wrap_sceIoLseek(SceUID fd, SceOff offset, int whence)
{
    SceOff r;
    vita_trace_enter(TRACE_OP_IO_LSEEK, (unsigned int)fd);
    r = __real_sceIoLseek(fd, offset, whence);
    vita_trace_exit();
    return r;
}

extern int __real_sceIoSync(const char *, int);
int __wrap_sceIoSync(const char *file, int flag)
{
    int r;
    vita_trace_enter(TRACE_OP_IO_SYNC, (unsigned int)file);
    r = __real_sceIoSync(file, flag);
    vita_trace_exit();
    return r;
}

extern int __real_sceIoRemove(const char *);
int __wrap_sceIoRemove(const char *file)
{
    int r;
    vita_trace_enter(TRACE_OP_IO_REMOVE, (unsigned int)file);
    r = __real_sceIoRemove(file);
    vita_trace_exit();
    return r;
}

extern int __real_sceIoMkdir(const char *, SceMode);
int __wrap_sceIoMkdir(const char *dir, SceMode mode)
{
    int r;
    vita_trace_enter(TRACE_OP_IO_MKDIR, (unsigned int)dir);
    r = __real_sceIoMkdir(dir, mode);
    vita_trace_exit();
    return r;
}

extern SceUID __real_sceIoDopen(const char *);
SceUID __wrap_sceIoDopen(const char *dirname)
{
    SceUID r;
    vita_trace_enter(TRACE_OP_IO_DOPEN, (unsigned int)dirname);
    r = __real_sceIoDopen(dirname);
    vita_trace_exit();
    return r;
}

extern int __real_sceIoDread(SceUID, SceIoDirent *);
int __wrap_sceIoDread(SceUID fd, SceIoDirent *dent)
{
    int r;
    vita_trace_enter(TRACE_OP_IO_DREAD, (unsigned int)fd);
    r = __real_sceIoDread(fd, dent);
    vita_trace_exit();
    return r;
}

extern int __real_sceIoDclose(SceUID);
int __wrap_sceIoDclose(SceUID fd)
{
    int r;
    vita_trace_enter(TRACE_OP_IO_DCLOSE, (unsigned int)fd);
    r = __real_sceIoDclose(fd);
    vita_trace_exit();
    return r;
}

extern int __real_sceIoGetstat(const char *, SceIoStat *);
int __wrap_sceIoGetstat(const char *file, SceIoStat *stat)
{
    int r;
    vita_trace_enter(TRACE_OP_IO_GETSTAT, (unsigned int)file);
    r = __real_sceIoGetstat(file, stat);
    vita_trace_exit();
 return r;
}

extern int __real_sceKernelDelayThread(SceUInt);
int __wrap_sceKernelDelayThread(SceUInt usec)
{
    int r;
    vita_trace_enter(TRACE_OP_DELAY, usec);
    r = __real_sceKernelDelayThread(usec);
    vita_trace_exit();
    return r;
}

extern int __real_sceDisplaySetFrameBuf(const SceDisplayFrameBuf *,
                                        SceDisplaySetBufSync);
int __wrap_sceDisplaySetFrameBuf(const SceDisplayFrameBuf *fbp,
                                 SceDisplaySetBufSync sync)
{
    int r;
    vita_trace_enter(TRACE_OP_DISP_SETFB, (unsigned int)fbp);
    r = __real_sceDisplaySetFrameBuf(fbp, sync);
    vita_trace_exit();
    return r;
}

extern int __real_sceDisplayWaitVblankStart(void);
int __wrap_sceDisplayWaitVblankStart(void)
{
    int r;
    vita_trace_enter(TRACE_OP_DISP_VBLANK, 0);
    r = __real_sceDisplayWaitVblankStart();
    vita_trace_exit();
    return r;
}

extern int __real_sceCtrlPeekBufferPositive(int, SceCtrlData *, int);
int __wrap_sceCtrlPeekBufferPositive(int port, SceCtrlData *pad,
                                     int nbufs)
{
    int r;
    vita_trace_enter(TRACE_OP_CTRL_PEEK, (unsigned int)port);
    r = __real_sceCtrlPeekBufferPositive(port, pad, nbufs);
    vita_trace_exit();
    return r;
}

extern int __real_sceTouchPeek(SceUInt, SceTouchData *, int);
int __wrap_sceTouchPeek(SceUInt port, SceTouchData *data, int nbufs)
{
    int r;
    vita_trace_enter(TRACE_OP_TOUCH_PEEK, port);
    r = __real_sceTouchPeek(port, data, nbufs);
    vita_trace_exit();
    return r;
}

/* ------------------------------------------------------------------
 * v02.28: sync family (see census note in vita_trace.h). arg = the
 * SceUID (or LwMutex work-area pointer) so a frozen slot names the
 * exact lock/pipe the thread died on.
 * ------------------------------------------------------------------ */

extern int __real_sceKernelLockMutex(SceUID, int, unsigned int *);
int __wrap_sceKernelLockMutex(SceUID mutexid, int lockCount,
                              unsigned int *timeout)
{
    int r;
    vita_trace_enter(TRACE_OP_MUTEX_LOCK, (unsigned int)mutexid);
    r = __real_sceKernelLockMutex(mutexid, lockCount, timeout);
    vita_trace_exit();
    return r;
}

extern int __real_sceKernelUnlockMutex(SceUID, int);
int __wrap_sceKernelUnlockMutex(SceUID mutexid, int unlockCount)
{
    int r;
    vita_trace_enter(TRACE_OP_MUTEX_UNLOCK, (unsigned int)mutexid);
    r = __real_sceKernelUnlockMutex(mutexid, unlockCount);
    vita_trace_exit();
    return r;
}

extern int __real_sceKernelTryLockMutex(SceUID, int);
int __wrap_sceKernelTryLockMutex(SceUID mutexid, int lockCount)
{
    int r;
    vita_trace_enter(TRACE_OP_MUTEX_TRYLOCK, (unsigned int)mutexid);
    r = __real_sceKernelTryLockMutex(mutexid, lockCount);
    vita_trace_exit();
    return r;
}

extern int __real_sceKernelLockLwMutex(SceKernelLwMutexWork *, int,
                                       unsigned int *);
int __wrap_sceKernelLockLwMutex(SceKernelLwMutexWork *pWork,
                                int lockCount, unsigned int *pTimeout)
{
    int r;
    vita_trace_enter(TRACE_OP_LWMUTEX_LOCK, (unsigned int)pWork);
    r = __real_sceKernelLockLwMutex(pWork, lockCount, pTimeout);
    vita_trace_exit();
    return r;
}

extern int __real_sceKernelUnlockLwMutex(SceKernelLwMutexWork *, int);
int __wrap_sceKernelUnlockLwMutex(SceKernelLwMutexWork *pWork,
                                  int unlockCount)
{
    int r;
    vita_trace_enter(TRACE_OP_LWMUTEX_UNLOCK, (unsigned int)pWork);
    r = __real_sceKernelUnlockLwMutex(pWork, unlockCount);
    vita_trace_exit();
    return r;
}

extern int __real_sceKernelTryLockLwMutex(SceKernelLwMutexWork *, int);
int __wrap_sceKernelTryLockLwMutex(SceKernelLwMutexWork *pWork,
                                   int lockCount)
{
    int r;
    vita_trace_enter(TRACE_OP_LWMUTEX_TRYLOCK, (unsigned int)pWork);
    r = __real_sceKernelTryLockLwMutex(pWork, lockCount);
    vita_trace_exit();
    return r;
}

extern int __real_sceKernelWaitSema(SceUID, int, SceUInt *);
int __wrap_sceKernelWaitSema(SceUID semaid, int signal, SceUInt *timeout)
{
    int r;
    vita_trace_enter(TRACE_OP_SEMA_WAIT, (unsigned int)semaid);
    r = __real_sceKernelWaitSema(semaid, signal, timeout);
    vita_trace_exit();
    return r;
}

extern int __real_sceKernelPollSema(SceUID, int);
int __wrap_sceKernelPollSema(SceUID semaid, int signal)
{
    int r;
    vita_trace_enter(TRACE_OP_SEMA_POLL, (unsigned int)semaid);
    r = __real_sceKernelPollSema(semaid, signal);
    vita_trace_exit();
    return r;
}

extern int __real_sceKernelSignalSema(SceUID, int);
int __wrap_sceKernelSignalSema(SceUID semaid, int signal)
{
    int r;
    vita_trace_enter(TRACE_OP_SEMA_SIGNAL, (unsigned int)semaid);
    r = __real_sceKernelSignalSema(semaid, signal);
    vita_trace_exit();
    return r;
}

extern int __real_sceKernelPollEventFlag(int, unsigned int,
                                         unsigned int, unsigned int *);
int __wrap_sceKernelPollEventFlag(int evid, unsigned int bits,
                                  unsigned int wait, unsigned int *outBits)
{
    int r;
    vita_trace_enter(TRACE_OP_EVTFLAG_POLL, (unsigned int)evid);
    r = __real_sceKernelPollEventFlag(evid, bits, wait, outBits);
    vita_trace_exit();
    return r;
}

extern int __real_sceKernelSetEventFlag(SceUID, unsigned int);
int __wrap_sceKernelSetEventFlag(SceUID evid, unsigned int bits)
{
    int r;
    vita_trace_enter(TRACE_OP_EVTFLAG_SET, (unsigned int)evid);
    r = __real_sceKernelSetEventFlag(evid, bits);
    vita_trace_exit();
    return r;
}

extern int __real_sceKernelClearEventFlag(SceUID, unsigned int);
int __wrap_sceKernelClearEventFlag(SceUID evid, unsigned int bits)
{
    int r;
    vita_trace_enter(TRACE_OP_EVTFLAG_CLEAR, (unsigned int)evid);
    r = __real_sceKernelClearEventFlag(evid, bits);
    vita_trace_exit();
    return r;
}

extern int __real_sceKernelSignalCond(SceUID);
int __wrap_sceKernelSignalCond(SceUID condId)
{
    int r;
    vita_trace_enter(TRACE_OP_COND_SIGNAL, (unsigned int)condId);
    r = __real_sceKernelSignalCond(condId);
    vita_trace_exit();
    return r;
}

extern int __real_sceKernelSendSignal(SceUID);
int __wrap_sceKernelSendSignal(SceUID thid)
{
    int r;
    vita_trace_enter(TRACE_OP_SIGNAL_SEND, (unsigned int)thid);
    r = __real_sceKernelSendSignal(thid);
    vita_trace_exit();
    return r;
}

extern int __real_sceKernelWaitThreadEndCB(SceUID, int *, SceUInt *);
int __wrap_sceKernelWaitThreadEndCB(SceUID thid, int *stat,
                                    SceUInt *timeout)
{
    int r;
    vita_trace_enter(TRACE_OP_THREAD_END_WAIT, (unsigned int)thid);
    r = __real_sceKernelWaitThreadEndCB(thid, stat, timeout);
    vita_trace_exit();
    return r;
}

extern int __real_sceKernelSendMsgPipe(SceUID, void *, unsigned int,
                                       int, void *, unsigned int *);
int __wrap_sceKernelSendMsgPipe(SceUID uid, void *message,
                                unsigned int size, int unk1, void *unk2,
                                unsigned int *timeout)
{
    int r;
    vita_trace_enter(TRACE_OP_MSGPIPE_SEND, (unsigned int)uid);
    r = __real_sceKernelSendMsgPipe(uid, message, size, unk1, unk2,
                                    timeout);
    vita_trace_exit();
    return r;
}

extern int __real_sceKernelReceiveMsgPipe(SceUID, void *, SceSize,
                                          int, void *, unsigned int *);
int __wrap_sceKernelReceiveMsgPipe(SceUID uid, void *message,
                                   SceSize size, int unk1, void *unk2,
                                   unsigned int *timeout)
{
    int r;
    vita_trace_enter(TRACE_OP_MSGPIPE_RECV, (unsigned int)uid);
    r = __real_sceKernelReceiveMsgPipe(uid, message, size, unk1, unk2,
                                       timeout);
    vita_trace_exit();
    return r;
}

extern int __real_sceKernelDelayThreadCB(SceUInt);
int __wrap_sceKernelDelayThreadCB(SceUInt usec)
{
    int r;
    vita_trace_enter(TRACE_OP_DELAY_CB, usec);
    r = __real_sceKernelDelayThreadCB(usec);
    vita_trace_exit();
    return r;
}

/* ------------------------------------------------------------------
 * v02.29: close the EVEN-slot holes (log51 verdict follow-up).
 * VM slot ended io_pwrite RETURNED; the untraced syscall set still
 * contained blocking candidates reachable right after a write -
 * sceIoSyncByFd (newlib fsync) above all. If the next HANG shows the
 * VM slot ODD on one of these, hypothesis B (wedge inside an
 * untraced syscall) wins; still EVEN => hypothesis D (scheduler-side
 * spin after the last switch mark) or a userland native loop no
 * wrapper sees. Exit-family calls never return on success - their
 * slots stay ODD forever, which is CORRECT evidence (a still-alive
 * process cannot have exited), and their arg is the exit status.
 * ------------------------------------------------------------------ */

extern int __real_sceIoSyncByFd(SceUID, int);
int __wrap_sceIoSyncByFd(SceUID fd, int flag)
{
    int r;
    vita_trace_enter(TRACE_OP_IO_SYNCBYFD, (unsigned int)fd);
    r = __real_sceIoSyncByFd(fd, flag);
    vita_trace_exit();
    return r;
}

extern long __real_sceIoLseek32(SceUID, long, int);
long __wrap_sceIoLseek32(SceUID fd, long offset, int whence)
{
    long r;
    vita_trace_enter(TRACE_OP_IO_LSEEK32, (unsigned int)fd);
    r = __real_sceIoLseek32(fd, offset, whence);
    vita_trace_exit();
    return r;
}

extern int __real_sceIoRename(const char *, const char *);
int __wrap_sceIoRename(const char *oldname, const char *newname)
{
    int r;
    vita_trace_enter(TRACE_OP_IO_RENAME, (unsigned int)oldname);
    r = __real_sceIoRename(oldname, newname);
    vita_trace_exit();
    return r;
}

extern int __real_sceIoRmdir(const char *);
int __wrap_sceIoRmdir(const char *path)
{
    int r;
    vita_trace_enter(TRACE_OP_IO_RMDIR, (unsigned int)path);
    r = __real_sceIoRmdir(path);
    vita_trace_exit();
    return r;
}

extern int __real_sceIoChstat(const char *, SceIoStat *, int);
int __wrap_sceIoChstat(const char *file, SceIoStat *stat, int bits)
{
    int r;
    vita_trace_enter(TRACE_OP_IO_CHSTAT, (unsigned int)file);
    r = __real_sceIoChstat(file, stat, bits);
    vita_trace_exit();
    return r;
}

extern int __real_sceIoChstatByFd(SceUID, const SceIoStat *, unsigned int);
int __wrap_sceIoChstatByFd(SceUID fd, const SceIoStat *buf,
                           unsigned int cbit)
{
    int r;
    vita_trace_enter(TRACE_OP_IO_CHSTAT_BYFD, (unsigned int)fd);
    r = __real_sceIoChstatByFd(fd, buf, cbit);
    vita_trace_exit();
    return r;
}

extern int __real_sceIoGetstatByFd(SceUID, SceIoStat *);
int __wrap_sceIoGetstatByFd(SceUID fd, SceIoStat *stat)
{
    int r;
    vita_trace_enter(TRACE_OP_IO_GETSTAT_BYFD, (unsigned int)fd);
    r = __real_sceIoGetstatByFd(fd, stat);
    vita_trace_exit();
    return r;
}

extern int __real_sceIoDevctl(const char *, int, void *, int, void *, int);
int __wrap_sceIoDevctl(const char *dev, int cmd, void *indata,
                       int inlen, void *outdata, int outlen)
{
    int r;
    vita_trace_enter(TRACE_OP_IO_DEVCTL, (unsigned int)dev);
    r = __real_sceIoDevctl(dev, cmd, indata, inlen, outdata, outlen);
    vita_trace_exit();
    return r;
}

extern int __real_sceAudioOutOutput(int, const void *);
int __wrap_sceAudioOutOutput(int port, const void *buf)
{
    int r;
    vita_trace_enter(TRACE_OP_AUDIO_OUT, (unsigned int)port);
    r = __real_sceAudioOutOutput(port, buf);
    vita_trace_exit();
    return r;
}

extern int __real_sceNetEpollWait(int, SceNetEpollEvent *, int, int);
int __wrap_sceNetEpollWait(int eid, SceNetEpollEvent *events,
                           int maxevents, int timeout)
{
    int r;
    vita_trace_enter(TRACE_OP_NET_EPOLL_WAIT, (unsigned int)eid);
    r = __real_sceNetEpollWait(eid, events, maxevents, timeout);
    vita_trace_exit();
    return r;
}

extern int __real_sceKernelExitDeleteThread(int);
int __wrap_sceKernelExitDeleteThread(int status)
{
    vita_trace_enter(TRACE_OP_THREAD_EXITDEL, (unsigned int)status);
    __real_sceKernelExitDeleteThread(status);
    vita_trace_exit(); /* not reached on success; slot stays ODD */
    return 0;
}

extern int __real_sceKernelExitProcess(int);
int __wrap_sceKernelExitProcess(int res)
{
    vita_trace_enter(TRACE_OP_PROCESS_EXIT, (unsigned int)res);
    __real_sceKernelExitProcess(res);
    vita_trace_exit(); /* not reached on success; slot stays ODD */
    return 0;
}
