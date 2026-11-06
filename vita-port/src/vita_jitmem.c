/*
 * vita_jitmem.c - executable-memory probe for the JIT (v03.71).
 * See vita_jitmem.h for the problem statement and the API facts.
 *
 * The probe answers four questions in one run, in this order, stopping at the
 * first failure so that a crash is attributable to exactly one step:
 *
 *   1. does sceKernelAllocMemBlockForVM() hand out a block at all?
 *   2. does the block accept stores while the domain is closed?
 *   3. does sceKernelOpenVMDomain() + sceKernelSyncVMDomain() make those
 *      bytes fetchable (i.e. does `bx` into them survive)?
 *   4. if yes, is the mapping still WRITABLE afterwards? The JIT has to
 *      patch compiled methods in place (CompiledMethod_arm.cpp calls
 *      OsMisc_flush_icache after overwriting BytesPerInt bytes), so an
 *      open-exclusive mapping would force a close/write/open cycle per patch.
 *
 * Q4 is deliberately answered by a store to the same cache line as freshly
 * executed code - that is the worst case for a "clean to PoU on open" scheme.
 */

#include <string.h>
#include <stdio.h>

#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/modulemgr.h>
#include <psp2/io/fcntl.h>
#include <psp2/pss.h>       /* v03.83: the PSS JIT-code-memory API */
#include <psp2/sysmodule.h> /* v04.08: load SCE_SYSMODULE_MONO_BRIDGE */

#include "vita_jitmem.h"
#include "vita_crumb.h"

/* v04.26: PROBE_CFG removed - see the note below vita_jit_probe(). */

/* ------------------------------------------------------------------------
 * v03.83: FOUND THE API THAT WAS DESIGNED FOR THIS ALL ALONG.
 *
 * Every previous round (v03.71..v03.82) went after sceKernelAllocMemBlockForVM
 * + sceKernelOpenVMDomain, and log160/161 showed that path dead on this
 * hardware: the VM block reports access=0x4 (R only), OpenVMDomain returns 0
 * without changing a single access bit, and writing to the block aborts.
 * Searches for "cache" symbols missed the answer because the API is not named
 * after caching - it is named after PSS (PlayStation Suite), whose Mono
 * runtime is a managed-code runtime with a JIT and therefore has exactly the
 * same problem a JVM has.
 *
 * psp2/pss.h (shipped with VitaSDK) declares:
 *      void *pss_code_mem_alloc(SceSize *);
 *      void  pss_code_mem_flush_icache(const void *, SceSize);
 *      void  pss_code_mem_lock(void);
 *      void  pss_code_mem_unlock(void);
 * and libSceLibMonoBridge_stub.a exports those plus two the header omits:
 *      pss_code_mem_initialize / pss_code_mem_terminate / pss_code_mem_free
 * with NIDs 0xD67BC7AB / 0x6899C39C / 0xB468C313 / 0xF466ABEC / 0x110F567B.
 * All of it links from user code (verified with a standalone test program), and
 * weak-stub linking works too, so a firmware without SceLibMonoBridge cannot
 * stop the app from starting.
 *
 * This is the supported way to get writable+executable memory and to flush the
 * instruction cache from userspace. Everything VM-domain related below is kept
 * only as a fallback record of what does NOT work.
 * ------------------------------------------------------------------------ */

/* The header omits these three, but the stub library exports them. */
extern void pss_code_mem_initialize(void);
extern void pss_code_mem_terminate(void);
extern void pss_code_mem_free(void *);

/* ------------------------------------------------------------------------
 * v04.05: NULL-CHECK EVERY PSS ENTRY POINT. This is what crashed log175.
 *
 * The build links SceLibMonoBridge_stub_weak on purpose, so a firmware without
 * that module cannot stop the app from starting. But a weak import that does
 * not resolve is NOT a harmless no-op - its address is literally 0. v03.83
 * called pss_code_mem_initialize() unconditionally, so on this console the very
 * first call jumped to address 0:
 *
 *     crumb.log:  "[JITPROBE] === PATH A: PSS code memory ==="
 *                 (nothing after it - the next line never printed)
 *     coredump  :  pc = 0x00000000   far = 0x00000000
 *                  fault status = 0x07 (translation fault, i.e. no mapping)
 *                  lr = caller inside vita_jit_probe
 *
 * Note the fault is a TRANSLATION fault, not a permission fault: address 0 has
 * no page at all, which is exactly what a NULL function pointer looks like.
 *
 * The pointers below are `volatile` on purpose. Without it GCC is allowed to
 * assume a function address is never NULL (-fdelete-null-pointer-checks is on
 * at -O2) and delete the guards. No compiler flags were changed for this - the
 * volatile qualification is local and self-documenting.
 * ------------------------------------------------------------------------ */
