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
        ticker_thread_id = sceKernelCreateThread(
            "cldc_ticker", ticker_thread_routine, 0x10000100, 0x10000, 0, 0, NULL);
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

/* Called by the launcher watchdog thread after a HANG is detected.
 * Slot (seq-1)&15 is the newest site; ordering below is newest first. */
void vita_site_dump(const char *path) {
    SceUID fd = sceIoOpen(path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND,
                          0777);
    if (fd < 0) return;
    char buf[128];
    int n;
    unsigned int s = vita_site_seq;
    unsigned int s2 = vita_site_seq;
    n = snprintf(buf, sizeof(buf),
                 "site seq=%u frozen=%s self=0x%08x\n", s,
                 s == s2 ? "yes" : "NO",
                 (unsigned int)(unsigned long)&vita_site_dump);
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
    int  vita_sched_dump_threads(char *out, int out_size);
}

void vita_wd_snapshot(const char *path) {
    SceUID fd = sceIoOpen(path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND,
                          0777);
    if (fd < 0) return;
    char buf[160];
    int n;

    /* Thread::current() expands to (Thread*)&_current_thread, i.e.
     * &jvm_fast_globals.current_thread - the handle cell itself. */
    Thread *ct = Thread::current();
    n = snprintf(buf, sizeof(buf), "wd_snapshot cur_thread=0x%08x\n",
                 (unsigned int)(unsigned long)ct);
    sceIoWrite(fd, buf, n);

    if (ct != NULL && (unsigned int)(unsigned long)ct > 0x1000) {
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
    /* v01.98: log22 quantified the ping-pong but could not separate
     * "pump starving in the fast lane" from "pump running but
     * delivering nothing". These four counters do: yield_fast climbing
     * while pump_runs stalls = starvation (the log22 hypothesis);
     * pump_runs climbing with no event delivery = the pump runs but
     * check_blocked_threads()/JVMSPI_CheckEvents drops the ball. */
    n = snprintf(buf, sizeof(buf),
                 "  lanes fast=%u slow=%u pump=%u switch=%u\n",
                 vita_sched_yield_fast(), vita_sched_yield_slow(),
                 vita_sched_pump_runs(), vita_sched_switch_count());
    sceIoWrite(fd, buf, n);
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
    }
    sceIoClose(fd);
}

} /* extern "C" */
