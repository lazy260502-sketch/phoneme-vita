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

/*
 * OsMemory.cpp:
 */

#include "incls/_precompiled.incl"
#include "incls/_OsMemory.cpp.incl"

#if ENABLE_PCSL
extern "C" {
#include <pcsl_memory.h>
}

#ifndef UNDER_CE
void *OsMemory_allocate(size_t size) {
  return pcsl_mem_malloc(size);
}

void OsMemory_free(void *p) {
  pcsl_mem_free(p);
}
#endif

address OsMemory_allocate_chunk(size_t initial_size,
                                size_t max_size, size_t alignment) {
  return (address)pcsl_mem_allocate_chunk((unsigned int)initial_size,
                                          (unsigned int)max_size, 
                                          (unsigned int)alignment);
}

size_t OsMemory_adjust_chunk(address chunk_ptr, size_t new_size) {
  return (size_t)
      pcsl_mem_adjust_chunk((void *)chunk_ptr, (unsigned int)new_size);
}

void OsMemory_free_chunk(address chunk_ptr) {
  pcsl_mem_free_chunk((void *)chunk_ptr);
}
#endif // ENABLE_PCSL

#if !ENABLE_PCSL && !SUPPORTS_ADJUSTABLE_MEMORY_CHUNK

// Here's a default implementation of OsMemory_allocate_chunk
// and OsMemory_adjust_chunk, based on OsMemory_malloc_bytes
//
// To use an alternative (real) implementation for your
// OS, define SUPPORTS_ADJUSTABLE_MEMORY_CHUNK=1 in your
// OS_<os_family>.hpp file

#define MAX_CHUNKS 2
static address chunks[MAX_CHUNKS] = {NULL, NULL};
static address chunks_orig[MAX_CHUNKS] = {NULL, NULL};
static size_t max_chunk_size[MAX_CHUNKS];
static size_t cur_chunk_size[MAX_CHUNKS];

#if defined(VITA)
/* ------------------------------------------------------------------------
 * v04.18: THE JAVA HEAP, ALLOCATED FROM AN EXECUTABLE VM-DOMAIN BLOCK.
 *
 * Why here and not in ObjectHeap: Chunk 1 holds the glue code, the old/young
 * generations AND the compiler area, and the compiler area is where the JIT
 * emits machine code. If the chunk itself is VM-domain memory, the code is
 * executable wherever ObjectHeap puts it - no compiler-area relocation, no GC
 * changes, no layout surgery. This is the smallest possible change that can
 * make the JIT run, and it is one function.
 *
 * The constraint: a VM-domain block is limited to 16 MiB (sceKernelAllocMem
 * BlockForVM documents the maximum; log159 measured the minimum at 1 MiB). The
 * requested chunk is max_size + alignment, so if that does not fit we simply
 * fall back to the existing malloc path - same behaviour as before this
 * version, and the log says which happened. That makes the change safe even
 * though the Java heap size is not known at compile time here (the cfg's
 * JAVA_HEAP_SIZE is a build default and MS_getHeapRequirement can override it
 * at runtime).
 *
 * Order, proven on hardware in log181: OpenVMDomain FIRST, then allocate, then
 * store. A block allocated while the domain is already open is the variant that
 * worked there; opening after the alloc returned 0x80010058 "already open".
 * The domain is left open for the life of the process because the JIT patches
 * code continuously.
 * ------------------------------------------------------------------------ */
#define VITA_VM_BLOCK_MAX   0x1000000u   /* 16 MiB, documented maximum */
#define VITA_VM_BLOCK_MIN   0x100000u    /* 1 MiB; see the alignment note */
/* v04.19: TWO undocumented constraints, both measured on hardware:
 *   (1) size must be a MULTIPLE OF 1 MiB. log182: a request of 1179700 bytes
 *       (the bit-vector chunk, 1.125 MiB) was rejected with 0x80024b05
 *       ILLEGAL_MEMBLOCK_SIZE while the probe's 1048576 was accepted in the
 *       same run. Every non-multiple in the probe's size sweep had failed the
 *       same way, which is what the earlier "1 MiB minimum" reading actually
 *       was - a minimum and an alignment, not one or the other.
 *   (2) the maximum is per block; total capacity is measured separately.
 * So always round the request UP to a MiB boundary and never ask for less than
 * one MiB. */