typedef void *(*pss_alloc_fn)(SceSize *);
typedef void  (*pss_flush_fn)(const void *, SceSize);
typedef void  (*pss_void_fn)(void);
typedef void  (*pss_free_fn)(void *);

static pss_void_fn  volatile g_pss_init    = pss_code_mem_initialize;
static pss_void_fn  volatile g_pss_term    = pss_code_mem_terminate;
static pss_alloc_fn volatile g_pss_alloc   = pss_code_mem_alloc;
static pss_flush_fn volatile g_pss_flush   = pss_code_mem_flush_icache;
static pss_free_fn  volatile g_pss_free    = pss_code_mem_free;

/* Every entry point or none - a partially resolved import set is not something
 * to reason about, and all five come from the same module. */
static int pss_available(void) {
    return g_pss_init != NULL && g_pss_term != NULL && g_pss_alloc != NULL &&
           g_pss_flush != NULL && g_pss_free != NULL;
}

/* A second, independent gate: is the providing module in the process at all?
 * The NULL check above depends on how the linker resolves a weak import, which
 * is exactly the kind of thing not to guess about after a crash. Enumerating
 * the module list answers the question directly, needs no kernel privilege
 * (vita_kernel_forensics.c already uses it at boot), and if the module is
 * absent we simply never touch a pss_* pointer.
 * Types 0x01..0x04 cover USER / SHARED / SYSTEM; the loop returns as soon as
 * any list reports the name. */
#define PSS_MODULE_NAME "SceLibMonoBridge"
static int pss_module_present(void) {
    static const unsigned char types[] = { 0x01, 0x02, 0x03, 0x04 };
    SceUID uids[128];
    SceKernelModuleInfo mi;   /* matches sceKernelGetModuleInfo's size */
    unsigned int t;
    int i, total = 0;

    for (t = 0; t < sizeof(types) / sizeof(types[0]); t++) {
        SceSize num = sizeof(uids) / sizeof(uids[0]);
        if (sceKernelGetModuleList(types[t], uids, &num) < 0) {
            continue;
        }
        total += (int)num;
        for (i = 0; i < (int)num; i++) {
            memset(&mi, 0, sizeof(mi));
            mi.size = sizeof(mi);
            if (sceKernelGetModuleInfo(uids[i], &mi) < 0) {
                continue;
            }
            if (strcmp(mi.module_name, PSS_MODULE_NAME) == 0) {
                crumb_printf("[JITPROBE] %s IS loaded (module list type 0x%02x)",
                             PSS_MODULE_NAME, types[t]);
                return 1;
            }
        }
    }
    /* Log what IS there once, so "absent" is actionable: if the runtime that
     * exports pss_* is simply named differently on this firmware, the names
     * will show it. Only names containing Mono / Pss / Psm are printed to keep
     * the line useful. */
    crumb_printf("[JITPROBE] %s NOT loaded (%d modules enumerated); looking "
                 "for anything PSS-related:", PSS_MODULE_NAME, total);
    for (t = 0; t < sizeof(types) / sizeof(types[0]); t++) {
        SceSize num = sizeof(uids) / sizeof(uids[0]);
        if (sceKernelGetModuleList(types[t], uids, &num) < 0) {
            continue;
        }
        for (i = 0; i < (int)num; i++) {
            memset(&mi, 0, sizeof(mi));
            mi.size = sizeof(mi);
            if (sceKernelGetModuleInfo(uids[i], &mi) < 0) {
                continue;
            }
            if (strstr(mi.module_name, "Mono") != NULL ||
                strstr(mi.module_name, "Pss") != NULL ||
                strstr(mi.module_name, "Psm") != NULL) {
                crumb_printf("[JITPROBE]   candidate: '%s'", mi.module_name);
            }
        }
    }
    return 0;
}

/* Sizes to try, doubling. 4 KiB is the ARM page size and the smallest thing
 * worth handing to a JIT; 16 MiB is the documented maximum for memblocks. */
#define PROBE_MIN_SIZE 0x1000u
#define PROBE_MAX_SIZE 0x1000000u

/* mov r0, #42 ; bx lr   - assembled with arm-vita-eabi-as, not typed from
 * memory (objdump: e3a0002a e12fff1e). Little-endian words in memory.
 * `mov r0,#43` (0xE3A0002B) is the in-place-patch variant. */
static const unsigned int probe_code[2] = { 0xE3A0002Au, 0xE12FFF1Eu };

#define PROBE_EXPECT 42

static int g_status = 0;   /* 0 = not run, 1 = code ran, -1 = failed */
static int g_ran = 0;

