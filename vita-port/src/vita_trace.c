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

#include <psp2/types.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/io/dirent.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/display.h>
#include <psp2/ctrl.h>
#include <psp2/touch.h>

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
    "ctrl_peek", "touch_peek", "thread_sleep"
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

void vita_trace_exit(void)
{
    unsigned int tid = (unsigned int)sceKernelGetThreadId();
    int i;

    for (i = 0; i < VITA_T_MAX; i++) {
        if (g_thr[i].tid == tid) {
            g_slot[i].seq &= ~1u; /* even = returned */
            return;
        }
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
    return r;
}

extern int __real_sceIoClose(SceUID);
int __wrap_sceIoClose(SceUID fd)
{
    int r;
    vita_trace_enter(TRACE_OP_IO_CLOSE, (unsigned int)fd);
    r = __real_sceIoClose(fd);
    vita_trace_exit();
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
