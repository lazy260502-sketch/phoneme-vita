/*
 * vita_jitmem.h - executable memory for the JIT (v03.71, P1-1 groundwork).
 *
 * WHY THIS EXISTS
 * ---------------
 * The CLDC-HI JIT keeps its generated code in a "compiler area" that
 * ObjectHeap carves out of the managed heap. On the Vita that heap is USER_RW
 * memory with the execute-never attribute set, so the first `bx` into
 * generated code takes a prefetch abort:
 *
 *     pc == lr == 0x83a5f52c   THREAD_REG_INFO +0x170 = 0x0d  (permission)
 *                              THREAD_REG_INFO +0x174 = FAR == pc
 *     -> WnR clear, FAR == pc  =>  instruction-fetch XN fault
 *
 * Two further obstacles were recorded with it:
 *   (a) OsMisc_flush_icache() came out as a bare `bx lr`. Root-caused in
 *       v03.71: the VitaSDK GCC folds __builtin___clear_cache() to nothing
 *       (-O0 emits a `nop {0}` placeholder, -O1 and above emit no code at
 *       all), so the C source was never wrong - the builtin is a no-op for
 *       this target. A hand-written `mcr p15, ...` sequence is not an option
 *       either: cache maintenance is PL1 on ARMv7-A and the app runs at PL0.
 *   (b) no user-mode cache API exists. Every Icache/Dcache export in VitaSDK
 *       is ksce*ForDriver / ForKernel, i.e. kernel plugins only.
 *
 * THE API THAT DOES EXIST (psp2/kernel/sysmem.h):
 *   sceKernelAllocMemBlockForVM(name, size)  size <= 16 MiB; the block is
 *                                            "VM domain" memory
 *   sceKernelSyncVMDomain(uid, addr, size)   flushes the VM caches for a range
 *   sceKernelOpenVMDomain()                  makes ALL VM-domain blocks
 *                                            executable
 *   sceKernelCloseVMDomain()                 makes them non-executable again
 *
 * So the pair (OpenVMDomain, SyncVMDomain) is the missing flush_icache, and a
 * VM block is the missing XN bypass. Neither has ever been used by this port,
 * and neither can be taken on faith - "rc=0 does not mean it took effect" is
 * the v03.06 lesson, and here the failure mode is a hard crash, not a wrong
 * number. Hence a probe that runs before any JIT work is attempted.
 */

#ifndef VITA_JITMEM_H
#define VITA_JITMEM_H

/* Runs the executable-memory proof, but only when
 * ux0:/data/J2ME00001/jitprobe.cfg exists - the probe executes freshly
 * written code, so on a firmware that does not honour the VM domain it takes
 * the whole app down with a prefetch abort. Opt-in, never implicit.
 *
 * Writes one "[JITPROBE]" section to crumb.log and a one-line verdict to
 * perf.log. Returns 1 if dynamically generated code ran, 0 if not. */
int vita_jit_probe(void);

/* Result of the last probe: 0 = not run, 1 = worked, -1 = failed. */
int vita_jit_probe_status(void);

/* ------------------------------------------------------------------------
 * v04.17: the JIT code area proper.
 *
 * log181 settled the API question - on real hardware the VM domain gives
 * memory that is writable, executable, mappable across the whole block, and
 * flushes with sceKernelSyncVMDomain:
 *     VM store survived: e3a0002a e12fff1e
 *     VM block end = 5a5a5a5a
 *     VM SyncVMDomain rc=0x00000000
 *     VM generated code returned 42 (want 42)
 * but the JIT still crashed, with pc == lr inside the Java heap and fault
 * status 0x0d (permission fault on FETCH) - because ObjectHeap still generates
 * into its own compiler area, which is managed heap memory and execute-never.
 *
 * vita_jit_code_area_acquire() allocates the VM block once and leaves the
 * domain OPEN for the life of the process (a JIT patches code continuously, so
 * open/close per patch is not an option). vita_jit_code_area_ready() reports
 * whether that backing store exists - it does NOT yet mean the VM uses it.
 * ------------------------------------------------------------------------ */
int vita_jit_code_area_acquire(void);   /* 1 = block ready, 0 = unavailable */
int vita_jit_code_area_ready(void);
void *vita_jit_code_area_base(void);
unsigned int vita_jit_code_area_size(void);
/* The missing flush_icache, now on a working foundation. No-op if not ready. */
void vita_jit_flush_icache(void *addr, unsigned int len);

#endif
