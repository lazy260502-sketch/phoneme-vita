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

} /* extern "C" */
