/*
 * os_port.h: PS Vita OS porting header
 *
 * This file provides minimal OS porting definitions
 * required by the VM build system and ANILib.
 */

#ifndef OS_PORT_VITA_H
#define OS_PORT_VITA_H

/* Include KNI type definitions first (jboolean, jint, jlong, etc.) */
#include "kni.h"

/* Include Vita-specific OS headers */
#include "OS_vita.hpp"

/* Standard includes */
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <pthread.h>
#include <sys/time.h>
#include <errno.h>

/* Platform-specific definitions */
#define ARCH_ARM 1
#define CPU_ARMv7 1

/* Memory mapping - Vita does not support mmap */
#define SUPPORTS_MEMORY_MAPPED_FILES 0

/* Thread support - Vita supports pthreads */
#define SUPPORTS_TIMER_THREAD 1

/* Exceptions - Vita does not use signal-based exceptions */
#define USE_VM_EXCEPTIONS 0

/* JIT cache flush */
#define FLUSH_ICACHE(start, size) __builtin___clear_cache((start), ((char*)(start)) + (size))

/* ANILib compatibility definitions */
#define OS_TIMEOUT   0
#define OS_SIGNALED  1

/* OS_Event definition */
typedef struct {
  int signaled;
  pthread_cond_t condition;
  pthread_mutex_t mutex;
} Os_EventStruct;

typedef Os_EventStruct * Os_Event;

/* OS_Thread definition */
typedef pthread_t Os_Thread;

/* ANILib event functions */
extern Os_Event Os_CreateEvent(jboolean *status);
extern void Os_WaitForEvent(Os_Event event);
extern jint Os_WaitForEventOrTimeout(Os_Event event, jlong ms);
extern void Os_SignalEvent(Os_Event event);
extern void Os_DisposeEvent(Os_Event event);

extern Os_Thread Os_CreateThread(int proc(void *parameter), void *arg, 
                                 jboolean *status);
extern void Os_DisposeThread(Os_Thread thread);

extern void Os_DisposeEvent(Os_Event event);

#endif /* OS_PORT_VITA_H */