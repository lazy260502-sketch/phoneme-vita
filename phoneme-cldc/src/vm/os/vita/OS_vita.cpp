/*
 * OS_vita.cpp: PS Vita implementation of the VM
 *               operating system porting interface
 *
 * This file defines the Vita-specific implementation
 * of the OS porting interface (class Os).  Refer to file
 * "/src/vm/share/runtime/OS.hpp" and the Porting
 * Guide for details.
 *
 * Vita has no signals, no mmap, no fork. We use sceKernel
 * functions for time and threads.
 */

#include "incls/_precompiled.incl"
#include "incls/_OS_vita.cpp.incl"

// several meta defines
#if (ENABLE_PERFORMANCE_COUNTERS || ENABLE_PROFILER || ENABLE_WTK_PROFILER \
     || ENABLE_TTY_TRACE)
#define NEED_CLOCK_TICKS 1
#endif

#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include <sched.h>
#include <unistd.h>

#if ENABLE_DYNAMIC_NATIVE_METHODS || ENABLE_JVMPI_PROFILE
#include <dlfcn.h>
#endif

// Vita-specific headers
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/cpu.h>   /* v02.14: SCE_KERNEL_CPU_MASK_* for ticker pinning */
#include <psp2/io/fcntl.h> /* v01.84 site ring: sceIoOpen for vita_site_dump */

/*=========================================================================
 * Performance counter support
 *=======================================================================*/

static jlong _performance_frequency = 0;

static inline jlong get_clock_ticks(void) {
    // Use Vita's high-resolution timer (microseconds)
    return (jlong)sceKernelGetProcessTimeWide();
}

static inline void init_clock_ticks(void) {
    // sceKernelGetProcessTimeWide returns microseconds
    _performance_frequency = 1000 * 1000;
}

/*=========================================================================
 * Dynamic library loading (disabled on Vita - all natives static)
 *=======================================================================*/

#if ENABLE_DYNAMIC_NATIVE_METHODS || ENABLE_JVMPI_PROFILE
void* Os::loadLibrary(const char* libName) {
    return dlopen(libName, RTLD_LAZY);
}

void* Os::getSymbol(void* handle, const char* name) {
    return dlsym(handle, name);
}
#endif

/*=========================================================================
 * Time functions
 *=======================================================================*/

jlong Os::java_time_millis() {
    // Use gettimeofday for epoch-based time
    struct timeval tv;
    ::jvm_gettimeofday(&tv, NULL);
    return (jlong)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

/*
 * Sleep for ms Milliseconds, a sleep of 0ms is from the
 * scheduler requiring a yield, therefore we should call
 * sched_yield and not really sleep.
 */
void Os::sleep(jlong ms) {
    if (ms == 0) {
        sceKernelDelayThread(0);
        return;
    }

    jlong end = Os::java_time_millis() + ms;
    while (Os::java_time_millis() < end) {
        jlong remaining = end - Os::java_time_millis();
        if (remaining > 0) {
            sceKernelDelayThread((SceUInt)(remaining * 1000));
        }
    }
}

/*=========================================================================
 * Timer / tick functions
 * Vita does not support signals, so we use a polling-based approach
 * or disable ticks entirely.
 *=======================================================================*/

#if SUPPORTS_TIMER_THREAD

// Timer thread implementation using Vita SDK threads
// (only if SUPPORTS_TIMER_THREAD is enabled)

#if ENABLE_TIMER_THREAD
#include <psp2/kernel/threadmgr.h>

static SceUID ticker_thread_id = -1;
static volatile bool ticker_running = false;
static volatile bool ticker_stopping = false;
static volatile bool ticker_stopped = false;
static bool ticker_created = false;
/* v01.83: log9 showed the pump gate never opening for 3s while the VM
 * spun in a thread create/die cycle. Expose the tick count so the
 * watchdog HANG report can tell "ticker dead/starved" from "ticks
 * delivered but gate closed". Read via extern from vita_watchdog.c. */
extern "C" volatile unsigned int vita_tick_count = 0;

static int ticker_thread_routine(SceSize args, void *argp) {
    while (!ticker_stopping) {
        ticker_running = true;
        /* v02.17: liveness stamp. vita_tick_count already counts ticks
         * that reached real_time_tick(), but a single sample cannot tell
         * "frozen" from "slow"; the watchdog takes TWO samples 400 ms
         * apart (see vita_wd_snapshot) and compares, and this stamp says
         * how long ago the ticker thread last ran at all (even a ticker
         * stuck BEFORE real_time_tick would still update it). */
        vita_ticker_loops++;
        vita_ticker_last_ms = sceKernelGetProcessTimeWide() / 1000;
        sceKernelDelayThread(TickInterval * 1000);
        if (ticker_running && !ticker_stopping) {
            vita_tick_count++;
            real_time_tick(TickInterval);
        }
    }
    ticker_stopped = true;
    return 0;
}

bool Os::start_ticks() {
    if (!EnableTicks || Deterministic) {
        return true;
    }
    ticker_running = true;
    ticker_stopping = false;
    if (!ticker_created) {
        ticker_created = true;
        /* v02.14: pin the ticker to the SAME core as the VM thread.
         *
         * Upstream linux delivers timer ticks via setitimer+SIGVTALRM, i.e.
         * the signal handler runs on the VM's own thread (and therefore the
         * VM's own core). The vita port replaced that with a standalone
         * thread created with cpuAffinityMask=0 (inherit), which let the
         * ticker land on cpu0 (log38 thread audit: 'cldc_ticker' last=0)
         * while the VM is pinned to cpu1 (0x20000).
         *
         * real_time_tick() then performs non-atomic cross-core writes to
         * _rt_timer_ticks / _real_time_has_ticked / _compiler_stack_limit /
         * _timer_has_ticked - a data race that the upstream same-thread
         * model never had. Pinning the ticker to the VM's core restores the
         * upstream "tick delivered on the same core as the VM" semantics and
         * removes the cross-core visibility problem. */
        int vm_aff = sceKernelGetThreadCpuAffinityMask(sceKernelGetThreadId());
        if (vm_aff < 0) {
            /* Query failed (should not happen): fall back to all user cores.
             * Still strictly better than the old affinity=0 only if 0 meant
             * "inherit"; here we are explicit. */
            vm_aff = SCE_KERNEL_CPU_MASK_USER_ALL;
        }
        /* v02.17 runtime A/B for the v02.14 pinning. If the file
         * ux0:/data/J2ME00001/ticker_cpu exists and starts with '0' the
         * ticker goes back to the pre-v02.14 behaviour (cpu0 / all user
         * cores = "0"); '1' or '2' pin it explicitly to that cpu. Absent
         * = keep the v02.14 "same core as the VM thread" default. This
         * tests, in ONE build, whether the log40/log41 "cpu1 scheduler
         * DEAD" picture is caused or cured by putting the ticker on the
         * VM's core. */
        {
            int ab = vita_cfg_digit("ticker_cpu", -1);
            if (ab == 0) {
                vm_aff = SCE_KERNEL_CPU_MASK_USER_ALL;
            } else if (ab == 1) {
                vm_aff = SCE_KERNEL_CPU_MASK_USER_1;
            } else if (ab == 2) {
                vm_aff = SCE_KERNEL_CPU_MASK_USER_2;
            }
        }
        ticker_thread_id = sceKernelCreateThread(
            "cldc_ticker", ticker_thread_routine, 0x10000100, 0x10000, 0,
            vm_aff, NULL);
        if (ticker_thread_id < 0) {
            return false;
        }
        sceKernelStartThread(ticker_thread_id, 0, NULL);
    }
    return true;
}

void Os::suspend_ticks() {
    ticker_running = false;
    Os::sleep(1);
}

void Os::resume_ticks() {
    ticker_running = true;
    start_ticks();
}

void Os::stop_ticks() {
    if (ticker_created) {
        ticker_stopping = true;
        for (int i = 0; i < 10 && !ticker_stopped; i++) {
            sceKernelDelayThread(TickInterval * 1000);
        }
        if (ticker_thread_id >= 0) {
            sceKernelDeleteThread(ticker_thread_id);
            ticker_thread_id = -1;
        }
        ticker_created = false;
        ticker_running = false;
        ticker_stopped = false;
    }
}

#else // !ENABLE_TIMER_THREAD

// No timer support - stub implementations
bool Os::start_ticks() {
    return true;
}

void Os::suspend_ticks() {
}

void Os::resume_ticks() {
}

void Os::stop_ticks() {
}

#endif // ENABLE_TIMER_THREAD

#else // !SUPPORTS_TIMER_THREAD

// Timer thread not supported - stub implementations
bool Os::start_ticks() {
    return true;
}

void Os::suspend_ticks() {
}

void Os::resume_ticks() {
}

void Os::stop_ticks() {
}

#endif // SUPPORTS_TIMER_THREAD

/*=========================================================================
 * Performance counters
 *=======================================================================*/

#if (ENABLE_PERFORMANCE_COUNTERS || ENABLE_PROFILER || ENABLE_WTK_PROFILER \
     || ENABLE_TTY_TRACE)

jlong Os::elapsed_counter() {
#if ENABLE_PERFORMANCE_COUNTERS
    jvm_perf_count.hrtick_read_count++;
#endif
    return get_clock_ticks();
}

jlong Os::elapsed_frequency() {
    return _performance_frequency;
}

#endif // NEED_CLOCK_TICKS

/*=========================================================================
 * Initialization and cleanup
 *=======================================================================*/

