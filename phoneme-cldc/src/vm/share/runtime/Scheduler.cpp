/*
 *   
 *
 * Copyright  1990-2007 Sun Microsystems, Inc. All Rights Reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER
 * 
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License version
 * 2 only, as published by the Free Software Foundation.
 * 
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License version 2 for more details (a copy is
 * included at /legal/license.txt).
 * 
 * You should have received a copy of the GNU General Public License
 * version 2 along with this work; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA
 * 02110-1301 USA
 * 
 * Please contact Sun Microsystems, Inc., 4150 Network Circle, Santa
 * Clara, CA 95054 or visit www.sun.com if you need additional
 * information or have any questions.
 */

# include "incls/_precompiled.incl"
# include "incls/_Scheduler.cpp.incl"

Thread *global_null_thread = NULL;

int        Scheduler::_active_count;
int        Scheduler::_async_count;
long       Scheduler::_exit_async_pending;
short      Scheduler::_priority_queue_valid;
jbyte      Scheduler::_last_priority_queue;
bool       Scheduler::_yield_on_thread_switch;

OopDesc*   _next_runnable_thread = NULL;

#if defined(VITA)
/* v01.98: log22 quantified the ping-pong (ring 15x switch marks, 1x pump
 * residue, wd_snapshot active_count=5) but could not tell "pump starving
 * in the fast lane" from "pump running but delivering nothing". Four
 * plain counters, exported read-only through the extern "C" bridge at
 * the bottom of this file: yield picks between two lanes - fast (a
 * runnable thread exists, event-wait loop skipped entirely) and slow
 * (no runnable thread, pump runs inside the wait loop). If a hang shows
 * yield_fast climbing while pump_runs stalls, the fast lane is starving
 * the pump - the log22 hypothesis, now testable. switch_count is the
 * denominator (every context switch passes switch_thread's head). */
unsigned int Scheduler::_vita_yield_fast_count  = 0;
unsigned int Scheduler::_vita_yield_slow_count  = 0;
unsigned int Scheduler::_vita_pump_runs         = 0;
unsigned int Scheduler::_vita_switch_count      = 0;
/* v02.01: log26 verdict overturned v02.00 - the queue is NEVER empty
 * (ghost waiters parked in the ring), so sleep_forever is never
 * entered (sf=0/0/0). The freeze instead sits in the unmarked ASM
 * berth-stub loop; every re-dispatch re-enters
 * get_next_runnable_thread(). These counters separate "one-shot
 * freeze" (dispatch stalls) from "ring keeps dispatching ghosts"
 * (dispatch climbs after hang onset). Declared at file head because
 * get_next_runnable_thread() (mid-file) increments them. */
static unsigned int _vita_gnrt_hits   = 0;
static unsigned int _vita_gnrt_nulls  = 0;
/* v02.30: log52 nailed the freeze at start_lightweight_thread_asm+0x18
 * with the kernel still reporting the VM thread RUNNING and zero threads
 * in any wait queue - so the freeze is user-space only. The open question
 * is whether the birth stub of the dispatched green thread is re-entered
 * through primordial_to_current_thread() (LR=stub entry is that gadget's
 * signature: 'ldr lr,[chain+4]; mov pc,lr'). These counters split the
 * switch-out side JUST_BORN visibility out of the _debugger_active short-
 * circuit (never active in this build) so the next dump can tell 'born,
 * dispatched, ran' apart from 'born, dispatched, never executed'.
 * _vita_swjb_seen counts switch-outs of threads that still had the
 * JUST_BORN bit set; _vita_swjb_clear counts the debugger-gated clears
 * (expected to stay 0 here); _vita_pe_enter counts entries into the
 * generated invoke_pending_entries stub (see SharedStubs_arm.cpp). */
static unsigned int _vita_swjb_seen  = 0;
static unsigned int _vita_swjb_clear = 0;
/* v02.36: id of the most recently Scheduler::start()ed thread; consumed
 * (and reset to -1) by the birth-delay gate in switch_thread() on that
 * thread's FIRST dispatch. -1 = no pending birth. */
static int vita_last_started_id = -1;
/* v02.36: declared here (not #include'd) because Scheduler.cpp is a
 * merged-src unit with no direct system includes; signature per
 * psp2/kernel/threadmgr.h. */
extern "C" int sceKernelDelayThread(unsigned int usec);
unsigned int _vita_pe_enter = 0;
/* v02.30 addendum: THREAD_JUST_BORN is only SET under _debugger_active
 * (Scheduler::start, this file) which is compiled to 0 in this build,
 * so swjb_seen/swjb_clear are expected to stay 0 - kept only so the
 * snapshot line does not lie by omission. The LIVE probes are the
 * time_slice counters below: Main_vita's slave-mode loop calls
 * JVM_TimeSlice() once per iteration, so _vita_ts_enter counts master
 * loop iterations (each early-return included) and _vita_ts_gadget
 * counts the ones that actually crossed primordial_to_current_thread()
 * into Java. At a freeze: ts_enter stalled = the master loop itself
 * never came back (log52's case: frozen inside the gadget crossing);
 * ts_enter still climbing with ts_gadget stalled = spinning on the
 * no-runnable-thread early return (dead green threads, birth channel
 * starved); both climbing = healthy switching, freeze is elsewhere. */
static unsigned int _vita_ts_enter  = 0;
static unsigned int _vita_ts_gadget = 0;
/* v02.39: first-dispatch decision counter. log61/62/64/65 all froze
 * the VM thread with pc inside the first 3 instructions of the birth
 * trampoline (start_lightweight_thread_asm+0/+4/+8), lr=trampoline
 * entry, sp=park level of the boot gadget frame (JVM::start+0x18c on
 * the C stack). run=1/ts_enter=0 pin the C side: the boot crossing
 * happened exactly once and mid-run dispatches never re-cross the
 * gadget. The open question is whether the frozen pc is REAL (the
 * trampoline executed 0-2 instructions, then the cpu was taken away)
 * or a SNAPSHOT (the green-thread switcher wrote the birth imprint
 * into the TCB but the cpu was lost before/while dispatching - the
 * kernel then keeps reporting RUNNING while nothing runs).
 * _vita_born_dsp counts the dispatch DECISIONS for the most recently
 * started thread (same anchor the v02.36 birth-delay gate consumes).
 * It is incremented in C++ right before the master-mode switch, i.e.
 * BEFORE any trampoline instruction could run. At a freeze:
 *   born_dsp = 0  -> the scheduler never even decided to dispatch the
 *                    newborn; the frozen pc cannot be a TCB snapshot
 *                    written by this path (snapshot hypothesis DEAD,
 *                    the trampoline really executed 0-2 instructions)
 *   born_dsp > 0  -> at least one birth dispatch was handed to the
 *                    switcher. Frozen pc at stub+0/+8 with born_dsp
 *                    counting only the DECISION still cannot prove
 *                    execution - but combined with pe consumption it
 *                    separates 'decided, lost cpu before entering the
 *                    stub' from 'decided, entered, lost cpu inside'. */
static unsigned int _vita_born_dsp = 0;
/* v02.05: Thread::timer_tick entry count (defined in Thread.cpp next
 * to the Java-site sampling probe) - read through the accessor bridge
 * at the bottom of this file. */
extern unsigned int _vita_ttk_count;
/* v02.11: tick-stage probe state, defined in Thread.cpp next to the
 * counter above - same accessor bridge. */
extern volatile unsigned int _vita_ttk_stage;
extern volatile unsigned int _vita_ttk_hits[8];
/* v02.17: dispatch ring - the last N threads switch_thread() switched
 * AWAY from, with the fields that matter for the create/die-spin
 * question (status + whether pending_entries is non-NULL, i.e. whether
 * the thread still has a Java body waiting to be entered). A single
 * frozen sample says where the VM stopped; the ring says WHAT it was
 * cycling through just before - which is the part log36..log41 could
 * not answer. Single writer (the VM thread), reader tolerates torn
 * values (the writer is frozen by the time the watchdog reads). */
#define VITA_DISP_RING 32
volatile unsigned int _vita_disp_ring[VITA_DISP_RING][4]; /* id,pri,st,pe */
volatile unsigned int _vita_disp_seq = 0;
/* v02.42: scheduler QUEUE-OP ring - the cause-side counterpart of the
 * dispatch ring. log67's livelock verdict (land 14.5M/s, dispatch ring
 * id=3 pe=0 x16, q5 CYCLE(2), switch/lw_exit/ts_gadget all FROZEN)
 * means the spin loop is a pure-asm closed loop that no longer passes
 * through ANY C++ checkpoint - so the last C++ queue operations
 * before the spin started are frozen in history: exactly what this
 * ring preserves. Six ops recorded:
 *   1=add_to_active    2=remove_from_active   3=selected-by-get_next
 *   4=nrt-cache-write  5=set_priority-move    6=justborn-nrt-clear
 * Entry = id, op, priority, pending_entries!=NULL. The pe column is
 * the case-critical one: log67 dispatched id=3 with pe=0 sixteen
 * times, and WHO re-queued / re-selected a pe=0 thread is the open
 * question. Single writer (VM thread), watchdog reads after freeze.
 *
 * v02.49: 5th column = CALLER (__builtin_return_address(0)). log72
 * nailed the ring anatomy but left two attribution gaps the 4-column
 * ring cannot close: (a) id4's ADD carried pe=0 - a birth ADD
 * (Thread::start appends run_entry BEFORE Scheduler::start->add_to_
 * active) must see pe=1, so either the entry vanished pre-dispatch or
 * the ADD came from another path (notify_wakeup / timeout wakeup);
 * (b) id3 logged 16x SEL with ZERO op-4 NRT - selection without a
 * switch_thread set_current commit means those SELs came from pure
 * QUERY callers (yield/sleep_forever/terminate all poll
 * get_next_runnable_thread for is_null), not dispatches. The caller
 * return address names the function for every op - resolve offline
 * against the ELF symtab. Level-0 return address is always available
 * on this target (arm-vita-eabi, -fno-omit-frame-pointer not needed
 * for level 0). */
#define VITA_Q_RING 64
volatile unsigned int _vita_q_ring[VITA_Q_RING][5]; /* id,op,pri,pe,caller */
volatile unsigned int _vita_q_seq = 0;
#define VITA_Q_REC(tp, op_, pri_, pe_)                                     \
  do {                                                                     \
    unsigned int qs_ = _vita_q_seq;                                        \
    {                                                                      \
      Thread *qp_ = (tp);                                                  \
      _vita_q_ring[qs_ & (VITA_Q_RING - 1)][0] =                           \
        (unsigned int)qp_->id();                                           \
      _vita_q_ring[qs_ & (VITA_Q_RING - 1)][1] = (unsigned int)(op_);       \
      _vita_q_ring[qs_ & (VITA_Q_RING - 1)][2] = (unsigned int)(pri_);      \
      _vita_q_ring[qs_ & (VITA_Q_RING - 1)][3] = (unsigned int)(pe_);       \
      _vita_q_ring[qs_ & (VITA_Q_RING - 1)][4] =                           \
        (unsigned int)(uintptr_t)__builtin_return_address(0);              \
    }                                                                      \
    _vita_q_seq = qs_ + 1;                                                 \
  } while (0)
#define VITA_DISP_REC(t)                                                   \
  do {                                                                     \
    unsigned int s_ = _vita_disp_seq;                                      \
    (void)s_;                                                              \
    {                                                                      \
      Thread *tp_ = (t);                                                   \
      _vita_disp_ring[s_ & (VITA_DISP_RING - 1)][0] =                      \
        (unsigned int)tp_->id();                                           \
      _vita_disp_ring[s_ & (VITA_DISP_RING - 1)][1] =                      \
        (unsigned int)tp_->priority();                                     \
      _vita_disp_ring[s_ & (VITA_DISP_RING - 1)][2] =                      \
        (unsigned int)tp_->status();                                       \
      _vita_disp_ring[s_ & (VITA_DISP_RING - 1)][3] =                      \
        (tp_->pending_entries() != NULL) ? 1u : 0u;                        \
    }                                                                      \
    _vita_disp_seq = s_ + 1;                                               \
  } while (0)
#endif

OopDesc*   Scheduler::_gc_global_head;
OopDesc*   Scheduler::_gc_current_thread = NULL;

int        Scheduler::_estimated_event_readiness = 0;
bool       Scheduler::_timer_has_ticked = false;
bool       Scheduler::_slave_mode_yielding = false;
jlong      Scheduler::_slave_mode_timeout = -2;
const char Scheduler::lowbits[16] = {1,1,2,1,3,1,2,1,4,1,2,1,3,1,2,1};
unsigned char Scheduler::_thread_execute_counts[MAX_TASKS][ThreadObj::NUM_PRIORITY_SLOTS + 1];

#if ENABLE_PERFORMANCE_COUNTERS
jlong      Scheduler::_slave_mode_yield_start_time;
#endif

#if ENABLE_ISOLATES
// Different scales of task priority.  Numbers represent total percent of 
// cycles that a task of that priority (1, 2 or 3) would get if there are
// competing tasks (num_tasks > 2)
// 0 -> no difference between priority levels.
// 1 -> linear scale
// 2 -> proportional to priority^1.5
// 3 -> proportional to priority^2
// 4 -> proportional to priority^2.5
// 5 -> proportional to priority^3
//
// Note that tasks of one priority *share* the percentage for that priority

const unsigned int
Scheduler::sched_priority[TASK_PRIORITY_SCALE_MAX][Task::PRIORITY_MAX+1] =
  {{0, 33, 33, 33},
   {0, 17, 33, 50},
   {0, 11, 31, 58},
   {0, 7, 29, 64},
   {0, 4, 26, 71},
   {0, 3, 21, 76}};

unsigned int Scheduler::_task_execute_counts[Task::PRIORITY_MAX+1];
#endif

inline ReturnOop Scheduler::find_waiting_thread(Oop* obj) {
  Thread::Raw current, start;
  current = start = Universe::scheduler_waiting();
  while (!current.is_null()) {
    JavaOop::Raw wait_object = current().wait_obj();
    if (wait_object.equals(obj)) {
      return current;
    }
    current = current().next_waiting();
  }
  return (ReturnOop)NULL;
}

bool Scheduler::has_waiters(Oop *obj) {
  // This function is used by SemaphoreLock to check if any thread is
  // on a semaphore.
  Thread::Raw thread = find_waiting_thread(obj);
  if (!thread.is_null()) {
    return true;
  } else {
    return false;
  }
}

#ifndef PRODUCT

#if ENABLE_ISOLATES
void Scheduler::check_active_queues(){
  for (int task_id = Task::FIRST_TASK; task_id < MAX_TASKS; task_id++) {
    Task::Raw task = Task::get_task(task_id);
    if (task.is_null()) {
      continue;
    }
    ObjArray::Raw priority_list = task().priority_queue();
    if (priority_list.is_null()) {
      // could get here during GC before we create priority_queue list
      continue;
    }
    Thread::Raw queue_thread, thread_list, last;
    for (int priority = ThreadObj::NUM_PRIORITY_SLOTS;
         priority > 0; priority--) {
      thread_list = priority_list().obj_at(priority);
      if (thread_list.is_null()) {
        continue;
      }
      GUARANTEE(threads_in_queue(priority, &task),
                "priority queue null, but bit is set");
      queue_thread = thread_list;
      last = queue_thread().previous();
      queue_thread = last;
      do {
        GUARANTEE(queue_thread().is_jvm_thread(),
                  "debugging check"); // CLEANUP
        queue_thread = queue_thread().next();
      } while (!queue_thread.equals(&last));
    }
  }
}

int Scheduler::live_thread_count_for_task(int task_id) {
  // debugging function

  int count = 0;
  Thread::Raw t = Universe::global_threadlist();
  while (t.not_null()) {
    if (t().task_id() == task_id) {
      ++ count;
    }
    t = t().global_next();
  }

  return count;
}
#endif // ENABLE_ISOLATES
#endif

