/*
 * OsMemory_vita.cpp: PS Vita memory management
 *
 * Vita has no mmap, so we use malloc/free from newlib.
 * Adjustable memory chunks are disabled (SUPPORTS_ADJUSTABLE_MEMORY_CHUNK=0).
 */

#include "incls/_precompiled.incl"
#include "incls/_OsMemory_vita.cpp.incl"

#if !ENABLE_PCSL

#ifdef __cplusplus
extern "C" {
#endif

void *OsMemory_allocate(size_t size) {
  return jvm_malloc(size);
}

void OsMemory_free(void *p) {
  jvm_free(p);
}

#if SUPPORTS_ADJUSTABLE_MEMORY_CHUNK

// Vita does not support mmap, so adjustable chunks are not available.
// If SUPPORTS_ADJUSTABLE_MEMORY_CHUNK is enabled, we provide stub
// implementations that use malloc.

void init_jvm_chunk_manager() {
  // No initialization needed
}

address OsMemory_allocate_chunk(size_t initial_size,
                                size_t max_size, size_t alignment)
{
  // Use malloc for chunk allocation on Vita
  void *p = jvm_malloc(max_size);
  if (p == NULL) {
    return NULL;
  }
  return (address)p;
}

size_t OsMemory_adjust_chunk(address chunk_ptr, size_t new_committed_size) {
  // Simple implementation: always succeed, return new size
  return new_committed_size;
}

void OsMemory_free_chunk(address chunk_ptr) {
  jvm_free((void*)chunk_ptr);
}

#endif // SUPPORTS_ADJUSTABLE_MEMORY_CHUNK

#ifdef __cplusplus
}
#endif

#endif // !ENABLE_PCSL
