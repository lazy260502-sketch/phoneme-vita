/*
 * os_port.cpp: PS Vita OS porting implementation
 *
 * This file implements the ANILib OS interface for PS Vita.
 */

#include "os_port.h"
#include <cstdlib>
#include "kni.h"

Os_Event Os_CreateEvent(jboolean *status) {
  Os_Event event = (Os_Event) malloc(sizeof(Os_EventStruct));

  pthread_cond_t temp_cond = PTHREAD_COND_INITIALIZER;
  pthread_mutex_t temp_mux = PTHREAD_MUTEX_INITIALIZER;

  event->signaled      = 0;
  event->condition     = temp_cond;
  event->mutex         = temp_mux;

  pthread_cond_init(&event->condition, NULL);
  pthread_mutex_init(&event->mutex, NULL);

  *status = KNI_TRUE;
  return event;
}

void Os_WaitForEvent(Os_Event event) {
  pthread_mutex_lock(&event->mutex);
  while (event->signaled == 0) {
    pthread_cond_wait(&event->condition, &event->mutex);
  }
  event->signaled--;
  pthread_mutex_unlock(&event->mutex);
}

jint Os_WaitForEventOrTimeout(Os_Event event, jlong ms) {
  int result = 0;
  struct timeval now = {0,0};
  struct timespec timeout = {0,0};

  if (ms < 0) {
    /* wait forever */
    Os_WaitForEvent(event);
    return OS_SIGNALED;
  }

  pthread_mutex_lock(&event->mutex);
  gettimeofday(&now, NULL);

  /* Upstream (anilib/linux/os_port.cpp) tests "ms != 0": compute the
   * ABSOLUTE timeout from the current time. The Vita port had "ms == 0"
   * (a transcription slip), so for any ms > 0 the timespec stayed
   * {0,0} = epoch 1970 and pthread_cond_timedwait returned ETIMEDOUT
   * immediately - the scheduler's 50ms event-pump wait became a busy
   * loop (fps=1, heartbeat counter racing, UC init crawl). */
  if (ms != 0) {
    timeout.tv_sec = now.tv_sec;
    timeout.tv_nsec = now.tv_usec * 1000;

    if (ms >= 1000) {
      timeout.tv_sec += ((signed long) (((ms) >> 32) & 0xffffffff));
      timeout.tv_sec += ((signed long) (((ms) & 0xffffffff))) / 1000;
      ms -= (ms/1000)*1000;
    }
    timeout.tv_nsec += ((int)ms * 1000000);
  } else {
    timeout.tv_sec = 0;
    timeout.tv_nsec = 0;
  }

  if (event->signaled == 0) {
    result =
      pthread_cond_timedwait(&event->condition, &event->mutex, &timeout);
  }

  if (result != ETIMEDOUT) {
    event->signaled--;
  }

  pthread_mutex_unlock(&event->mutex);
  if (result == ETIMEDOUT) {
    return OS_TIMEOUT;
  } else {
    return OS_SIGNALED;
  }
}

void Os_SignalEvent(Os_Event event) {
  pthread_mutex_lock(&event->mutex);
  event->signaled++;
  pthread_cond_signal(&event->condition);
  pthread_mutex_unlock(&event->mutex);
}

void Os_DisposeEvent(Os_Event event) {
  pthread_mutex_destroy(&event->mutex);
  pthread_cond_destroy(&event->condition);

  free((void*)event);
}

Os_Thread Os_CreateThread(int proc(void *parameter), void *arg,
                          jboolean *status) {
  pthread_attr_t attr;
  pthread_t os_thread;
  void* (*routine)(void*) = (void*(*)(void *))proc;

  if (pthread_attr_init(&attr) != 0) {
    *status = KNI_FALSE;
    return 0;
  }

  if (pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED) != 0) {
    *status = KNI_FALSE;
    return 0;
  }

  if (pthread_create(&os_thread, &attr, routine, arg) != 0) {
    *status = KNI_FALSE;
    return 0;
  } else {
    *status = KNI_TRUE;
    return os_thread;
  }
}

void Os_DisposeThread(Os_Thread thread) {
  /* IMPL_NOTE: nothing to do? */
}