int vita_jit_probe_status(void) {
    return g_status;
}

/* Translate the codes this probe can actually produce. "rc=0x80024b05" cost a
 * log round-trip to read; the name is the whole finding. */
static const char *vm_err(unsigned int rc) {
    switch (rc) {
    case 0x80024B00u: return "SYSMEM_MEMBLOCK_ERROR";
    case 0x80024B01u: return "ILLEGAL_MEMBLOCK_TYPE";
    case 0x80024B04u: return "ILLEGAL_MEMBLOCK_CODE";
    case 0x80024B05u: return "ILLEGAL_MEMBLOCK_SIZE";
    case 0x80020005u: return "INVALID_ARGUMENT";
    case 0x80020008u: return "ILLEGAL_PERMISSION";
    case 0x8002000Bu: return "ILLEGAL_SIZE";
    case 0x8002000Eu: return "ILLEGAL_ATTR";
    case 0x8002012Cu: return "(devkit-only feature?)";
    /* v04.10: the generic errno domain. LoadModule answers from here and
     * log179 printed "0x80010002?" because none of these were translated -
     * "?" hid the fact that the answer was simply "no such module". */
    case 0x80010001u: return "EPERM";
    case 0x80010002u: return "ENOENT (no such module/file)";
    case 0x80010005u: return "EIO";
    case 0x8001000Du: return "EACCES";
    case 0x80010016u: return "EBUSY";
    /* sysmodule domain */
    case 0x805A1001u: return "SYSMODULE_UNLOADED";
    case 0x805A1002u: return "SYSMODULE_ALREADY_LOADED";
    default:          return "?";
    }
}

/* v04.26: the probe is no longer gated by jitprobe.cfg.
 *
 * It is already lazy in the only way that matters - vita_main.c calls it once,
 * and only when the user asked for jit!=0 - so the extra file made a second,
 * redundant switch that could disagree with the first. log184 is exactly that
 * failure: jit=2 was set, jitprobe.cfg was not present, so
 *     [JITPROBE] skipped: ux0:/data/J2ME00001/jitprobe.cfg absent
 *     jit: executable-memory probe FAILED (see [JITPROBE])
 *     jit: cfg=2 -> policy 0 (interpreter); no executable memory
 * i.e. the user turned on the JIT and got a permanent "no executable memory"
 * verdict from a file they never heard of. A skipped probe now means what it
 * says: the caller did not ask for it. */

/* Ask the kernel what the mapping actually allows, and RETURN the access bits
 * so the caller can refuse to touch memory it may not touch.
 * SceKernelMemBlockInfo has an `access` field of SCE_KERNEL_MEMORY_ACCESS_X/W/R
 * bits, so this answers "is it writable / executable" WITHOUT running or
 * writing anything. That matters because both can kill the process, and
 * crumb.log is flushed immediately, so these lines survive a crash while a
 * later line would not. Returns -1 if the query itself failed. */
static int dump_perm_sz(const char *tag, void *addr, unsigned int size) {
    SceKernelMemBlockInfo info;
    int rc;
    memset(&info, 0, sizeof(info));
    info.size = sizeof(info);
    rc = sceKernelGetMemBlockInfoByRange(addr, size, &info);
    if (rc < 0) {
        crumb_printf("[JITPROBE] perm %s @%p rc=0x%08x = %s", tag, addr,
                     (unsigned int)rc, vm_err((unsigned int)rc));
        return -1;
    }
    crumb_printf("[JITPROBE] perm %s @%p type=0x%08x access=0x%x -> %s%s%s",
                 tag, addr, (unsigned int)info.type, (unsigned int)info.access,
                 (info.access & SCE_KERNEL_MEMORY_ACCESS_R) ? "R" : "-",
                 (info.access & SCE_KERNEL_MEMORY_ACCESS_W) ? "W" : "-",
                 (info.access & SCE_KERNEL_MEMORY_ACCESS_X) ? "X" : "-");
    return (int)info.access;
}

/* Query both with a 4-byte probe and with the whole block: the kernel's range
 * lookup may answer from a cached per-block record when the requested range is
 * a single word, and log160's answer is what this whole design hangs on. */
static int dump_perm(const char *tag, void *addr, unsigned int size) {
    int a = dump_perm_sz(tag, addr, 4);
    if (size > 4) {
        char tag2[32];
        snprintf(tag2, sizeof(tag2), "%s full", tag);
        dump_perm_sz(tag2, addr, size);
    }
    return a;
}