#if ENABLE_ISOLATES || !defined(PRODUCT)
bool Scheduler::is_in_some_active_queue(Thread* thread) {
  Task::Raw task;
#if ENABLE_ISOLATES
  for (int task_id = Task::FIRST_TASK; task_id < MAX_TASKS; task_id++) {
    task = Task::get_task(task_id);
    if (task.is_null()) {
      continue;
    }
    ObjArray::Raw priority_list = task().priority_queue();
#else
    ObjArray::Raw priority_list = Universe::scheduler_priority_queues();
#endif
    Thread::Raw queue_thread, thread_list, last;
    for (int priority = ThreadObj::NUM_PRIORITY_SLOTS; priority > 0; priority--){
      thread_list = priority_list().obj_at(priority);
      if (thread_list.is_null()) {
        continue;
      }
      GUARANTEE(threads_in_queue(priority, &task),
                "priority queue null, but bit is set");
      queue_thread = thread_list;
      last = queue_thread().previous();
      queue_thread = last;
      do {
        if (queue_thread.equals(thread)) {
          return true;
        }
        queue_thread = queue_thread().next();
      } while (!queue_thread.equals(&last));
    }
#if ENABLE_ISOLATES
  }
#endif
  return false;
}
#endif

bool Scheduler::is_in_list(Thread* thread, Thread* list) {
  if (list->is_null()) {
    return false;
  }
  // This function may be called where it's not possible to create a handle,
  // hence the use of ::Raw
  Thread::Raw p = list->obj();
  Thread::Raw last = p;
  do {
    if (p.equals(thread)) {
      return true;
    }
    p = p().next();
  } while (!p.equals(last));
  return false;
}

void Scheduler::add_to_global_list(Thread *thread) {
  thread->set_global_next(Universe::global_threadlist());
  *Universe::global_threadlist() = *thread;
}

void Scheduler::add_to_list(Thread* thread, Thread* list_head) {
  GUARANTEE(!is_in_list(thread, list_head), "cannot add twice");

  if (list_head->is_null()) {
    thread->set_next(thread);
    thread->set_previous(thread);
    *list_head = thread->obj();
  } else {
    // Always add to the end of the list to avoid thread starvation. Since the
    // list is circular, we can do it by changing list_head->previous.
    Thread::Raw prev = list_head->previous();
    thread->set_previous(&prev);
    prev().set_next(thread);
    list_head->set_previous(thread);
    thread->set_next(list_head);
  }
}

void Scheduler::remove_from_list(Thread* thread, Thread* list) {
  Thread::Raw next, previous;

  GUARANTEE(is_in_list(thread, list), "must be in list");
  if (list->equals(thread)) {
    next = list->next();
    if (next.equals(list)) {
      // Removing last active thread
      *list = (ReturnOop)NULL;
    } else {
      // Removing active thread
      *list = next.obj();
    }
  }

  next     = thread->next();
  previous = thread->previous();

  next().set_previous(&previous);
  previous().set_next(&next);
  thread->clear_next();
  thread->clear_previous();
}

/**  Given a thread add/remove it from the appropriate priority queue.
 */
void Scheduler::adjust_priority_list(Thread *thread, bool is_add) {
  jint priority = thread->priority();
#if ENABLE_ISOLATES
  Task::Raw task = Task::get_task(thread->task_id());
  GUARANTEE(!task.is_null(), "No task for thread");
  ObjArray::Raw priority_lists = task().priority_queue();
#else
  ObjArray::Raw priority_lists = Universe::scheduler_priority_queues();
#endif
  Thread::Raw thread_list = priority_lists().obj_at(priority);
  if (is_add) {
    add_to_list(thread, &thread_list);
    // set the bit to show that there are threads in this queue
    set_priority_valid(thread, priority);
    // set the cache to this thread.  It may quite
    // likely be the next one that should run.
    if (TraceThreadsExcessive) {
      TTY_TRACE_CR(("add: thread 0x%x (id=%d, prio=%d) to list 0x%x",
                    thread->obj(), thread->id(), priority, thread_list.obj()));
    }
    GUARANTEE(thread->is_jvm_thread(), "debugging check"); // CLEANUP
    if (priority_lists().obj_at(priority) == NULL) {
      // Unless the list was empty, do not change the head of the list,
      // otherwise we may have starvation.
      priority_lists().obj_at_put(priority, thread);
    }
#if defined(VITA)
    VITA_Q_REC(thread, 1, priority,
               (thread->pending_entries() != NULL) ? 1u : 0u);
#endif
  } else {
    if (TraceThreadsExcessive) {
      TTY_TRACE_CR(("remove: thread 0x%x (id=%d, prio=%d) from list 0x%x",
                    thread->obj(), thread->id(), priority, thread_list.obj()));
    }
    remove_from_list(thread, &thread_list);
    if (thread_list.is_null()) {
      // no more threads on this queue so clear the bit
      clear_priority_valid(thread, priority);
    }

    GUARANTEE(thread_list.is_null() || thread_list().is_jvm_thread(), 
              "debugging check"); // CLEANUP

    priority_lists().obj_at_put(priority, &thread_list);
    // if the cache entry points to this thread, clear it so it gets reloaded
    // later in yield or switch_thread to the next runnable thread
    if (thread->equals(_next_runnable_thread)) {
      _next_runnable_thread = thread_list.obj();
      if (TraceThreadsExcessive) {
        TTY_TRACE_CR(("adjust: _next_runnable_thread 0x%x (id=%d) ",
              _next_runnable_thread, 
             (_next_runnable_thread ? thread_list().id() : -1)));
      }
    }
#if defined(VITA)
    VITA_Q_REC(thread, 2, priority,
               (thread->pending_entries() != NULL) ? 1u : 0u);
#endif
  }
}

void Scheduler::add_to_active(Thread* thread) {
  if (_debugger_active) {
    thread->set_active();
  }
  adjust_priority_list(thread, true);
}

void Scheduler::remove_from_active(Thread* thread) {
  if (_debugger_active) {
    thread->set_suspended();
  }
  adjust_priority_list(thread, false);
}

void Scheduler::add_to_asynchronous(Thread* thread) {
  ++ _async_count;
  add_to_list(thread, Universe::scheduler_async());
}

void Scheduler::remove_from_asynchronous(Thread* thread) {
  remove_from_list(thread, Universe::scheduler_async());
  -- _async_count;
}

#if ENABLE_ISOLATES
void Scheduler::suspend_threads(int task_id) {
  Task::Raw task = Task::get_task(task_id);
  ObjArray::Raw priority_list = task().priority_queue();
  Thread::Raw queue_thread;
  Thread::Raw thread_list;
  Thread::Raw next_thread;
  Thread::Raw last_thread;
  
  // Search the active thread list for the threads to be suspended
  for (int priority = ThreadObj::NUM_PRIORITY_SLOTS; priority > 0; priority--) {
    thread_list = priority_list().obj_at(priority);
    if (thread_list.not_null()) {
      next_thread = thread_list;

      last_thread = next_thread().previous();
      do {
        queue_thread = next_thread;
        next_thread = queue_thread().next();
        if (queue_thread().task_id() == task_id) {
          remove_from_active(&queue_thread);
          add_to_suspend(&queue_thread);
        }
      } while (!queue_thread.equals(&last_thread));
    }
  }
  Scheduler::yield();
}

void Scheduler::resume_threads(int task_id) {
  if (Universe::suspend_task_queue()->not_null()) {
    Thread::Raw next_thread = Universe::suspend_task_queue();
    Thread::Raw queue_thread;
    Thread::Raw last_thread;

    last_thread = next_thread().previous();
    do {
      queue_thread = next_thread;
      next_thread = queue_thread().next();
      if (queue_thread().task_id() == task_id) {
        remove_from_suspend(&queue_thread);
        add_to_active(&queue_thread);
      }
    } while (!queue_thread.equals(&last_thread));
    Scheduler::yield();
  }
}

void Scheduler::add_to_suspend(Thread *thread) {
  if (TraceThreadsExcessive) {
    TTY_TRACE_CR(("suspend: thread 0x%x (tid=%d)    task_id: %d",
                  thread->obj(), thread->id(), thread->task_id()));
  }
  add_to_list(thread, Universe::suspend_task_queue());
}

void Scheduler::remove_from_suspend(Thread *thread) {
  if (TraceThreadsExcessive) {
    TTY_TRACE_CR(("resume: thread 0x%x (tid=%d)    task_id: %d",
                  thread->obj(), thread->id(), thread->task_id()));
  }
  remove_from_list(thread, Universe::suspend_task_queue());
}
#endif

/** We'd come to here if all threads are blocked forever waiting
    on monitors. This may be caused by a deadlock, or by applications
    that purposefully wait on monitors that will never get notified.
*/
/* v02.00: sleep_forever forensics - log24 froze with the VM thread
 * RUNNING (not WAITING like the tone thread) and PC parked at the
 * green-thread berth stub. These counters make the idle loop visible
 * to the watchdog snapshot: entries = times sleep_forever was entered,
 * spins = idle-loop iterations, last_ms = Os::sleep span of the most
 * recent spin (0x7fffffff in the upstream branch = dead sleep).
 * Declared here (before sleep_forever) because the loop below
 * increments them; the extern "C" readouts live at the file tail. */
static unsigned int _vita_sf_entries  = 0;
static unsigned int _vita_sf_spins    = 0;
static unsigned int _vita_sf_last_ms  = 0;

void Scheduler::sleep_forever() {
#ifndef PRODUCT
  if (!DisableDeadlockFinder) {
    check_deadlocks();
  }
#endif

#if defined(VITA)
  /* v02.00 THE REAL FIX: upstream master mode treats the all-blocked
   * state as an app deadlock ("waiting on monitors that will never get
   * notified") because on real phones a host slave loop owns the event
   * pump. This port runs master mode with NO host loop: a game waiting
   * for a keypress IS the all-blocked state. log22/23/24 all froze in
   * the upstream else-branch below - Os::sleep() never pumps events,
   * never wakes timed sleepers, so the runnable queue can never be
   * re-filled and the VM sleeps forever (ring seq frozen, pump==CE
   * frozen, heartbeat frozen, PC residue at the green-thread berth).
   * Replace the eternal sleep with a bounded idle loop: wake due
   * sleepers, drive the same 50ms-throttled event pump that
   * switch_thread uses, nap 50 ms. Key/touch/timer events then re-fill
   * the queue and the loop exits. */
  SETUP_ERROR_CHECKER_ARG;
  _vita_sf_entries++;
  while (*get_next_runnable_thread() == NULL) {
    wake_up_timed_out_sleepers(JVM_SINGLE_ARG_CHECK);
    pump_events_vita();
    _vita_sf_spins++;
    _vita_sf_last_ms = 50;
    Os::sleep(50);
  }
  _vita_sf_last_ms = 0;
  /* Mirror the debugger branch two calls up: install the woken thread
   * as current before returning into switch_thread. */
  Thread::set_current(get_next_runnable_thread());
#else
  if (AbortOnInfiniteWait) {
    TTY_TRACE_CR(("Infinite wait detected. Aborting ..."));
    *((int*)0x01) = 1; // IMPL_NOTE: perhaps adding an Os::abort()?
    current_thread_to_primordial();
    SHOULD_NOT_REACH_HERE();
  } else {
    while (*get_next_runnable_thread() == NULL) {
      Os::sleep(0x7fffffff);
    }
  }
#endif
}

/*
 * Universe::scheduler_waiting() is a thread object that serves as
 * the head of a linked list of threads waiting.  Threads pointed at
 * by the _next pointer in this head thread are sleeping.  If a thread
 * is waiting and it is the first waiter for some object then we add it
 * to the _next_waiting list.  Another thread waiting for same object is
 * linked to first one via _next pointer.  Both lists single linked,
 * terminated by null.
 *
 *  sleep queue                    waiting for Object A    waiting for Obj B
 *
 * sched_waiting._nxt_waiting -> thrdA._nxt_waiting -> thrdB._nxt_waiting(NULL)
 *   _next                            \_next
 *     |                                 |
 *     |                         threadE (waiting for Obj A)
 *     |                           
 *  threadC 
 *     |
 *  threadD
 *
*/

ReturnOop Scheduler::add_waiting_thread(Thread *thread, JavaOop *obj) {

  remove_from_active(thread);
  Thread::Raw pending_waiters =  find_waiting_thread(obj);
  thread->clear_previous();

  if (pending_waiters.is_null()) {
    // link this thread into scheduler_waiting->_next_waiting queue
    Thread* wait_queue = Universe::scheduler_waiting();
    GUARANTEE(!wait_queue->is_null(), "scheduler_waiting is null");
    Thread::Raw tail = wait_queue->global_next();
    tail().set_next_waiting(thread);
    wait_queue->set_global_next(thread);
    pending_waiters = thread->obj();
    thread->clear_next();
  } else {
    GUARANTEE(obj->equals(pending_waiters().wait_obj()),
              "Wait objects not equal");
    //    thread->set_next(&pending_waiters);
    Thread::Raw tail = pending_waiters;
    while (tail().next() != NULL) {
      tail = tail().next();
    }
    tail().set_next(thread);
    thread->clear_next();
  }
  thread->clear_next_waiting();
  thread->set_wait_obj(obj);
  return pending_waiters;
}

ReturnOop Scheduler::add_sync_thread(Thread* thread, Thread *pending_waiters,
                                   JavaOop *obj) {
  remove_from_active(thread);
  if (pending_waiters->is_null()) {
    pending_waiters = thread;
    thread->clear_next();
  } else {
    Thread::Raw tail = pending_waiters;
    while (tail().next_waiting() != NULL) {
      tail = tail().next_waiting();
    }
    tail().set_next_waiting(thread);
  }
  thread->clear_next_waiting();
  thread->set_wait_obj(obj);
  return pending_waiters->obj();
}

void Scheduler::remove_waiting_thread(Thread* thread) {

  Thread::Raw list;
  JavaOop obj = thread->wait_obj();
  if (obj.is_null()) {
    // sleeping thread
    list = Universe::scheduler_waiting();
  } else {
    list = find_waiting_thread(&obj);
    GUARANTEE(!list.is_null(), "Waiting thread not in any list");
  }
  if (list.equals(thread)) {
    // Removing the head of the list
    Thread::Raw current, tail;
    current = Universe::scheduler_waiting();
    tail = current().global_next();
    Thread::Raw next = current().next_waiting();
    while(next.not_null() && !list.equals(&next)) {
      current = next;
      next = next().next_waiting();
    }
    GUARANTEE(list.equals(&next), "Waiting thread not in list");
    if (thread->next() == NULL) {
      // last thread waiting for this object
      next = thread->next_waiting();
      current().set_next_waiting(&next);
      if (tail.equals(thread)) {
        // removing last waiting queue head, current is now the tail
        Universe::scheduler_waiting()->set_global_next(&current);
      }
    } else {
      // More threads waiting for this object
      Thread::Raw next_waiting = next().next_waiting();
      next = thread->next();
      current().set_next_waiting(&next);
      next().set_next_waiting(&next_waiting);
      if (tail.equals(thread)) {
        // 'next' is now the tail
        Universe::scheduler_waiting()->set_global_next(&next);
      }
    }
  } else {
    // Removing thread from middle of list of threads waiting for object
    Thread::Raw current = list;
    Thread::Raw next = current().next();
    while(next.not_null() && !thread->equals(&next)) {
      current = next;
      next = next().next();
    }
    GUARANTEE(thread->equals(&next), "Waiting thread not in list");
    next = thread->next();
    current().set_next(&next);
  }
  thread->clear_next_waiting();
  thread->clear_next();
}


void Scheduler::add_to_sleeping(Thread* thread) {
  if (_debugger_active) {
    thread->set_status((thread->status() &
                        ~THREAD_NOT_ACTIVE_MASK) | THREAD_SLEEPING);
  }
  // First list is the sleep queue.
  Thread::Raw current = Universe::scheduler_waiting();
  Thread::Raw next = current().next();
  while(next.not_null()) {
    current = next;
    next = next().next();
  }
  current().set_next(thread);
  thread->clear_next();
  thread->clear_wait_obj();
}

#if ENABLE_ISOLATES

