/*
 * Globals_vita.hpp: Command line switches for the PS Vita platform.
 *
 * Based on Globals_linux.hpp, adapted for PS Vita.
 */

#ifndef GLOBALS_VITA_HPP
#define GLOBALS_VITA_HPP

// Vita does not support signal-based timer interrupts
#ifndef SUPPORTS_TIMER_INTERRUPT
#define SUPPORTS_TIMER_INTERRUPT 0
#endif

// No adjustable memory chunks on Vita (no mmap)
#ifndef SUPPORTS_ADJUSTABLE_MEMORY_CHUNK
#define SUPPORTS_ADJUSTABLE_MEMORY_CHUNK 0
#endif

// No memory-mapped files on Vita
#ifndef SUPPORTS_MEMORY_MAPPED_FILES
#define SUPPORTS_MEMORY_MAPPED_FILES 0
#endif

#define PLATFORM_RUNTIME_FLAGS_GENERIC(develop, product)                      \
  product(int, TickInterval, 10,                                              \
          "Set the delay interval for servicing compiler generation")         \
  product(int, ExecutionLoops, 1,                                             \
          "the number of times we run the VM (for measuring start-up time)")

#if ENABLE_ARM_VFP
#define PLATFORM_RUNTIME_FLAGS_VFP(develop, product)                          \
  product(bool, RunFastMode, false,                                           \
          "Configure the ARM VFP coprocessor to run in RunFast mode "         \
          "and execute extra instructions to ensure TCK compilance")
#else
#define PLATFORM_RUNTIME_FLAGS_VFP(develop, product)
#endif

#define PLATFORM_RUNTIME_FLAGS(develop, product)         \
        PLATFORM_RUNTIME_FLAGS_GENERIC(develop, product) \
        PLATFORM_RUNTIME_FLAGS_VFP(develop, product)

#define stricmp strcasecmp

/* v01.84 site ring (see OS_vita.cpp): hang forensics for native-code
 * loops that never pass yield()/tick checkpoints. Called from share/
 * call sites guarded by #if defined(VITA); defined in OS_vita.cpp. */