#define VITA_VM_ALIGN       0x100000u
static size_t vita_vm_round(size_t want) {
  return (want + VITA_VM_ALIGN - 1) & ~((size_t)VITA_VM_ALIGN - 1);
}

extern "C" {
  /* SceUID is a 32-bit int; declared explicitly so this file does not have to
   * pull in the VitaSDK headers (it is a shared VM source). */
  int    sceKernelAllocMemBlockForVM(const char *name, unsigned int size);
  int    sceKernelGetMemBlockBase(int uid, void **base);
  int    sceKernelFreeMemBlock(int uid);
  int    sceKernelOpenVMDomain(void);
  int    sceKernelSyncVMDomain(int uid, void *data, unsigned int size);
}

static int    chunks_vm[MAX_CHUNKS] = { -1, -1 };
static int    g_vm_domain_open = 0;
/* v04.49: THE HEAP BLOCK IS KEPT ACROSS ROUNDS, NOT FREED AND RE-ASKED.
 *
 * log189 evidence. One launcher process ran two rounds:
 *   round 0 (file manager, heap_mb=14)
 *       [heapmem] chunk0 BACKED BY VM-DOMAIN MEMORY: 0x85d00000, 15728640
 *       [heapmem] freeing chunk0 (VM block uid=0x4004013d)
 *   round 1 (NFS, heap_mb=14 - SAME request)
 *       [heapmem] VM alloc of 15728640 bytes (MiB-aligned) FAILED: 0x80024b0b
 * i.e. a 15 MiB block was freed and the very next identical request for it
 * failed with MEMBLOCK_OVERFLOW. Whatever the kernel does with a freed VM
 * block, handing it straight back is not reliable - and it degrades with every
 * round, so the user can start at most three games before the process can no
 * longer allocate a heap at all.
 *
 * So the block is cached here and reused. It is never freed for the life of
 * the process; the size is rounded UP to 1 MiB so a later, slightly larger
 * request can also be satisfied from it. Reuse keeps the block at the same
 * base address, which is also what the JIT wants (generated code and the
 * flush_icache lookup both key off chunks_vm[]).
 *
 * The contents are stale on reuse (old heap, old JIT code). That is safe:
 * ObjectHeap::create() rebuilds every structure it uses and resets the
 * compiler area's used counter, so nothing reads the previous round's data. */
static int    g_vm_keep_uid  = -1;
static size_t g_vm_keep_size = 0;
static void  *g_vm_keep_base = NULL;

/* v04.18: read by vita_main.c before it lets the JIT run. Set only for the
 * HEAP chunk (index 0); the bit-vector chunk is not code and does not matter. */
static int    g_heap_chunk_executable = 0;

int vita_heap_executable(void) {
  return g_heap_chunk_executable;
}

/* v04.48: THE LAST LINE OF DEFENCE AGAINST THE log188 CRASH.
 *
 * The heap can only be executable when it lives in a VM-domain block. Every
 * failure below (block too big, OVERFLOW, not writable, no base) falls back to
 * plain malloc - which is perfectly correct for the interpreter, but fatal for
 * the JIT: the compiler area then sits in execute-never memory and the first
 * call into generated code aborts on instruction fetch.
 *
 * log188 is that crash. runMidlet.c had predicted "heap_mb=15 is fine", the
 * 16 MiB block came back 0x80024b0b (SCE_KERNEL_ERROR_MEMBLOCK_OVERFLOW), the
 * heap fell back to malloc, and nothing told the compiler to stand down:
 *     pc == lr == 0x82dcf814, inside the Java heap, fault status 0x0d
 * i.e. exactly the log181 signature that v03.75 was supposed to have closed.
 *
 * Predictions are made before anything is allocated and can be wrong (the
 * usable block maximum turned out to be 15 MiB, not the documented 16 MiB).
 * This check is made AFTER the outcome is known, so it cannot be wrong.
 * Compiler::compile() tests `!UseCompiler` on every tick (Compiler.cpp:235),
 * so flipping it here stops compilation before any code is generated.
 * CompilerAreaPercentage is zeroed as well so ObjectHeap does not reserve a
 * code area it can never use. */
