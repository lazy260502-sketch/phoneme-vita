/*
 * vita_membw.h - framebuffer write-bandwidth probe (v03.45).
 *
 * Why this exists
 * ---------------
 * log140 (v03.41) shows the frame budget split like this, on every line:
 *
 *     [PERF] f=300 fps=9.4 period=105.40 flip=25.55 prepare=17.27
 *     [PERF] f=600 fps=9.1 period=109.38 flip=25.53 prepare=17.12
 *     [PERF] f=900 fps=9.8 period=101.37 flip=25.09 prepare=17.05
 *
 * `prepare` = our own CPU work inside flip_to_display() = letterbox clear +
 * scaled blit + overlays. It is 17.1 ms with a spread of 16.86..18.12 ms
 * across thousands of frames - far too stable to be "the VM thread was
 * preempted", which is what an ALU-bound loop would look like.
 *
 * The numbers that make the hypothesis concrete:
 *   bytes written per frame = 407 x 543 px x 4 B  = 884 KB
 *   prepare                 = 17.1 ms
 *   implied write bandwidth = 884 KB / 17.1 ms    = 52 MB/s
 *
 * 52 MB/s is not a Cortex-A9 ALU limit, it is what UNCACHED memory writes
 * cost. And vita_fbmem.h deliberately allocates the framebuffer from CDRAM
 * with SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW, which is uncached on the CPU
 * side - chosen in v01.67 because a cacheable framebuffer made the real
 * hardware show a black screen (the display controller reads memory, the
 * CPU's dirty lines stay in cache).
 *
 * So "make the framebuffer cacheable" is NOT an option: that is exactly the
 * v01.67 bug. What CAN help is changing HOW the uncached writes are issued:
 * a 32-bit store is one bus transaction per pixel, whereas a 64-bit store
 * halves the transaction count, and a library memcpy issues LDM/STM bursts.
 *
 * This probe measures those variants on the real hardware instead of
 * guessing, and prints what a full frame would cost with each. It is
 * measurement only - it writes into the back framebuffer BEFORE the first
 * memset in lfjport_ui_init(), so nothing visible is disturbed.
 */

#ifndef VITA_MEMBW_H
#define VITA_MEMBW_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Run the measurement once. `fb` must be a writable WxH*4 byte block of
 * UNCACHED memory that is not currently being scanned out (the back
 * buffer). `src565` is any readable RGB565 buffer of at least `src_pixels`
 * pixels (the virtual screen). `frame_bytes` is the real per-frame write
 * size so the report can translate MB/s into ms/frame.
 *
 * Returns non-zero if it ran. Cost: one pass over `bytes` per variant, a
 * few times over - about 60 ms on hardware, once per process. */
int vita_membw_probe(uint32_t *fb, int bytes,
                     const uint16_t *src565, int src_pixels,
                     int frame_bytes);

/* Last results, for the on-screen diagnostics overlay. Any pointer may be
 * NULL. Values are tenths of MB/s; 0 means "not measured". */
void vita_membw_get(unsigned int *fill32, unsigned int *fill64,
                    unsigned int *copy_lib_cached,
                    unsigned int *copy_lib_uncached,
                    unsigned int *conv_cached, unsigned int *conv_uncached,
                    unsigned int *frame_us_32b, unsigned int *frame_us_64b);

#ifdef __cplusplus
}
#endif

#endif /* VITA_MEMBW_H */