void Scheduler::wake_up_async_threads(int task_id) {
  Thread::Raw t = Universe::scheduler_async();
  Thread::Raw next;
  int count = _async_count;
  for (int i = 0; i < count; i++) {
    next = t().next();
    if (t().task_id() == task_id) {
      t().set_async_redo(1);
      // IMPL_NOTE: don't know if this is needed
      // t().set_noncurrent_pending_exception(&termination_signal);
      unblock_thread(&t);
      t().set_async_redo(0);
    }
    t = next;
  }
}

// Wake up all sleeping threads of a terminated isolate.
// These will catch their isolate termination signal when they'll be
// scheduled.
// We could be more efficient by tracking a counter of sleeping thread per task.
void Scheduler::wake_up_terminated_sleepers(int task_id JVM_TRAPS) {
  if (!is_slave_mode() && !Universe::scheduler_async()->is_null()) {
    // Is this being called too often??
    check_blocked_threads(0);
  }
  UsingFastOops fast_oops;
  // Wake up all sleeping threads that run on behalf of the terminated task.
  Oop::Fast termination_signal = Task::get_termination_object();

  GUARANTEE(Universe::scheduler_waiting() != NULL, "Sleep queue at front");
  Thread::Fast this_waiting, next_waiting;
  Thread::Fast this_thread, next_thread;
  this_waiting = Universe::scheduler_waiting();
  while (!this_waiting.is_null()) {
    next_waiting = this_waiting().next_waiting();
    this_thread = this_waiting;
    while (!this_thread.is_null()) {
      next_thread = this_thread().next();
      if (this_thread().task_id() == task_id) {
        if (TraceThreadsExcessive) {
          TTY_TRACE_CR(("wakeup_timed_terminated_sleepers: signaling "
                        "terminated thread 0x%x",
                        (int)(address_word)this_thread().obj()));
        }
        if (Thread::current()->equals(this_thread())) {
          Thread::set_current_pending_exception(&termination_signal);
        } else {
          this_thread().set_noncurrent_pending_exception(&termination_signal);
        }
        remove_waiting_thread(&this_thread);
        notify_wakeup(&this_thread JVM_CHECK);
      }
      this_thread = next_thread;
    }
    this_waiting = next_waiting;
  }
  if (_async_count > 0) {
    wake_up_async_threads(task_id);
  }
  if (Universe::suspend_task_queue()->not_null()) {
    resume_threads(task_id);
  }

#if ENABLE_JAVA_DEBUGGER
  Thread::Fast thread = Universe::global_threadlist();
  while (thread.not_null()) {
    if (thread().task_id() == task_id) {      
      thread().set_suspend_count(0);
      thread().clear_dbg_suspended();      
    }
    thread = thread().global_next();
  }
#endif
}
#endif

#if defined(VITA)
void Scheduler::pump_events_vita() {
  // v01.93: extracted from the v01.83 gate inside
  // wake_up_timed_out_sleepers() so non-yield contexts (green-thread
  // spawn path, see start0) can drive the same throttled pump. The
  // static is shared: whichever site fires first owns the 50 ms window.
  static jlong vita_last_pump_ms = 0;
  jlong vita_now = Os::java_time_millis();
  if (_timer_has_ticked || _estimated_event_readiness > 0 ||
      (vita_now - vita_last_pump_ms) >= 50) {
    vita_last_pump_ms = vita_now;
    _timer_has_ticked = false;
    /* v01.98: count actual pump executions (not just entries). Paired
     * with the yield lane counters this separates "pump starving in the
     * fast lane" from "pump running but delivering nothing". */
    _vita_pump_runs++;
    /* v01.94: ring must see the pump actually run. log20 froze with the
     * same signature as log19 (last CE completes ANI OUT, next never
     * starts) but no longer passes through start0 - so mark here to
     * distinguish "pump starved entirely" from "pump ran and froze
     * inside check_blocked_threads()/JVMSPI_CheckEvents". */
    vita_site_mark();
    check_blocked_threads(0);
  }
}
#endif // VITA

void Scheduler::wake_up_timed_out_sleepers(JVM_SINGLE_ARG_TRAPS) {
#if defined(VITA)
  // On Vita all javacall audio/media paths are synchronous (no Java thread
  // ever blocks in an async native call), so Universe::scheduler_async()
  // stays empty while a game is running. The upstream gate below would then
  // NEVER invoke the event pump (checkForSystemSignal), leaving input,
  // touch and media events stuck in the native ring forever - the game
  // appears frozen (no key/touch response) although the VM is alive.
  // Drive the event pump from the timer tick instead.
  //
  // v01.83: log9 coredump showed the VM spinning in a thread create/die
  // cycle (pc frozen at start_lightweight_thread_asm+0x8, CE counter
  // frozen for 3s, RUNNING wait=none). In that loop this gate never
  // opened: _estimated_event_readiness only counts down inside
  // check_blocked_threads() itself, and the ticker-driven
  // _timer_has_ticked can starve when the cycle hogs the CPU. Add a
  // wall-clock backstop: any path that keeps calling yield()/this
  // function now drives the pump at least every 50 ms, which also
  // throttles the cycle (JVMSPI_CheckEvents waits up to 50 ms in ANI).
  // v01.93: body moved to Scheduler::pump_events_vita() so the
  // start_lightweight_thread_asm spawn path (which never yields) can
  // share it - see Java_java_lang_Thread_start0 in Natives.cpp.
  if (!is_slave_mode()) {
    pump_events_vita();
  }
#else
  if (!is_slave_mode() && !Universe::scheduler_async()->is_null()) {
    if (_timer_has_ticked || _estimated_event_readiness > 0) {
      _timer_has_ticked = false;
      check_blocked_threads(0);
    }
  }
#endif

  // Wake up all sleeping threads that have timed out.
  jlong time = Os::java_time_millis();
  GUARANTEE(Universe::scheduler_waiting() != NULL, "Sleep queue at front");
  UsingFastOops fast_oops;
  Thread::Fast this_waiting, next_waiting;
  Thread::Fast this_thread, next_thread;
  this_waiting = Universe::scheduler_waiting();
  while (!this_waiting.is_null()) {
    next_waiting = this_waiting().next_waiting();
    this_thread = this_waiting;
    while (!this_thread.is_null()) {
      next_thread = this_thread().next();
      if ((this_thread().wakeup_time()) != 0 && 
          (time >= this_thread().wakeup_time())) {
        if (TraceThreadsExcessive) {
          TTY_TRACE_CR(("wakeup_timed_out_sleepers: signaling thread 0x%x"
                        " (id=%d)", (int)(address_word)this_thread().obj(),
                        this_thread().id()));
        }
        remove_waiting_thread(&this_thread);
        notify_wakeup(&this_thread JVM_CHECK);
      }
      this_thread = next_thread;
    }
    this_waiting = next_waiting;
  }
}

bool Scheduler::initialize() {
#if ENABLE_PERFORMANCE_COUNTERS
  _slave_mode_yield_start_time = -1;
#endif
  _active_count = 0;
  _async_count = 0;
  _exit_async_pending = 0;
  _priority_queue_valid = 0;
#if ENABLE_ISOLATES
  if (TaskPriorityScale < 0 || TaskPriorityScale >= TASK_PRIORITY_SCALE_MAX) {
    TaskPriorityScale = TASK_PRIORITY_SCALE_MAX - 1;
  }
  // If we had a platform independent tick rate, this is where we
  // would calculate the per task execution token count
  //_task_token_count = 1000 / TickInterval / SchedulerDivisor;
#endif
  if (!GenerateROMImage) {
    return Os::start_ticks();
  } else {
    return true;
  }
}

void Scheduler::dispose() {
  if (!GenerateROMImage) {
    Os::stop_ticks();
  }
}

void Scheduler::terminate(Thread* thread JVM_TRAPS) {
  JVM_IGNORE_TRAPS;
#if defined(VITA)
  /* v02.31: terminate is the queue-removal half of green-thread death
   * (called from lightweight_thread_exit). Count entries and - see
   * the post-check at the bottom - whether this thread is STILL on
   * the global list after removal. A still-listed hit means either
   * double-terminate or a re-inserted zombie: both feed the redispatch
   * macro-loop verdict. */
  {
    extern unsigned int vita_v231_term;
    vita_v231_term++;
  }
#endif
  if (TraceThreadsExcessive) {
    TTY_TRACE_CR(("terminating 0x%x", thread->obj()));
  }
  _active_count--;

  // send before we remove the thread from the global list as we may allocate
  // and GC so we need a thread to store handles.
  // cleanup
  if (_debugger_active) {
    VMEvent::thread_event(thread, false);
  }

  Thread::Raw t = Universe::global_threadlist();
  Thread::Raw next, t_prev;
  if (t.equals(thread)) {
    *Universe::global_threadlist() = t().global_next();
  } else {
    t_prev = t.obj();
    t = t().global_next();
    while (t.not_null()) {
      if (t.equals(thread)) {
        next = t().global_next();
        t_prev().set_global_next(&next);
        break;
      }
      t_prev = t.obj();
      t = t().global_next();
    }
  }
  if (!TestCompiler) {
    Oop::Raw null_oop;
    thread->set_global_next((Thread*)&null_oop);
    ThreadObj::Raw tobj = thread->thread_obj();
    if (!tobj.is_null()) {
      tobj().clear_thread();
    }
  }
#if ENABLE_ISOLATES
  {
    const int tid = thread->task_id();
    UsingFastOops fast_oops;
    ObjArray::Fast oa;
    int thread_count;
    {
      Task::Raw task = Task::get_task(tid);
      thread_count = task().thread_count();
      oa = task().priority_queue();
    }
    // Don't want any handles when we call cleanup_terminated_task
    if (thread_count == 0) {
      // This is the last thread of a task. So we can clean up.
      ObjectHeap::finalize_task( tid );
      oa().obj_at_clear(thread->priority());
      Task::clear_priority_queue_valid(tid, thread->priority());
      // At this point (thread == _current_thread), so it will be
      // scanned by GC. The thread object may be a class loaded by the
      // task. If we don't clear it, some objects in this task may not
      // be cleaned up.
      thread->clear_thread_obj();
      // Task::cleanup_terminated_task() called later so we can
      // clear _current_thread at a safe point
      _next_runnable_thread = NULL;
    } else {
      // 'thread' is the currently active (soon-to-be dead) thread
      remove_from_active(thread);
    }
    if (!TestCompiler) {
      thread->set_task_id(Task::INVALID_TASK_ID);
    } else {
      // When we're running the romizer, we want thread to stick around,
      // so don't nuke its task id.
    }
  }
#else
  // 'thread' is the currently active (soon-to-be dead) thread
  remove_from_active(thread);
#endif

  // In ENABLE_ISOLATES mode, thread->task_id() is now INVALID_TASK_ID. This
  // makes it impossible to run GC. We disable it regardless of ENABLE_ISOLATES
  // for better test coverage.

  AllocationDisabler no_allocation_should_happen_for_the_rest_of_this_function;

  if (TraceThreadsExcessive) {
    TTY_TRACE_CR(("terminate: next 0x%x", get_next_runnable_thread()->obj()));
  }

  if (Universe::async_watcher()->equals(thread)) {
    *Universe::async_watcher() = (ReturnOop)NULL;
  }

#if defined(VITA)
  /* v02.31 post-check: the removal loop above must have unlinked this
   * thread. Raw pointer walk (no handles - we are inside an
   * AllocationDisabler), compare by obj() address. */
  {
    extern unsigned int vita_v231_term_listed;
    for (Thread::Raw t_ = Universe::global_threadlist()->obj();
         t_.not_null(); t_ = t_().global_next()) {
      if (t_.obj() == thread->obj()) {
        vita_v231_term_listed++;
        break;
      }
    }
  }
#endif
}

void Scheduler::terminate_all() {
  // Make it appear we have all but one Thread left in the system. This is
  // used to do native stuff only. No more Java code will be executed.
  {
    Thread *thread = Thread::current();
    OopDesc* thread_obj = thread->thread_obj();
    jint priority = ((ThreadObj)thread_obj).priority();

    // clear out any threads in any priority queue except the last thread
#if ENABLE_ISOLATES
    int thread_task_id = thread->task_id();
    for (int task_id = Task::FIRST_TASK; task_id < MAX_TASKS; task_id++) {
      Task::Raw task = Task::get_task(task_id);
      if (task.is_null()) {
        continue;
      }
      task().clear_all_priority_queue_valid();
      ObjArray::Raw pq = task().priority_queue();
      for (int i = 0; i <= ThreadObj::NUM_PRIORITY_SLOTS; i++) {
        pq().obj_at_clear(i);
      }
      if (task_id == thread_task_id) {
        pq().obj_at_put(priority, thread);
      }
    }
#else
    _priority_queue_valid = 0;
    ObjArray* pq = Universe::scheduler_priority_queues();
    for (int i = 0; i <= ThreadObj::NUM_PRIORITY_SLOTS; i++) {
      pq->obj_at_clear(i);
    }
    pq->obj_at_put(priority, thread);
#endif
    set_priority_valid(thread, priority);
  } 

  // We can't create handles here ...
  Thread *thread = Thread::current();
  thread->set_next(thread);
  thread->set_previous(thread);
  _next_runnable_thread = NULL;

  if (!Universe::scheduler_async()->is_null()) {
    //tty->print_cr("warning: async threads still active!");
    Universe::scheduler_async()->set_null();
    _async_count = 0;
  }
  _active_count = 1;
  if (TraceThreadsExcessive) {
    TTY_TRACE_CR(("terminate all"));
  }

  // We manually created an active list with one thread on it, the
  // current thread.  It had better be returned when we call
  // get_next_runnable_thread()
  thread = get_next_runnable_thread();
  GUARANTEE(!thread->is_null(), "Thread disappeared");
}

void Scheduler::wait_for(Thread* thread, jlong timeout) {
  // This can be called for either a synchronization variable or for a
  // wait variable.  This may or may not be the current thread.
  if (TraceThreadsExcessive) {
    TTY_TRACE_CR(("wait_for: Thread 0x%x (id=%d)", (int)(address_word)thread->obj(),
                  thread->id()));
  }

  jlong wakeup = timeout;
  if (wakeup > 0) {
    jlong os_time = Os::java_time_millis();
    wakeup += os_time;
    if (wakeup < os_time) {
      wakeup = max_jlong;
    }
  }
  thread->set_wakeup_time(wakeup);

  if (Thread::current()->equals(thread)) {
    yield();
  }
}

void Scheduler::wait(JavaOop* obj, jlong millis JVM_TRAPS) {
  // first check if this thread owns the lock on the object.
  // Synchronizing on an interned string object is handled by
  // allocating a 'proxy' object that is actually stored in the
  // stack lock
  UsingFastOops fast_oops;

  Thread *thread = Thread::current();
  NOT_PRODUCT(trace("enter wait", thread, (Thread *)obj));
  if (obj->klass() == _interned_string_near_addr) {
    JavaOop::Raw lock_object =
      Synchronizer::get_lock_object_ref(obj, thread, true JVM_CHECK);
    if (!lock_object.is_null()) {
      *obj = lock_object;
    }
  }
  if (!Synchronizer::is_locked_by(obj, thread)) {
    Throw::illegal_monitor_state_exception(thread_unlocked_wait JVM_THROW);
  }

#if ENABLE_CLDC_11
  if (thread->is_pending_interrupt()) {
    NOT_PRODUCT(trace("wait: pending interrupt", thread, (Thread *)obj));
    Scheduler::handle_pending_interrupt(JVM_SINGLE_ARG_NO_CHECK);
    return;
  }
#endif

  {
    // locked java_near's and stack allocations are not a good thing to have
    // around during a GC.
    AllocationDisabler no_allocation;
    StackLock* stack_lock = StackLock::from_java_oop(obj);
    Synchronizer::exit(stack_lock);
    // Store waiters information in thread so we can find it again, when waking
    thread->set_wait_stack_lock(stack_lock);
  }
  if (_debugger_active) {
    thread->set_status((thread->status() & ~THREAD_NOT_ACTIVE_MASK) |THREAD_CONVAR_WAIT);
    }

  Thread::Fast pending_waiters = add_waiting_thread(thread, obj);

  wait_for(thread, millis);
}