static void vita_heap_not_executable(int i, const char *why) {
  if (i != 0) {
    return;                     /* only the heap chunk carries code */
  }
  if (UseCompiler) {
    crumb_printf("[heapmem] %s -> the Java heap is NOT executable; "
                 "forcing UseCompiler=0 (the JIT would fault on its first "
                 "call, this is the log188 crash)", why);
    UseCompiler = false;
    CompilerAreaPercentage = 0;
    CompilerAreaSlack = 0;
  }
  g_heap_chunk_executable = 0;
}

/* v04.47: the cache flush that OsMisc_flush_icache could never perform.
 *
 * Why this lives here: sceKernelSyncVMDomain needs the block UID, and the UID
 * table (chunks_vm[]) is private to this file. Returns 1 when a VM block
 * covering `start` was flushed, 0 when the address is not VM-backed (then the
 * caller falls back to __builtin___clear_cache, whose value is zero on this
 * target but is harmless to keep for other builds).
 *
 * Evidence this was the missing piece (log187):
 *   - [heapmem] chunk0 BACKED BY VM-DOMAIN MEMORY: 0x85d00000, 15728640 bytes
 *     => the Java heap (and the JIT's compiler area inside it) IS executable;
 *   - the round then died with pc=0x868348ac / lr=0x868340f8, i.e. INSIDE that
 *     block (0x85d00000 + 0xb348ac), and runMidlet() never returned;
 *   - JVMCompiledMethod::flush_icache() tail-calls OsMisc_flush_icache, and
 *     OsMisc_flush_icache was a bare return: __builtin___clear_cache() is a
 *     no-op under VitaSDK gcc (v03.71 finding, re-confirmed by disassembly in
 *     v04.47: two instructions then the next function).
 * So the JIT wrote code, the icache kept the old contents, and the first
 * indirect branch into a fresh method executed garbage.
 *
 * Size check: sceKernelSyncVMDomain requires the range to be inside the block.
 * A CompiledMethod never straddles two blocks (the compiler area is carved from
 * one heap chunk), so passing [start, start+size) through is enough; a failure
 * (non-zero rc) simply means "this address is not in that block", which the
 * loop turns into "try the next block, else report not-flushed". */
extern "C" int vita_vm_flush_icache(void *start, int size) {
  int i;
  if (start == NULL || size <= 0) {
    return 0;
  }
  for (i = 0; i < MAX_CHUNKS; i++) {
    if (chunks_vm[i] < 0) {
      continue;
    }
    if (sceKernelSyncVMDomain(chunks_vm[i], start, (unsigned int)size) == 0) {
      return 1;
    }
  }
  /* v04.49: the cached heap block is not in chunks_vm[] between rounds, but a
   * late flush (finalizers, class unloading) can still target it. */
  if (g_vm_keep_uid >= 0 && g_vm_keep_base != NULL &&
      (char *)start >= (char *)g_vm_keep_base &&
      (char *)start < (char *)g_vm_keep_base + g_vm_keep_size) {
    if (sceKernelSyncVMDomain(g_vm_keep_uid, start, (unsigned int)size) == 0) {
      return 1;
    }
  }
  return 0;
}