extern "C" {
    void vita_site_mark(void) __attribute__((noinline));
    void vita_site_dump(const char *path);
    /* v02.11: tick-driven JIT compile switch (defined in OS_vita.cpp).
     * Read once from ux0:/data/J2ME00001/tick_compile.cfg; absent or
     * unreadable = enabled (the b332 behaviour). Consulted by
     * Compiler::on_timer_tick() under #if defined(VITA). */
    int vita_tick_compile_enabled(void);
    /* v01.85 verifier deep tracing - names the class.method being
     * verified at hang time (see OS_vita.cpp). v01.86: explicit length
     * - Symbols are length-prefixed, NOT NUL-terminated. */
    void vita_site_verifier_class(const char *name, int len);
    void vita_site_verifier_method(const char *name, int len);
    void vita_site_dump_verify(const char *path);
    /* v01.88 clinit tracing - log14 froze with the verifier OFF (verify
     * class=(none)) after 345 frames of normal play: the spin moved
     * into <clinit> execution itself on the green-thread Java stack.
     * Same single-writer discipline as the verifier hooks. */
    void vita_site_clinit_class(const char *name, int len);
    void vita_site_clinit_return(void);
    void vita_site_dump_clinit(const char *path);
    /* v02.05: interpreter Java-site sampling - Thread::timer_tick
     * records the current class.method (length-bounded Symbol copy,
     * PRODUCT-safe) so a HANG dump can name the Java loop when the
     * lane/pump counters freeze (the log29 open question). */
    void vita_site_java_frame(void);
    void vita_site_dump_java(const char *path);
    extern volatile unsigned int vita_java_sample_count;
    /* v01.90: progress counters readable by the launcher watchdog
     * (defined in OS_vita.cpp). vita_clinit_count advances during a
     * class-load storm even while the CE pump is starved; vita_gc_count
     * names a stop-the-world GC spin (invisible to every other hook
     * because the coredump has no Java heap). */
    extern volatile unsigned int vita_clinit_count;
    extern volatile unsigned int vita_gc_count;
    /* v01.91: eager-preload progress counter (see JVM::load_main_class
     * and Universe::load_jar_entry) - advanced once per successfully
     * defined jar class during the boot-time define sweep. */
    extern volatile unsigned int vita_preload_count;

    /*=====================================================================
     * v02.17 one-shot forensics.
     *
     * Real-hardware runs are expensive (rebuild + reinstall + relaunch),
     * so this revision adds EVERY probe the open questions need in one
     * build. All of them are plain counters/text written from the VM
     * thread(s) and read by the launcher watchdog after a HANG - no
     * syscalls on foreign threads (the two probes that did that, log33
     * sceKernelGetThreadRunStatus and log34, killed the watchdog).
     *
     * State lives in OS_vita.cpp; the increments sit next to the code
     * they measure (file named in each counter's note there):
     *   vita_run_count           JVM::run() entries  (JVM.cpp)
     *   vita_sched_start_count   Scheduler::start()  (Scheduler.cpp)
     *   vita_thread_start_count  Thread::start()     (Thread.cpp)
     *   vita_thread_finish_count Thread::finish()    (Thread.cpp)
     *   vita_lw_exit_count       Thread::lightweight_thread_exit()
     *   vita_start0_count        Java_java_lang_Thread_start0 (Natives.cpp)
     *   vita_justborn_dispatch   switch_thread() of a THREAD_JUST_BORN thread
     *   vita_ticker_loops        cldc_ticker loop iterations
     *   vita_ticker_last_ms      wall clock (ms) of the last ticker iteration
     * The last-created Java thread is named into a small history ring so a
     * create/die spin shows the SAME class.method repeating.
     *===================================================================*/
    extern volatile unsigned int vita_run_count;
    extern volatile unsigned int vita_sched_start_count;
    extern volatile unsigned int vita_thread_start_count;
    extern volatile unsigned int vita_thread_finish_count;
    extern volatile unsigned int vita_lw_exit_count;
    extern volatile unsigned int vita_start0_count;
    extern volatile unsigned int vita_justborn_dispatch;
    extern volatile unsigned int vita_ticker_loops;
    extern volatile long long   vita_ticker_last_ms;
#define VITA_TC_HIST 8
    extern volatile unsigned int vita_tc_count;
    extern volatile char vita_tc_class[VITA_TC_HIST][40];
    extern volatile char vita_tc_method[VITA_TC_HIST][40];
    /* Copies one entry into the ring at slot (count & (VITA_TC_HIST-1)).
     * Lengths are the Symbol lengths (Symbols are NOT NUL-terminated). */
    void vita_tc_note(const char *cls, int clen, const char *mth, int mlen);
    /* v02.17 runtime A/B switch: returns 0 when the file
     * ux0:/data/J2ME00001/<name> exists and its first byte is '0'/'n'/'N',
     * otherwise dflt. Read once per name and cached. Same discipline as
     * vita_tick_compile_enabled(), but usable for many switches - lets a
     * single build test several hypotheses by editing files on ux0: data
     * instead of rebuilding (the expensive part of real-hardware work). */
    int vita_cfg_flag(const char *name, int dflt);
    /* Same protocol but returns the first byte as a digit 0..9 (so one
     * switch can select among variants, e.g. ticker_cpu = 0/1/2).
     * Absent / unreadable / not a digit => dflt. */
    int vita_cfg_digit(const char *name, int dflt);
    /* v02.17 VM-side tick-compile gate (see OS_vita.cpp). Consulted by
     * Compiler::on_timer_tick() in addition to vita_tick_compile_enabled():
     * returns 0 when the ticker has been starved >1 s (the VM thread is
     * monopolising the core, so a synchronous compile is the worst thing
     * to start now). Mode from ux0:/data/J2ME00001/tickgate:
     * absent/0 = no gate (v02.11 behaviour), 1 = stale-ticker gate,
     * 2 = never compile on ticks. */
    int vita_jit_tick_compile_enabled(void);
    /* The resolved mode (0/1/2) for the watchdog dump. */
    int vita_tickgate_mode(void);
}

#endif // GLOBALS_VITA_HPP