void Scheduler::notify(JavaOop* object, bool all, bool must_be_owner
                       JVM_TRAPS) {
  Thread *thread = Thread::current();
  if (must_be_owner) {
    GUARANTEE(is_in_some_active_queue(thread), "Must be active");
  }
  if (TraceThreadsExcessive) {
    TTY_TRACE(((all ? "notifyAll" : "notify")));
    TTY_TRACE_CR((" 0x%x, 0x%x", thread->obj(), object->obj()));
  }
  // first check if this thread owns the lock
  // Synchronizing on an interned string object is handled by
  // allocating a 'proxy' object that is actually stored in the
  // stack lock
  if (object->klass() == _interned_string_near_addr) {
    GUARANTEE(must_be_owner == true, 
              "must_be_owner may be false only for SemaphoreLock objects");
    JavaOop::Raw lock_object =
      Synchronizer::get_lock_object_ref(object, thread, true JVM_CHECK);
    if (!lock_object.is_null()) {
      *object = lock_object;
    }
  }
  if (must_be_owner && !Synchronizer::is_locked_by(object, thread)) {
    Throw::illegal_monitor_state_exception(
      ( all ? thread_unlocked_notifyall : thread_unlocked_notify) JVM_THROW);
  }

  UsingFastOops fast_oops;
  Thread::Fast waiting_thread = find_waiting_thread(object);
  Thread::Fast waker, next_waker;
  if (!waiting_thread.is_null()) {
    if (TraceThreadsExcessive) {
      TTY_TRACE_CR(("notify: signaling object 0x%x", object->obj()));
    }
    waker = waiting_thread;
    do {
      next_waker = waker().next();
      remove_waiting_thread(&waker);
      notify_wakeup(&waker JVM_CHECK);
      waker = next_waker;
    } while (all && !waker.is_null());
  }
  if (must_be_owner) {
    GUARANTEE(is_in_some_active_queue(thread), "Sanity");
  } 
}

// An internal function called when a waiting thread is notified or times out.
// It still has to regain the synchronization like
void Scheduler::notify_wakeup(Thread* thread JVM_TRAPS) {
  JavaOop::Raw obj;
#if ENABLE_ISOLATES
  bool is_active = true;
#endif
  GUARANTEE(!is_in_some_active_queue(thread), "Must not be active");
  if (TraceThreadsExcessive) {
    TTY_TRACE_CR(("notify: thread 0x%x (id=%d)", thread->obj(),
                  thread->id()));
  }

  add_to_active(thread);

  obj = thread->wait_obj();
  if (obj.not_null()) {
    // If obj was null, then we were sleeping rather than waking
    GUARANTEE(thread->wait_stack_lock() != NULL, "Sanity");

    // Try to synchronize on that object using our stack lock.
    StackLock* stack_lock = thread->wait_stack_lock();
    stack_lock->set_owner(&obj);
    thread->clear_wait_stack_lock();
    thread->clear_wait_obj();
#if ENABLE_ISOLATES
    is_active = Synchronizer::enter(thread, stack_lock JVM_CHECK);
#else
                Synchronizer::enter(thread, stack_lock JVM_CHECK);
#endif
  }
#if ENABLE_ISOLATES
  // At this point the stack lock is owned by this thread so we
  // can safely suspend it.  Since monitors are Task private there
  // is no chance that another task will be blocked by this thread
  // owning the lock.
  if (Task::is_suspended_task(thread->task_id())) {
    // The thread may not be active -- the Synchronizer::enter() above 
    // may yet put it again into a waiting queue.
    if (is_active) {
      remove_from_active(thread);
      add_to_suspend(thread);
    }
  }
#endif
}

/** Determine which thread should be run next.
    Setup the cache _next_runnable_thread so that switch_thread()
    can quickly find the next thread to switch to.
 */
void Scheduler::yield() {
  SETUP_ERROR_CHECKER_ARG;
  wake_up_timed_out_sleepers(JVM_SINGLE_ARG_CHECK);

  Thread *next_thread = get_next_runnable_thread(Thread::current());
  if (next_thread->not_null()) {
#if defined(VITA)
    /* v01.98: fast lane taken - the else branch below (where the event
     * pump lives) is skipped entirely. See the counter block note at the
     * top of this file. */
    Scheduler::_vita_yield_fast_count++;
#endif
    if (TraceThreadsExcessive) {
      if (!next_thread->equals(Thread::current())) {
        TTY_TRACE_CR(("yield: next_runnable thread 0x%x (id=%d)",
                      next_thread->obj(), next_thread->id()));
      }
    } 
    if (is_slave_mode()) {
      slave_mode_wait_for_event_or_timer(0);
    }
  } else {
#if defined(VITA)
    /* v01.98: slow lane taken - the event pump runs inside this branch. */
    Scheduler::_vita_yield_slow_count++;
#endif
    Thread::Raw wait_thrd;
    Thread::Raw t;
    if (TraceThreadsExcessive) {
      TTY_TRACE_CR(("yield: no runnable threads"));
    }
    while (*get_next_runnable_thread() == NULL) {
      // All threads are waiting for something. Let's sleep until one
      // of them wakes up.
      jlong min_wakeup_time = max_jlong;
      bool sleeper_found = false;
      wait_thrd = Universe::scheduler_waiting();
      while (!wait_thrd.is_null()) {
        t = wait_thrd.obj();
        while (!t.is_null()) {
          if (t().wakeup_time() != 0) {
            min_wakeup_time = min(min_wakeup_time, t().wakeup_time());
            sleeper_found = true;
          }
          t = t().next();
        }
        wait_thrd = wait_thrd().next_waiting();
      }

      // Must check here before calling wait_for_event... since slave mode
      // will return 'true' and we'll never resume other threads
      if (JavaDebugger::is_debugger_option_on()) {
        JavaDebugger::dispatch(100);
      }
      if (wait_for_event_or_timer(sleeper_found, min_wakeup_time)) {
        return;
      }

      wake_up_timed_out_sleepers(JVM_SINGLE_ARG_CHECK);
    }
    next_thread = get_next_runnable_thread();
  }
  if (JavaDebugger::is_debugger_option_on()) {
    JavaDebugger::dispatch(0);
  }

  if (!_debugger_active && !is_slave_mode()) {
    // if debugger connected, above call may suspend all runnable threads
    // This state will get handled in switch_thread_xxx_mode.
    // In slave mode, _next_runnable_thread may be set after JVM_TimeSlice()
    // returns.
    GUARANTEE(_next_runnable_thread != NULL, "No active threads");
  }
  // the rest will be handled by switch_thread
}

// return value: true = we have done waiting inside the loop 
// in Scheduler::yield().
bool Scheduler::wait_for_event_or_timer(bool sleeper_found,
                                        jlong min_wakeup_time) {
  jlong sleep_time;

  if (!sleeper_found) {
    if (Universe::scheduler_async()->is_null()) {
      if (JavaDebugger::is_debugger_option_on()) {
        // We can get here because of a convoluted path:  A thread is dying
        // and so it calls Thread::exitInternal via an entry_activation.
        // This will eventually get to switch_thread.  We may have a
        // 'just born' thread which will send a new_thread_event to the
        // debugger which will suspend all threads.  However this dying
        // thread will continue to die, eventually call yield in
        // lightweight_thread_exit and then get here.
        // If the active count > 0 we return false
        if (_active_count > 0) {
          // there are other threads but the debugger has suspended them
          return false;
        }
      }
      if (TraceThreadsExcessive) {
        TTY_TRACE_CR(("wait_for_event_or_timer: returning"));
      }

      // No more threads?, we're probably shutting down (??)
      return true;
    }

    sleep_time = -1;
  } else {
    sleep_time = (min_wakeup_time - Os::java_time_millis()) + 1;
    if (sleep_time < 0) {
      // Clock has advanced somewhat, but make sure we're not passing
      // a negative timeout, which means wait forever!
      sleep_time = 0;
    }
  }

  if (!is_slave_mode()) {
    master_mode_wait_for_event_or_timer(sleep_time);
  } else {
    slave_mode_wait_for_event_or_timer(sleep_time);
    return true;
  }

  return false;
}

void Scheduler::master_mode_wait_for_event_or_timer(jlong sleep_time) {
  GUARANTEE(!SlaveMode, "sanity");

  // We do suspend and resume ticks only if we gonna wait for long time
  // (sleep_time < 0) == sleep forever until an event happens.
  if (JavaDebugger::is_debugger_option_on()) {
    sleep_time = 100;
  }
  if (sleep_time < 0 || sleep_time > 500) {
    Os::suspend_ticks();
    Scheduler::check_blocked_threads(sleep_time);
    Os::resume_ticks();
  } else {
    Scheduler::check_blocked_threads(sleep_time);
  }
}

void Scheduler::slave_mode_wait_for_event_or_timer(jlong sleep_time) {

  if (JavaDebugger::is_debugger_option_on()) {
    if (sleep_time <0 || sleep_time > 100) {
      sleep_time = 100;
    }
  }
  if (sleep_time != 0 || _timer_has_ticked) {
    _slave_mode_timeout = sleep_time;
    _slave_mode_yielding = true;
  }
}

void Scheduler::sleep_current_thread(jlong millis) {
  Thread *thread = Thread::current();
  remove_from_active(thread);
  jlong os_time = Os::java_time_millis();
  jlong wakeup = millis + os_time;
  if (wakeup < os_time) {
    wakeup = max_jlong;
  }
  thread->set_wakeup_time(wakeup);
  add_to_sleeping(thread);

  yield();
}

void Scheduler::start(Thread* thread JVM_TRAPS) {
  NOT_PRODUCT(trace("start", thread));
#if defined(VITA)
  /* v01.94: spawn registration point. Together with the start0 mark
   * (Natives.cpp) and Thread::finish mark the ring can now tell apart
   * "loop re-enters Scheduler::start" from "loop reaches start0" and
   * "loop dies threads" - naming the cycle directly on next hang. */
  vita_site_mark();
  /* v02.17: visible-through-the-watchdog denominator for the same
   * question (the ring above only keeps 16 slots and is overwritten). */
  vita_sched_start_count++;
  /* v02.36: birth-delay anchor - see the matching block in
   * switch_thread(). Records the most recently started thread so its
   * FIRST dispatch can be delayed. */
  vita_last_started_id = thread->id();
#endif
  if (!Universe::is_bootstrapping()) {
    allocate_blocked_threads_buffer(_active_count+1 JVM_CHECK);
  }
  _active_count++;

  if (TraceThreadsExcessive) {
    TTY_TRACE_CR(("start: thread 0x%x", thread->obj()));
  }
  // Add thread to threads list
  add_to_active(thread);

  Scheduler::add_to_global_list(thread);

  // The "stillborn" flag is used by java.lang.Thread.join()
  // to check if the thread was spawned successfully.
  if (!Universe::is_bootstrapping()) {
    ThreadObj::Raw obj = thread->thread_obj();
    obj().clear_stillborn();
  }

  if (_debugger_active) {
    thread->set_status(THREAD_JUST_BORN);
  }
}
/**  Called from Natives.cpp to move this thread from one priority to another.
*/

void Scheduler::set_priority(Thread* thread, int old_priority) {
  NOT_PRODUCT(trace("set priority", thread));
  ThreadObj::Raw obj = thread->thread_obj();
  jint new_priority = obj().priority();

  // remove thread from it's priority list (if active)
#if ENABLE_ISOLATES
  Task::Raw task = Task::get_task(thread->task_id());
  GUARANTEE(!task.is_null(), "No task for thread");
  ObjArray::Raw priority_lists = task().priority_queue();
#else
  ObjArray::Raw priority_lists = Universe::scheduler_priority_queues();
#endif
  Thread::Raw thread_list = priority_lists().obj_at(old_priority);
  if (is_in_list(thread, &thread_list)) {
    remove_from_list(thread, &thread_list);

    priority_lists().obj_at_put(old_priority, &thread_list);
    if (thread_list.is_null()) {
      clear_priority_valid(thread, old_priority);
    }

    thread_list = priority_lists().obj_at(new_priority);
    add_to_list(thread, &thread_list);
    if (priority_lists().obj_at(new_priority) == NULL) {
      // Unless the list was empty, do not change the head of the list,
      // otherwise we may have starvation.
      priority_lists().obj_at_put(new_priority, thread);
    }
    set_priority_valid(thread, new_priority);
#if defined(VITA)
    VITA_Q_REC(thread, 5, new_priority,
               (thread->pending_entries() != NULL) ? 1u : 0u);
#endif
  }
  // if thread wasn't in an active list then it may be asleep or waiting
  // it will get added to correct list when it calls add_to_active()
}

#if ENABLE_CLDC_11
/**  Called from Natives.cpp to handle thread interrupt.
*/

void Scheduler::interrupt_thread(Thread* thread JVM_TRAPS) {
  NOT_PRODUCT(trace("interrupt thread", thread));
  UsingFastOops fast_oops;
  bool found = false;
  GUARANTEE(Universe::scheduler_waiting() != NULL, "Sleep queue at front");

  Thread::Fast this_waiting;
  Thread::Fast this_thread;
  this_waiting = Universe::scheduler_waiting();
  while (found == false && !this_waiting.is_null()) {
    this_thread = this_waiting;
    while (!this_thread.is_null()) {
      if (this_thread().equals(thread)) {
        NOT_PRODUCT(trace("interrupt_thread: thread found", &this_thread));
        if (TraceThreadsExcessive) {
          TTY_TRACE_CR(("interrupt_thread: signaling thread 0x%x (id=%d)",
                        (int)(address_word)this_thread().obj(), this_thread().id()));
        }
        UsingFastOops fast_oops;
        Throwable::Fast exception;
        String::Fast message;
        found = true;

        // IMPL_NOTE: an out-of-memory will be sent to both target thread
        // and current thread??
        // I think if the interrupting thread gets an exception then the
        // interruptee just remains in its wait state with the interrupt flag
        // set.  The thread doing
        // the interrupt() call got the error, not the thread waiting.
        // If this causes a deadlock then it's a program error;
        // Solution is to free memory and try again or wait with a timeout.

        exception
         = Throw::allocate_exception(Symbols::java_lang_InterruptedException(),
                                     &message JVM_NO_CHECK);
        if (exception.is_null()) {
          // Out of memory, presumably current_thread has a pending
          // exception at this point
          if (TraceThreadsExcessive) {
            TTY_TRACE_CR(("interrupt_thread: OOME, thread 0x%x",
                          (int)(address_word)thread->obj()));
          }
          thread->set_pending_interrupt();
          return;
        } else {
          if (Thread::current()->equals(thread)) {
            Thread::set_current_pending_exception(&exception);
          } else {
            thread->set_noncurrent_pending_exception(&exception);
          }
        }

        remove_waiting_thread(&this_thread);
        notify_wakeup(&this_thread JVM_CHECK);
        break;
      }
      this_thread = this_thread().next();
    }
    this_waiting = this_waiting().next_waiting();
  }

  if (!found) {
    if (TraceThreadsExcessive) {
      TTY_TRACE_CR(("interrupt_thread: set pending interrupt for thread 0x%x",
                   (int)(address_word)thread->obj()));
    }
    thread->set_pending_interrupt();
  }

}

/**  Called from Natives.cpp to handle pending interrupts.
*/

void Scheduler::handle_pending_interrupt(JVM_SINGLE_ARG_TRAPS) {
  NOT_PRODUCT(trace("handle pending interrupt ", Thread::current()));
  UsingFastOops fast_oops;
  Throwable::Fast exception;
  String::Fast message;
  exception = Throw::allocate_exception(Symbols::java_lang_InterruptedException(),
                                        &message JVM_NO_CHECK_AT_BOTTOM);

  Thread::set_current_pending_exception(&exception);
  Thread::current()->clear_pending_interrupt();
}
#endif /* ENABLE_CLDC_11 */

