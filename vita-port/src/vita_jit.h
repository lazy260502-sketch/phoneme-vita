#ifndef VITA_JIT_H
#define VITA_JIT_H

#ifdef __cplusplus
extern "C" {
#endif

/* Called at the start of every VM round with that round's JIT policy
 * (0 = interpreter, 1/2 = JIT). Captures the VM's compiler defaults on the
 * first call and restores them for policy != 0 (see vita_jit.c). */
void vita_jit_round_begin(int round, int policy);

/* Called after runMidlet() returns: logs compile counters and the memory
 * permissions of the block holding the first generated code. */
void vita_jit_round_end(int round);

#ifdef __cplusplus
}
#endif

#endif