/* Try to back one chunk with VM-domain memory. Returns 1 on success. */
static int vita_try_vm_chunk(int i, size_t want, size_t alignment) {
  int uid;
  void *base = NULL;
  size_t ask;

  want = vita_vm_round(want);          /* v04.19: MiB-aligned, or rejected */

  if (want < VITA_VM_BLOCK_MIN || want > VITA_VM_BLOCK_MAX) {
    crumb_printf("[heapmem] chunk%d needs %u bytes (MiB-aligned %u); outside "
                 "the VM block range (%u..%u) -> malloc", i,
                 (unsigned int)(want - (VITA_VM_ALIGN - 1)), (unsigned int)want,
                 VITA_VM_BLOCK_MIN, VITA_VM_BLOCK_MAX);
    vita_heap_not_executable(i, "heap chunk does not fit a VM block");
    return 0;
  }
  if (!g_vm_domain_open) {
    int rc = sceKernelOpenVMDomain();
    crumb_printf("[heapmem] OpenVMDomain rc=0x%08x", (unsigned int)rc);
    g_vm_domain_open = 1;   /* one attempt per process; the domain is sticky */
  }

  /* v04.49: REUSE THE CACHED BLOCK BEFORE ASKING THE KERNEL FOR A NEW ONE.
   * Only the heap chunk (i == 0) is cached - the bit-vector chunk never enters
   * this path at all (see the `i == 0` guard at the call site). */
  if (i == 0 && g_vm_keep_uid >= 0 && g_vm_keep_base != NULL &&
      g_vm_keep_size >= want) {
    /* Prove it is still alive and writable: a freed or remapped block would
     * fault on the first store, and that must not happen inside the VM. */
    ((volatile unsigned int *)g_vm_keep_base)[0] = 0xA5A5A5A5u;
    *(volatile unsigned int *)((char *)g_vm_keep_base + want - 16) = 0x5A5A5A5Au;
    if (((volatile unsigned int *)g_vm_keep_base)[0] == 0xA5A5A5A5u &&
        *(volatile unsigned int *)((char *)g_vm_keep_base + want - 16) ==
            0x5A5A5A5Au) {
      ((volatile unsigned int *)g_vm_keep_base)[0] = 0;
      *(volatile unsigned int *)((char *)g_vm_keep_base + want - 16) = 0;
      chunks_vm[i]   = g_vm_keep_uid;
      chunks_orig[i] = (address)g_vm_keep_base;
      if (i == 0) {
        g_heap_chunk_executable = 1;
      }
      crumb_printf("[heapmem] chunk%d REUSING the cached VM block: %p, "
                   "%u bytes requested of %u cached (no new kernel alloc)",
                   i, g_vm_keep_base, (unsigned int)want,
                   (unsigned int)g_vm_keep_size);
      return 1;
    }
    crumb_printf("[heapmem] cached VM block %p is no longer writable -> "
                 "dropping it and allocating fresh", g_vm_keep_base);
    g_vm_keep_uid  = -1;
    g_vm_keep_base = NULL;
    g_vm_keep_size = 0;
  }

  ask = want;
  uid = sceKernelAllocMemBlockForVM("j2me_java_heap", (unsigned int)ask);
  if (uid < 0) {
    crumb_printf("[heapmem] VM alloc of %u bytes (MiB-aligned) FAILED: "
                 "0x%08x -> malloc", (unsigned int)ask, (unsigned int)uid);
    vita_heap_not_executable(i, "VM block allocation failed");
    return 0;
  }
  if (sceKernelGetMemBlockBase(uid, &base) < 0 || base == NULL) {
    crumb_printf("[heapmem] VM GetMemBlockBase FAILED -> malloc");
    sceKernelFreeMemBlock(uid);
    vita_heap_not_executable(i, "VM block has no base address");
    return 0;
  }

  /* Prove the block is really writable before the VM starts building the heap
   * on it. The kernel reports access=0x4 ("R only") for this memory even when
   * it is fully writable - log181 showed a store and a call succeeding on a
   * block whose access field said R only - so the only trustworthy check is an
   * actual store, at both ends. */
  ((volatile unsigned int *)base)[0] = 0xA5A5A5A5u;
  *(volatile unsigned int *)((char *)base + ask - 16) = 0x5A5A5A5Au;
  if (((volatile unsigned int *)base)[0] != 0xA5A5A5A5u ||
      *(volatile unsigned int *)((char *)base + ask - 16) != 0x5A5A5A5Au) {
    crumb_printf("[heapmem] VM block %p not writable end-to-end -> malloc",
                 base);
    sceKernelFreeMemBlock(uid);
    vita_heap_not_executable(i, "VM block is not writable");
    return 0;
  }
  ((volatile unsigned int *)base)[0] = 0;
  *(volatile unsigned int *)((char *)base + ask - 16) = 0;
  sceKernelSyncVMDomain(uid, base, (unsigned int)ask);

  chunks_vm[i]   = uid;
  chunks_orig[i] = (address)base;
  /* v04.49: remember the heap block so the next round can reuse it instead of
   * asking the kernel for an identical one - log189 shows that re-request is
   * what fails. Only chunk 0, because only it goes through this path. */
  if (i == 0) {
    g_vm_keep_uid  = uid;
    g_vm_keep_base = base;
    g_vm_keep_size = ask;
  }
  /* Chunk 0 is the heap (layout comment in ObjectHeap.hpp: glue code, both
   * generations and the compiler area all live in chunk 1 of the diagram);
   * chunk 1 is the bit-vector / marking-stack chunk and holds no code.
   * Only chunk 0 flips the flag. v04.24: the log used to claim "the Java heap"
   * for EITHER chunk, which made log183 read as if the heap had been placed in
   * executable memory when it was only the bit-vector. */
  if (i == 0) {
    g_heap_chunk_executable = 1;
    crumb_printf("[heapmem] chunk0 BACKED BY VM-DOMAIN MEMORY: %p, %u bytes "
                 "=> THE JAVA HEAP IS EXECUTABLE (compiler area included); "
                 "JIT may run", base, (unsigned int)ask);
  } else {
    crumb_printf("[heapmem] chunk%d backed by VM memory: %p, %u bytes "
                 "(bit-vector only, holds no code - the heap itself was %s)",
                 i, base, (unsigned int)ask,
                 g_heap_chunk_executable ? "already executable"
                                         : "NOT executable, JIT stays off");
  }
  return 1;
}
#endif /* VITA */