JVMSPI_BlockedThreadInfo *
Scheduler::get_blocked_threads(int *blocked_threads_count) {
  *blocked_threads_count =_async_count;

  if (_async_count == 0) {
    return NULL;
  } else {
    update_blocked_threads_buffer();
    return (JVMSPI_BlockedThreadInfo *)
        Universe::blocked_threads_buffer()->base_address();
  }
}

// Ensure we have enough space in Universe::blocked_threads_buffer for
// all the currently live threads (just in case every one of them may
// become blocked).
void Scheduler::allocate_blocked_threads_buffer(int target_count JVM_TRAPS) {
  GUARANTEE(!_jvm_in_quick_native_method,
            "SNI functions not allowed in quick native methods");

  int current_count = 0;
  const int unit_size = sizeof(JVMSPI_BlockedThreadInfo);
  if (!Universe::blocked_threads_buffer()->is_null()) {
    current_count = Universe::blocked_threads_buffer()->length() / unit_size;
  }

  // Don't be too allocation happy - always adjust the size of the buffer
  // in step of 4.
  if (target_count < 4) {
    target_count = 4;
  }
  if (current_count > target_count) {
    if (current_count - target_count < 4) {
      return;
    }
  } else {
    if (target_count - current_count < 4) {
      target_count = current_count + 4;
    }
  }

  // Need to allocate a new buffer
  UsingFastOops fast_oops;
  const int nbytes = target_count * unit_size;
  JavaOop::Fast exception = Thread::current_pending_exception();
  Thread::clear_current_pending_exception();

  // blocked_threads_buffer is shared between isolates
  const int task = ObjectHeap::start_system_allocation();
  TypeArray::Fast new_buffer = Universe::new_byte_array(nbytes JVM_NO_CHECK);
  ObjectHeap::finish_system_allocation( task );
  if (new_buffer().is_null()) { // allocation failed
    if (target_count < current_count) {
      // We're shrinking -- if we fail to shrink that's fine.
      //
      // We are called by Scheduler::terminate(). The current thread
      // may already have a pending exception. Let's just restore
      // things as it was before, such that this call is 'quiet'
      Thread::set_current_pending_exception(&exception);
    } else {
      // let the OOME filter up
    }
  } else {
    // restore possible exception
    Thread::set_current_pending_exception(&exception);
    *Universe::blocked_threads_buffer() = new_buffer;
  }
}

void Scheduler::update_blocked_threads_buffer() {
#ifdef AZZERT
  int current_count = Universe::blocked_threads_buffer()->length() /
                      sizeof(JVMSPI_BlockedThreadInfo);
  GUARANTEE(current_count >= _async_count, "buffer should have been allocated")
#endif

  JVMSPI_BlockedThreadInfo *blocked_threads = (JVMSPI_BlockedThreadInfo*)
      Universe::blocked_threads_buffer()->base_address();

  Thread::Raw t = Universe::scheduler_async();
  for (int i=0; i<_async_count; i++) {
    TypeArray::Raw byte_array = t().async_info();
    blocked_threads[i].thread_id = (JVMSPI_ThreadID) t.obj();
    if (byte_array.is_null()) {
      blocked_threads[i].reentry_data = (void *) NULL;
      blocked_threads[i].reentry_data_size = 0;
    } else {
      blocked_threads[i].reentry_data = (void *) byte_array().base_address();
      blocked_threads[i].reentry_data_size = byte_array().length();
    }
    t = t().next();
  }
}

void Scheduler::check_blocked_threads(jlong timeout) {
  if (VerifyOnly) {
    SHOULD_NOT_REACH_HERE();
    return;
  }

  if (_estimated_event_readiness > 0) {
    _estimated_event_readiness --;
  }

  update_blocked_threads_buffer();
  JVMSPI_BlockedThreadInfo* blocked_threads = (JVMSPI_BlockedThreadInfo*)
    Universe::blocked_threads_buffer()->base_address();

#if ENABLE_PERFORMANCE_COUNTERS
  jlong start_time = Os::elapsed_counter();
#endif

#if ENABLE_WTK_PROFILER
  WTKProfiler::suspend();
#endif

  JVMSPI_CheckEvents(blocked_threads, _async_count, timeout);

#if ENABLE_PERFORMANCE_COUNTERS
  jlong elapsed = Os::elapsed_counter() - start_time;
  jvm_perf_count.total_event_hrticks += elapsed;
  jvm_perf_count.total_event_checks ++;
#endif

#if ENABLE_WTK_PROFILER
  WTKProfiler::resume();
#endif
}

void Scheduler::block_current_thread() {
  GUARANTEE(!_jvm_in_quick_native_method,
            "SNI functions not allowed in quick native methods");

  Thread* thread = Thread::current();

  thread->set_async_redo(1);
  remove_from_active(thread);
  add_to_asynchronous(thread);

  _yield_on_thread_switch = true;
}

void Scheduler::unblock_thread(Thread *thread) {
  GUARANTEE(thread->async_redo() != 0, "Must be set");

  // This will cause the native method to be executed again by the
  // invokenative bytecode handler in the interpreter. The invokenative
  // bytecode will clear thread->async_redo(), which was set in
  // Scheduler::block_thread

  remove_from_asynchronous(thread);
  if (TraceThreadsExcessive) {
    TTY_TRACE_CR(("unblock: thread 0x%x", thread->obj()));
  }

#if ENABLE_ISOLATES
  if (Task::is_suspended_task(thread->task_id())) {
    add_to_suspend(thread);
  } else {
    add_to_active(thread);
  }
#else
  add_to_active(thread);
#endif

  // Tell the VM to be checking for events more eagerly to help
  // improve responsiveness -- If we have recently received some
  // events, we will try to check for events (calling
  // JVMSPI_CheckEvents in master mode, or leaving JVM_TimeSlice in
  // slave mode) for every thread switch.
  _estimated_event_readiness = 5;
}

void *Scheduler::allocate_blocked_thread_data(int data_size JVM_TRAPS) {
  UsingFastOops fast_oops;
  Thread* thread = Thread::current();
  TypeArray::Fast oop;

  // IMPL_NOTE: there's potential problem with size returned by
  // get_blocked_thread_data() as it could differ from what was passed
  // before. I think in long run we should get rid of int* size
  // parameter completely.
  if (data_size <= (int)CachedAsyncDataSize) {
    oop = thread->cached_async_info();
    if (oop.is_null()) {
      oop = Universe::new_byte_array(CachedAsyncDataSize JVM_NO_CHECK);
      if (oop.is_null()) {
        // we don't need OOME, return value should be checked
        Thread::clear_current_pending_exception();
        return NULL;
      }
      thread->set_cached_async_info(&oop);
    }
    thread->set_async_info(&oop);

    GUARANTEE(oop().length() <= (int)CachedAsyncDataSize, "sanity");

    return oop().base_address();
  }

  TypeArray::Fast byte_array = 
    Universe::new_byte_array(data_size JVM_NO_CHECK);
  if (byte_array.is_null()) {
    // we don't need OOME, return value should be checked
    Thread::clear_current_pending_exception();
    return NULL;
  }

  GUARANTEE(byte_array().length() == data_size, "sanity");

  thread->set_async_info(&byte_array);
  return byte_array().base_address();
}

void *Scheduler::get_blocked_thread_data(int *data_size) {
  TypeArray::Raw byte_array = Thread::current()->async_info();
  if (byte_array.is_null()) {
    if (data_size != NULL) {
      *data_size = 0;
    }
    return NULL;
  } else {
    if (data_size != NULL) {
      *data_size = byte_array().length();
    }
    return byte_array().base_address();
  }
}

/** Used by slave mode to execute some bytecodes.
    It then return to the caller who may call it again.
    It is important that there be no pending handles when this is called.
    When primoridal_to_current_thread() returns, we may be in a different
    Java thread, and the GC will be very confused.
*/

jlong Scheduler::time_slice(JVM_SINGLE_ARG_TRAPS) {
  GUARANTEE(is_slave_mode(), "sanity");
#if defined(VITA)
  /* v02.30: master-loop liveness probe (see file-head note). */
  _vita_ts_enter++;
#endif

  // It is important that there be no pending handles when this is called.
  // When primoridal_to_current_thread() returns, we may be in a different
  // Java thread, and the GC will be very confused.
  GUARANTEE(_last_handle == NULL, "No handles");

  wake_up_timed_out_sleepers(JVM_SINGLE_ARG_CHECK_0); // IMPL_NOTE: what to do when allocations fail??

  if (JavaDebugger::is_debugger_option_on()) {
    JavaDebugger::dispatch(0);
  }

  if (*get_next_runnable_thread() == NULL) {
    if (Universe::is_stopping()) {
      // If the debugger issued a VM_Exit command, we may have killed the VM
      // before any threads started.  If so just return -2 so as to stop the VM
      return -2;
    }

    if (JavaDebugger::is_debugger_option_on()) {
      // no threads ready.  We could just fall through to the next
      // return but in case someone changes it, let's make it clear
      // we want to return with a small timeout value.
        return 100;
    }

    // need revisit the program's main loop:
    //
    // In slave mode, if all thread were blocked (by
    // SNI_BlockThread(), or by Thread.sleep(), or by Object.wait())
    // when the VM returned control to the master, the master must
    // wait for at least one blocked thread to be executable, or for a
    // waiting thread to timeout, before it calls JVM_TimeSlice()
    // again. Otherwise it's just not good for battery conservation!
    return 100;
  }

  Thread *next_thread = get_next_runnable_thread();
  if (!next_thread->equals(Thread::current())) {
    Thread::set_current(next_thread);
#if defined(VITA)
    /* v02.42 op 4: JVM_TimeSlice nrt-cache consume point. */
    VITA_Q_REC(next_thread, 4, next_thread->priority(),
               (next_thread->pending_entries() != NULL) ? 1u : 0u);
#endif
  }
#if ENABLE_ISOLATES
  {
    Task::Raw task = next_thread->task_for_thread();
    if (task().is_terminating()){
#ifdef AZZERT
      GUARANTEE(!task.is_null() && task().status() >= Task::TASK_STOPPING,
                "task of terminating thread must be stopping");
#endif
      // The task that this thread belongs to is dead
      Thread::set_current_pending_exception(Task::get_termination_object());
    }
  }
#endif

  _slave_mode_yielding = false;
  _slave_mode_timeout = -2;
  _timer_has_ticked = false;

  // Need to clear timer tick, otherwise if we have spent too much
  // time outside of the VM, the timer tick would already be set and
  // we'd be kicked out of the VM immediately.
  Thread::clear_timer_tick();

#if ENABLE_PERFORMANCE_COUNTERS
  if (_slave_mode_yield_start_time != -1) {
    jlong elapsed = Os::elapsed_counter() - _slave_mode_yield_start_time;
    jvm_perf_count.total_event_hrticks += elapsed;
    jvm_perf_count.total_event_checks ++;
  }
#endif

#if defined(VITA)
  /* v02.30: last C-level instruction before crossing into Java via the
   * primordial gadget. If a future dump freezes inside the gadget (as
   * log52 did) this counter's frozen value is the number of COMPLETED
   * crossings minus this one. */
  _vita_ts_gadget++;
#endif
  primordial_to_current_thread();

  if (_estimated_event_readiness > 0) {
    _estimated_event_readiness --;
  }

  // If the Scheduler never called set_slave_mode_scheduler_info()
  // before we come to here, _slave_mode_timeout will have a value
  // of -2, which means the VM should be shut down.
  return _slave_mode_timeout;
}

#if 0
void Scheduler::iterate(do_thread_proc do_thread) {
  threads_do_list(do_thread, NULL, Universe::global_threadlist()->obj());
}
#endif

#ifndef PRODUCT
void Scheduler::check_deadlocks() {
  int dl = DeadlockFinder::find_and_print_deadlocks(tty);
  if (dl > 0) {
    tty->print_cr("********** WARNING: %d DEADLOCK%sFOUND ************", 
                  dl, dl>1 ? "S " : " ");
  }
}

void Scheduler::trace(const char* msg, Thread* first, Thread* second) {
#if USE_DEBUG_PRINTING
  if (!TraceThreadEvents) return;

  tty->print("0x%lx Scheduler::%s ", first->obj(), msg);
  if (first) {
    first->print_value();
  }
  if (second) {
    tty->print(" ");
    second->print_value();
  }
  tty->cr();
#endif
}

void Scheduler::print() {
#if USE_DEBUG_PRINTING
  tty->print_cr("Active threads:");
  Thread::Raw t, thread_list, last;
  Task::Raw task;
#if ENABLE_ISOLATES
  for (int task_id = Task::FIRST_TASK; task_id < MAX_TASKS; task_id++) {
    task = Task::get_task(task_id);
    if (task.is_null()) {
      continue;
    }
    ObjArray::Raw priority_list = task().priority_queue();
    if (priority_list.is_null()) {
      continue;
    }
#else
  ObjArray::Raw priority_list = Universe::scheduler_priority_queues();
#endif
  for (int prio = ThreadObj::NUM_PRIORITY_SLOTS; prio > 0; prio--) {
    thread_list = priority_list().obj_at(prio);
    if (thread_list.is_null()) {
      continue;
    }
    GUARANTEE(threads_in_queue(prio, &task),
              "priority queue null, but bit is set");
    t = thread_list;
    last = t().previous();
    t = last;
    do {
      t = t().next();
      tty->print("  [prio=%d] ", prio);
      t().print_value();
      if (t.equals(Thread::current())) {
        tty->print(" (*** current *** )");
      } else {
        tty->print(" (ready)");
      }
      tty->cr();
    } while (!t.equals(&last));
  }
#if ENABLE_ISOLATES
  }
#endif

  tty->cr();
  tty->print_cr("Async Threads:");
  for (t = Universe::scheduler_async(); t.not_null(); t = t().next()) {
    tty->print("  ");
    t().print_value();
    tty->print(" (async)");
    tty->cr();
    if (t().next() == Universe::scheduler_async()->obj()) {
      break;
    }
  }

  // Threads that are blocked on Object.wait()
  tty->cr();
  tty->print_cr("Waiting threads:");

  jlong now = Os::java_time_millis();
  Thread::Raw wt, wt_start;
  Thread::Raw thrd;
  wt_start = wt = Universe::scheduler_waiting();
  while (!wt.is_null()) {
    thrd = wt.obj();
    while (!thrd.is_null()) {
      if (wt.equals(&wt_start)) {
        // 'fake' scheduler_waiting thread, skip it
        thrd = thrd().next();
        continue;
      }
      tty->print("  ");
      thrd().print_value();
      JavaOop::Raw obj = thrd().wait_obj();
      tty->print(" (waiting for 0x%x, timeout=", obj.obj());
      if (thrd().wakeup_time() == 0) {
        tty->print("forever)");
      } else if ((thrd().wakeup_time() - now) < ((jlong)100000)) {
        tty->print("%d)", (int)((thrd().wakeup_time()-now)));
      } else {
        tty->print("%I64d)", (thrd().wakeup_time()-now));
      }
      tty->cr();
      thrd = thrd().next();
    }
    wt = wt().next_waiting();
  }

#if ENABLE_ISOLATES
  tty->print_cr("Waiting tasks:");

  Thread* head = Universe::suspend_task_queue();
  if (!head->is_null()) {
    Thread::Raw t = head->obj();
    Thread::Raw last = t;
    do {
      tty->print("  ");
      t().print_value();
      if (t.equals(Thread::current())) {
        tty->print(" (*** current *** )");
      } else {
        tty->print(" (ready)");
      }
      tty->cr();
      t = t().next();
    } while (!t.equals(&last));
  }
#endif

  tty->cr();
#endif // USE_DEBUG_PRINTING
}

#endif

void Scheduler::threads_do_list(do_thread_proc do_thread,
                                void do_oop(OopDesc**),
                                OopDesc* list_head) {
  AllocationDisabler no_allocation;

  // Do the threads, if there are any,
  for (Thread::Raw thread = list_head; thread.not_null();
                     thread = thread().global_next()) {
    // If an Isolate build we have to go through all the threads in the system
    // we need to switch the class list for every thread because
    // every thread can have a different task_id than the 
    // current running thread.  Otherwise we will be using
    // the wrong class list for the thread.o
    {
      TaskGCContext tmp(thread().task_id());
      do_thread(&thread, do_oop);
    }
  }
}

