/*
 * OsMisc_vita.cpp: PS Vita miscellaneous OS functions
 *
 * Based on OsMisc_linux.cpp, adapted for PS Vita.
 */

#include "incls/_precompiled.incl"
#include "incls/_OsMisc_vita.cpp.incl"

#ifdef __cplusplus
extern "C" {
#endif

const JvmPathChar *OsMisc_get_classpath() {
  // Vita does not have environment variables in the traditional sense.
  // Classpath is set via command line or hardcoded.
  static JvmPathChar* classpath = NULL;
  if (classpath == NULL) {
    // Try to get from environment (newlib may support this)
    char *ascii;
    if ((ascii = getenv("CLASSPATH")) != NULL) {
      int len = strlen(ascii);
      classpath = (JvmPathChar*)malloc((len+1) * sizeof(JvmPathChar));
      for (int i=0; i<len; i++) {
        classpath[i] = (JvmPathChar)ascii[i];
      }
      classpath[len] = 0;
    }
  }

  return classpath;
}

#if !defined(PRODUCT) || USE_DEBUG_PRINTING

const char *OsMisc_jlong_format_specifier() {
  return "%lld";
}

const char *OsMisc_julong_format_specifier() {
  return "%llu";
}

#endif

// Vita does not support mprotect-based page protection
// (no mmap, no signals for VM exceptions)

// Flush instruction cache after JIT code generation
// On ARM, we need to flush both data and instruction cache
//
// v04.47: __builtin___clear_cache is EMPTY on this target. v03.71 established
// it ("-O1 and above emit no code at all"), and the v04.47 disassembly of the
// shipped ELF confirms it: OsMisc_flush_icache is two instructions long and the
// next function starts immediately after. A hand-written `mcr p15, ...` is not
// an option either - cache maintenance is PL1 on ARMv7-A and this code runs at
// PL0.
//
// The one flush that works from user mode is sceKernelSyncVMDomain on the VM
// block that backs the Java heap, which is where the JIT's compiler area lives
// ([heapmem] chunk0 BACKED BY VM-DOMAIN MEMORY). JVMCompiledMethod::flush_icache()
// tail-calls this function once per compiled method, so the syscall cost is paid
// per method, not per instruction.
//
// log187 is what the missing flush looked like: JIT enabled (heap_mb <= 15 kept
// UseCompiler on), pc inside the heap block, runMidlet() never returned.
extern "C" int vita_vm_flush_icache(void *start, int size);

void OsMisc_flush_icache(address start, int size) {
  if (vita_vm_flush_icache((void *)start, size)) {
    return;
  }
  __builtin___clear_cache(start, start + size);
}

#ifdef __cplusplus
}
#endif