//#ifndef PRODUCT 
#ifdef AZZERT
static int _cnt[MAX_CHUNKS] = {0, 0};

static void mark_unused_chunk_space(int i) {
  _cnt[i] ++;

  unsigned int base = (int)chunks[i];
  int *start = (int *)((base + cur_chunk_size[i]) + 0x03 & (~0x03));
  int *end   = (int *)((base + max_chunk_size[i])        & (~0x03));
  int *p;

  int marker = _cnt[i] + 0xdeadbeef;
  for (p = start; p<end; p++) {
    *p = marker;
  }
}

static void check_unused_chunk_space(int i) {
  unsigned int base = (unsigned int)chunks[i];
  int *start = (int *)((base + cur_chunk_size[i]) + 0x03 & (~0x03));
  int *end   = (int *)((base + max_chunk_size[i])        & (~0x03));
  int *p;

  int marker = _cnt[i] + 0xdeadbeef;
  for (p = start; p<end; p++) {
    GUARANTEE(*p == marker, "Uncommitted chunk was written into");
  }
  AZZERT_ONLY_VAR(marker);
}
#endif

address OsMemory_allocate_chunk(size_t initial_size,
                                size_t max_size, size_t alignment)
{
  GUARANTEE(chunks[0] == NULL || chunks[1] == NULL,
            "OsMemory_allocate_chunk supports only 2 chunks");

  /* v04.26: BACK TO THE ORIGINAL SLOT RULE. v04.24's size-derived slot was a
   * regression, and log184 is the proof: round 0 succeeds, every later round
   * fails with
   *     [heap] chunk alloc FAILED ... Could not allocate VM heap
   * where log182 (before that change) had no such line at all.
   *
   * The mistake was mixing two schemes: allocation was switched to size-derived
   * (heap > 8 MiB -> slot 0, else slot 1) while OsMemory_free_chunk,
   * OsMemory_adjust_chunk and OsMemory_heap_expand_chunk all still identify the
   * slot by pointer identity over chunks[]. Once those disagree, a free clears
   * the wrong slot, the live chunk is leaked, and the next round's 32 MiB
   * request no longer fits - which is exactly a "first game works, second game
   * crashes" symptom.
   *
   * The original rule is kept, with the one thing it genuinely needed: a check
   * that the chosen slot is really free. That was the actual defect the size
   * change was meant to fix (log183: a failed heap allocation left chunks[0] ==
   * NULL, so the bit-vector was handed slot 0 and took the heap's place). The
   * check below is enough on its own and keeps every consumer on the same
   * convention. */
  int i;

  if (chunks[0] == NULL) {
    i = 0;
  } else if (chunks[1] == NULL) {
    i = 1;
  } else {
    return NULL;              /* both slots busy: never overwrite a live chunk */
  }

#if defined(VITA)
  /* v04.26: ONLY THE HEAP CHUNK MAY USE A VM-DOMAIN BLOCK.
   *
   * v04.18 wired both chunks through vita_try_vm_chunk, so every round also
   * allocated and freed a 2 MiB VM block for the bit-vector. log184 shows the
   * cost: round 0 succeeds, every later round dies with
   *     [heap] chunk alloc FAILED ... Could not allocate VM heap
   * (and exit_probe: round 0 status=0, rounds 1+ status=-1). log182, which had
   * no VM path at all, had no such failure. The likely cause is that
   * sceKernelOpenVMDomain() and the block allocation permanently charge the
   * process memory budget, so the 32 MiB heap malloc no longer fits on the
   * second attempt - the arena numbers in log184 are right at the boundary
   * (free=33577072 against a 33562624 request, ~14 KB of slack).
   *
   * The bit-vector chunk holds no code, so giving it executable memory was
   * pointless from the start; with this guard it never touches the VM domain and
   * the default path is byte-identical to v04.20 again.
   * i == 0 is the heap (the slot rule above makes the first chunk the heap). */
  if (i == 0 && vita_try_vm_chunk(i, max_size + alignment, alignment)) {
    chunks[i] = (address)align_size_up((size_t)chunks_orig[i], alignment);
    max_chunk_size[i] = max_size;
    cur_chunk_size[i] = initial_size;
    AZZERT_ONLY(mark_unused_chunk_space(i));
    return chunks[i];
  }
#endif

  chunks_orig[i] = (address)OsMemory_allocate(max_size + alignment);
  if (chunks_orig[i] == NULL) {
    return NULL;
  }

  chunks[i] = (address)align_size_up((size_t)chunks_orig[i], alignment);

  max_chunk_size[i] = max_size;
  cur_chunk_size[i] = initial_size;

  AZZERT_ONLY(mark_unused_chunk_space(i));

  return chunks[i];
}