void Os::initialize() {
#if NEED_CLOCK_TICKS
    init_clock_ticks();
#endif

#if SUPPORTS_ADJUSTABLE_MEMORY_CHUNK
    extern "C" void init_jvm_chunk_manager();
    init_jvm_chunk_manager();
#endif

    // Vita does not need signal handlers - we use polling for timer
    // and VM exceptions are handled differently

#if defined(VITA)
    /* v02.38 vm_cpu pin: the log61/62 coredump verdict - VM thread
     * RUNNING + wait=none + runClocks d=0 + KICK prio OK but suspend
     * 0x80020008 - the kernel run-queue loses a MIGRATING thread (aff
     * 0x70000, chgcpu ~2800). Pinning the VM thread to one core makes
     * migration impossible. Runtime A/B via ux0:/data/J2ME00001/vm_cpu
     * (digit 0/1/2; absent = keep inherited affinity, printed below so
     * every dump self-documents). */
    {
        extern int vita_cfg_digit(const char *name, int dflt);
        extern void vita_boot_log(const char *fmt, ...);
        int pin = vita_cfg_digit("vm_cpu", -1);
        SceUID self = sceKernelGetThreadId();
        int aff = sceKernelGetThreadCpuAffinityMask(self);
        if (pin >= 0 && pin <= 2) {
            int mask = (SCE_KERNEL_CPU_MASK_USER_0 << pin);
            int rc = sceKernelChangeThreadCpuAffinityMask(self, mask);
            vita_boot_log("[vita] vm_cpu=%d aff 0x%x->0x%x rc=0x%x",
                          pin, aff, mask, rc);
        } else {
            vita_boot_log("[vita] vm_cpu=off aff=0x%x", aff);
        }
    }
#endif
}

void Os::dispose() {
    // Nothing special to clean up on Vita
}

/*=========================================================================
 * Compiler timer
 *=======================================================================*/

static jlong _compiler_timer_start = 0;
#if ENABLE_COMPILER
static bool _compiler_timer_has_ticked = false;
#endif

void Os::start_compiler_timer() {
#if ENABLE_COMPILER
    if (MaxCompilationTime == TickInterval) {
        _compiler_timer_start = (jlong)0;
        _compiler_timer_has_ticked = false;
    } else {
        _compiler_timer_start = Os::java_time_millis();
    }
#endif
}

bool Os::check_compiler_timer() {
#if ENABLE_COMPILER
    if (_compiler_timer_start == (jlong)0) {
        return _compiler_timer_has_ticked;
    } else {
        jint elapsed_ms = (jint)(Os::java_time_millis() - _compiler_timer_start);
        return (elapsed_ms >= MaxCompilationTime);
    }
#else
    return true;
#endif
}

/*=========================================================================
 * v01.84 site ring: last-executed-site tracker for hang forensics
 *
 * log9+log10 coredumps froze the VM thread at start_lightweight_thread_asm+4
 * (dispatch frame on the OS stack) with CE frozen 3+ s while cldc_ticker
 * kept ticking. That rules out every loop passing through yield() or an
 * interpreter tick checkpoint (the v01.83 pump backstop sits inside
 * yield()). The spin lives in native C++ on a green thread's Java stack,
 * and the Java heap is not in the coredump, so the dump cannot name the
 * loop. vita_site_mark() stores one return address per call at strategic
 * native entry points (16-slot ring + seq). The launcher watchdog dumps
 * the ring on HANG: frozen seq + repeated tail names the loop.
 * Lock-free / allocation-free: single writer (VM thread), readers
 * tolerate torn seq. Dump path uses sceIo* directly - never stdio
 * (log10 residue frames point at the stdio domain; do not forensics
 * inside the crime scene).
 * Declaration lives in Globals_vita.hpp (included by every VM TU via
 * the generated .incl chain), so share/ call sites need no new headers.
 *=======================================================================*/