void Scheduler::oops_doer(Thread* thread, void do_oop(OopDesc**)) {
  thread->nonstack_oops_do(do_oop);
}

void Scheduler::oops_do(void do_oop(OopDesc**)) {
    // GUARANTEE(ObjectHeap::is_gc_active(), "This may be called used by GC only");
  do_oop((OopDesc**)&_current_thread);
  do_oop((OopDesc**)&_current_pending_exception);
  do_oop((OopDesc**)&_next_runnable_thread);
#if ENABLE_ISOLATES
  do_oop((OopDesc**)&_current_task);
#endif
  threads_do_list(oops_doer, do_oop, _gc_global_head);
  // Oop handles chained on stack
  ForAllHandles( handle ) {
    do_oop(&(handle->_obj));
  }

  // KNI handles
  _KNI_HandleInfo *info = last_kni_handle_info;
  while (info != NULL) {
    GUARANTEE(info->declared_count <= info->total_count,
              "KNI handles overflow");
    for (int i=0; i<info->declared_count; i++) {
      do_oop((OopDesc**)&info->handles[i]);
    }
    info = info->prev;
  }
}

void Scheduler::gc_prologue(void do_oop(OopDesc**)) {
  AllocationDisabler no_allocation;
  // We need to save these raw pointers during
  // ObjectHeap::update_object_pointers(). At that point
  // _next_runnable_thread, etc, may have been changed to
  // point to the new location of these lists. However, the
  // list themselves have not been moved yet.
  _gc_current_thread = Thread::current()->obj();
  Frame::set_gc_state();

  _gc_global_head = Universe::global_threadlist()->obj();
  Thread::Raw thread = _gc_global_head;
  for( ; thread.not_null(); thread = thread().global_next()) {

    // If an Isolate build we have to go through all the threads in the system
    // we need to switch the class list for every thread because
    // every thread can have a different task_id than the 
    // current running thread.  Otherwise we will be using
    // the wrong class list for the thread
    {
      TaskGCContext tmp(thread().task_id());
      thread().gc_prologue(do_oop);
    }
  }

#ifdef AZZERT
  ForAllHandles( handle ){
    // Make sure that there are no JavaNears that are actually stack locks.
    // This really confuses the GC, and should never happen.
    GUARANTEE(handle->is_null() || !ObjectHeap::contains(handle->obj())
               || !handle->is_java_near() || !((JavaNear*)handle)->is_locked(),
               "JavaNear in stack not allowed during GC");
  }
#endif
}

void Scheduler::gc_epilogue( void ) {
#ifndef PRODUCT
  _gc_global_head  = NULL;
#endif
  _gc_current_thread = NULL;
  // At this point, _next_runnable_thread is valid again  
  for( Thread::Raw thread = Universe::global_threadlist()->obj();
       thread.not_null(); thread = thread().global_next()) {
    thread().gc_epilogue();
  }
  Frame::set_normal_state();
#if ENABLE_ISOLATES
  // CLEANUP
  Scheduler::check_active_queues();
#endif
}


inline void Scheduler::switch_thread_slave_mode(Thread *next_thread,
                                                Thread* thread JVM_TRAPS) {
  JVM_IGNORE_TRAPS;
  if (!_slave_mode_yielding && next_thread->is_null()) {
    // All threads are deadlocked -- they are all waiting for
    // some locks with no timeout. Let's just quit from JVM_TimeSlice()
    // with a really long timeout value
    _slave_mode_timeout = 0x7fffffff;
    _slave_mode_yielding = true;

    if (JavaDebugger::is_debugger_option_on()) {
      // The debugger has suspended all threads. Come back in 100 ms ...
      _slave_mode_timeout = 100;
    }
  }

  if (_slave_mode_yielding) {
    // Return control to the main loop of the program. The VM will be
    // executed again when the main loop calls JVM_TimeSlice().
#if ENABLE_PERFORMANCE_COUNTERS
    _slave_mode_yield_start_time = Os::elapsed_counter();
#endif
    current_thread_to_primordial();
  } else if (!next_thread->equals(thread)) {
    // why will this happen?
    Thread::set_current(next_thread);
#if defined(VITA)
    /* v02.42 op 4: slave-mode nrt-cache consume point. */
    VITA_Q_REC(next_thread, 4, next_thread->priority(),
               (next_thread->pending_entries() != NULL) ? 1u : 0u);
#endif
  }
}

inline void Scheduler::switch_thread_master_mode(Thread *next_thread,
                                                 Thread* thread JVM_TRAPS) {
  if (next_thread->is_null()) {
    // all threads are suspended
    if (TraceThreadsExcessive) {
      TTY_TRACE_CR(("switch_thread: no runnable"));
    }

    if (JavaDebugger::is_debugger_option_on()) {
      // wait for a debugger command to restart one
      while (*get_next_runnable_thread() == NULL) {
        JavaDebugger::dispatch(100);
        wake_up_timed_out_sleepers(JVM_SINGLE_ARG_CHECK);
      }
      next_thread = get_next_runnable_thread();
      Thread::set_current(next_thread);
    } else {
      sleep_forever();
    }
  } else {
    if (!next_thread->equals(thread)) {
      if (TraceThreadsExcessive) {
        TTY_TRACE_CR(("switch_thread: thread 0x%x, next 0x%x", thread->obj(),
                      next_thread->obj()));
      }
      //      next_thread->set_start_time(Thread::global_tick_count());
      Thread::set_current(next_thread);
#if defined(VITA)
      /* v02.42 op 4: master-mode nrt-cache consume point. */
      VITA_Q_REC(next_thread, 4, next_thread->priority(),
                 (next_thread->pending_entries() != NULL) ? 1u : 0u);
#endif
    }
  }
}

/*
 * Called from the interpreter to load a new thread into
 * Thread::current.  This code is complicated by the needs of the java
 * debugger as well as the differences between slave mode and master
 * mode. The code tries to use the cache
 * _next_runnable_thread to determine the next thread to
 * run but may have to call get_next_runnable_thread() if it is null.  The
 * 'Normal' case of one priority (NORMAL_PRIORITY == 5) and no
 * debugger is that we should just get the next thread from the cached
 * value, set the current_thread and return.
 */

#ifdef UNDER_CE
extern "C" {
    int _quit_now = 0;
}
#endif

extern "C" void switch_thread(Thread* thread) {
  // IMPL_NOTE: threads should not be switched between
  // quick native call and call to System.quickNativeThrow(),
  // because _jvm_quick_native_exception is global and thus
  // threads switching can result in calling KNI_throwNew 
  // from quick native in one thread but throwing exception 
  // (calling System.quickNativeThrow()) in the other thread.
  if (_jvm_quick_native_exception != NULL) {
    return;
  }
  
  GUARANTEE(_jvm_quick_native_exception == NULL, 
      "Should not switch threads when throwing exception from quick native");
  
  SETUP_ERROR_CHECKER_ARG;
#if defined(VITA)
  /* v01.95: interpreter dispatch point. log21 coredump froze the VM
   * thread inside the green-thread stub cluster (PC=invoke_pending_
   * entries entry, LR=start_lightweight_thread_asm entry, fp=0) with
   * the ring showing ONLY pump marks - the cycle never reaches any
   * C++ site. If the interpreter were dispatching Java at all it must
   * pass through here every quantum: a frozen ring WITH this mark
   * present means pure stub ping-pong, WITHOUT it means a dispatch
   * freeze before the first switch. Distinguishes the two directly. */
  vita_site_mark();
  /* v01.98: denominator for the lane/pump counters. switch_thread is
   * extern "C" (outside the class), so qualify the static explicitly. */
  Scheduler::_vita_switch_count++;
  /* v02.17: who is this quantum's outgoing thread? The ring keeps the
   * last 16 dispatch decisions with their status/pending bits, so a
   * create-and-die spin (same id, alternating status) is visibly
   * different from a genuine round-robin over the 7 Java threads. */
  VITA_DISP_REC(thread);
  /* v01.99 FIX: log23 (v01.98 forensics) closed the chain: at hang the
   * ring holds ONLY switch_thread marks (16x), lanes freeze at fast=377
   * slow=0 while switch=512169 keeps climbing, and pump stops at 370 ==
   * the CE count. The spinning loop is pure Java compute: every quantum
   * it re-enters switch_thread (the only back-branch checkpoint), but it
   * NEVER passes through yield() or start0 - so every pump site hung in
   * v01.83/v01.93 (yield fast lane, thread spawn) can never fire. With
   * the pump dead, JVMSPI_CheckEvents never drains the native event
   * ring, the blocked Java thread (wait on monitor / callSerially) is
   * never woken, and the loop never sees the wakeup flag -> deadlock.
   * Fix: drive the SAME throttled pump from this checkpoint - it is the
   * one site every quantum is guaranteed to pass. pump_events_vita()
   * carries its own 50 ms wall-clock gate plus timer/readiness opens,
   * so a healthy VM pays at most one check per 50 ms, and the hang loop
   * now drains input events -> wakes the waiter -> breaks the cycle.
   * check_blocked_threads(0) re-enters JVMSPI_CheckEvents in ANI mode
   * which sleeps up to 50 ms per call, adding back-pressure that also
   * throttles the spin. */
  Scheduler::pump_events_vita();
#endif
  if (Scheduler::_yield_on_thread_switch) {
    // We need to call yield() after Scheduler::block_current_thread() is
    // called. yield() may cause JVMSPI_CheckEvent() to be called, and the
    // implementation of JVMSPI_CheckEvent() in MIDP may not be able to
    // deal with it while a native method is running, so we delay it to here.
    Scheduler::_yield_on_thread_switch = false;
    Scheduler::yield();
  }

#ifdef UNDER_CE
  {
    if (_quit_now) {
      JVM_Stop(0);
    }
  }
#endif

  // We are switching away from a thread.  If the JUST_BORN bit is
  // set, clear it and send an event.  The stack pointers are now
  // set in the thread structure.
#if defined(VITA)
  /* v02.30: count JUST_BORN switch-outs OUTSIDE the _debugger_active
   * short-circuit (log52: the frozen VM never reaches the branch inside,
   * so v02.17's counter could never move in this build). This counts
   * every switch-away from a thread that still carries the bit. */
  if (thread->status() & THREAD_JUST_BORN) {
    _vita_swjb_seen++;
  }
  /* v02.36: birth-delay A/B, round 2 - the v02.35 gate was dead.
   * THREAD_JUST_BORN is only set under _debugger_active (see Scheduler::
   * start below), so the v02.35 'status & THREAD_JUST_BORN' condition
   * never fired: log56/57/58 froze with swjb_seen=0 and birth_delay=1
   * having never delayed once. New anchor: Scheduler::start() records
   * the last started thread's id; the block right before the actual
   * context switch delays when the thread being switched TO is that
   * last-started one (i.e. its first dispatch). An int id is used, not
   * an Oop, so the anchor survives GC compaction. Gate file unchanged:
   * ux0:/data/J2ME00001/birth_delay, digit = ms. Absent/0 = v02.34
   * behaviour. Attribution (log57/58 fingerprints, same binary): dead
   * pc walked stub+0x18 vs stub+0 and set_current totals differed per
   * run (126/100/104 vs 97 in log53/54/55) - a deterministic deadlock
   * would not walk; this is a timing race, and this experiment moves
   * the switch off whatever window it collides with. */
#endif
  if (_debugger_active && thread->status() & THREAD_JUST_BORN) {
#if defined(VITA)
    /* v02.17: count JUST_BORN dispatches separately from Thread::start()
     * calls. A thread created but never dispatched (start count climbs,
     * this stays flat) points at the VM thread never reaching the
     * interpeter after spawn - which is exactly the log41 freeze site
     * (primordial_to_current_thread -> invoke_pending_entries). */
    vita_justborn_dispatch++;
    _vita_swjb_clear++;
#endif
    thread->set_status(thread->status() & ~THREAD_JUST_BORN);
    VMEvent::thread_event(thread, true);
    // more than likely we suspended all threads so null this out so we
    // do the right thing below
    _next_runnable_thread = NULL;
#if defined(VITA)
    /* v02.42: op 6 - the just-born dispatch clears the nrt cache right
     * here; log64-67 all froze somewhere after this. Knowing the last
     * ops before the spin = what the cache held when it went dark. */
    VITA_Q_REC(thread, 6, thread->priority(),
               (thread->pending_entries() != NULL) ? 1u : 0u);
#endif
  }

  Thread *next_thread;
  if (*(next_thread = (Thread *)&_next_runnable_thread) == NULL) {
    // reload cache
    next_thread = Scheduler::get_next_runnable_thread();
    if (TraceThreadsExcessive) {
      TTY_TRACE_CR(("null thread: 0x%x", next_thread->obj()));
    }
  }

  // call before actual thread swicth
#if ENABLE_WTK_PROFILER
  if (!next_thread->is_null() && !next_thread->equals(thread)) {
    jprof_record_thread_switch();
  }
#endif

#if defined(VITA)
  /* v02.36 birth-delay gate (anchor set in Scheduler::start()): delay
   * right before the FIRST dispatch of the most recently started
   * thread, after ALL scheduler bookkeeping - only the switch itself
   * moves in time. */
  {
    extern int vita_cfg_digit(const char *name, int dflt);
    if (next_thread->not_null() &&
        next_thread->id() == vita_last_started_id) {
      int dms = vita_cfg_digit("birth_delay", 0);
      if (dms > 0) {
        sceKernelDelayThread(dms * 1000);
      }
      vita_last_started_id = -1; /* fire only on the FIRST dispatch */
      /* v02.39: count the first-dispatch DECISION itself (see the
       * _vita_born_dsp note at the file head). This is the last C++
       * instruction before the master-mode switch hands the cpu to
       * the birth trampoline. */
      _vita_born_dsp++;
    }
  }
#endif

  if (Scheduler::is_slave_mode()) {
    Scheduler::switch_thread_slave_mode(next_thread, thread
                                        JVM_NO_CHECK_AT_BOTTOM);
  } else {
    Scheduler::switch_thread_master_mode(next_thread, thread
                                         JVM_NO_CHECK_AT_BOTTOM);
  }

  GUARANTEE(_current_stack_limit == Thread::current()->stack_limit(), 
            "stack limit mismatch");
}

/** Search for the next runnable thread.
    If there is already one cached in _next_runnable_thread and we are
    not yielding, just return that one.  Otherwise...
    First see if there are any threads with higher priority, if not then just
    try to return a thread on the last queue accessed, if we still fail then
    scan the queues.

    In the case of Isolates, we provide 'fair scheduling' for each isolate
    so that one isolate that has high priority threads doesn't starve
    any other isolate.  Within each
    isolate higher priority threads will tend to run before any other
    threads in that isolate.
*/