// Expand or shrink a chunk returned by allocate_chunk().
// The chunk is never moved.
//
// Returns 0 if fail to expand (shrink will always succeed). Returns old
// size if successful.
size_t OsMemory_adjust_chunk(address chunk_ptr, size_t new_size) {
  GUARANTEE(chunks[0] == chunk_ptr || chunks[1] == chunk_ptr, "Sanity check");
  int i;

  if (chunks[0] == chunk_ptr) {
    i = 0;
  } else {
    i = 1;
  }

  AZZERT_ONLY(check_unused_chunk_space(i));
  size_t old_size = cur_chunk_size[i];
  if (new_size <= max_chunk_size[i]) {
    cur_chunk_size[i] = new_size;
    AZZERT_ONLY(mark_unused_chunk_space(i));
    return old_size;
  } else {
    return 0;
  }
}

void OsMemory_free_chunk(address chunk_ptr) {
  GUARANTEE(chunks[0] == chunk_ptr || chunks[1] == chunk_ptr, "Sanity check");
  int i;

  if (chunks[0] == chunk_ptr) {
    i = 0;
  } else {
    i = 1;
  }

#if defined(VITA)
  /* v04.18: a chunk backed by a VM-domain block must be released through the
   * kernel, not through free(). chunks_orig[i] still holds the block base.
   *
   * v04.49: BUT NOT THE HEAP BLOCK. It is handed back to the cache instead.
   * log189 round 0 freed its 15 MiB block and round 1's identical request for
   * it came back 0x80024b0b MEMBLOCK_OVERFLOW, so freeing it buys nothing and
   * costs the next round the ability to have an executable heap at all. The
   * block stays with the process; ObjectHeap::create() reinitialises every
   * structure it contains. */
  if (chunks_vm[i] >= 0) {
    if (i == 0 && g_vm_keep_uid == chunks_vm[i]) {
      crumb_printf("[heapmem] chunk0 kept for the next round: VM block "
                   "uid=0x%08x stays allocated at %p (%u bytes)",
                   (unsigned int)chunks_vm[i], chunks_orig[i],
                   (unsigned int)g_vm_keep_size);
      chunks_vm[i] = -1;
      chunks_orig[i] = NULL;
      chunks[i] = NULL;
      return;
    }
    crumb_printf("[heapmem] freeing chunk%d (VM block uid=0x%08x)",
                 i, (unsigned int)chunks_vm[i]);
    sceKernelFreeMemBlock(chunks_vm[i]);
    chunks_vm[i] = -1;
    chunks_orig[i] = NULL;
    chunks[i] = NULL;
    return;
  }
#endif

  OsMemory_free(chunks_orig[i]);
  chunks[i] = NULL;
}

