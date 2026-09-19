/*
 * OS_vita.hpp: PS Vita OS-specific declarations.
 *
 * Vita uses newlib which provides most POSIX headers.
 * No mmap, no signals for VM exceptions, no ucontext.
 */

#ifndef  __USE_GNU
#define __USE_GNU
#endif

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/times.h>
#include <sched.h>
#include <errno.h>

// Vita does not have sys/mman.h - memory management is custom
// Vita does not use signal-based exceptions (USE_VM_EXCEPTIONS=0)
// Vita does not use ucontext

#if ENABLE_TIMER_THREAD
#include <pthread.h>
#include <semaphore.h>
#endif

#undef __USE_GNU

// Vita has no SIGUSR1 - define a placeholder
#define SUSPENDSIG -1
