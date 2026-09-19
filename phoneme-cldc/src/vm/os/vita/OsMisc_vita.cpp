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
void OsMisc_flush_icache(address start, int size) {
  // Use ARM barrier instructions to ensure cache coherence
  // __builtin___clear_cache is available on GCC for ARM
  __builtin___clear_cache(start, start + size);
}

#ifdef __cplusplus
}
#endif