size_t OsMemory_heap_initial_size(size_t /*min_size*/, size_t max_size) {  return max_size;
}

size_t OsMemory_heap_expansion_target(size_t /*current_size*/, 
                                      size_t /*required_size*/, 
                                      size_t max_size,
                                      const bool /*is_full_collect*/) {
  return max_size;
}
size_t OsMemory_heap_reduction_target(size_t current_size,
                                      size_t /*used_size*/,
                                      size_t /*min_size*/) {
  return current_size;
}

#endif /* !ENABLE_PCSL && !SUPPORTS_ADJUSTABLE_MEMORY_CHUNK */

#if (ENABLE_PCSL || SUPPORTS_ADJUSTABLE_MEMORY_CHUNK) && \
    !SUPPORTS_CUSTOM_HEAP_ADJUSTMENT

/*
 * This is a default heap size adjustmemt policy:
 *
 * + initial size is 10% of HeapCapacity, but no less than 100K and no more
 *   than 1M.
 * + When we have done one GC and there still insufficient space, we double
 *   the heap size.
 * + After a collection, if the free space is more than half of the heap
 *   size, shrink heap by half.
 */

size_t OsMemory_heap_initial_size(size_t min_size, size_t max_size) {
  GUARANTEE(min_size <= max_size, "sanity");
  size_t init_size = min_size;
  if (init_size == 0) {
    // min_size is 0 means "pick the right initial size"
    init_size = max_size / 10;
  }

  if (UseCompiler) {
    // For better performance, we need at least 100KB, which would
    // give 20KB for the compiler_area
    if (init_size < 100 * 1024) {
      init_size = 100 * 1024;
    }
  }
  if (init_size > 1024 * 1024) {
    init_size = 1024 * 1024;
  }
  if (min_size != 0 && init_size < min_size) {
    init_size = min_size;
  }
  if (init_size > max_size) {
    init_size = max_size;
  }

  return init_size;
}

size_t OsMemory_heap_expansion_target(size_t current_size, 
                                      size_t required_size, 
                                      size_t max_size,
                                      const bool is_full_collect) {
  // In this sample implementation, we grow the heap only after a full
  // GC.
  //
  // To achieve better performance, some platforms may try to grow
  // the heap eagerly just after a young GC has executed.
  if (is_full_collect && current_size < required_size) {
    size_t new_size = required_size + current_size;
    if (new_size > max_size) {
      new_size = max_size;
    }
    return new_size;
  } else {
    return current_size;
  }
}

size_t OsMemory_heap_reduction_target(size_t current_size,
                                      size_t used_size,
                                      size_t min_size) {
  // Pretty naive heap shrinking strategy: If we have a lot
  // of unused space after a full collection, reduce the
  // unused space by half.
  size_t free_unused = current_size - used_size;
  if (free_unused > (current_size / 2)) {
    size_t new_size = current_size * 3 / 4;
    if (new_size < min_size) {
      new_size = min_size;
    }
    if (new_size < used_size) {
      new_size = used_size;
    }
    if (UseCompiler && new_size < 100 * 1024) {
      // For better performance, we need at least 100KB, which would
      // give 20KB for the compiler_area
      new_size = 100 * 1024;
    }

    return new_size;
  } else {
    return current_size;
  }
}
#endif // (ENABLE_PCSL || SUPPORTS_ADJUSTABLE_MEMORY_CHUNK) &&
       // !SUPPORTS_CUSTOM_HEAP_ADJUSTMENT