Thread *Scheduler::get_next_runnable_thread(Thread *starting_thread) {

  // Search for the highest priority thread to run
  // If starting_thread is not null then we want a thread other than that
  // thread if possible
  // Called on java stack via lightweight_thread_exit, no handles other
  // than Raw ones allowed.

  Thread::Raw thread;
  Thread::Raw last;
  int priority;
  int start_priority = ThreadObj::NUM_PRIORITY_SLOTS;
  int priority_queue_valid;
  int last_priority_queue;
  bool is_yield = (starting_thread != NULL);
  Task::Raw task;
  int tid = 0;
#if ENABLE_ISOLATES
  int task_loop_count = 2;
  bool task_quota_exceeded = false;
  int task_priority;
#endif
  bool thread_quota_exceeded = false;
  int inner_loop_count;

  // optimization; if not yielding and _next_runnable_thread != NULL
  // then we just return that value
  if (!is_yield && _next_runnable_thread != NULL) {
    // we don't charge this task since that occured when we yielded
    return (Thread *)&_next_runnable_thread;
  }
#if ENABLE_ISOLATES
  do {
    int start_tid;
    task = Task::current()->obj();
    if (!task.is_null()) {
      tid = task().task_id();
    } else {
      tid = Task::FIRST_TASK;
    }
    tid = ((tid+1) % MAX_TASKS);
    start_tid = tid;
    do {
      if (tid == 0) {
        continue;  // task 0 is system task, nothing really there to run
      }
      task = Task::get_task(tid);
      if (task.is_null()) {
        continue;
      }
      // Try to give all isolates a fair chance at running
      // Check to see if this task has used up it's quota.
      // If so go to the next task and continue the do loop.
      task_priority = Task::get_priority(tid);
      if (Task::get_num_tasks() > 2) {
        if (_task_execute_counts[task_priority] >
            sched_priority[TaskPriorityScale][task_priority]) {
          // This task has used up its 'quota', try to find another
          // another task.  Mark that at least one task has hit the limit.
          task_quota_exceeded = true;
          continue;
        }
      }
      // create local copy of variables
      priority_queue_valid = task().priority_queue_valid();
      last_priority_queue = task().last_priority_queue();
      ObjArray::Raw priority_list = task().priority_queue();
#else
      ObjArray::Raw priority_list = Universe::scheduler_priority_queues();
      priority_queue_valid = _priority_queue_valid;
      last_priority_queue = _last_priority_queue;
#endif
      

      // get the highest priority thread ready
      // we see if there are any threads of higher priority
      // than the one we last ran, if not then use that queue to start looking
      // for a thread.

      if (no_higher_priority_threads(priority_queue_valid,
                                     last_priority_queue)) {
        start_priority = last_priority_queue;
      }
      inner_loop_count = 2;
      do {
        thread_quota_exceeded = false;

#ifdef PRODUCT
        // in PRODUCT mode we optimize this so as to avoid spinning around
        // in the loop.  In non-PRODUCT build the GUARANTEEs below catch
        // any inconsistencies between priority_queue_valid and the queues
        // themselves
        if (priority_queue_valid == 0) {
          // no queues marked as having any runnable threads
          // If MVM mode, we'll check the next task
          // otherwise we return NULL
          break;
        }
#endif
      for (priority = start_priority; priority > 0; priority--) {
        last = thread = priority_list().obj_at(priority);
        if (thread.is_null()) {
          GUARANTEE(!threads_in_queue(priority, &task),
                    "No threads in queue, but bit is set");
          continue;
        }
        GUARANTEE(threads_in_queue(priority, &task),
                  "Threads in queue, but bit is clear");
#if ENABLE_ISOLATES
        if (is_yield) {
          last = thread = thread().next();
        }
#else
        if (is_yield && starting_thread->equals(thread)) {
          // we want to yield from starting_thread so start looking at
          // the next thread (it may be the same as starting thread if
          // there is only one thread on this queue).
          last = thread = starting_thread->next();
        }
#endif
        do {
          // NOTE: _debugger_active is #defined to be '0' (zero) in non
          // debugger builds.  Hence the compiler should optimize out this
          // block completely
          if (_debugger_active) {
            // The debugger complicates things.  A debugger can suspend
            // threads independent of the VM.
            // Check to see if this thread we just found is suspended
            if ((thread().status() &
                 (THREAD_DBG_SUSPENDED | THREAD_SUSPENDED))) {
              // This thread is suspended, go to the next one
              thread = thread().next();
              if (thread.equals(last)) {
                if (have_lower_priority_threads(priority_queue_valid,
                                                priority) ||
                    ENABLE_ISOLATES) {
                  // We have checked all threads at this priority,
                  // they are all suspended.  Try next task
                  break;
                } else {
                  // SVM mode:
                  // All threads are suspended so just return NULL
                  // by falling out of this while loop
                  break;
                }
              } else {
                // Check for more threads at this priority level
                continue;
              }
            }
          }
          // Have a thread, check quota for this priority
          if (_thread_execute_counts[tid][priority] > (priority * 2)) {
            // we've exceeded this threads fair share, try the next priority
            thread_quota_exceeded = true;
            break;
          }
          // move this thread to the head of the queue
          set_last_priority_queue(&thread, priority);
#if ENABLE_ISOLATES
          if (TaskFairScheduling && Task::get_num_tasks() > 2) {
            // bump execution count for this task
            _task_execute_counts[task_priority]++;
          }
#endif
          // put this thread at the head of the appropriate queue
          priority_list().obj_at_put(priority, &thread);
          _next_runnable_thread = thread.obj();
#if defined(VITA)
          /* v02.43 FIX: pass a Thread* built from the Raw handle, not
           * (Thread*)thread.obj(). BasicOop::int_field dereferences
           * _obj one more time, so casting the OopDesc* (ThreadObj*) to
           * Thread* made log68's SEL records read *(klass + id_offset)
           * as the id (0xE5902014 = an ARM instruction word from ROM).
           * (Thread*)&thread is layout-compatible (PRODUCT: BasicOop has
           * no vtable, _obj is the first word), so qp_->_obj is the
           * ThreadObj* and id() reads the real field -- same semantics
           * as the ADD/REM sites that pass their Thread* parameter. */
          VITA_Q_REC((Thread *)&thread, 3, priority,
                     (thread().pending_entries() != NULL) ? 1u : 0u);
#endif
          _thread_execute_counts[tid][priority]++;
          return (Thread *)&_next_runnable_thread;
        } while (!last.equals(thread));
      }
      // We've cycled through each priority in this task.
      // If thread_quota_exceeded is set, reset the thread counts
      if (thread_quota_exceeded) {
        // thread_quota_exceeded = false;
        reset_thread_counts(tid);
      }
      // if thread_quota_exceeded == false then we got here because
      // the debugger has suspended some otherwise runnable threads
      // Don't bother looping back in that case, we'll just end up
      // here again
      } while (thread_quota_exceeded && --inner_loop_count > 0);
#if ENABLE_ISOLATES
    } while ((tid = ((tid+1) % MAX_TASKS)) != start_tid);
    if (task_quota_exceeded) {
      task_quota_exceeded = false;
      reset_task_counts();
    } else {
      // Terminate this loop
      task_loop_count = 0;
    }
  } while (--task_loop_count > 0);
#endif
  _next_runnable_thread = NULL;
#if defined(VITA)
  _vita_gnrt_nulls++;
#endif
  return (Thread *)&_next_runnable_thread;
}


 

#if defined(VITA)
/* v01.95: extern "C" read-only accessors for the launcher watchdog's
 * live-globals snapshot (vita_wd_snapshot in OS_vita.cpp). OS_vita's
 * generated include chain has Thread.hpp but not Scheduler.hpp, so the
 * snapshot reaches these counters through C linkage instead of the
 * class - see the v01.95 note in OS_vita.cpp. */
extern "C" int vita_sched_async_count(void) {
  return Scheduler::get_async_count();
}
extern "C" int vita_sched_active_count(void) {
  return Scheduler::active_count();
}
extern "C" int vita_sched_timer_ticked(void) {
  return (int)Scheduler::get_timer_has_ticked();
}
extern "C" int vita_sched_ev_readiness(void) {
  return Scheduler::get_estimated_event_readiness();
}
/* v01.98: lane/pump/switch counters for vita_wd_snapshot. */
extern "C" unsigned int vita_sched_yield_fast(void) {
  return Scheduler::get_vita_yield_fast_count();
}
extern "C" unsigned int vita_sched_yield_slow(void) {
  return Scheduler::get_vita_yield_slow_count();
}
extern "C" unsigned int vita_sched_pump_runs(void) {
  return Scheduler::get_vita_pump_runs();
}
extern "C" unsigned int vita_sched_switch_count(void) {
  return Scheduler::get_vita_switch_count();
}
/* v02.00: sleep_forever idle-loop readouts (state declared above
 * sleep_forever, before first use). */
extern "C" unsigned int vita_sched_sf_entries(void) {
  return _vita_sf_entries;
}
extern "C" unsigned int vita_sched_sf_spins(void) {
  return _vita_sf_spins;
}
extern "C" unsigned int vita_sched_sf_last_ms(void) {
  return _vita_sf_last_ms;
}
/* v02.01: dispatch readouts (state declared at file head, before
 * get_next_runnable_thread uses them). */
extern "C" unsigned int vita_sched_gnrt_hits(void) {
  return _vita_gnrt_hits;
}
extern "C" unsigned int vita_sched_gnrt_nulls(void) {
  return _vita_gnrt_nulls;
}
/* v02.05: timer-tick chain readouts for vita_wd_snapshot (see the
 * log29 note in OS_vita.cpp). _rt_timer_ticks is a plain global
 * (GlobalDefinitions.hpp:1919, defined in InterpreterSkeleton.cpp),
 * incremented by Thread::set_timer_tick() from the ticker thread and
 * zeroed by clear_timer_tick() at the tail of Thread::timer_tick.
 * The counter of timer_tick entries lives in Thread.cpp next to the
 * probe call. */
extern "C" unsigned int vita_sched_rt_ticks(void) {
  return (unsigned int)_rt_timer_ticks;
}
extern "C" unsigned int vita_sched_ttk_count(void) {
  return _vita_ttk_count;
}
/* v02.11: tick-stage readouts (state in Thread.cpp, see the probe note
 * there). A frozen stage with hits[N] == hits[N-1] names the step of
 * Thread::timer_tick() the VM thread is wedged in. */
extern "C" unsigned int vita_sched_ttk_stage(void) {
  return _vita_ttk_stage;
}
extern "C" unsigned int vita_sched_ttk_hits(int i) {
  return (i >= 0 && i < 8) ? _vita_ttk_hits[i] : 0u;
}

/* v02.17: next-runnable cache readouts. _next_runnable_thread is the
 * scheduler's cached answer to "who runs next"; it is the value
 * switch_thread() consumes BEFORE calling get_next_runnable_thread().
 * A frozen non-NULL cache with a dead/terminated object is a different
 * failure from a frozen NULL cache (which means the last dispatch had
 * to walk the priority queues). */
extern "C" unsigned int vita_sched_nrt_cell(void) {
  return (unsigned int)(unsigned long)&_next_runnable_thread;
}
extern "C" unsigned int vita_sched_nrt_obj(void) {
  return (unsigned int)(unsigned long)_next_runnable_thread;
}
extern "C" unsigned int vita_sched_justborn(void) {
  return vita_justborn_dispatch;
}
/* v02.30: birth-stub visibility counters for vita_wd_snapshot (see the
 * declaration note at the file head). */
extern "C" unsigned int vita_sched_swjb_seen(void) {
  return _vita_swjb_seen;
}
extern "C" unsigned int vita_sched_swjb_clear(void) {
  return _vita_swjb_clear;
}
extern "C" unsigned int vita_sched_pe_enter(void) {
  return _vita_pe_enter;
}
/* v02.30: master-loop liveness counters for vita_wd_snapshot (see the
 * file-head note for the three-way readout). */
extern "C" unsigned int vita_sched_ts_enter(void) {
  return _vita_ts_enter;
}
extern "C" unsigned int vita_sched_ts_gadget(void) {
  return _vita_ts_gadget;
}
/* v02.39: first-dispatch decision count for vita_wd_snapshot (see the
 * _vita_born_dsp note at the file head for the three-way readout). */
extern "C" unsigned int vita_sched_born_dsp(void) {
  return _vita_born_dsp;
}

extern "C" int vita_sched_nrt_info(char *out, int out_size) {
  /* v02.18 FIX (the log42 probe self-wound): this used to build a
   * Thread::Raw handle and call t().priority() - which chains through
   * thread_obj (+32) into the object header, and wait_obj() the same
   * way. The watchdog thread has NO GC frame; when the cached object
   * was mid-teardown the chained deref data-aborted INSIDE the probe
   * (vita_sched_nrt_info+0x38, ldr r7,[r3,#4] with r3 = stale
   * thread_obj), killing the process and eating sample 2 + the
   * dispatch ring + the java-stack dump. Rewrite: raw word reads at
   * the ThreadDesc offsets the class accessors themselves use
   * (Thread::*_offset() is FIELD_OFFSET(ThreadDesc, _field) - no
   * handle, no chained deref), each guarded by a range check. A torn
   * read is fine: the thread under the freeze is not moving. */
  int pos = 0;
  int n;
  unsigned int obj = (unsigned int)(unsigned long)_next_runnable_thread;
  if (obj == 0) {
    n = snprintf(out + pos, out_size - pos, "  nrt: (null)\n");
    if (n > 0) pos += n;
    return pos;
  }
  if (obj < 0x81000000u || obj >= 0xa0000000u) {
    n = snprintf(out + pos, out_size - pos,
                 "  nrt obj=0x%08x out of range (skip)\n", obj);
    if (n > 0) pos += n;
    return pos;
  }
  {
    /* volatile: the VM thread may in principle still be writing these
     * words; we accept a torn snapshot over a chained deref. */
    volatile unsigned int *td = (volatile unsigned int *)obj;
    unsigned int id    = td[Thread::id_offset() / 4];
    unsigned int tobj  = td[Thread::thread_obj_offset() / 4];
    unsigned int wait  = td[Thread::wait_obj_offset() / 4];
    unsigned int pe    = td[Thread::pending_entries_offset() / 4];
    unsigned int jsp   = td[Thread::stack_pointer_offset() / 4];
    unsigned int lim   = td[Thread::stack_limit_offset() / 4];
    unsigned int st    = td[Thread::status_offset() / 4];
    (void)lim;
    /* priority lives on the java.lang.Thread mirror (thread_obj ->
     * ThreadObj::priority()); the log42 crash was EXACTLY this chain,
     * so print the raw pointer and let the reader decide - one fewer
     * deref on the watchdog's head. */
    n = snprintf(out + pos, out_size - pos,
                 "  nrt obj=0x%08x id=%u st=%08x%s%s%s jsp=0x%08x "
                 "wait=0x%08x pe=0x%08x tobj=0x%08x\n",
                 obj, id, st,
                 (st & THREAD_TERMINATING) ? " TERM" : "",
                 (st & THREAD_JUST_BORN)   ? " NEW"  : "",
                 (st & THREAD_DEAD)        ? " DEAD" : "",
                 jsp, wait, pe, tobj);
    if (n > 0 && pos + n < out_size) pos += n;
  }
  return pos;
}

/* v02.17: dispatch ring dump - newest first. */
extern "C" int vita_sched_dump_dispatch(char *out, int out_size) {
  int pos = 0;
  int n;
  unsigned int seq = _vita_disp_seq;
  unsigned int i;
  n = snprintf(out + pos, out_size - pos,
               "dispatch ring (last %d, newest first):\n", VITA_DISP_RING / 2);
  if (n > 0) pos += n;
  for (i = 0; i < VITA_DISP_RING && i < (VITA_DISP_RING / 2); i++) {
    unsigned int idx = (seq - 1u - i) & (VITA_DISP_RING - 1u);
    n = snprintf(out + pos, out_size - pos,
                 "  [%2u] id=%u pri=%u st=%08x%s%s%s%s pe=%u\n", i,
                 _vita_disp_ring[idx][0], _vita_disp_ring[idx][1],
                 _vita_disp_ring[idx][2],
                 (_vita_disp_ring[idx][2] & THREAD_TERMINATING) ? " TERM":"",
                 (_vita_disp_ring[idx][2] & THREAD_JUST_BORN)   ? " NEW":"",
                 (_vita_disp_ring[idx][2] & THREAD_DEAD)        ? " DEAD":"",
                 (_vita_disp_ring[idx][2] & THREAD_NOT_ACTIVE_MASK) ? " INA":"",
                 _vita_disp_ring[idx][3]);
    if (n > 0 && pos + n < out_size) pos += n; else break;
  }
  return pos;
}

/* v02.42: scheduler queue-op ring dump. The CAUSE-side counterpart of
 * the dispatch ring above: the last C++ queue operations before the
 * pure-asm livelock loop went dark (log67: switch/lw_exit/ts_gadget
 * all frozen => the loop passes no C++ checkpoint, so these entries
 * are frozen at the moment the spin started - the "who did it" record
 * log67 could not produce). Op legend: 1=add_to_active 2=remove_from_
 * active 3=selected(get_next_runnable_thread head-rotate) 4=nrt-cache
 * -write(switch_thread) 5=set_priority-move 6=justborn-nrt-clear.
 * The pe column answers the case-critical question directly: any op
 * that re-queues or re-selects a thread with pe=0 names the culprit
 * site; the seq delta between the last entry and the hang says how
 * long the system spun before the watchdog fired. */
