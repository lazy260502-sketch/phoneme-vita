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
    /* v01.85 verifier deep tracing - names the class.method being
     * verified at hang time (see OS_vita.cpp). v01.86: explicit length
     * - Symbols are length-prefixed, NOT NUL-terminated. */
    void vita_site_verifier_class(const char *name, int len);
    void vita_site_verifier_method(const char *name, int len);
    void vita_site_dump_verify(const char *path);
}

#endif // GLOBALS_VITA_HPP