extern "C" {

/*=========================================================================
 * v02.11 tick-driven JIT compile switch.
 *
 * log35's freeze fingerprint - _rt_timer_ticks climbing while the tick
 * body entry count ttk and the Java-site sample count jsmp sat frozen
 * at 357, with the slow lane at 0 and the event pump frozen at the
 * same value as CE - puts the VM thread INSIDE Thread::timer_tick(),
 * past the Java-site sample and short of Scheduler::yield(). The only
 * heavy work in that window is the synchronous compile on the tick
 * (Compiler::on_timer_tick under #if ENABLE_COMPILER; that symbol is
 * in midp_vita.velf and crumb.log says UseCompiler=1).
 * One build, two behaviours: this returns 1 unless the file
 * ux0:/data/J2ME00001/tick_compile.cfg exists and starts with '0'
 * (or 'n'/'N'). Read once and cached; called from the tick on the VM
 * thread, so the file is opened at most once per process.
 *=========================================================================*/

static int _vita_tick_compile = -1; /* -1 = not decided yet */

int vita_tick_compile_enabled(void) {
    if (_vita_tick_compile < 0) {
        _vita_tick_compile = 1;  /* default = b332 behaviour */
        SceUID fd = sceIoOpen("ux0:/data/J2ME00001/tick_compile.cfg",
                              SCE_O_RDONLY, 0);
        if (fd >= 0) {
            char c = 0;
            if (sceIoRead(fd, &c, 1) == 1 &&
                (c == '0' || c == 'n' || c == 'N')) {
                _vita_tick_compile = 0;
            }
            sceIoClose(fd);
        }
    }
    return _vita_tick_compile;
}

/*=========================================================================
 * v02.17 VM-side tick-compile gate.
 *
 * v02.11 added a static switch for the tick-driven JIT compile, but the
 * hypothesis it tests (log35: freeze inside the synchronous compile
 * that Thread::timer_tick() runs on the VM thread) needs a second,
 * self-diagnosing form: compile only when the ticker is keeping up.
 * The ticker stamps vita_ticker_last_ms every loop iteration (10 ms
 * cadence), so "the ticker has not run for >1 s" means the VM thread is
 * monopolising the core and a long synchronous compile is exactly the
 * worst thing to start. Mode is read once from
 * ux0:/data/J2ME00001/tickgate:
 *   absent / 0 -> disabled (the v02.11 behaviour, no gate at all)
 *   1          -> skip the compile when the ticker is >1 s stale
 *   2          -> never compile on ticks (same as tick_compile.cfg "0")
 * One file, three behaviours, no rebuild - and the watchdog dump prints
 * the mode so the log says which one was active.
 *=======================================================================*/
static int _vita_tickgate_mode = -1;

int vita_jit_tick_compile_enabled(void) {
    int mode = _vita_tickgate_mode;
    if (mode < 0) {
        mode = vita_cfg_digit("tickgate", 0);
        if (mode < 0 || mode > 2) mode = 0;
        _vita_tickgate_mode = mode;
    }
    if (mode == 0) {
        return 1;
    }
    if (mode == 2) {
        return 0;
    }
    /* mode 1: a stale ticker means the VM is hogging the core. */
    if (vita_ticker_last_ms > 0) {
        long long now_ms = sceKernelGetProcessTimeWide() / 1000;
        if (now_ms - vita_ticker_last_ms > 1000) {
            return 0;
        }
    }
    return 1;
}

int vita_tickgate_mode(void) {
    (void)vita_jit_tick_compile_enabled(); /* force the one-time read */
    return _vita_tickgate_mode;
}

volatile unsigned int vita_site_seq = 0;

#define VITA_SITE_RING_SIZE 16
static const void *vita_site_ring[VITA_SITE_RING_SIZE];

/* noinline so the return address of the CALLER (the marked site) is
 * reliably captured; every entry in the ring is addr2line-able. */
__attribute__((noinline)) void vita_site_mark(void) {
    unsigned int s = vita_site_seq;
    vita_site_ring[s & (VITA_SITE_RING_SIZE - 1)] =
        __builtin_return_address(0);
    vita_site_seq = s + 1;
}

/*=========================================================================
 * v02.17 one-shot forensics - state.
 *
 * Real-hardware iterations are expensive, so v02.17 puts every probe the
 * remaining open questions need into ONE build. All state here is plain
 * volatile data written by VM threads and read by the launcher watchdog
 * after a HANG; no syscalls, no locks, no allocation - the two probe
 * designs that did touch foreign TCBs (log33 GetThreadRunStatus, log34)
 * each killed the watchdog thread we depend on.
 *
 * The counters answer, in one glance at the hang log:
 *   - did the ticker thread keep running after the freeze?  (ticker_loops
 *     frozen while the thread is alive == ticker starved, i.e. the core it
 *     is pinned to is no longer scheduling)
 *   - is the VM cycling through the SAME Java thread creation repeatedly?
 *     (tc_hist ring: same class.method 8/8 == create/die spin)
 *   - does the bootstrap path advance at all (run/sched_start/start0) or
 *     does it stop exactly at the first new-thread dispatch?
 *=========================================================================*/

volatile unsigned int vita_run_count = 0;
volatile unsigned int vita_sched_start_count = 0;
volatile unsigned int vita_thread_start_count = 0;
volatile unsigned int vita_thread_finish_count = 0;
volatile unsigned int vita_lw_exit_count = 0;
/* v02.31: macro-loop (zombie redispatch) probes. The log53 verdict put
 * the freeze in the birth-stub <-> ipe window with ALL v02.17 lane
 * counters frozen (switch/yield/gnrt) - so if a loop runs at all it
 * must cycle through the C++ bodies the stub tail invokes via
 * call_on_primordial_stack: finish / force_terminated /
 * thread_task_cleanup / lightweight_thread_exit(+terminate+yield).
 * Those bodies live in _MergedSrc002/004 (live objects, rebuilt every
 * time - NOT the loopgen-frozen Interpreter_arm.o). At the hang, two
 * watchdog snapshots decide:
 *   - any of finish/force/term/setcur still climbing  -> macro-loop
 *     confirmed AND named (which body it spins through)
 *   - all frozen, cpu1 still busy                     -> the loop is
 *     confined to the stub cluster asm itself (no C++ body involved)
 * term_listed>0 -> terminate ran on a thread that stayed on the global
 * list (double-terminate or re-inserted zombie).
 * exit-ring: last 8 lightweight_thread_exit records (id, status, pe,
 * chain-word at +0x64 the primordial gadget walks). If the VM keeps
 * dispatching an id whose LAST exit-ring record shows the death view,
 * redispatch-after-death is proven without waiting for a dump. */
volatile unsigned int vita_v231_finish     = 0;
volatile unsigned int vita_v231_force_term = 0;
volatile unsigned int vita_v231_setcur     = 0;
volatile unsigned int vita_v231_term       = 0;
volatile unsigned int vita_v231_term_listed = 0;
/* v02.40: landing-pad probes. Definitions live in
 * InterpreterRuntime_arm.cpp (_MergedSrc004.o, fresh-compiled): the
 * bootstrap sentinel word now points at vita_landing_pad instead of
 * the frozen trampoline, so land>0 proves the sentinel handoff ran
 * at least once in fresh code; fp/lr record the entry template
 * (expected fp=0, lr=pad address). */
extern "C" {
extern volatile unsigned int vita_v240_land;
extern volatile unsigned int vita_v240_fp;
extern volatile unsigned int vita_v240_lr;
/* v02.42: pe liveness counters from the landing pad (see
 * InterpreterRuntime_arm.cpp): slot 0 = crossings with pe==NULL,
 * slot 1 = crossings with pe!=NULL. */
extern volatile unsigned int vita_v242_pe[2];
/* v02.44: bouncer anatomy from the landing pad (see
 * InterpreterRuntime_arm.cpp): ct = _current_thread object at the
 * last pad crossing, r6 = the exit-table base the birth stub will
 * index. rd7 dereferences r6 to dissect the birth frame. */
extern volatile unsigned int vita_v244_ct;
extern volatile unsigned int vita_v244_r6;
/* v02.45: saturation forensics from the landing pad (see
 * InterpreterRuntime_arm.cpp): hist = last-4-bounce (r6, lr) ring,
 * flood = gate state + full register scene saved at trip (layout in
 * the pad source comment). When flood[1] != 0 the trap dumper below
 * already wrote ux0:/data/J2ME00001/ring_trap.log and exited. */
extern volatile unsigned int vita_v245_hist[17];   /* v02.50: 4 words/slot */
extern volatile unsigned int vita_v245_flood[24];  /* v02.50: +[14] cpe, [20] brakes */
extern volatile unsigned int vita_v250_brakes;     /* v02.50: brake lap counter */
extern void vita_landing_pad(void);
}
/* v02.49: ring-counter block for ring_trap.log - see the v249 line in
 * vita_wd_snapshot_one(). taskclean is defined in Thread.cpp (inside
 * the v02.49 counter note there); finish/force/setcur/term live in
 * this file above; land/pe0/pe1 in InterpreterRuntime_arm.cpp. */
extern volatile unsigned int vita_v249_taskclean;
#define VITA_V231_EXIT_RING 8
volatile unsigned int vita_v231_exit_ring[VITA_V231_EXIT_RING][4];
volatile unsigned int vita_v231_exit_seq = 0;
extern "C" void vita_v231_exit_ring_rec(unsigned int id, unsigned int st,
                                        unsigned int pe, unsigned int chain)
{
    unsigned int s = vita_v231_exit_seq;
    vita_v231_exit_ring[s & (VITA_V231_EXIT_RING - 1)][0] = id;
    vita_v231_exit_ring[s & (VITA_V231_EXIT_RING - 1)][1] = st;
    vita_v231_exit_ring[s & (VITA_V231_EXIT_RING - 1)][2] = pe;
    vita_v231_exit_ring[s & (VITA_V231_EXIT_RING - 1)][3] = chain;
    vita_v231_exit_seq = s + 1;
}
volatile unsigned int vita_start0_count = 0;
volatile unsigned int vita_justborn_dispatch = 0;
volatile unsigned int vita_ticker_loops = 0;
volatile long long   vita_ticker_last_ms = 0;

volatile unsigned int vita_tc_count = 0;
volatile char vita_tc_class[VITA_TC_HIST][40];
volatile char vita_tc_method[VITA_TC_HIST][40];

void vita_tc_note(const char *cls, int clen, const char *mth, int mlen) {
    unsigned int s = vita_tc_count;
    int slot = (int)(s & (VITA_TC_HIST - 1));
    int i;
    if (clen < 0) clen = 0;
    if (clen > 39) clen = 39;
    if (mlen < 0) mlen = 0;
    if (mlen > 39) mlen = 39;
    for (i = 0; i < clen && cls; i++) vita_tc_class[slot][i] = cls[i];
    vita_tc_class[slot][i] = 0;
    for (i = 0; i < mlen && mth; i++) vita_tc_method[slot][i] = mth[i];
    vita_tc_method[slot][i] = 0;
    vita_tc_count = s + 1;
}

/*=========================================================================
 * v02.17 runtime A/B switch reader.
 *
 * Same idea as vita_tick_compile_enabled(), generalised: a file named
 * ux0:/data/J2ME00001/<name> whose first byte is '0'/'n'/'N' turns the
 * named behaviour OFF. A single build can therefore test several
 * hypotheses by editing tiny files on the memory card - the alternative
 * (one build per hypothesis) is what makes real-hardware work painful.
 * Names are looked up in a small static cache so each file is opened at
 * most once per process; the lookup runs on the VM thread.
 *=========================================================================*/
#define VITA_CFG_MAX 8
static const char *vita_cfg_names[VITA_CFG_MAX];
static int         vita_cfg_vals[VITA_CFG_MAX];
static int         vita_cfg_used = 0;

static int vita_cfg_name_eq(const char *a, const char *b) {
    if (a == b) return 1;
    if (a == NULL || b == NULL) return 0;
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

int vita_cfg_flag(const char *name, int dflt) {
    int i;
    for (i = 0; i < vita_cfg_used; i++) {
        if (vita_cfg_name_eq(vita_cfg_names[i], name)) return vita_cfg_vals[i];
    }
    {
        int val = dflt;
        char path[64];
        SceUID fd;
        int n = snprintf(path, sizeof(path), "ux0:/data/J2ME00001/%s", name);
        if (n > 0 && n < (int)sizeof(path)) {
            fd = sceIoOpen(path, SCE_O_RDONLY, 0);
            if (fd >= 0) {
                char c = 0;
                if (sceIoRead(fd, &c, 1) == 1 &&
                    (c == '0' || c == 'n' || c == 'N')) {
                    val = 0;
                }
                sceIoClose(fd);
            }
        }
        if (vita_cfg_used < VITA_CFG_MAX) {
            vita_cfg_names[vita_cfg_used] = name;
            vita_cfg_vals[vita_cfg_used] = val;
            vita_cfg_used++;
        }
        return val;
    }
}

/* v02.17: same file protocol but returns the VALUE as a small integer, so
 * one switch can select among several variants (ticker_cpu: 0/1/2).
 * Absent / unreadable / not a digit => dflt. Not cached separately from
 * vita_cfg_flag's table, because the returned value is what matters. */
int vita_cfg_digit(const char *name, int dflt) {
    int val = dflt;
    char path[64];
    SceUID fd;
    int n = snprintf(path, sizeof(path), "ux0:/data/J2ME00001/%s", name);
    if (n > 0 && n < (int)sizeof(path)) {
        fd = sceIoOpen(path, SCE_O_RDONLY, 0);
        if (fd >= 0) {
            char c = 0;
            if (sceIoRead(fd, &c, 1) == 1 && c >= '0' && c <= '9') {
                val = c - '0';
            }
            sceIoClose(fd);
        }
    }
    return val;
}

/*=========================================================================
 * v01.85 verifier deep tracing: log10's ring named the loop's
 * neighborhood (last event = Verifier::verify_class entry, then 3+ s of
 * silence) but not the loop itself - verification runs on the green
 * thread's Java stack, invisible to both coredump and OS stack. These
 * two hooks give the next dump a name: the class.method being verified
 * at hang time, plus per-phase marks so the ring can tell which
 * verifier phase is spinning.
 * Written from the VM thread (single writer, same discipline as the
 * ring); read by the watchdog via sceIo* only.
 *=======================================================================*/

static char vita_verify_class_name[64];
static char vita_verify_method_name[64];

/* Called at the top of Verifier::verify_class_internal per class.
 * v01.86: takes an explicit length - Symbols are length-prefixed and
 * NOT NUL-terminated, so a strlen-style copy ran past the name into
 * adjacent symbol data (the garbage bytes after "com/app/filemanager/UI"
 * in log12's site_ring.log). */
void vita_site_verifier_class(const char *name, int len) {
    int i = 0;
    if (name && len > 0) {
        if (len > 63) len = 63;
        for (; i < len; i++)
            vita_verify_class_name[i] = name[i];
    }
    vita_verify_class_name[i] = 0;
}

/* Called at the top of VerifyMethodCodes::verify per method (name may
 * be NULL for <init>/<clinit> - caller passes what it has). Same
 * length-bounded copy as above (v01.86). */
void vita_site_verifier_method(const char *name, int len) {
    int i = 0;
    if (name && len > 0) {
        if (len > 63) len = 63;
        for (; i < len; i++)
            vita_verify_method_name[i] = name[i];
    }
    vita_verify_method_name[i] = 0;
}

/* Called from the watchdog thread after a HANG. Appends the current
 * verifier target (if any) to the site ring log. */
void vita_site_dump_verify(const char *path) {
    SceUID fd = sceIoOpen(path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND,
                          0777);
    if (fd < 0) return;
    char buf[160];
    int n = snprintf(buf, sizeof(buf), "verify class=%s method=%s\n",
                     vita_verify_class_name[0] ? vita_verify_class_name
                                               : "(none)",
                     vita_verify_method_name[0] ? vita_verify_method_name
                                                : "(none)");
    sceIoWrite(fd, buf, n);
    sceIoClose(fd);
}

/*=========================================================================
 * v01.88 clinit tracing: log14 froze with the verifier OFF (verify
 * class=(none)) after 345 frames of normal gameplay, ring seq=3 with
 * three open_entry marks - the spin moved from verification into the
 * execution of the fourth lazily-loaded class's <clinit> itself, on
 * the green thread's Java stack (invisible to coredump and OS stack,
 * same as the verifier spins were). This hook records the class whose
 * clinit is about to run so the HANG dump names it.
 *=======================================================================*/

static char vita_clinit_class_name[64];

/* v01.89: clinit history ring - log15's ring showed 13 clinit marks
 * interleaved with 3 open_entry marks (a class-load storm inside
 * UI.<clinit>), but the single-slot name only recorded the LAST class.
 * This history names the whole chain the storm walked through. Same
 * single-writer discipline (VM thread writes, watchdog reads). */
#define VITA_CLINIT_HIST 16
static char vita_clinit_hist[VITA_CLINIT_HIST][48];
/* v01.90: exported (drop static) so the launcher watchdog can treat it
 * as a liveness/progress counter - a class-load storm advances it even
 * while the CE pump is starved (log16: CE frozen 3.2 s while the storm
 * kept loading classes on real ux0: I/O). */
volatile unsigned int vita_clinit_count = 0;
static volatile unsigned int vita_clinit_depth = 0;
static volatile unsigned int vita_clinit_depth_max = 0;

/* v01.90: GC pass counter. The coredump cannot see the Java heap, so a
 * stop-the-world GC spin is invisible to every current forensic hook;
 * a monotonically advancing count at least names "GC was running and
 * how many times" in the HANG dump. */
volatile unsigned int vita_gc_count = 0;

/* v01.91: eager-preload progress counter (incremented in
 * Universe::load_jar_entry after a successful define, see
 * JVM::load_main_class for the log17 rationale). The watchdog reads
 * it so the boot-time define sweep - which freezes the CE pump by
 * design - is not mistaken for a hang. */
volatile unsigned int vita_preload_count = 0;

/* Called at the top of InstanceClass::clinit per class. Same explicit
 * length copy as the verifier hooks (Symbols are length-prefixed). */
void vita_site_clinit_class(const char *name, int len) {
    int i = 0;
    if (name && len > 0) {
        if (len > 63) len = 63;
        for (; i < len; i++)
            vita_clinit_class_name[i] = name[i];
    }
    vita_clinit_class_name[i] = 0;
    /* v01.89: append to the history ring (48-char slots) */
    {
        unsigned int c = vita_clinit_count;
        char *slot = vita_clinit_hist[c & (VITA_CLINIT_HIST - 1)];
        int j = 0;
        if (len > 47) len = 47;
        for (; j < len; j++)
            slot[j] = name[j];
        slot[j] = 0;
        vita_clinit_count = c + 1;
        /* depth tracking: clinit chains nest (A.clinit -> new B ->
         * B.clinit -> ...). A storm shows high depth or many marks. */
        unsigned int d = ++vita_clinit_depth;
        if (d > vita_clinit_depth_max)
            vita_clinit_depth_max = d;
    }
}

/* Called from the watchdog thread after a HANG. Appends the clinit
 * target (if any) to the site ring log. v01.89: also dumps the last
 * 16 clinit classes (chain history) and the depth counters - log15
 * proved a single name is not enough to see a load storm. */
void vita_site_dump_clinit(const char *path) {
    SceUID fd = sceIoOpen(path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND,
                          0777);
    if (fd < 0) return;
    char buf[96];
    int n = snprintf(buf, sizeof(buf), "clinit class=%s\n",
                     vita_clinit_class_name[0] ? vita_clinit_class_name
                                               : "(none)");
    sceIoWrite(fd, buf, n);
    n = snprintf(buf, sizeof(buf), "clinit stats count=%u depth=%u max=%u gc=%u preload=%u\n",
                 vita_clinit_count, vita_clinit_depth, vita_clinit_depth_max,
                 vita_gc_count, vita_preload_count);
    sceIoWrite(fd, buf, n);
    /* history: newest first (same ordering as the site ring) */
    unsigned int c = vita_clinit_count;
    for (int i = 0; i < VITA_CLINIT_HIST; i++) {
        const char *slot = vita_clinit_hist[(c - 1 - i) &
                                            (VITA_CLINIT_HIST - 1)];
        if (slot[0] == 0) continue;
        n = snprintf(buf, sizeof(buf), "clinit [%2d] %s\n", i, slot);
        sceIoWrite(fd, buf, n);
    }
    sceIoClose(fd);
}

/* v01.89: called when a clinit RETURNS (InstanceClass::set_initialized
 * path is the completion signal). Decrements the nesting depth so the
 * watchdog dump can tell "storm of sequential clinits" (depth low,
 * count high) from "deep recursive clinit chain" (depth high). */
void vita_site_clinit_return(void) {
    if (vita_clinit_depth > 0)
        vita_clinit_depth--;
}

/*=========================================================================
 * v02.05 interpreter Java-site sampling: log29's verdict (overturning
 * log27/28 - those symbolizations were slide-error artifacts) is the
 * log23 fingerprint: every liveness counter frozen (fast=372 slow=0
 * pump=381==CE switch=188996, clinit=com/app/filemanager/UI) while
 * the VM thread pc sat at interpreter_timer_tick+0x14. Three
 * hypotheses survive: (a) the interpreter no longer passes back-
 * branch checkpoints (the pc sample was the last instant before the
 * freeze), (b) checkpoints fire but Thread::timer_tick never
 * completes (frozen mid-call), (c) they fire and complete but the
 * yield inside bails early every time. This probe samples the
 * CURRENT Java class.method from the top of Thread::timer_tick (the
 * exact function the frozen pc pointed into): a sample count that
 * keeps CLIMBING while the lane/pump counters stay frozen kills (a)
 * and names the Java loop; a frozen count confirms (a). Written from
 * the VM thread (single writer, same discipline as the verifier and
 * clinit hooks), read by the watchdog through sceIo only.
 * PRODUCT-safe: explicit length-bounded Symbol copies only - no
 * Stream/Signature parsing (Method::print_name_on is
 * USE_DEBUG_PRINTING-gated and empty in PRODUCT builds).
 *=======================================================================*/

static char vita_java_class_name[64];
static char vita_java_method_name[64];
volatile unsigned int vita_java_sample_count = 0;

/* Called from the top of Thread::timer_tick (see Thread.cpp) on the
 * VM thread. JavaFrame construction matches the ENABLE_PROFILER block
 * that upstream ships at the same point - the frame at a timer tick
 * is always an interpreted Java frame. */
void vita_site_java_frame(void) {
    vita_java_sample_count++;
    /* JavaFrame(Thread*) matches the upstream ENABLE_PROFILER block at
     * this exact point in Thread::timer_tick - the frame at a timer
     * tick is always an interpreted Java frame. */
    Thread *thread = Thread::current();
    JavaFrame f(thread);
    /* Raw handles: the () operator (returning the stack-handle view)
     * only exists on the ::Raw forms, not on Method directly. */
    Method::Raw m(f.method());
    Symbol::Raw mn = m().name();
    if (mn.not_null()) {
        int len = mn().length();
        char *src = mn().base_address();
        int i = 0;
        if (src && len > 0) {
            if (len > 63) len = 63;
            for (; i < len; i++)
                vita_java_method_name[i] = src[i];
        }
        vita_java_method_name[i] = 0;
    }
    InstanceClass::Raw h = m().holder();
    if (h.not_null()) {
        Symbol::Raw cn = h().name();
        if (cn.not_null()) {
            int len = cn().length();
            char *src = cn().base_address();
            int i = 0;
            if (src && len > 0) {
                if (len > 63) len = 63;
                for (; i < len; i++)
                    vita_java_class_name[i] = src[i];
            }
            vita_java_class_name[i] = 0;
        }
    }
}

/* Called from the watchdog thread after a HANG. Appends the sampled
 * Java site (if any) to the site ring log, plus the counter that
 * separates "interpreter still ticking" from "checkpoints dead". */
void vita_site_dump_java(const char *path) {
    SceUID fd = sceIoOpen(path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND,
                          0777);
    if (fd < 0) return;
    char buf[128];
    int n = snprintf(buf, sizeof(buf), "java site=%s.%s\n",
                     vita_java_class_name[0] ? vita_java_class_name
                                             : "(none)",
                     vita_java_method_name[0] ? vita_java_method_name
                                              : "(none)");
    sceIoWrite(fd, buf, n);
    n = snprintf(buf, sizeof(buf), "java samples=%u\n",
                 vita_java_sample_count);
    sceIoWrite(fd, buf, n);
    sceIoClose(fd);
}

/* Called by the launcher watchdog thread after a HANG is detected.
 * Slot (seq-1)&15 is the newest site; ordering below is newest first. */
void vita_site_dump(const char *path) {
    SceUID fd = sceIoOpen(path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND,
                          0777);
    if (fd < 0) return;
    char buf[128];
    int n;
    /* v02.15: the v02.11 form read vita_site_seq twice back-to-back with
     * NO delay between the reads, so s == s2 was a tautology and every
     * log printed "frozen=yes" (log35..log39 all inherit the artifact -
     * which is why every frozen-ring inference drawn from those logs is
     * void). This helper runs on the WATCHDOG thread, which is alive
     * even when the VM thread is not, so a real liveness test IS
     * possible here: 200 ms between the two reads makes the flag mean
     * what it always claimed - "the VM produced no new site mark in
     * 200 ms". Keep the wording so old logs stay comparable. */
    unsigned int s = vita_site_seq;
    sceKernelDelayThread(200 * 1000);
    unsigned int s2 = vita_site_seq;
    /* v02.11: also print the address of vita_site_mark itself. Ring
     * slots hold return addresses inside THIS image, and log35's hot
     * slot decoded 0x40 below the nm value for vita_site_mark - with
     * the marker's own runtime address in the same file every future
     * slot is directly addr2line-able without guessing a slide. */
    n = snprintf(buf, sizeof(buf),
                 "site seq=%u frozen=%s self=0x%08x mark=0x%08x\n", s,
                 s == s2 ? "yes" : "NO",
                 (unsigned int)(unsigned long)&vita_site_dump,
                 (unsigned int)(unsigned long)&vita_site_mark);
    sceIoWrite(fd, buf, n);
    for (int i = 0; i < VITA_SITE_RING_SIZE; i++) {
        int idx = (int)((s - 1 - (unsigned int)i) &
                        (VITA_SITE_RING_SIZE - 1));
        const void *v = vita_site_ring[idx];
        if (v == NULL) continue;
        n = snprintf(buf, sizeof(buf), "  [%2d] 0x%08x\n", i,
                     (unsigned int)(unsigned long)v);
        sceIoWrite(fd, buf, n);
    }
    sceIoClose(fd);
}

/*=========================================================================
 * v01.95 live-globals snapshot: log21's coredump proved the capture
 * holes are structural - Vita3K only grabs dirty anonymous pages, so
 * the main module's .data/.bss head (jvm_fast_globals._current_thread,
 * VerifyOnly, Scheduler::_timer_has_ticked / _estimated_event_readiness)
 * and the whole Java heap (Thread oop fields, Java stacks) are NEVER in
 * the dump. Every one of those values is one pointer deref away from
 * code that already links into the VM image, so instead of reading a
 * corpse, read the PATIENT: this runs in the VM image (sees every
 * symbol without slide arithmetic), called by the launcher watchdog
 * after a HANG, right before the site-ring dumps. Read-only, single
 * reader, torn values tolerated (the thread is frozen anyway).
 * NOTE: _OS_vita.cpp.incl pulls Thread.hpp but NOT Scheduler.hpp, so
 * the scheduler counters are reached through extern "C" accessors
 * (defined in Scheduler.cpp) instead of the C++ class - keeps the
 * generated include chain untouched.
 *=======================================================================*/

extern "C" {
    /* Defined in Scheduler.cpp (v01.95). */
    int  vita_sched_async_count(void);
    int  vita_sched_active_count(void);
    int  vita_sched_timer_ticked(void);
    int  vita_sched_ev_readiness(void);
    /* v01.98: lane/pump/switch counters + thread inventory. */
    unsigned int vita_sched_yield_fast(void);
    unsigned int vita_sched_yield_slow(void);
    unsigned int vita_sched_pump_runs(void);
    unsigned int vita_sched_switch_count(void);
    /* v02.00: sleep_forever forensics (see Scheduler.cpp). */
    unsigned int vita_sched_sf_entries(void);
    unsigned int vita_sched_sf_spins(void);
    unsigned int vita_sched_sf_last_ms(void);
    /* v02.01: dispatch counters (see Scheduler.cpp). */
    unsigned int vita_sched_gnrt_hits(void);
    unsigned int vita_sched_gnrt_nulls(void);
    /* v02.05: timer-tick chain forensics (see Scheduler.cpp) -
     * _rt_timer_ticks snapshot + Thread::timer_tick entry count. */
    unsigned int vita_sched_rt_ticks(void);
    unsigned int vita_sched_ttk_count(void);
    /* v02.11: tick-stage probe readouts (state in Thread.cpp). */
    unsigned int vita_sched_ttk_stage(void);
    unsigned int vita_sched_ttk_hits(int i);
    int  vita_sched_dump_threads(char *out, int out_size);
    /* v02.17: next-runnable thread state, dispatch ring, Java-stack top,
     * JUST_BORN dispatch counter (all defined in Scheduler.cpp). */
    unsigned int vita_sched_nrt_cell(void);
    unsigned int vita_sched_nrt_obj(void);
    int  vita_sched_nrt_info(char *out, int out_size);
    unsigned int vita_sched_justborn(void);
    int  vita_sched_dump_dispatch(char *out, int out_size);
    int  vita_sched_dump_qring(char *out, int out_size);
    int  vita_sched_dump_stack(char *out, int out_size);
    /* v02.46: per-thread SYMBOLIC Java stack walk (Scheduler.cpp) -
     * trace_stack/print_on/pss are all #ifndef PRODUCT, this is the
     * PRODUCT-surviving equivalent. */
    int  vita_sched_dump_jstack(char *out, int out_size);
}

/* v02.17: one text-append helper (avoid the hand-counted sceIoWrite
 * lengths that made older dumps hard to extend). */
static void vita_wd_puts(SceUID fd, const char *s) {
    int l = 0;
    if (s == NULL) return;
    while (s[l]) l++;
    if (l > 0) sceIoWrite(fd, s, l);
}

/* v02.17: ONE snapshot pass. Called twice by vita_wd_snapshot() with a
 * 400 ms gap so every counter appears with its delta in the same file -
 * a single sample cannot separate "frozen" from "slow", which is exactly
 * the ambiguity that made log36..log41 hard to read. */
static void vita_wd_snapshot_one(SceUID fd) {
    char buf[256];
    int n;

    /* Thread::current() expands to (Thread*)&_current_thread, i.e.
     * &jvm_fast_globals.current_thread - the handle cell itself.
     * v02.05 FIX: v02.01 added an explicit second dereference here
     * (cur = *(OopDesc**)ct) and then called cur->status() etc. But
     * phoneME handles are "method call dereferences *this": ct->foo()
     * already reads through the cell. The explicit deref made every
     * read land one OopDesc header off - log29's wd_snapshot printed
     * garbage for status/pending/jsp. Use ct directly; keep printing
     * the cell contents address (not dereferenced again) for run-to-
     * run comparison with the older logs. */
    Thread *ct = Thread::current();
    n = snprintf(buf, sizeof(buf),
                 "wd_snapshot cur_cell=0x%08x cur_thread=0x%08x\n",
                 (unsigned int)(unsigned long)ct,
                 (unsigned int)(unsigned long)*(OopDesc **)ct);
    sceIoWrite(fd, buf, n);

    if (ct != NULL && (unsigned int)(unsigned long)*(OopDesc **)ct > 0x1000) {
        jint st = ct->status();
        ReturnOop pe = ct->pending_entries();
        n = snprintf(buf, sizeof(buf),
                     "  status=0x%08x pending=0x%08x jsp=0x%08x\n",
                     (unsigned int)st,
                     (unsigned int)(unsigned long)pe,
                     (unsigned int)ct->stack_pointer());
        sceIoWrite(fd, buf, n);
        if (st & THREAD_TERMINATING)
            sceIoWrite(fd, "  TERMINATING\n", 14);
        if (st & THREAD_JUST_BORN)
            sceIoWrite(fd, "  JUST_BORN\n", 12);
        if (st & THREAD_DEAD)
            sceIoWrite(fd, "  DEAD\n", 7);
        if (st & THREAD_NOT_ACTIVE_MASK)
            sceIoWrite(fd, "  NOT_ACTIVE\n", 13);
    }

    n = snprintf(buf, sizeof(buf),
                 "  active_count=%d async_count=%d VerifyOnly=%d\n",
                 vita_sched_active_count(), vita_sched_async_count(),
                 (int)VerifyOnly);
    sceIoWrite(fd, buf, n);
    n = snprintf(buf, sizeof(buf),
                 "  timer_ticked=%d ev_readiness=%d\n",
                 vita_sched_timer_ticked(), vita_sched_ev_readiness());
    sceIoWrite(fd, buf, n);
    /* v02.05: timer-tick chain - log29 froze with pc inside
     * Thread::timer_tick while every lane/pump counter was frozen.
     * rt_ticks is the raw _rt_timer_ticks global (ticker writes it,
     * clear_timer_tick zeroes it after a completed Thread::timer_tick);
     * ttk is the entry count of Thread::timer_tick itself. The pair
     * separates "ticker alive but back-branch checkpoints dead"
     * (rt_ticks climbing, ttk frozen) from "checkpoints fire but
     * timer_tick never completes" (ttk frozen at a value that stopped
     * advancing WITH rt_ticks, sample count also frozen). */
    n = snprintf(buf, sizeof(buf),
                 "  rt_ticks=%u ttk=%u jsmp=%u\n",
                 vita_sched_rt_ticks(), vita_sched_ttk_count(),
                 vita_java_sample_count);
    sceIoWrite(fd, buf, n);
    /* v02.11: which step of the last tick is the VM thread inside?
     * stage 0 = not inside a tick (a completed tick ends at 7). A
     * frozen stage N with hits[N] == hits[N-1] names the wedged step:
     * 2 = after java-frame sample, 3 = inside the JIT compile, 5 = in
     * the yield/switch path, 7 = tick completed. */
    n = snprintf(buf, sizeof(buf),
                 "  ttk_stage=%u hits 1..7=%u/%u/%u/%u/%u/%u/%u\n",
                 vita_sched_ttk_stage(),
                 vita_sched_ttk_hits(1), vita_sched_ttk_hits(2),
                 vita_sched_ttk_hits(3), vita_sched_ttk_hits(4),
                 vita_sched_ttk_hits(5), vita_sched_ttk_hits(6),
                 vita_sched_ttk_hits(7));
    sceIoWrite(fd, buf, n);
    /* v01.98: log22 quantified the ping-pong but could not separate
     * "pump starving in the fast lane" from "pump running but
     * delivering nothing". These four counters do: yield_fast climbing
     * while pump_runs stalls = starvation (the log22 hypothesis);
     * pump_runs climbing with no event delivery = the pump runs but
     * check_blocked_threads()/JVMSPI_CheckEvents drops the ball. */
    n = snprintf(buf, sizeof(buf),
                 "  lanes fast=%u slow=%u pump=%u switch=%u sf=%u/%u/%u"
                 " gnrt=%u/%u\n",
                 vita_sched_yield_fast(), vita_sched_yield_slow(),
                 vita_sched_pump_runs(), vita_sched_switch_count(),
                 vita_sched_sf_entries(), vita_sched_sf_spins(),
                 vita_sched_sf_last_ms(),
                 vita_sched_gnrt_hits(), vita_sched_gnrt_nulls());
    sceIoWrite(fd, buf, n);

    /*-------------------------------------------------------------------
     * v02.17 read-outs.
     * v02.18 REORDER: the ring (5) and the java-stack top (6) now come
     * FIRST. log42's snapshot died inside the nrt probe (4) before it
     * ever reached them, and the post-mortem could not tell "not yet
     * reached" from "probe crashed" - with a [rdN ok] line after every
     * section a truncated file names the exact section that died.
     *-----------------------------------------------------------------*/

    /* (5) Dispatch ring: the last N threads switch_thread() switched AWAY
     * from, newest first. Tells which threads the dispatcher cycles
     * through right before the freeze (same id repeating == a hot loop
     * through one thread; a fixed rotation == a real round-robin). */
    {
        /* v02.42 (5a) queue-op ring FIRST: it is the CAUSE-side record
         * (who re-queued / re-selected whom, frozen at the moment the
         * pure-asm spin loop went dark). The dispatch ring below only
         * shows the EFFECT (who the last dispatches cycled through).
         * See vita_sched_dump_qring in Scheduler.cpp for the op legend. */
        static char qring_dump[2048];
        int len = vita_sched_dump_qring(qring_dump, (int)sizeof(qring_dump));
        if (len > 0) sceIoWrite(fd, qring_dump, len);
        vita_wd_puts(fd, "[rd0 qring ok]\n");
    }
    {
        static char disp_dump[2048];
        int len = vita_sched_dump_dispatch(disp_dump, (int)sizeof(disp_dump));
        if (len > 0) sceIoWrite(fd, disp_dump, len);
        vita_wd_puts(fd, "[rd1 dispatch ok]\n");
    }

    /* (6) Top of the Java stacks. The coredump never contains the Java
     * heap, so the bootstrap frame built by setup_stack_asm (frame
     * pointer 0 + landing pad + TestCompiler) has never
     * been visible. The watchdog CAN read it - it runs in the same
     * process. A top-of-stack matching setup_stack_asm proves the thread
     * has never executed; anything else shows how far it got.
     * v02.40: the sentinel word is now vita_landing_pad (fresh code),
     * NOT start_lightweight_thread_asm - see the v240 probe line. */
    {
        static char stack_dump[1024];
        int len = vita_sched_dump_stack(stack_dump, (int)sizeof(stack_dump));
        if (len > 0) sceIoWrite(fd, stack_dump, len);
        vita_wd_puts(fd, "[rd2 stacktop ok]\n");
    }

    /* (4) Next-runnable cache: the scheduler's own answer to "who should
     * run". _next_runnable_thread is a handle cell; print the cell, the
     * object and the object's fields (id/status/pending/jsp/wait).
     * v02.18: the field read is now raw-offset (see Scheduler.cpp) - a
     * stale object DEGRADES to a garbage line instead of a data abort
     * that kills the whole snapshot. */
    n = snprintf(buf, sizeof(buf),
                 "  nrt cell=0x%08x obj=0x%08x\n",
                 vita_sched_nrt_cell(), vita_sched_nrt_obj());
    sceIoWrite(fd, buf, n);
    {
        static char nrt_info[256];
        int len = vita_sched_nrt_info(nrt_info, (int)sizeof(nrt_info));
        if (len > 0) sceIoWrite(fd, nrt_info, len);
        vita_wd_puts(fd, "[rd3 nrt ok]\n");
    }

    /* (1) Ticker liveness. vita_tick_count counts ticks that REACHED
     * real_time_tick(); vita_ticker_loops counts ticker loop iterations
     * (stamped BEFORE the delay), so a ticker alive but stuck inside or
     * just after real_time_tick() still advances loops. ago_ms is how
     * long ago the ticker last ran: growing between the two samples ==
     * the ticker thread is not being scheduled at all (starved / its
     * pinned core no longer schedules). This is the direct test of the
     * "ticker pinned to the same core as the VM (v02.14)" hypothesis. */
    {
        long long now_ms = sceKernelGetProcessTimeWide() / 1000;
        long long ago = (vita_ticker_last_ms > 0)
                        ? (now_ms - vita_ticker_last_ms) : -1;
        n = snprintf(buf, sizeof(buf),
                     "  ticker loops=%u tick_count=%u last_ms=%lld ago_ms=%lld"
                     " cpu_cfg=%d tickgate=%d tick_compile=%d\n",
                     vita_ticker_loops, (unsigned int)vita_tick_count,
                     vita_ticker_last_ms, ago,
                     vita_cfg_digit("ticker_cpu", -1),
                     vita_tickgate_mode(), vita_tick_compile_enabled());
        sceIoWrite(fd, buf, n);
        vita_wd_puts(fd, "[rd4 ticker ok]\n");
    }

    /* (2) Bootstrap / teardown path counters - the log41 freeze site was
     * the new-thread entry path (primordial_to_current_thread ->
     * start_lightweight_thread_asm -> invoke_pending_entries). These
     * separate "the dispatcher stopped producing threads" from "threads
     * are produced and die in a spin". */
    n = snprintf(buf, sizeof(buf),
                 "  boot run=%u sched_start=%u th_start=%u th_finish=%u"
                 " lw_exit=%u start0=%u justborn=%u birth_delay=%d\n",
                 vita_run_count, vita_sched_start_count,
                 vita_thread_start_count, vita_thread_finish_count,
                 vita_lw_exit_count, vita_start0_count,
                 vita_sched_justborn(),
                 vita_cfg_digit("birth_delay", 0));
    sceIoWrite(fd, buf, n);
    /* v02.30: birth-stub visibility (log52 froze the VM at the birth
     * stub with LR=stub entry - the primordial gadget signature; see
     * Scheduler.cpp file-head note). swjb_seen counts switch-outs of
     * threads still carrying JUST_BORN; pe_enter counts entries into
     * the generated invoke_pending_entries stub that had a live body.
     * At a freeze: pe_enter >> swjb_seen means consumption without
     * dispatch records (the birth-stub re-entry loop); pe_enter stuck
     * while justborn-class bits remain means the stub never ran.
     * v02.30 addendum: swjb_seen, swjb_clear and pe_enter are compiled
     * dead in this build (JUST_BORN is only set under _debugger_active=0;
     * the pe_enter asm patch lives in the loopgen-frozen Interpreter_arm.o,
     * see rebuild_vm.sh pack_lib). The LIVE probes are ts_enter/ts_gadget
     * below - read those first. */
    {
        extern unsigned int vita_sched_swjb_seen(void);
        extern unsigned int vita_sched_swjb_clear(void);
        extern unsigned int vita_sched_pe_enter(void);
        extern unsigned int vita_sched_ts_enter(void);
        extern unsigned int vita_sched_ts_gadget(void);
        extern unsigned int vita_sched_born_dsp(void);
        n = snprintf(buf, sizeof(buf),
                     "  v230 swjb_seen=%u swjb_clear=%u pe_enter=%u"
                     " ts_enter=%u ts_gadget=%u born_dsp=%u\n",
                     vita_sched_swjb_seen(), vita_sched_swjb_clear(),
                     vita_sched_pe_enter(),
                     vita_sched_ts_enter(), vita_sched_ts_gadget(),
                     vita_sched_born_dsp());
        sceIoWrite(fd, buf, n);
    }

    /* v02.31: macro-loop probes (definitions next to the other vita
     * counters near the top of this file). Read these FIRST at a hang:
     * any still climbing between the two snapshots names the spinning
     * body; all frozen with cpu1 busy means the loop never leaves the
     * stub-cluster asm. */
    n = snprintf(buf, sizeof(buf),
                 "  v231 finish=%u force=%u setcur=%u term=%u"
                 " term_listed=%u exit_seq=%u\n",
                 vita_v231_finish, vita_v231_force_term,
                 vita_v231_setcur, vita_v231_term,
                 vita_v231_term_listed, vita_v231_exit_seq);
    sceIoWrite(fd, buf, n);

    /* v02.40: landing-pad probes (see definitions near the v231
     * counters). land counts sentinel handoffs executed in fresh
     * (non-frozen) code; fp/lr are the last recorded entry template.
     * At a freeze: land=0 -> the handoff never ran; land>0 with a
     * frozen pc still in the trampoline head -> the boundary is
     * between the pad and the trampoline's first loop iteration. */
    n = snprintf(buf, sizeof(buf),
                 "  v240 land=%u fp=%08x lr=%08x pad=%08x\n",
                 vita_v240_land, vita_v240_fp, vita_v240_lr,
                 (unsigned int)&vita_landing_pad);
    sceIoWrite(fd, buf, n);
    /* v02.42: landing-pad pe liveness - the counter pair that splits
     * log67's spin into "empty trampoline bounce" (pe0 climbing, pe1
     * flat) vs "body present but never consumed" (pe1 climbing too).
     * Slot semantics defined next to vita_v242_pe in
     * InterpreterRuntime_arm.cpp. */
    n = snprintf(buf, sizeof(buf),
                 "  v242 pe0=%u pe1=%u\n",
                 vita_v242_pe[0], vita_v242_pe[1]);
    sceIoWrite(fd, buf, n);
    /* v02.49: THE missing log72 evidence. The frozen qring (seq=536,
     * zero REM records after the ring closed) CONTRADICTS a bounce
     * cycle that runs the birth-stub death tail every lap - each lap
     * would pass lightweight_thread_exit -> terminate ->
     * remove_from_active (op-2 REM) and blow the seq counter to tens
     * of millions. But ring_trap.log never printed ANY of these
     * counters, so the two candidate topologies could not be told
     * apart: (A) ring closes BEFORE the death tail in pure asm (pad
     * <-> stub <-> sentinel pop), all C++ frozen; (B) ring runs part
     * or all of the death tail C++ every lap and the qring freeze has
     * another explanation. One line settles it: any of these still
     * huge/climbing while qring seq stayed small names the spinning
     * body; all frozen confirms topology (A). Definitions:
     * finish/force/setcur/term = Thread.cpp + Scheduler.cpp (v02.31),
     * taskclean = Thread.cpp (v02.49, was the one uncovered body),
     * land/pe0/pe1 = InterpreterRuntime_arm.cpp pad (v02.40/v02.42),
     * switch = Scheduler.cpp switch_thread (v01.98). */
    n = snprintf(buf, sizeof(buf),
                 "  v249 finish=%u force=%u setcur=%u term=%u taskclean=%u"
                 " land=%u pe0=%u pe1=%u switch=%u\n",
                 vita_v231_finish, vita_v231_force_term,
                 vita_v231_setcur, vita_v231_term,
                 vita_v249_taskclean,
                 vita_v240_land, vita_v242_pe[0], vita_v242_pe[1],
                 vita_sched_switch_count());
    sceIoWrite(fd, buf, n);
    /* v02.50: brake bookkeeping + at-trip cpe (hist 4th word holds
     * the per-crossing view; flood[14] is the value at trip).
     * v02.51: + door verdict - flood[16] is the [r6] word the brake
     * was about to jump through, flood[17] tells if it matched the
     * real lightweight_thread_exit (1) or not (0 - parked instead,
     * the log74 crash mode). */
    n = snprintf(buf, sizeof(buf),
                 "  v250 brakes=%u cpe_at_trip=%08x door=%08x verdict=%u\n",
                 vita_v250_brakes, vita_v245_flood[14],
                 vita_v245_flood[16], vita_v245_flood[17]);
    sceIoWrite(fd, buf, n);
    {
        unsigned int c = vita_v231_exit_seq;
        unsigned int start = (c >= VITA_V231_EXIT_RING)
                               ? (c - VITA_V231_EXIT_RING) : 0;
        unsigned int i;
        for (i = start; i < c; i++) {
            unsigned int idx = i & (VITA_V231_EXIT_RING - 1);
            n = snprintf(buf, sizeof(buf),
                         "  exit[%u] id=%u st=%08x pe=%u chain=%08x\n",
                         i, vita_v231_exit_ring[idx][0],
                         vita_v231_exit_ring[idx][1],
                         vita_v231_exit_ring[idx][2],
                         vita_v231_exit_ring[idx][3]);
            sceIoWrite(fd, buf, n);
        }
    }

    /* (3) The last 8 Java threads created (class.method). A create/die
     * spin prints the same pair over and over - which names the Java
     * code responsible without needing the (unavailable) Java heap. */
    {
        unsigned int c = vita_tc_count;
        unsigned int start = (c >= VITA_TC_HIST) ? (c - VITA_TC_HIST) : 0;
        n = snprintf(buf, sizeof(buf), "  last %d java threads created:\n",
                     VITA_TC_HIST);
        sceIoWrite(fd, buf, n);
        for (unsigned int k = start; k < c; k++) {
            int slot = (int)(k & (VITA_TC_HIST - 1));
            n = snprintf(buf, sizeof(buf), "    [%u] %s.%s\n", k,
                         vita_tc_class[slot][0] ? (const char *)&vita_tc_class[slot]
                                                : "(?)",
                         vita_tc_method[slot][0] ? (const char *)&vita_tc_method[slot]
                                                 : "(?)");
            sceIoWrite(fd, buf, n);
        }
        vita_wd_puts(fd, "[rd5 threads ok]\n");
    }

    /* v01.98: WHO are the active_count threads? Walk the global thread
     * list (ground truth) and the priority queues (scheduler's runnable
     * opinion) - see vita_sched_dump_threads in Scheduler.cpp. Buffer
     * sized for VITA_SCHED_DUMP_MAX(48) lines x2 + headers. */
    {
        static char thread_dump[4096];
        int len = vita_sched_dump_threads(thread_dump,
                                          (int)sizeof(thread_dump));
        if (len > 0)
            sceIoWrite(fd, thread_dump, len);
        vita_wd_puts(fd, "[rd6 dumpthreads ok]\n");
    }

    /* v02.44 rd7: birth frame anatomy. log69's coredump caught the VM
     * thread at start_lightweight_thread_asm+0x8 with lr=pad, fp=0,
     * r6=0x81ed0804 - 0xBC above id4's saved stack_pointer (0x81ed0748).
     * The birth stub's whole exit table is indexed off r6 ([r6]=
     * lightweight_thread_exit .. [r6+16]=TestCompiler), and every
     * forward path beyond the stub goes through C++ counters that are
     * ALL frozen at 0. So the live hypothesis is a corrupted slot
     * bouncing back (L444 -> call_on_primordial_stack([r6]) -> pad),
     * and the frame's runtime words are the missing evidence - the
     * coredump never maps the Java heap, so the pad saved them for us:
     * ct (last _current_thread seen) and r6 (exit-table base). Here we
     * dump [r6-8 .. r6+16] and compare against the setup_stack_asm
     * template + the saved stack_pointer. call_on_primordial_stack's
     * per-iteration push would leave lr/pad at [r6-8]/[r6-4] - the
     * fingerprint of the closing edge. */
    {
        unsigned int ct = vita_v244_ct;
        unsigned int r6 = vita_v244_r6;
        n = snprintf(buf, sizeof(buf),
                     "[rd7 birthframe] v244_ct=%08x v244_r6=%08x\n",
                     ct, r6);
        sceIoWrite(fd, buf, n);
        if (r6 != 0 && (r6 & 3) == 0) {
            unsigned int *fr = (unsigned int *)(uintptr_t)r6;
            n = snprintf(buf, sizeof(buf),
                         "  frame[-2..+4]: %08x %08x | %08x %08x %08x "
                         "%08x %08x\n",
                         fr[-2], fr[-1], fr[0], fr[1], fr[2], fr[3],
                         fr[4]);
            sceIoWrite(fd, buf, n);
            /* Careful: the [+16] word is NOT a code address. setup_stack_asm
             * stores the TestCompiler FLAG there, and in this PRODUCT
             * build TestCompiler is a compile-time const false (develop
             * flag via ENABLE_PERFORMANCE_COUNTERS=0 -> DECLARE_
             * DEVELOPER_FLAG under PRODUCT -> const bool = false), so
             * the template value is 0 and the stub's "cmp r0,#0; bne
             * L444" takes the FULL exit chain when the frame is intact.
             * A nonzero [+16] therefore means residual stack garbage
             * steering the stub straight to L444 - and [r6] then names
             * the bounce target. Also compare v244_r6 across the two
             * samples: call_on_primordial_stack does stmdb r6!,{lr,fp}
             * per traversal (r6 -= 8, never restored on the non-return
             * path), so a monotonically sliding r6 between samples is
             * that gadget's fingerprint. */
            n = snprintf(buf, sizeof(buf),
                         "  template: exit=%08x uncaught=%08x finish=%08x "
                         "force_term=%08x flagword(+16)=0 (TestCompiler "
                         "const false) pad=%08x\n",
                         (unsigned int)(uintptr_t)&Thread::lightweight_thread_exit,
                         (unsigned int)(uintptr_t)&Thread::lightweight_thread_uncaught_exception,
                         (unsigned int)(uintptr_t)&Thread::finish,
                         (unsigned int)(uintptr_t)&force_terminated,
                         (unsigned int)(uintptr_t)&vita_landing_pad);
            sceIoWrite(fd, buf, n);
        }
        if (ct != 0) {
            /* Thread::stack_pointer_offset: r6 restore in
             * primordial_to_current_thread loads [obj,#0x64] - verified
             * against the shipped binary. */
            unsigned int *obj = (unsigned int *)(uintptr_t)ct;
            n = snprintf(buf, sizeof(buf),
                         "  ct.stack_pointer=%08x (r6 - ct.stack_pointer = %08x)\n",
                         obj[0x64 / 4], r6 - obj[0x64 / 4]);
            sceIoWrite(fd, buf, n);
        }
        vita_wd_puts(fd, "[rd7 birthframe ok]\n");
    }
    /* v02.45: flood-gate backup readout. If the trap fired, tripped!=0
     * and ring_trap.log is already on disk (this whole log may not even
     * exist - the process exited at trip). If the ring somehow misses
     * the pad, consec keeps the pe==NULL streak visible from the
     * watchdog side. Either way this line tells us which world we are
     * in when only a watchdog log comes back. */
    {
        n = snprintf(buf, sizeof(buf),
                     "[rd7 flood] consec=%u tripped=%u (see ring_trap.log "
                     "if tripped)\n",
                     vita_v245_flood[0], vita_v245_flood[1]);
        sceIoWrite(fd, buf, n);
    }
    /* v02.46: symbolic Java stacks from the watchdog side too - the
     * fallback view when the trap never fires (ring misses the pad). */
    {
        static char jdump[4096];
        int len = vita_sched_dump_jstack(jdump, (int)sizeof(jdump));
        if (len > 0) sceIoWrite(fd, jdump, len);
    }
}

void vita_wd_snapshot(const char *path) {
    SceUID fd = sceIoOpen(path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND,
                          0777);
    if (fd < 0) return;
    /* v02.17: every counter is printed TWICE, 400 ms apart, in one file.
     * The delta between the two blocks is what carries the information
     * (which counter still advances after the VM froze); without it a
     * single sample is ambiguous and cost several log rounds to
     * disambiguate. */
    vita_wd_puts(fd, "\n===== v02.18 sample 1 =====\n");
    vita_wd_snapshot_one(fd);
    sceKernelDelayThread(400 * 1000);
    vita_wd_puts(fd, "===== v02.18 sample 2 (+400ms) =====\n");
    vita_wd_snapshot_one(fd);
    sceIoClose(fd);
}

/* v02.45: ring self-trap dumper. The pad's flood gate calls this on a
 * private stack after 256 consecutive pe==NULL bounces - a condition
 * legitimate execution cannot reach (normal births cross with
 * pe!=NULL; log69 froze with pe1=5 while pe0 climbed ~10M/s). The
 * scene is in vita_v245_flood (registers), vita_v245_hist (last 4
 * bounce edges). This function writes ONE file and exits the process:
 * the freeze becomes an immediate visible crash with the complete
 * scene, from INSIDE the spinning thread - which can read the Java
 * stack pages no coredump ever maps.
 *
 * v02.48: the CALLER changed (log71: sceIoOpen from the lightweight
 * thread froze in-kernel - psp2core showed the main thread parked in
 * SceLibKernel on the very first sceIoOpen of this function, and
 * ring_trap.log never appeared). The pad now stores the scene and
 * parks (wfe, memory-only); the launcher WATCHDOG thread - a real
 * OS thread on the other core - polls flood[1] and calls this on the
 * spinner's behalf. Body unchanged: read flood/hist, write the file,
 * exit. Still must not return (the watchdog treats it as noreturn);
 * keep it leaf-ish: no heap, no locks, snprintf into a stack buffer
 * only. */
/* v02.50: brake_mode parameter. brake_mode=0: legacy behavior
 * (dump + exit, the PARK handoff). brake_mode=1: the BRAKE handoff
 * - identical dump plus a header line with the brake counter, then
 * RETURN without exiting: the ghost left the ring through the
 * designed death door and the VM may self-heal; the watchdog that
 * called this keeps polling CE and only exits if recovery never
 * comes. */
void vita_ring_trap(int brake_mode)
{
    char buf[320];
    int n;
    SceUID fd = sceIoOpen("ux0:/data/J2ME00001/ring_trap.log",
                          SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC,
                          0777);
    if (fd < 0) {
        if (brake_mode) {
            return;               /* keep the process alive to heal */
        }
        sceKernelExitProcess(0x45);
    }
    const unsigned int *F = vita_v245_flood;
    const unsigned int *H = vita_v245_hist;

    /* v02.51: door verdict FIRST - the one fact log74 died for.
     * flood[16] = the [r6] word at the door check, flood[17] = 1
     * (matches lightweight_thread_exit, safe bx) / 0 (bad door,
     * parked). */
    n = snprintf(buf, sizeof(buf),
                 "== RING TRAP v02.51 %s brakes=%u door=%08x verdict=%u ==\n",
                 brake_mode ? "BRAKE" : "PARK",
                 vita_v250_brakes, F[16], F[17]);
    sceIoWrite(fd, buf, n);
    sceIoWrite(fd, buf, n);
    n = snprintf(buf, sizeof(buf),
                 "consec=%u tripped=%u ct=%08x\n",
                 F[0], F[1], F[2]);
    sceIoWrite(fd, buf, n);
    n = snprintf(buf, sizeof(buf),
                 "r0=%08x r3=%08x r4=%08x r5=%08x\n"
                 "r6=%08x r7=%08x r8=%08x r9=%08x\n"
                 "r10=%08x fp=%08x sp=%08x lr=%08x\n",
                 F[2], F[3], F[4], F[5], F[6], F[7], F[8], F[9],
                 F[10], F[11], F[12], F[13]);
    sceIoWrite(fd, buf, n);

    /* Last 4 bounce edges (oldest first): who jumped into the pad and
     * where r6 was - the per-bounce delta names the sliding gadget.
     * v02.49: 3rd word per crossing = [r6-8], the fp slot under the
     * sentinel. call_on_primordial_stack pushes (lr,fp) at
     * [r6-4],[r6-8] and pops them back: if this word differs across
     * the recorded bounces the death-tail C++ prologue executed
     * inside the ring; frozen at the pristine template value means
     * the ring closes before any push (pure asm topology). */
    vita_wd_puts(fd, "bounce hist (r6, lr, [r6-8], cpe) oldest->newest:\n");
    {
        unsigned int idx = H[0];
        for (int k = 4; k > 0; k--) {
            unsigned int i = (idx - (unsigned int)k) & 3u;
            unsigned int r6v = H[1 + 4 * i];
            unsigned int lrv = H[2 + 4 * i];
            unsigned int f8v = H[3 + 4 * i];
            unsigned int cpev = H[4 + 4 * i];
            unsigned int prev6 = H[1 + 4 * ((i - 1u) & 3u)];
            n = snprintf(buf, sizeof(buf),
                         "  r6=%08x lr=%08x [r6-8]=%08x cpe=%08x"
                         " (d_r6=%+d)\n",
                         r6v, lrv, f8v, cpev, (int)r6v - (int)prev6);
            sceIoWrite(fd, buf, n);
        }
    }

    /* The exit table the birth stub indexes (r6) and the birth frame
     * the scheduler actually wrote (ThreadObj stack_pointer at
     * [0x64/4]). The gap between the two windows IS the log69 riddle
     * (0xBC) - now with the bytes. */
    n = snprintf(buf, sizeof(buf),
                 "template: exit=%08x uncaught=%08x finish=%08x "
                 "force_term=%08x flagword(+16)=0 pad=%08x\n",
                 (unsigned int)(uintptr_t)&Thread::lightweight_thread_exit,
                 (unsigned int)(uintptr_t)&Thread::lightweight_thread_uncaught_exception,
                 (unsigned int)(uintptr_t)&Thread::finish,
                 (unsigned int)(uintptr_t)&force_terminated,
                 (unsigned int)(uintptr_t)&vita_landing_pad);
    sceIoWrite(fd, buf, n);

    if ((F[6] & 3u) == 0u && F[6] != 0u) {
        unsigned int *fr = (unsigned int *)(uintptr_t)F[6];
        n = snprintf(buf, sizeof(buf),
                     "exit-table window [r6-8..r6+24]:\n  "
                     "%08x %08x | %08x %08x %08x %08x %08x %08x\n",
                     fr[-2], fr[-1], fr[0], fr[1], fr[2], fr[3],
                     fr[4], fr[5]);
        sceIoWrite(fd, buf, n);
    }
    if (F[2] != 0u) {
        unsigned int *obj = (unsigned int *)(uintptr_t)F[2];
        unsigned int saved_sp = obj[0x64 / 4];
        n = snprintf(buf, sizeof(buf),
                     "ct: pe=%08x stack_pointer=%08x "
                     "(r6 - sp = %+d)\n",
                     obj[36 / 4], saved_sp,
                     (int)F[6] - (int)saved_sp);
        sceIoWrite(fd, buf, n);
        if ((saved_sp & 3u) == 0u && saved_sp != 0u) {
            unsigned int *bf = (unsigned int *)(uintptr_t)saved_sp;
            n = snprintf(buf, sizeof(buf),
                         "birth-frame window [sp-8..sp+24]:\n  "
                         "%08x %08x | %08x %08x %08x %08x %08x %08x\n",
                         bf[-2], bf[-1], bf[0], bf[1], bf[2], bf[3],
                         bf[4], bf[5]);
            sceIoWrite(fd, buf, n);
        }
    }
    /* The C stack at trip (sp saved at pad entry): residual frames
     * name the code that was running before the ring closed. */
    if ((F[12] & 3u) == 0u && F[12] != 0u) {
        unsigned int *sw = (unsigned int *)(uintptr_t)F[12];
        n = snprintf(buf, sizeof(buf),
                     "c-stack window [sp..sp+60]:\n  "
                     "%08x %08x %08x %08x %08x %08x %08x %08x\n"
                     "  %08x %08x %08x %08x %08x %08x %08x %08x\n",
                     sw[0], sw[1], sw[2], sw[3], sw[4], sw[5], sw[6],
                     sw[7], sw[8], sw[9], sw[10], sw[11], sw[12],
                     sw[13], sw[14], sw[15]);
        sceIoWrite(fd, buf, n);
    }

    /* Full scheduler forensics - everything the watchdog would have
     * printed, from the same instant (no 400ms-later sampling).
     * v02.46: + symbolic Java stacks of ALL threads - the frame list
     * names what each thread was executing, the thing no coredump
     * (native stacks only) and no previous probe could see.
     * v02.49: + the ring-counter line FIRST - the log72 topology
     * question (does the bounce loop run the death-tail C++?) must be
     * answerable even if a later dump truncates. */
    vita_wd_puts(fd, "-- scheduler forensics --\n");
    {
        static char dump[4096];
        int len;
        n = snprintf(buf, sizeof(buf),
                     "  v249 finish=%u force=%u setcur=%u term=%u"
                     " taskclean=%u land=%u pe0=%u pe1=%u switch=%u\n",
                     vita_v231_finish, vita_v231_force_term,
                     vita_v231_setcur, vita_v231_term,
                     vita_v249_taskclean,
                     vita_v240_land, vita_v242_pe[0], vita_v242_pe[1],
                     vita_sched_switch_count());
        sceIoWrite(fd, buf, n);
        /* v02.50: brake bookkeeping + the cpe word both dump sites
         * need (hist 4th word holds the per-crossing view; this is
         * the at-trip value from flood[14]). */
        n = snprintf(buf, sizeof(buf),
                     "  v250 brakes=%u cpe_at_trip=%08x\n",
                     vita_v250_brakes, F[14]);
        sceIoWrite(fd, buf, n);
        len = vita_sched_dump_qring(dump, (int)sizeof(dump));
        if (len > 0) sceIoWrite(fd, dump, len);
        len = vita_sched_dump_dispatch(dump, (int)sizeof(dump));
        if (len > 0) sceIoWrite(fd, dump, len);
        len = vita_sched_nrt_info(dump, (int)sizeof(dump));
        if (len > 0) sceIoWrite(fd, dump, len);
        len = vita_sched_dump_threads(dump, (int)sizeof(dump));
        if (len > 0) sceIoWrite(fd, dump, len);
        len = vita_sched_dump_jstack(dump, (int)sizeof(dump));
        if (len > 0) sceIoWrite(fd, dump, len);
        len = vita_sched_dump_stack(dump, (int)sizeof(dump));
        if (len > 0) sceIoWrite(fd, dump, len);
    }
    sceIoClose(fd);
    if (brake_mode) {
        return;                     /* v02.50 BRAKE: scene preserved,
         * process stays alive - the watchdog monitors CE recovery */
    }
    sceKernelExitProcess(0x45);
}

} /* extern "C" */