extern "C" int vita_sched_dump_qring(char *out, int out_size) {
  static const char *opname[7] = {
    "?", "ADD", "REM", "SEL", "NRT", "PRI", "JBC"
  };
  int pos = 0;
  int n;
  unsigned int seq = _vita_q_seq;
  unsigned int i;
  n = snprintf(out + pos, out_size - pos,
               "queue-op ring (last %d, newest first, seq=%u):\n",
               VITA_Q_RING / 2, seq);
  if (n > 0) pos += n;
  for (i = 0; i < VITA_Q_RING && i < (VITA_Q_RING / 2); i++) {
    unsigned int idx = (seq - 1u - i) & (VITA_Q_RING - 1u);
    unsigned int op = _vita_q_ring[idx][1];
    n = snprintf(out + pos, out_size - pos,
                 "  [%2u] id=%u %-3s pri=%u pe=%u from=%08x\n", i,
                 _vita_q_ring[idx][0],
                 (op <= 6) ? opname[op] : "?",
                 _vita_q_ring[idx][2], _vita_q_ring[idx][3],
                 _vita_q_ring[idx][4]);
    if (n > 0 && pos + n < out_size) pos += n; else break;
  }
  return pos;
}

/* v02.17: top of the CURRENT thread's Java stack, word by word. The
 * coredump only ever contains NATIVE (pthread) stacks, so the Java
 * bootstrap frame described in InterpreterRuntime_arm.cpp:setup_stack_asm
 * (frame pointer 0, then the sentinel word, ..., TestCompiler)
 * has never been observable. If the top words still match that template
 * the thread never entered Java at all; any other shape says how far it
 * got. 16 words is enough to cover the whole template.
 * v02.40: the sentinel word is now vita_landing_pad (fresh-compiled
 * landing pad in InterpreterRuntime_arm.cpp), not
 * start_lightweight_thread_asm - the pad forwards to the trampoline
 * after bumping the v240 counters. */
extern "C" int vita_sched_dump_stack(char *out, int out_size) {
  int pos = 0;
  int n;
  Thread *ct = Thread::current();
  n = snprintf(out + pos, out_size - pos, "java-stack-top (current):\n");
  if (n > 0) pos += n;
  if (ct == NULL) {
    n = snprintf(out + pos, out_size - pos, "  (no current thread)\n");
    if (n > 0) pos += n;
    return pos;
  }
  {
    unsigned int jsp = (unsigned int)ct->stack_pointer();
    unsigned int i;
    if (jsp < 0x81000000 || jsp >= 0xa0000000) {
      n = snprintf(out + pos, out_size - pos,
                   "  jsp=0x%08x out of range (skip)\n", jsp);
      if (n > 0) pos += n;
      return pos;
    }
    n = snprintf(out + pos, out_size - pos,
                 "  stack_pointer()=0x%08x stack_limit()=0x%08x\n", jsp,
                 (unsigned int)ct->stack_limit());
    if (n > 0) pos += n;
    for (i = 0; i < 16; i++) {
      unsigned int w = *(volatile unsigned int *)(jsp + i * 4);
      n = snprintf(out + pos, out_size - pos, "  [jsp+%02u] 0x%08x\n",
                   i * 4, w);
      if (n > 0 && pos + n < out_size) pos += n; else break;
    }
  }
  return pos;
}

/* v01.98: log22 left the central question open - active_count=5 but the
 * VM does no useful work (pure stub ping-pong). WHO are the 5 threads
 * and what state is each in? get_next_runnable_thread() only walks the
 * priority queues and never re-checks status, so a dead-but-still-
 * queued thread stays "runnable" forever - this dump makes that visible
 * by walking BOTH structures: the global thread list (ground truth of
 * what exists) and the priority queues (the scheduler's opinion of what
 * is runnable), printing id/priority/status for each. The output is a
 * fixed-size text buffer handed back through the os_vita sceIo writer;
 * watchdog context, so Raw handles only and no allocation. */
#define VITA_SCHED_DUMP_MAX 48
extern "C" int vita_sched_dump_threads(char *out, int out_size) {
  /* one line per thread + queue entries; both loops below are bounded
   * by VITA_SCHED_DUMP_MAX lines each, plus header lines */
  int pos = 0;
  int n;

  n = snprintf(out + pos, out_size - pos,
               "threads(active-list):\n");
  if (n > 0) pos += n;

  /* global list: every Thread object that exists. ForAllThreads filters
   * on last_java_frame_exists() which is wrong for a corpse dump, so
   * walk the raw global_next chain instead.
   * v02.00: also print WHAT each thread is waiting on. _wait_obj is
   * the monitor object (null when queued-runnable), _wakeup_time is
   * the absolute deadline (0 = infinite wait, no timeout). This pair
   * answers "why is the queue empty": the all-blocked window is
   * exactly when every thread has wait_obj set and none has timed
   * out yet. */
  {
    int count = 0;
    for (Thread::Raw t = Universe::global_threadlist()->obj();
         t.not_null() && count < VITA_SCHED_DUMP_MAX;
         t = t().global_next(), count++) {
      /* v02.18 FIX: status()/wait_obj()/wakeup_time() are plain word
       * reads at FIELD_OFFSET(ThreadDesc, _field) - safe on the raw
       * handle. But priority() chains through thread_obj exactly the
       * way the log42 nrt probe crashed, so drop it here too and keep
       * the raw pointer instead. */
      jint st = t().status();
      unsigned int wo =
          (unsigned int)(unsigned long)t().wait_obj();
      jlong wt = t().wakeup_time();
      n = snprintf(out + pos, out_size - pos,
                   "  t%p id=%d st=%08x%s%s%s wait=0x%08x %s\n",
                   (void*)t.obj(), t().id(), (unsigned int)st,
                   (st & THREAD_TERMINATING) ? " TERM" : "",
                   (st & THREAD_JUST_BORN)   ? " NEW"  : "",
                   (st & THREAD_DEAD)        ? " DEAD" : "",
                   wo,
                   (wt == 0) ? "no-timeout" : "timed");
      if (n > 0 && pos + n < out_size) pos += n; else break;
    }
    if (count >= VITA_SCHED_DUMP_MAX) {
      n = snprintf(out + pos, out_size - pos, "  ...(truncated)\n");
      if (n > 0) pos += n;
    }
  }

  n = snprintf(out + pos, out_size - pos,
               "threads(priority-queues):\n");
  if (n > 0) pos += n;

  /* priority queues: the scheduler's runnable set. NUM_PRIORITY_SLOTS
   * is 10 (ThreadObj.hpp:106); slot 0 unused, iterate 1..9.
   * v02.00 FIX: with ENABLE_ISOLATES (this build, vita_arm.cfg) the
   * queues live in Task(FIRST_TASK)._priority_queue - see
   * Universe::setup_thread_priority_list(). The old read of
   * Universe::scheduler_priority_queues() was the never-assigned
   * non-isolates slot, so it printed "(null)" unconditionally: a
   * forensics tool bug, not evidence of an empty queue. Read the real
   * per-task queue instead. Task::get_task is a ReturnOop pure read
   * (GUARANTEE compiles out in PRODUCT), safe in watchdog context. */
  {
    ObjArray::Raw priority_list;
#if ENABLE_ISOLATES
    Task::Raw task = Task::get_task(Task::FIRST_TASK);
    priority_list = task().priority_queue();
#else
    priority_list = Universe::scheduler_priority_queues();
#endif
    if (priority_list.is_null()) {
      n = snprintf(out + pos, out_size - pos, "  (null)\n");
      if (n > 0) pos += n;
    } else {
      for (int priority = 1; priority < ThreadObj::NUM_PRIORITY_SLOTS;
           priority++) {
        Thread::Raw t = priority_list().obj_at(priority);
        if (t.is_null()) continue;
        n = snprintf(out + pos, out_size - pos,
                     "  q%d:", priority);
        if (n > 0) pos += n;
        int count = 0;
        /* v02.11: the priority queues are CIRCULAR singly-linked lists,
         * so a fixed-count walk reprints the same 1-2 entries until the
         * output buffer fills. log35 read q5 as ~32 id=4/id=3 pairs and
         * looked like a 2-element-ring anomaly: it is a dump-walk
         * artifact (the buffer ran out first, which also silently
         * dropped every later queue). Stop when the walk returns to the
         * head, and say which bound stopped it. */
        Thread::Raw head = t;
        void *head_obj = (void*)head.obj();
        while (!t.is_null() && count < VITA_SCHED_DUMP_MAX) {
          jint st = t().status();
          n = snprintf(out + pos, out_size - pos,
                       " t%p(id=%d,st=%08x)",
                       (void*)t.obj(), t().id(), (unsigned int)st);
          if (n > 0 && pos + n < out_size) {
            pos += n;
          } else {
            if (pos + 64 < out_size) {
              n = snprintf(out + pos, out_size - pos, " (buffer full)");
              if (n > 0) pos += n;
            }
            t = Thread::Raw();
            break;
          }
          t = t().next();
          count++;
          if (!t.is_null() && (void*)t.obj() == head_obj) {
            if (pos + 64 < out_size) {
              n = snprintf(out + pos, out_size - pos, " CYCLE(%d)", count);
              if (n > 0) pos += n;
            }
            break;
          }
        }
        if (!t.is_null() && count >= VITA_SCHED_DUMP_MAX &&
            pos + 64 < out_size) {
          n = snprintf(out + pos, out_size - pos, " (max)");
          if (n > 0) pos += n;
        }
        n = snprintf(out + pos, out_size - pos, "\n");
        if (n > 0) pos += n;
      }
    }
  }
  return pos;
}

/* v02.46: per-thread SYMBOLIC Java stack walk, for the ring trap and
 * the watchdog. Why hand-rolled: Thread::trace_stack and every
 * *Frame::print_on live inside #ifndef PRODUCT (Frame.cpp:1013-1623)
 * and pss() needs ENABLE_PRODUCT_PRINT_STACK (0 in this build), so the
 * library has no stack printer at all. This walk uses only
 * PRODUCT-surviving primitives (all verified in the shipped ELF):
 * Frame::init (T), is_entry_frame/caller_is/is_first_frame (inline in
 * Frame.hpp), JavaFrame::method (T), Method::name/signature (T, in
 * _MergedSrc004), holder() (inline -> Universe::class_from_id, weak
 * symbol in lib), JavaClass::name (inline), Symbol base_address/
 * length (inline), bci_with_flags (T). Raw UTF8 signatures (the pretty
 * decoder is not in PRODUCT) - "(I)V" reads fine anyway.
 * Constraints honored: Raw handles only, FIELD_OFFSET reads, no
 * allocation, no tty, no locks - safe from the trap's private stack
 * and from the watchdog thread. fp is range-checked against the
 * thread's own stack before EVERY dereference: the livelock premise
 * is a corrupted jsp, so a garbage fp must stop the walk, not crash
 * the dumper. Single-task assumption: no-arg holder() resolves class
 * ids through the current task's class list; this MIDP build runs one
 * task and the trap context IS the spinning thread's task. */
static int vita_jstack_append_sym(char *out, int pos, int out_size,
                                  const Symbol::Raw &s, int maxlen) {
  if (s.is_null()) {
    if (pos + 6 < out_size) {
      out[pos++] = '('; out[pos++] = 'n'; out[pos++] = 'u';
      out[pos++] = 'l'; out[pos++] = 'l'; out[pos++] = ')';
    }
    return pos;
  }
  int len = s().length();
  if (len > maxlen) len = maxlen;
  const char *p = s().base_address();
  for (int i = 0; i < len && pos + 1 < out_size; i++) {
    char c = p[i];
    out[pos++] = (c >= 32 && c < 127) ? c : '?';
  }
  return pos;
}

extern "C" int vita_sched_dump_jstack(char *out, int out_size) {
  int pos = 0;
  int n;
  n = snprintf(out + pos, out_size - pos, "java stacks (symbolic):\n");
  if (n > 0) pos += n;
  {
    int tcount = 0;
    for (Thread::Raw t = Universe::global_threadlist()->obj();
         t.not_null() && tcount < 8;
         t = t().global_next(), tcount++) {
      /* operator() hands back a Thread& view of the Raw wrapper -
       * no copy, no handle-list side effects (PRODUCT pops are
       * no-ops anyway). */
      Thread &th = t();
      address lo = th.stack_limit();
      address hi = th.stack_base();
      if ((uintptr_t)lo > (uintptr_t)hi) {
        address tmp = lo; lo = hi; hi = tmp;
      }
      n = snprintf(out + pos, out_size - pos,
                   "thread id=%d obj=%p stack=[%08x,%08x]\n",
                   th.id(), (void*)th.obj(),
                   (unsigned int)(uintptr_t)lo,
                   (unsigned int)(uintptr_t)hi);
      if (n > 0 && pos + n < out_size) pos += n; else break;

      /* GUARANTEE in Frame::init compiles out in PRODUCT, so a thread
       * that never entered Java would set fp=NULL here - handled by
       * the range check below like any other bad fp. */
      Frame fr(&th);
      if (fr.fp() == 0) {
        n = snprintf(out + pos, out_size - pos, "  (fp=0, not started)\n");
        if (n > 0 && pos + n < out_size) pos += n; else break;
        continue;
      }
      int fidx = 0;
      while (fidx < 32) {
        address fp = fr.fp();
        if ((uintptr_t)fp < (uintptr_t)lo ||
            (uintptr_t)fp > (uintptr_t)hi) {
          n = snprintf(out + pos, out_size - pos,
                       "  stop: fp=%08x out of stack range\n",
                       (unsigned int)(uintptr_t)fp);
          if (n > 0 && pos + n < out_size) pos += n;
          break;
        }
        if (fr.is_entry_frame()) {
          if (fr.as_EntryFrame().is_first_frame()) {
            n = snprintf(out + pos, out_size - pos, "  [first frame]\n");
            if (n > 0 && pos + n < out_size) pos += n;
            break;
          }
          if (pos + 24 < out_size) {
            out[pos++] = ' '; out[pos++] = ' '; out[pos++] = '[';
            out[pos++] = 'e'; out[pos++] = 'n'; out[pos++] = 't';
            out[pos++] = 'r'; out[pos++] = 'y'; out[pos++] = ']';
            out[pos++] = '\n';
          }
          fr.as_EntryFrame().caller_is(fr);
        } else {
          Method::Raw m = fr.as_JavaFrame().method();
          n = snprintf(out + pos, out_size - pos, "  [%2d] ", fidx);
          if (n > 0 && pos + n < out_size) pos += n; else { pos = out_size - 1; break; }
          if (m.is_null()) {
            n = snprintf(out + pos, out_size - pos,
                         "(null method) fp=%08x\n",
                         (unsigned int)(uintptr_t)fp);
            if (n > 0 && pos + n < out_size) pos += n;
            break;
          }
          InstanceClass::Raw h = m().holder();
          Symbol::Raw cn;
          if (!h.is_null()) cn = h().name();
          Symbol::Raw mn = m().name();
          Symbol::Raw sig = m().signature();
          pos = vita_jstack_append_sym(out, pos, out_size, cn, 64);
          if (pos + 1 < out_size) out[pos++] = '.';
          pos = vita_jstack_append_sym(out, pos, out_size, mn, 48);
          pos = vita_jstack_append_sym(out, pos, out_size, sig, 48);
          n = snprintf(out + pos, out_size - pos, " bci=%d fp=%08x\n",
                       (int)fr.as_JavaFrame().bci(),
                       (unsigned int)(uintptr_t)fp);
          if (n > 0 && pos + n < out_size) pos += n; else { pos = out_size - 1; break; }
          fr.as_JavaFrame().caller_is(fr);
        }
        fidx++;
      }
      if (fidx >= 32 && pos + 24 < out_size) {
        n = snprintf(out + pos, out_size - pos, "  (max frames)\n");
        if (n > 0) pos += n;
      }
    }
  }
  return pos;
}
#endif