int vita_jit_probe(void) {
    SceSize req;
    void *base = NULL;
    SceSize got_size = 0;
    unsigned int *code;
    int (*fn)(void);
    int got;
    int try_idx;

    if (g_ran) {
        return g_status > 0;
    }
    g_ran = 1;

    /* ---- PATH A: the PSS JIT code memory (v03.83) -------------------------
     * pss_code_mem_alloc(SceSize *) takes a pointer, which reads as an in/out
     * parameter, but the header documents nothing. Rather than guess once and
     * pay for a wasted round, try the plausible conventions in order and log
     * what each returned - the log then says which one the implementation
     * actually honours.
     *   1. *size = 1 MiB  (requested amount, in/out)
     *   2. *size = 0      ("give me the default chunk"; Mono allocates code
     *                      memory in fixed chunks, so 0 is a plausible idiom)
     *   3. *size = 4 KiB  (smallest page-aligned request)
     *
     * v04.05: availability is checked BEFORE the first call. See the pointer
     * table at the top of this file: an unresolved weak import is address 0,
     * and v03.83 calling it directly is precisely how log175 got pc=0. */
    crumb_printf("[JITPROBE] === PATH A: PSS code memory (pss_code_mem_*) ===");
    crumb_printf("[JITPROBE] entry points: init=%p term=%p alloc=%p flush=%p "
                 "free=%p", (void *)g_pss_init, (void *)g_pss_term,
                 (void *)g_pss_alloc, (void *)g_pss_flush, (void *)g_pss_free);
    if (!pss_available()) {
        crumb_printf("[JITPROBE] PATH A unavailable: SceLibMonoBridge is not "
                     "resident (a weak import that did not resolve is a NULL "
                     "address, not a no-op - v04.05 checks instead of calling)");
        goto fail_pss;
    }
    /* v04.08: TRY TO LOAD THE MODULE instead of giving up when it is absent.
     * log178: SceLibMonoBridge was not in the process (6 modules enumerated,
     * none of them the bridge), which is expected - it is part of the PSM
     * runtime and a plain app does not get it for free. But the sysmodule ID
     * EXISTS in the SDK:
     *     SCE_SYSMODULE_MONO_BRIDGE = 0x003F
     *     SCE_SYSMODULE_MONO        = 0x0040
     *     SCE_SYSMODULE_PSM         = 0x0041
     * and this port already loads sysmodules this way (SceSysmodule_stub is
     * linked, SCE_SYSMODULE_IME is loaded for the keyboard). So ask the kernel
     * to load it and then re-check the module list - the stub section for
     * SceLibMonoBridge is already in our ELF and vita-elf-create will bind it
     * once the library is present in the process.
     *
     * Order matters: BRIDGE alone first (that is the one exporting pss_*), then
     * MONO only if that fails, because the bridge is the smaller dependency and
     * loading more runtime than necessary into a game process is a risk with no
     * upside. Every rc is logged - on a cracked console this is the one call
     * most likely to be refused, and "refused" has to be visible in the log
     * rather than looking like the module was never there. */
    if (!pss_module_present()) {
        static const SceSysmoduleModuleId cand[] = {
            SCE_SYSMODULE_MONO_BRIDGE,
            SCE_SYSMODULE_MONO
        };
        unsigned int c;
        for (c = 0; c < sizeof(cand) / sizeof(cand[0]); c++) {
            int st = sceSysmoduleIsLoaded(cand[c]);
            int rc;
            crumb_printf("[JITPROBE] sysmodule 0x%04x IsLoaded=%d "
                         "0x%08x", (unsigned int)cand[c], st,
                         (unsigned int)st);
            if (st == 0) {
                crumb_printf("[JITPROBE]   already loaded, skipping LoadModule");
            } else {
                rc = sceSysmoduleLoadModule(cand[c]);
                crumb_printf("[JITPROBE] sceSysmoduleLoadModule(0x%04x) "
                             "rc=0x%08x%s", (unsigned int)cand[c],
                             (unsigned int)rc,
                             (rc < 0) ? vm_err((unsigned int)rc) : "");
            }
            if (pss_module_present()) {
                break;
            }
        }
    }
    if (!pss_module_present()) {
        crumb_printf("[JITPROBE] PATH A skipped: the providing module is "
                     "still absent after LoadModule attempts, so calling any "
                     "pss_* would jump to address 0 (this is what crashed "
                     "log175)");
        /* v04.14: FALL THROUGH to PATH B instead of returning. This `return 0`
         * was written in v04.05, before PATH B did anything but dump a
         * permission field; when v04.11 turned PATH B into a real write+exec
         * test, this line silently made it dead code. log180 is the proof: the
         * log contains PATH A and no path B at all, so the test that had never
         * been run once was never even reached. */
        goto path_b;
    }
    /* The module is in the process now. But an fstub is a RELATIVE import: it
     * was resolved by vita-elf-create at build time against the module being
     * absent, so the table entry may still be 0 even though the module just
     * appeared. Re-log the pointer values so the next round can tell the two
     * situations apart - "module present but stub still 0" would mean the
     * import has to be made optional at link time instead, which is a
     * different fix and worth knowing before spending another round on it. */
    crumb_printf("[JITPROBE] post-load entry points: init=%p term=%p alloc=%p "
                 "flush=%p free=%p", (void *)g_pss_init, (void *)g_pss_term,
                 (void *)g_pss_alloc, (void *)g_pss_flush, (void *)g_pss_free);
    g_pss_init();
    crumb_printf("[JITPROBE] pss_code_mem_initialize() returned");

    for (try_idx = 0; try_idx < 3; try_idx++) {
        req = (try_idx == 0) ? 0x100000u : (try_idx == 1) ? 0u : 0x1000u;
        crumb_printf("[JITPROBE] pss_code_mem_alloc(*size=%u) ...", req);
        base = g_pss_alloc(&req);
        crumb_printf("[JITPROBE]   -> ptr=%p  *size=%u%s", base, req,
                     (base != NULL) ? "  <-- ACCEPTED" : "");
        if (base != NULL) {
            got_size = req;
            break;
        }
    }

    if (base != NULL) {
        /* Alignment matters: ARM code must be word aligned, and bx to a
         * non-aligned address drops the low bits (or faults on Thumb entry). */
        crumb_printf("[JITPROBE] got %u bytes at %p aligned4=%d aligned8=%d",
                     got_size, base,
                     (((unsigned int)(uintptr_t)base) & 3) == 0,
                     (((unsigned int)(uintptr_t)base) & 7) == 0);

        code = (unsigned int *)base;

        /* Store. crumb_append() writes straight to sceIo with no buffering, so
         * if the next log round ends at this marker, the store is what aborted
         * and nothing after it ran. */
        crumb_printf("[JITPROBE] MARKER: about to store %u bytes at %p "
                     "(if this is the last line, the store aborted)",
                     (unsigned int)sizeof(probe_code), (void *)code);
        code[0] = probe_code[0];
        code[1] = probe_code[1];
        crumb_printf("[JITPROBE] store survived");
        if (code[0] != probe_code[0] || code[1] != probe_code[1]) {
            crumb_printf("[JITPROBE] read back %08x %08x - store did not stick",
                         code[0], code[1]);
            goto fail_pss;
        }
        crumb_printf("[JITPROBE] read back ok: %08x %08x", code[0], code[1]);

        /* This is the call the whole port has been missing since v01.45:
         * a USERLAND instruction-cache flush. Every other route was dead
         * (VitaSDK's GCC folds __builtin___clear_cache to nothing on this
         * target; mcr p15 is PL1; ksceKernelIcacheInvalidateRange is kernel
         * only and does not even link). */
        crumb_printf("[JITPROBE] pss_code_mem_flush_icache(%p, %u) ...",
                     (void *)code, (unsigned int)sizeof(probe_code));
        g_pss_flush(code, (SceSize)sizeof(probe_code));
        crumb_printf("[JITPROBE] flush_icache returned");

        /* Execute. `mov r0,#42; bx lr` either returns 42 or the process dies
         * with an instruction-fetch abort - a complete answer either way, and
         * the marker makes the abort attributable. */
        fn = (int (*)(void))base;
        crumb_printf("[JITPROBE] MARKER: about to call generated code at %p "
                     "(if this is the last line, the fetch aborted)",
                     (void *)fn);
        got = fn();
        crumb_printf("[JITPROBE] generated code returned %d (want %d)",
                     got, PROBE_EXPECT);
        if (got != PROBE_EXPECT) {
            crumb_printf("[JITPROBE] wrong return value - the CPU did not "
                         "execute what we wrote");
            goto fail_pss;
        }

        /* In-place patch. A JIT must rewrite compiled methods
         * (CompiledMethod_arm.cpp rewrites BytesPerInt bytes and then calls the
         * icache flush), so the region has to stay writable after it became
         * executable. Patch the already-executed line and call again. */
        code[0] = 0xE3A0002Bu;          /* mov r0, #43 */
        crumb_printf("[JITPROBE] in-place patch written: %08x", code[0]);
        g_pss_flush(code, (SceSize)sizeof(probe_code));
        got = fn();
        crumb_printf("[JITPROBE] after in-place patch returned %d (want 43)",
                     got);
        if (got != 43) {
            crumb_printf("[JITPROBE] VERDICT: PSS code memory is executable "
                         "and writable, but the patch was not visible - the "
                         "flush does not cover the modified line. A JIT needs "
                         "a full-range flush per rewrite.");
            g_pss_free(base);
            g_pss_term();
            g_status = 1;
            return 1;
        }

        crumb_printf("[JITPROBE] VERDICT: WORKS. PSS code memory is writable, "
                     "executable and patchable, and pss_code_mem_flush_icache "
                     "is the userspace icache flush the JIT needs.");
        crumb_printf("[JITPROBE] use it as: pss_code_mem_initialize() once at "
                     "startup, pss_code_mem_alloc(&size) for the code area, "
                     "pss_code_mem_flush_icache(addr,len) after every patch, "
                     "pss_code_mem_free + pss_code_mem_terminate at shutdown");
        g_pss_free(base);
        g_pss_term();
        g_status = 1;
        return 1;
    }

fail_pss:
    crumb_printf("[JITPROBE] PATH A did not deliver a code buffer");
    if (pss_available()) {
        g_pss_term();
    }

path_b:
    /* ---- PATH B: VM domain, FULL test (v04.10) ---------------------------
     * This is the test that has never actually completed, and why:
     *   log159        : the store happened BEFORE OpenVMDomain -> data abort
     *                   (that was my bug, not the platform's answer)
     *   log160/161    : OpenVMDomain succeeded, but v03.77's hard gate on the
     *                   access field refused to store, so nothing was learned
     *   v03.81..04.09 : PATH B degraded to "alloc + dump permissions" once PSS
     *                   looked promising, so it never stored either
     * Meanwhile "OpenVMDomain makes all VM domain memblocks executable" is
     * still the documented behaviour, and log160's evidence against it
     * (access stays 0x4) rests on a field whose meaning is itself unproven.
     * So run the whole sequence here: open, alloc, store, flush, call.
     *
     * OpenVMDomain is called both BEFORE and AFTER the alloc: a block created
     * while the domain is already open is the one variant never tried. */
    crumb_printf("[JITPROBE] === PATH B: VM domain, full write+exec test ===");
    {
        SceUID uid;
        void *vb = NULL;
        int rc;

        rc = sceKernelOpenVMDomain();
        crumb_printf("[JITPROBE] VM OpenVMDomain (before alloc) rc=0x%08x%s",
                     (unsigned int)rc, (rc < 0) ? vm_err((unsigned int)rc) : "");

        uid = sceKernelAllocMemBlockForVM("j2me_jit_probe", 0x100000u);
        if (uid < 0) {
            crumb_printf("[JITPROBE] VM alloc failed: 0x%08x = %s",
                         (unsigned int)uid, vm_err((unsigned int)uid));
        } else if (sceKernelGetMemBlockBase(uid, &vb) < 0 || vb == NULL) {
            crumb_printf("[JITPROBE] VM GetMemBlockBase failed");
        } else {
            unsigned int *vc = (unsigned int *)vb;

            rc = sceKernelOpenVMDomain();
            crumb_printf("[JITPROBE] VM OpenVMDomain (after alloc) rc=0x%08x%s",
                         (unsigned int)rc,
                         (rc < 0) ? vm_err((unsigned int)rc) : "");
            dump_perm("VM block", vb, 0x100000u);

            /* THE TEST. crumb is unbuffered, so if the next round's log ends at
             * this marker, the store aborted and the answer is "VM domain is
             * read-only, full stop". */
            crumb_printf("[JITPROBE] MARKER: VM store at %p (if this is the "
                         "last line, the store aborted)", (void *)vc);
            vc[0] = probe_code[0];
            vc[1] = probe_code[1];
            crumb_printf("[JITPROBE] VM store survived: %08x %08x",
                         vc[0], vc[1]);

            if (vc[0] == probe_code[0] && vc[1] == probe_code[1]) {
                /* Whole-block writability: a JIT writes past the first page. */
                unsigned int *tail =
                    (unsigned int *)((char *)vb + 0x100000u - 16);
                crumb_printf("[JITPROBE] MARKER: VM store at block end %p",
                             (void *)tail);
                *tail = 0x5A5A5A5Au;
                crumb_printf("[JITPROBE] VM block end = %08x", *tail);

                rc = sceKernelSyncVMDomain(uid, vb, 0x100000u);
                crumb_printf("[JITPROBE] VM SyncVMDomain rc=0x%08x%s",
                             (unsigned int)rc,
                             (rc < 0) ? vm_err((unsigned int)rc) : "");

                crumb_printf("[JITPROBE] MARKER: VM call %p (if this is the "
                             "last line, the fetch aborted - no X bit)",
                             (void *)vb);
                got = ((int (*)(void))vb)();
                crumb_printf("[JITPROBE] VM generated code returned %d "
                             "(want %d)", got, PROBE_EXPECT);

                if (got == PROBE_EXPECT) {
                    crumb_printf("[JITPROBE] VERDICT: VM DOMAIN WORKS. "
                                 "Writable, executable; SyncVMDomain is the "
                                 "flush; 1 MiB minimum block. Use this instead "
                                 "of PSS.");
                    sceKernelCloseVMDomain();
                    sceKernelFreeMemBlock(uid);
                    g_status = 1;
                    return 1;
                }
            }
            sceKernelCloseVMDomain();
        }
        if (uid >= 0) {
            sceKernelFreeMemBlock(uid);
        }
    }

    /* ---- PATH C: how much VM-domain memory exists IN TOTAL? (v04.19) ------
     * log182 showed the Java heap needs 33562752 bytes while one VM block is
     * capped at 16777216, so "put the heap in a VM block" needs the TOTAL
     * budget to be larger than one block - otherwise the only options are
     * shrinking the heap or moving just the compiler area.
     * Blocks are 1 MiB-granular (log182: 1179700 was rejected, 1048576 was
     * accepted in the same run), so this asks for one 16 MiB block and then
     * keeps adding 1 MiB blocks while they succeed, reporting the total. Two
     * numbers matter: whether a 16 MiB block succeeds at all, and how many more
     * MiB fit alongside it. Both are measured, not assumed. */
    crumb_printf("[JITPROBE] === PATH C: total VM-domain memory budget ===");
    {
        static const unsigned int MB = 0x100000u;
        static int held[24];
        int nheld = 0, j, total = 0;
        int big = sceKernelAllocMemBlockForVM("j2me_budget16", 16 * MB);
        if (big >= 0) {
            held[nheld++] = big;
            total += 16;
            crumb_printf("[JITPROBE] 16 MiB block OK (documented maximum)");
        } else {
            crumb_printf("[JITPROBE] 16 MiB block FAILED: 0x%08x = %s",
                         (unsigned int)big, vm_err((unsigned int)big));
        }
        for (j = 0; j < 20; j++) {
            int u = sceKernelAllocMemBlockForVM("j2me_budget1", 1 * MB);
            if (u < 0) {
                crumb_printf("[JITPROBE] next 1 MiB block at total %d MiB "
                             "FAILED: 0x%08x = %s", total,
                             (unsigned int)u, vm_err((unsigned int)u));
                break;
            }
            held[nheld++] = u;
            total += 1;
        }
        crumb_printf("[JITPROBE] TOTAL VM-DOMAIN BUDGET = %d MiB across %d "
                     "blocks (the Java heap asked for 32 MiB in log182)",
                     total, nheld);
        for (j = 0; j < nheld; j++) {
            sceKernelFreeMemBlock(held[j]);
        }
        /* The domain is deliberately left OPEN: the heap may need it moments
         * from now, and closing it would also undo the compiler-area plan. */
    }

    crumb_printf("[JITPROBE] VERDICT: no JIT code memory available.");
    crumb_printf("[JITPROBE]   PATH A: PSS needs SCE_SYSMODULE_MONO_BRIDGE, "
                 "and LoadModule answered ENOENT - the PSM runtime is not "
                 "installed on this console.");
    crumb_printf("[JITPROBE]   PATH B: the VM domain is not writable+"
                 "executable (see the last marker above).");
    crumb_printf("[JITPROBE]   NEXT: on a hacked console the remaining route "
                 "is a KERNEL plugin. kubridge is linked but NOT loaded - its "
                 "weak stub returns 0 - so add ur0:tai/kubridge.skprx to "
                 "ur0:tai/config.txt and re-run. kuKernelCpuUnrestrictedMemcpy "
                 "would then give kernel R/W, which is what a page-table patch "
                 "for RWX needs.");
    g_status = -1;
    return 0;
}
/* ========================================================================
 * v04.17: the JIT code area, on the foundation log181 proved works.
 *
 * One VM-domain block, allocated once, domain kept OPEN. A JIT rewrites code
 * constantly (CompiledMethod_arm patches inline caches in place), so it cannot
 * close the domain between writes.
 *
 * Size: 4 MiB. The block minimum on this firmware is 1 MiB (measured, log159)
 * and the maximum is 16 MiB; 4 MiB is 4x the compiler area the port was running
 * with (CompilerAreaPercentage=20 of a ~5 MiB heap area, log181 shows
 * arena=4980K), which leaves room for the JIT to fill up without touching the
 * managed heap at all.
 * ======================================================================== */
