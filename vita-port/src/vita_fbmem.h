/*
 * vita_fbmem.h - CDRAM framebuffer block allocator
 *
 * v01.67 real-hw black-screen fix. The menu and the LCDUI layer used to
 * hand sceDisplaySetFrameBuf a memalign()ed heap buffer. That is normal
 * CACHEABLE user memory: on real hardware the Cortex-A9 D-cache keeps
 * the CPU writes in-cache and the display controller reads main memory,
 * so the panel shows whatever was there before the writes - a black
 * screen. Vita3K does not emulate caches, which is why the same build
 * looked fine in the emulator (same reason the pic-veneer boot crash
 * never reproduced there).
 *
 * The official SDK samples (common/debugScreen.c, camera, ime) allocate
 * their framebuffers from CDRAM via sceKernelAllocMemBlock
 * (SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW): that mapping is UNCACHED on
 * the CPU side, so every store is immediately visible to the display
 * controller. This helper wraps that pattern for both framebuffer
 * owners (vita_menu.c menu_fb, vita_display.c vita_fb).
 *
 * CDRAM comes in 256KB granularity; the caller passes the exact byte
 * size and this rounds up. The block stays allocated until
 * vita_fbmem_release() - the menu deliberately keeps its block for the
 * whole process (see vita_menu.c), the display layer frees on finalize.
 */
#ifndef VITA_FBMEM_H
#define VITA_FBMEM_H

#include <psp2/kernel/sysmem.h>

typedef struct VitaFbMem {
    SceUID block;
    void  *base;
} VitaFbMem;

/* Allocate an uncached CDRAM block of `size` bytes (rounded up to the
 * 256KB CDRAM granularity). Returns 0 on success. */
static inline int vita_fbmem_alloc(VitaFbMem *m, unsigned int size)
{
    unsigned int rounded = (size + 0x3FFFF) & ~0x3FFFF;
    m->block = sceKernelAllocMemBlock("j2me_fb",
                                      SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW,
                                      rounded, NULL);
    if (m->block < 0) {
        m->base = NULL;
        return -1;
    }
    if (sceKernelGetMemBlockBase(m->block, &m->base) < 0) {
        sceKernelFreeMemBlock(m->block);
        m->block = -1;
        m->base = NULL;
        return -1;
    }
    return 0;
}

static inline void vita_fbmem_release(VitaFbMem *m)
{
    if (m->block >= 0) {
        sceKernelFreeMemBlock(m->block);
    }
    m->block = -1;
    m->base = NULL;
}

#endif /* VITA_FBMEM_H */