#define JIT_AREA_SIZE 0x400000u   /* 4 MiB */

static SceUID  g_area_uid  = -1;
static void   *g_area_base = NULL;
static unsigned int g_area_size = 0;

int vita_jit_code_area_ready(void) {
    return (g_area_base != NULL) ? 1 : 0;
}

void *vita_jit_code_area_base(void) {
    return g_area_base;
}

unsigned int vita_jit_code_area_size(void) {
    return g_area_size;
}

/* Idempotent. Order matters and is the order log181 proved:
 *   OpenVMDomain  -> alloc -> store  (a block created while the domain is
 *   already open is the variant that worked; opening after the alloc returned
 *   0x80010058 in the same run, i.e. "already open")
 */
int vita_jit_code_area_acquire(void) {
    int rc;
    void *base = NULL;

    if (g_area_base != NULL) {
        return 1;
    }

    rc = sceKernelOpenVMDomain();
    crumb_printf("[JITAREA] OpenVMDomain rc=0x%08x%s", (unsigned int)rc,
                 (rc < 0) ? vm_err((unsigned int)rc) : "");

    g_area_uid = sceKernelAllocMemBlockForVM("j2me_jit_code", JIT_AREA_SIZE);
    if (g_area_uid < 0) {
        crumb_printf("[JITAREA] alloc %u bytes failed: 0x%08x = %s",
                     JIT_AREA_SIZE, (unsigned int)g_area_uid,
                     vm_err((unsigned int)g_area_uid));
        g_area_uid = -1;
        return 0;
    }
    if (sceKernelGetMemBlockBase(g_area_uid, &base) < 0 || base == NULL) {
        crumb_printf("[JITAREA] GetMemBlockBase failed");
        sceKernelFreeMemBlock(g_area_uid);
        g_area_uid = -1;
        return 0;
    }

    /* Prove it is really usable before the VM is told to use it: one store at
     * the head and one at the very end. log181 showed both succeed on this
     * firmware while the kernel still reports access=0x4, so the access field
     * is NOT a usable gate - only an actual store is. */
    {
        volatile unsigned int *head = (volatile unsigned int *)base;
        volatile unsigned int *tail =
            (volatile unsigned int *)((char *)base + JIT_AREA_SIZE - 16);
        head[0] = 0xE3A0002Au;    /* mov r0,#42 */
        head[1] = 0xE12FFF1Eu;    /* bx lr       */
        *tail = 0xA5A5A5A5u;
        if (head[0] != 0xE3A0002Au || head[1] != 0xE12FFF1Eu ||
            *tail != 0xA5A5A5A5u) {
            crumb_printf("[JITAREA] write-back mismatch head=%08x,%08x "
                         "tail=%08x - not usable", head[0], head[1], *tail);
            sceKernelFreeMemBlock(g_area_uid);
            g_area_uid = -1;
            return 0;
        }
        head[0] = 0;              /* leave the area clean for the compiler */
        head[1] = 0;
        *tail = 0;
    }

    rc = sceKernelSyncVMDomain(g_area_uid, base, JIT_AREA_SIZE);
    crumb_printf("[JITAREA] SyncVMDomain rc=0x%08x%s", (unsigned int)rc,
                 (rc < 0) ? vm_err((unsigned int)rc) : "");

    g_area_base = base;
    g_area_size = JIT_AREA_SIZE;
    crumb_printf("[JITAREA] JIT code area ready: %p, %u bytes (domain stays "
                 "open for the life of the process)", base, g_area_size);

    /* Execute the head once, so a silently non-executable block is caught now
     * rather than as a game crash later. */
    crumb_printf("[JITAREA] MARKER: executing %p to verify the X bit",
                 base);
    {
        volatile unsigned int *h = (volatile unsigned int *)base;
        int got;
        h[0] = 0xE3A0002Au;
        h[1] = 0xE12FFF1Eu;
        sceKernelSyncVMDomain(g_area_uid, base, 64);
        got = ((int (*)(void))base)();
        crumb_printf("[JITAREA] code area returned %d (want 42)", got);
        h[0] = 0;
        h[1] = 0;
        sceKernelSyncVMDomain(g_area_uid, base, 64);
        if (got != 42) {
            crumb_printf("[JITAREA] writable but NOT executable - unusable");
            sceKernelFreeMemBlock(g_area_uid);
            g_area_base = NULL;
            g_area_size = 0;
            g_area_uid = -1;
            return 0;
        }
    }
    return 1;
}

void vita_jit_flush_icache(void *addr, unsigned int len) {
    if (g_area_base == NULL || addr == NULL || len == 0) {
        return;
    }
    /* The whole point of the block: this is the userspace icache flush that
     * __builtin___clear_cache refuses to emit for this target. */
    sceKernelSyncVMDomain(g_area_uid, addr, len);
}
