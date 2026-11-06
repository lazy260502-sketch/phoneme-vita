/*
 * vita_display.c - PS Vita LCDUI port layer (lfjport) for phoneME MIDP
 *
 * Replaces phoneme-midp's lfjport_fb_export.o (whose lfjport_refresh was a
 * TODO stub). The CMake build deletes that member from libobj_no_main.a and
 * links this file instead, so every symbol exported by the old object must
 * be defined here:
 *   gxj_system_screen_buffer + 21 lfjport_* functions (see lfjport_export.h).
 *
 * Rendering model: MIDP paints (both chameleon high-level UI and Canvas
 * graphics) into gxj_system_screen_buffer (RGB565). lfjport_refresh() blits
 * that virtual phone screen (240x320) scaled, centered, onto the Vita's
 * 960x544 framebuffer and flips it with sceDisplaySetFrameBuf.
 */

#include <kni.h>
#include <psp2/display.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/gxm.h>
#include <psp2/io/fcntl.h>
#include <sys/time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <gxj_putpixel.h>
#include <lfjport_export.h>

#include "vita_fbmem.h"
#include "vita_ime.h"
#include "vita_gxm.h"
#include "vita_vkey.h"
#include "vita_shot.h" /* v02.92: screenshot capture */
#include "vita_perf.h" /* v02.97: frame-time accounting (P1-3) */
#include "vita_splash.h" /* v02.99: startup loading screen (P1-2) */
#include "vita_membw.h"  /* v03.45: framebuffer write-bandwidth probe */
#include "vita_diag.h"    /* v03.70: on-screen diagnostics overlay (P1-8) */

/* ------------------------------------------------------------------ */
/* Virtual phone screen (what MIDP games see) vs Vita physical screen.
 *
 * v02.82: the virtual screen is no longer fixed at 240x320 / 320x240.
 * Games are written for many resolutions (128x160, 176x208, 176x220,
 * 240x320, 320x240, 360x640, 480x800, ...) and a game that asks for
 * getWidth()/getHeight() lays itself out from that number. Feeding
 * everyone 240x320 leaves the exception screen small, clips fixed-size
 * UIs and offsets touch input. The launcher now reads per-game settings
 * (game.cfg next to game.jar) and calls vita_display_set_size() before
 * the VM starts.
 *
 * The buffer is static (no allocation failure path before the VM starts)
 * and sized for the largest supported virtual screen: 800x800. That is
 * 640000 * (2 + 1) bytes = 1.9 MB of .bss - the same order as the two
 * CDRAM framebuffers the display already owns. */
#define VITA_MAX_W 800
#define VITA_MAX_H 800

#ifndef VITA_DEFAULT_LANDSCAPE
#define VITA_DEFAULT_LANDSCAPE 0
#endif

#define VITA_PHYS_W 960
#define VITA_PHYS_H 544

/* Scale modes (vita_display_set_scale_mode). */
#define VITA_SCALE_FIT     0  /* keep aspect ratio, centered (default) */
#define VITA_SCALE_STRETCH 1  /* fill 960x544, ignore aspect ratio       */
#define VITA_SCALE_INTEGER 2  /* largest integer multiple, centered     */

static int virt_w = VITA_DEFAULT_LANDSCAPE ? 320 : 240;
static int virt_h = VITA_DEFAULT_LANDSCAPE ? 240 : 320;
static int scale_mode = VITA_SCALE_FIT;

/* Static buffers for the system screen buffer, sized for the largest
 * supported virtual screen (see above). */
static gxj_pixel_type vita_screen_pixels[VITA_MAX_W * VITA_MAX_H];
static gxj_alpha_type vita_screen_alpha[VITA_MAX_W * VITA_MAX_H];

gxj_screen_buffer gxj_system_screen_buffer = {
    VITA_DEFAULT_LANDSCAPE ? 320 : 240,
    VITA_DEFAULT_LANDSCAPE ? 240 : 320,
    vita_screen_pixels,
    vita_screen_alpha
};

/* v04.10: where blit_scaled reads the virtual screen from. Normally the live buffer; the keypad-flash repaint points
 * it at the last finished frame (see vita_display_vkey_tick). */
static const gxj_pixel_type *g_blit_src = vita_screen_pixels;

/* Defined below; the orientation wrapper needs it. */
void vita_display_set_size(int w, int h);

/* Switch the virtual screen orientation. Must be called before the VM
 * starts (the Java UI layer reads the size once at init). Kept for the
 * callers that only know portrait/landscape; set_size() is the general
 * form. */
void vita_display_set_orientation(int landscape) {
    if (landscape) {
        vita_display_set_size(320, 240);
    } else {
        vita_display_set_size(240, 320);
    }
}

/* Set an arbitrary virtual screen size. Clamped to the static buffer and
 * to something sane: the midp UI layer cannot work with 0 or negative
 * dimensions. Called before lfjport_ui_init(), i.e. before the Java UI
 * reads the size. */
void vita_display_set_size(int w, int h) {
    if (w <= 0 || h <= 0) {
        return;
    }
    if (w > VITA_MAX_W) {
        w = VITA_MAX_W;
    }
    if (h > VITA_MAX_H) {
        h = VITA_MAX_H;
    }
    /* A game asking for a larger portrait screen than the buffer allows
     * (e.g. 480x800 fits, 1024x1024 does not) keeps the aspect ratio by
     * scaling the request down rather than cropping it. */
    if ((long)w * h > (long)VITA_MAX_W * VITA_MAX_H) {
        int s = 1;
        while ((long)(w / (s + 1)) * (h / (s + 1)) >=
               (long)VITA_MAX_W * VITA_MAX_H) {
            s++;
        }
        w /= s;
        h /= s;
    }
    virt_w = w;
    virt_h = h;
    gxj_system_screen_buffer.width = virt_w;
    gxj_system_screen_buffer.height = virt_h;
}

/* Computed scaling: virtual screen scaled to fit physical, centered.
 * Declared here because vita_display_get_rect() (used before the first
 * frame) reports them. */
static int dst_w, dst_h, dst_x, dst_y;
/* Fixed point 16.16 step per destination pixel. */
static int step_x, step_y;

void vita_display_set_scale_mode(int mode) {
    if (mode < VITA_SCALE_FIT || mode > VITA_SCALE_INTEGER) {
        mode = VITA_SCALE_FIT;
    }
    scale_mode = mode;
}

int vita_display_get_width(void) {
    return virt_w;
}

int vita_display_get_height(void) {
    return virt_h;
}

/* v02.85: the virtual keypad needs to know where the game rectangle is
 * so it can use the letterbox bars instead of covering the game. */
void vita_display_get_rect(int *x, int *y, int *w, int *h) {
    if (x != NULL) *x = dst_x;
    if (y != NULL) *y = dst_y;
    if (w != NULL) *w = dst_w;
    if (h != NULL) *h = dst_h;
}

/* Physical framebuffer (A8B8G8R8, 4 bytes/pixel), allocated in ui_init. */
static uint32_t *vita_fb = NULL;
/* v01.67 real-hw black-screen fix: CDRAM (uncached) block behind vita_fb.
 * A cached memalign buffer is invisible to the display controller on
 * real hardware - see vita_fbmem.h.
 * v01.70: DOUBLE BUFFERED - one CDRAM block holds two frames; the blit
 * writes the back buffer and flip_to_display() submits it with
 * NEXTFRAME and swaps. Single-buffered the blit raced the scanout
 * (flicker/tearing). */
static VitaFbMem vita_fb_blk = { -1, NULL };
static uint32_t *vita_fb_front = NULL;
static uint32_t *vita_fb_back = NULL;
static int display_ready = 0;
/* v02.93: the screenshot banner straddles the letterbox bars, which the
 * scaled blit never writes. Track whether the previous frame painted it
 * so the first frame after it expires can wipe the bars (otherwise a
 * ghost of the banner stays in the black margins forever). */
static int g_toast_was = 0;
/* v03.01: 1 while the frame on the panel still carries the banner. Set by
 * whichever thread submitted the last frame, read by the screenshot poller
 * to decide whether the notice has to be taken off by itself. */
static volatile int g_toast_on_panel = 0;

/* v02.99: the splash overlay covers the WHOLE panel, but the scaled blit
 * only writes the game rectangle. So the moment the splash ends, the bars
 * still hold its last frame - the title, the stage line and the tail of the
 * progress bar - and nothing ever overwrites them, because clear_letterbox()
 * only runs for the IME and the screenshot banner. Same edge-detection as
 * g_toast_was: wipe the bars on the first frame after the splash leaves.
 * g_splash_own_flip marks the repaints the splash itself asked for (those
 * are not the MIDlet drawing).
 * v03.05: the wipe is a COUNTDOWN OF TWO, not a single frame. There are two
 * CDRAM buffers (front = scanning out, back = being drawn) and every flip
 * swaps them, so one cleared frame leaves the other buffer's bars full of
 * the old overlay. The next flip then swaps THAT buffer in and the loading
 * UI is back - then gone - then back: the "一闪一闪" the user reported
 * after the game had already started. g_post_ime_clear (v02.65) has always
 * used 2 for exactly this reason; the splash and the banner simply never
 * got the same treatment. */
static int g_splash_was = 0;
static int g_splash_own_flip = 0;
static int g_splash_clear = 0;   /* frames that still owe a bar wipe */
static int g_toast_clear = 0;    /* same, for the screenshot banner */
/* v02.64: time of the last submitted frame (see vita_display_ime_tick). */
static unsigned long long g_last_flip_us = 0;

/* v02.92: the frame currently on the panel (front buffer). Used by the
 * screenshot poller when the VM thread has stopped flipping, so a frozen
 * screen can still be captured. NULL before the display is up. */
const unsigned int *vita_display_get_front(int *w, int *h) {
    if (w != NULL) *w = VITA_PHYS_W;
    if (h != NULL) *h = VITA_PHYS_H;
    return vita_fb_front;
}

/* v02.93: repaint the frame that is already on the panel with the
 * screenshot banner on top, then submit it once. Called by the shot
 * poller after a frozen-VM capture - in that state the VM thread is not
 * flipping, so nobody else would ever paint the "saved" notice. It only
 * ever writes the FRONT buffer and re-submits the same address, so it
 * cannot disturb the blit/swap pair the VM thread uses; at worst one
 * frame shows a half-painted banner if the VM wakes up mid-write. */
void vita_display_toast_flip(void) {
    SceDisplayFrameBuf fb;
    if (!display_ready || vita_fb_front == NULL) {
        return;
    }
    if (vita_ime_is_active()) {
        return;      /* the dialog owns the display right now */
    }
    /* v02.99: while the loading screen is up there is nothing worth
     * capturing or announcing, and re-submitting a banner frame would
     * count as a "foreign" frame and end the splash prematurely. */
    if (vita_splash_owns_screen()) {
        return;
    }
    vita_shot_draw_toast(vita_fb_front, VITA_PHYS_W, VITA_PHYS_H);
    /* The banner now lives in the front buffer, i.e. outside the blit
     * path; remember it so the next normal frame wipes the bars. */
    g_toast_was = 1;
    __atomic_store_n(&g_toast_on_panel, 1, __ATOMIC_RELEASE);
    memset(&fb, 0, sizeof(fb));
    fb.size = sizeof(SceDisplayFrameBuf);
    fb.base = vita_fb_front;
    fb.pitch = VITA_PHYS_W;
    fb.pixelformat = SCE_DISPLAY_PIXELFORMAT_A8B8G8R8;
    fb.width = VITA_PHYS_W;
    fb.height = VITA_PHYS_H;
    sceDisplaySetFrameBuf(&fb, SCE_DISPLAY_SETBUF_NEXTFRAME);
}

/* v03.01: 1 while the frame on the panel still carries the banner. */
int vita_display_toast_on_panel(void) {
    return __atomic_load_n(&g_toast_on_panel, __ATOMIC_ACQUIRE);
}

/* v02.93: time of the last submitted frame. The screenshot poller compares
 * it against the save time to decide whether the VM is repainting - if it
 * is not, the banner needs a frame of its own. */
unsigned long long vita_display_last_flip_us(void) {
    return __atomic_load_n(&g_last_flip_us, __ATOMIC_RELAXED);
}

/* v02.65: letterbox hygiene around the system keyboard. The blit only writes
 * the scaled MIDlet rectangle; the bars around it were never touched. The
 * keyboard is full-screen wide, so whatever it painted into the bars stayed
 * there for good ("overlaps the UC screen", log111). g_was_ime = the last
 * frame went through the dialog path; g_post_ime_clear = number of normal
 * frames that still have to wipe the bars (one per CDRAM buffer). */
static int g_was_ime = 0;
static int g_post_ime_clear = 0;

/* Computed scaling: virtual screen scaled to fit physical, centered. */
static void blit_run_build(void);   /* v03.60, defined below near blit */
static void compute_scaling(void) {
    if (scale_mode == VITA_SCALE_STRETCH) {
        /* Fill the panel; the fixed-point steps below still resample. */
        dst_w = VITA_PHYS_W;
        dst_h = VITA_PHYS_H;
    } else {
        /* Fit the virtual screen inside 960x544 keeping the aspect
         * ratio: scale = min(PHYS_W/VIRT_W, PHYS_H/VIRT_H).
         * v02.82: INTEGER mode uses the same expression but truncates
         * the scale to a whole multiple, so every virtual pixel becomes
         * an n x n block (no resampling blur). */
        int sx = (VITA_PHYS_W << 12) / virt_w;
        int sy = (VITA_PHYS_H << 12) / virt_h;
        int scale = (sx < sy) ? sx : sy;
        if (scale_mode == VITA_SCALE_INTEGER) {
            scale &= ~0xFFFu;      /* whole multiples only (16.16 -> 1x) */
            if (scale == 0) {
                scale = 0x1000;    /* 1x: the virtual screen is bigger
                                    * than the panel in one dimension */
            }
        }
        dst_w = (virt_w * scale) >> 12;
        dst_h = (virt_h * scale) >> 12;
    }
    if (dst_w > VITA_PHYS_W) dst_w = VITA_PHYS_W;
    if (dst_h > VITA_PHYS_H) dst_h = VITA_PHYS_H;
    dst_x = (VITA_PHYS_W - dst_w) / 2;
    dst_y = (VITA_PHYS_H - dst_h) / 2;
    step_x = (virt_w << 16) / dst_w;
    step_y = (virt_h << 16) / dst_h;
    blit_run_build();
}

/* Convert RGB565 to 0xAABBGGRR (SCE_DISPLAY_PIXELFORMAT_A8B8G8R8). */
static inline uint32_t rgb565_to_abgr(gxj_pixel_type p) {
    uint32_t r5 = (p >> 11) & 0x1F;
    uint32_t g6 = (p >> 5) & 0x3F;
    uint32_t b5 = p & 0x1F;
    uint32_t r = (r5 << 3) | (r5 >> 2);
    uint32_t g = (g6 << 2) | (g6 >> 4);
    uint32_t b = (b5 << 3) | (b5 >> 2);
    return 0xFF000000u | (b << 16) | (g << 8) | r;
}

/* ---- v03.60: why blit_scaled() was rewritten -------------------------
 *
 * log154 measured the split of `prepare` for the first time:
 *
 *     [PERF] seg(n=300) blit=14.69 ltbox=0.00 vkey=2.41 shot=0.00 ... of 17.11
 *
 * so 86% of our own per-frame CPU time is this one loop. The same log's
 * bandwidth probe (`v_conv565`, a plain sequential rgb565->abgr conversion)
 * ran at 25.0 ns/pixel on the same hardware, i.e. 5.52 ms extrapolated to
 * the 407x543 game rectangle - but the real blit took 14.69 ms, 2.8x slower.
 *
 * The reason is in the disassembly: gcc AUTO-VECTORISED the probe
 * (vld2.16 / vmovl.u16 / vshll.u16, a whole vector of pixels per iteration)
 * and left blit_scaled as 17 scalar instructions per pixel, because its
 * source index was `src_row[sx >> 16]` - a GATHER. ARMv7 NEON has no gather
 * instruction, so the vectoriser cannot touch that loop, and the dependent
 * `sx += step_x; sx >> 16` chain plus the strided loads also defeat the
 * prefetcher.
 *
 * The two-pass shape below keeps the exact same pixel mapping (so no image
 * can shift) while giving the vectoriser a shape it can handle:
 *
 *   pass 1  convert the source row into a small staging buffer, sequentially.
 *           This is byte-for-byte the probe's loop, so it runs at ~25 ns/px
 *           and there are only virt_w of them (240) instead of dst_w (407).
 *   pass 2  write each converted value into its destination run. The inner
 *           loop is a fill of one repeated word - vectorisable - and reads
 *           only the staging buffer, which is L1-hot (<= 3.2 KB).
 *
 * blit_run[] holds, per source column s, the first destination column that
 * maps to it. That is exactly ceil(s * 65536 / step_x), which partitions the
 * destination row without changing any pixel: for integer x,
 *   floor(x*step_x/65536) == s  <=>  ceil(s*65536/step_x) <= x < ceil((s+1)*65536/step_x)
 * Downscaling leaves some runs empty (blit_run[s] == blit_run[s+1]) and the
 * fill loop simply does nothing for them. */

#define BLIT_STAGE_MAX VITA_MAX_W   /* one converted source row */

/* One converted source row. Static (BSS) rather than stack: 800*4 = 3.2 KB
 * would be a large frame for a function the VM thread calls every frame. */
static uint32_t blit_stage[BLIT_STAGE_MAX];
/* First destination column of each source column, plus a terminator. */
static int blit_run[BLIT_STAGE_MAX + 1];
static int blit_run_valid = 0;

static void blit_run_build(void) {
    int s;
    if (virt_w <= 0 || dst_w <= 0 || step_x <= 0 || virt_w > BLIT_STAGE_MAX) {
        blit_run_valid = 0;
        return;
    }
    for (s = 0; s < virt_w; s++) {
        /* ceil(s * 65536 / step_x) */
        blit_run[s] = (int)(((long long)s * 65536ll + step_x - 1) / step_x);
        if (blit_run[s] > dst_w) {
            blit_run[s] = dst_w;
        }
    }
    blit_run[virt_w] = dst_w;
    blit_run_valid = 1;
}

/*
 * Map a physical (960x544) touch coordinate back to the virtual J2ME
 * screen (240x320 portrait / 320x240 landscape), i.e. invert the
 * centered/letterboxed scaling done by compute_scaling().
 * Returns 0 when the touch is inside the virtual screen, -1 when it
 * lands in the letterbox (out of range; caller should ignore it).
 */
int vita_display_map_touch(int px, int py, int *vx, int *vy) {
    if (vx == NULL || vy == NULL) return -1;
    if (px < dst_x || px >= dst_x + dst_w ||
        py < dst_y || py >= dst_y + dst_h) {
        return -1;
    }
    *vx = (px - dst_x) * virt_w / dst_w;
    *vy = (py - dst_y) * virt_h / dst_h;
    if (*vx < 0) *vx = 0;
    if (*vx >= virt_w) *vx = virt_w - 1;
    if (*vy < 0) *vy = 0;
    if (*vy >= virt_h) *vy = virt_h - 1;
    return 0;
}

/* v02.44: white-screen measurement (marks go to boot_timing.log via
 * vita_timing_markf in vita_main.c). The FIRST flip can itself be the white
 * screen's beginning (an app may submit a blank frame before painting), so
 * "first_flip" alone cannot say when the white screen ends. frame_has_
 * content() samples ~2048 pixels of the virtual screen and reports whether
 * at least 8 differ from pure white (0xFFFF in RGB565); "first_content_flip"
 * is the first such frame. A black or coloured frame counts as content, as
 * it is not a white screen. Only the first 900 flips of a round are
 * checked, so a white-background app costs nothing afterwards. */
extern void vita_timing_markf(const char *fmt, ...);
static int g_flip_n = 0;
static int g_content_found = 0;

void vita_display_timing_reset(void) {
    g_flip_n = 0;
    g_content_found = 0;
}

static int frame_has_content(void) {
    int total = virt_w * virt_h;
    int step = total / 2048;
    int i, diff = 0;

    if (step < 1) {
        step = 1;
    }
    for (i = 0; i < total; i += step) {
        if (vita_screen_pixels[i] != 0xFFFF && ++diff >= 8) {
            return 1;
        }
    }
    return 0;
}

/* ---- v02.53: GXM + CommonDialog bridge for the system IME ----
 * sceImeDialog is built on the CommonDialog framework, which renders
 * through GXM into a render target the app supplies. This port draws with
 * sceDisplaySetFrameBuf (software blit, no GXM), so GXM was never
 * initialized. CommonDialog needs:
 *   1. sceGxmInitialize (one-time; otherwise the dialog errors with
 *      SCE_COMMON_DIALOG_ERROR_GXM_IS_UNINITIALIZED 0x80020436)
 *   2. sceCommonDialogUpdate every frame with renderTarget = back buffer
 * The vsync callback is NULL on purpose: the port keeps submitting with
 * sceDisplaySetFrameBuf (a GXM display-queue callback would double-submit).
 * CommonDialog only paints into the buffer; flip_to_display() submits it.
 * GXM init lives in vita_gxm.c and is called from vita_ime_show() BEFORE
 * sceImeDialogInit (v02.54: doing it here caused a chicken-and-egg deadlock
 * - see log104, Init rc=0x80020436).
 *
 * v02.59 (log106): Init rc=0 / poll status=1 (the dialog logically came up)
 * but the keyboard never rendered and the frame froze (watchdog #3
 * vm=RUNNING sw+0 site+0 jsmp+0 since_ms=6081). CommonDialog renders
 * THROUGH GXM, which needs two things the port never provided:
 *   1. sceGxmMapMemory on the CDRAM framebuffer - GXM cannot touch CPU
 *      memory it has not been given; without it the render is invisible.
 *   2. a real SceGxmSyncObject in displaySyncObject - NULL gives the
 *      dialog's render nothing to signal, and the render/submit race
 *      (no GPU sync) stalls the VM thread.
 * The official sample (samples/ime) calls sceGxmMapMemory right after the
 * CDRAM alloc and passes a valid sync object every frame. */
/* v02.71: remember WHICH block was mapped, not just "a block was mapped".
 * log114: each MIDlet round allocates a fresh CDRAM block (round 0
 * 0x61400000, round 1 0x62800000) but the old boolean was never cleared, so
 * round 1 skipped sceGxmMapMemory and the dialog rendered into an unmapped
 * block -> GPU crash (psp2core ...-GPUCRASH, GPU_INFO holds 0x62b2a024 inside
 * the unmapped block; "[CD] map fb" appears once in the whole log). */
static void *vita_fb_gxm_mapped_base = NULL;
static unsigned int vita_fb_gxm_mapped_size = 0;

static void vita_cd_unmap_fb(void) {
    if (vita_fb_gxm_mapped_base != NULL) {
        /* Let the display queue drop every reference first. */
        sceGxmDisplayQueueFinish();
        int rc = sceGxmUnmapMemory(vita_fb_gxm_mapped_base);
        fprintf(stderr, "[CD] unmap fb base=%p rc=0x%08x\n",
                vita_fb_gxm_mapped_base, rc);
        vita_fb_gxm_mapped_base = NULL;
        vita_fb_gxm_mapped_size = 0;
    }
}

static void vita_cd_ensure_resources(void) {
    /* v02.63: only the GXM memory mapping lives here now. The sync object
     * and the CommonDialog render+submit moved into vita_gxm_dialog_frame()
     * (vita_gxm.c), which needs a fully working GXM display queue. */
    if (vita_fb_blk.base != NULL && vita_fb_blk.base != vita_fb_gxm_mapped_base) {
        unsigned int bytes = VITA_PHYS_W * VITA_PHYS_H * sizeof(uint32_t) * 2;
        unsigned int rounded = (bytes + 0x3FFFF) & ~0x3FFFF;
        int rc;
        /* A previous round's block (already freed, or about to be): drop
         * its mapping before mapping the new one. */
        vita_cd_unmap_fb();
        rc = sceGxmMapMemory(vita_fb_blk.base, rounded,
                             SCE_GXM_MEMORY_ATTRIB_READ |
                             SCE_GXM_MEMORY_ATTRIB_WRITE);
        fprintf(stderr, "[CD] map fb base=%p bytes=%u rc=0x%08x\n",
                vita_fb_blk.base, rounded, rc);
        if (rc >= 0) {
            vita_fb_gxm_mapped_base = vita_fb_blk.base;
            vita_fb_gxm_mapped_size = rounded;
        }
    }
}

/* v03.01: nearest-neighbour copy of the virtual screen into `dst`
 * (960x544 A8B8G8R8), inside the computed game rectangle. Shared by the
 * normal flip and by the banner clean-up repaint below. */
static void blit_scaled(uint32_t *dst) {
    int x, y, s;

    if (!blit_run_valid) {
        /* Fallback: the original one-pass gather. Kept because a geometry
         * that does not fit the staging buffer (virt_w > VITA_MAX_W, which
         * cannot happen through vita_display_set_size, but this function is
         * also reachable from the screenshot path) must still draw something
         * correct rather than nothing. */
        for (y = 0; y < dst_h; y++) {
            const gxj_pixel_type *src_row =
                &g_blit_src[((y * step_y) >> 16) * virt_w];
            uint32_t *dst_row = &dst[(dst_y + y) * VITA_PHYS_W + dst_x];
            int sx = 0;
            for (x = 0; x < dst_w; x++) {
                dst_row[x] = rgb565_to_abgr(src_row[sx >> 16]);
                sx += step_x;
            }
        }
        return;
    }

    if (step_x >= 0x10000) {
        /* v03.60: DOWNSCALING (destination narrower than the virtual screen).
         * Every destination column needs its own source pixel, so there is
         * nothing to share between neighbours and the run table would be
         * almost entirely empty - converting a whole virtual row first would
         * cost MORE than the columns actually used. The host benchmark
         * measured exactly that (2-pass 0.56x, i.e. 1.8x SLOWER, for a
         * 480x800 virtual screen). So keep the original gather here: no
         * speedup, but no regression either. */
        for (y = 0; y < dst_h; y++) {
            const gxj_pixel_type *src_row =
                &g_blit_src[((y * step_y) >> 16) * virt_w];
            uint32_t *dst_row = &dst[(dst_y + y) * VITA_PHYS_W + dst_x];
            int sx = 0;
            for (x = 0; x < dst_w; x++) {
                dst_row[x] = rgb565_to_abgr(src_row[sx >> 16]);
                sx += step_x;
            }
        }
        return;
    }

    for (y = 0; y < dst_h; y++) {
        const gxj_pixel_type *src_row =
            &g_blit_src[((y * step_y) >> 16) * virt_w];
        uint32_t *dst_row = &dst[(dst_y + y) * VITA_PHYS_W + dst_x];

        /* pass 1: convert the whole source row sequentially. This is the
         * shape gcc turns into NEON (see the comment above blit_stage). */
        for (s = 0; s < virt_w; s++) {
            blit_stage[s] = rgb565_to_abgr(src_row[s]);
        }

        /* pass 2: expand into the destination runs. The inner loop writes
         * one repeated word; bailing out when the run is empty keeps a
         * geometry with any empty runs from costing a loop entry. */
        for (s = 0; s < virt_w; s++) {
            int x0 = blit_run[s];
            int x1 = blit_run[s + 1];
            uint32_t c;
            if (x0 >= x1) {
                continue;
            }
            c = blit_stage[s];
            for (x = x0; x < x1; x++) {
                dst_row[x] = c;
            }
        }
    }
}

/* v02.65: paint everything OUTSIDE the scaled MIDlet rectangle opaque black.
 * Rows above/below the rectangle in full, left/right bars row by row.
 * v02.69: memset per run instead of a pixel loop - this ran up to 62 times a
 * second on the VM thread (log113: 16-32 fps while the keyboard was up). */
static void clear_letterbox(uint32_t *fb) {
    /* v02.71: the fill byte is 0x00. v02.69 turned the per-pixel loop into
     * memset(..., 0xFF, ...), but memset writes the SAME byte into all four
     * channels: 0xFF gives 0xFFFFFFFF = opaque WHITE, so the bars went white
     * whenever the keyboard was up (log114). 0x00 is black, the same value
     * lfjport_ui_init() uses for the bars, and what the SDK sample's
     * memset(data, 0xff000000, ...) really writes (the int is truncated to
     * the byte 0x00). The display ignores alpha. */
    const size_t row_bytes = (size_t)VITA_PHYS_W * sizeof(uint32_t);
    int y;
    for (y = 0; y < VITA_PHYS_H; y++) {
        uint32_t *row = &fb[y * VITA_PHYS_W];
        if (y < dst_y || y >= dst_y + dst_h) {
            memset(row, 0x00, row_bytes);
        } else {
            if (dst_x > 0) {
                memset(row, 0x00, (size_t)dst_x * sizeof(uint32_t));
            }
            if (dst_x + dst_w < VITA_PHYS_W) {
                memset(&row[dst_x + dst_w], 0x00,
                       (size_t)(VITA_PHYS_W - dst_x - dst_w) * sizeof(uint32_t));
            }
        }
    }
}

/* v03.01: take an expired banner off the panel when nobody else will.
 * Symptom this fixes: "提示显示完成后，没有 clear 吗，有时候还能看到绿色框".
 * The cleanup inside flip_to_display() only runs on the first frame AFTER
 * the banner, and a static screen (pause menu) or a frozen VM never sends
 * one - so the last submitted frame kept the green box for good.
 * This repaints the *visible* buffer in place (black bars, fresh game
 * rectangle, keypad, camera button, no banner) and submits that same
 * address once. It never touches the back buffer or the swap pair, and
 * the shot poller only calls it after 200 ms without a flip, so a live
 * MIDlet still owns its own frames. */
void vita_display_toast_clear(void) {
    SceDisplayFrameBuf fb;

    if (!display_ready || vita_fb_front == NULL || vita_ime_is_active()) {
        return;
    }
    if (vita_splash_owns_screen()) {
        return;      /* the loading screen covers everything anyway */
    }
    if (!__atomic_load_n(&g_toast_on_panel, __ATOMIC_ACQUIRE)) {
        return;      /* a clean frame is already on the panel */
    }

    /* Order mirrors flip_to_display: bars first, then the game, then the
     * overlays that live on top of both. */
    clear_letterbox(vita_fb_front);
    blit_scaled(vita_fb_front);
    vita_vkey_draw(vita_fb_front, dst_x, dst_y, dst_w, dst_h);
    vita_shot_draw_button(vita_fb_front, VITA_PHYS_W, VITA_PHYS_H);

    memset(&fb, 0, sizeof(fb));
    fb.size = sizeof(SceDisplayFrameBuf);
    fb.base = vita_fb_front;
    fb.pitch = VITA_PHYS_W;
    fb.pixelformat = SCE_DISPLAY_PIXELFORMAT_A8B8G8R8;
    fb.width = VITA_PHYS_W;
    fb.height = VITA_PHYS_H;
    sceDisplaySetFrameBuf(&fb, SCE_DISPLAY_SETBUF_NEXTFRAME);
    sceDisplayWaitVblankStart();

    /* The panel is clean now and this frame counts as a submit. */
    __atomic_store_n(&g_toast_on_panel, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&g_last_flip_us, sceKernelGetProcessTimeWide(),
                     __ATOMIC_RELAXED);
    g_toast_was = 0;
    /* v03.05: this call cleaned the FRONT buffer only; the back buffer still
     * carries the banner in its bars, so the next two flips have to wipe it
     * or the banner flashes back in on the frame after this one. */
    g_toast_clear = 2;
}

static void flip_to_display(void) {
    SceDisplayFrameBuf fb;
    int rc;
    uint32_t *tmp;
    int toast_drawn = 0;
    /* v02.97: frame accounting (P1-3). t0 = entry, t_prep = the point
     * right before the submit: everything before that is our own CPU
     * work (letterbox, scaled blit, overlays), everything after it is
     * the display queue and sceDisplayWaitVblankStart. The split is
     * what tells us whether a slow frame is ours or the vsync. */
    unsigned long long t0, t_prep;
    unsigned int prepare_us, total_us;
    /* v03.59: the prepare split. Separated from t0/t_prep so that every
     * segment boundary is one clock read on the SAME thread - the whole
     * point is to subtract them from `prepare` and see what is left over
     * (log153: prepare=17.1 ms while the conversion loop alone measured
     * 5.24 ms, so 12 ms was somewhere else inside this function). */
    unsigned long long ts;
    VitaFrameSeg seg;

    if (vita_fb == NULL) {
        return;
    }

    t0 = sceKernelGetProcessTimeWide();
    memset(&seg, 0, sizeof(seg));

    /* v04.51: a foreign flip ends the splash even when we cannot see content.
     *
     * This used to require `spl_preload_done`, which NOBODY EVER SET: the
     * preload sweep that was supposed to call vita_splash_preload_done() no
     * longer exists, so vita_splash_on_foreign_flip() returned early forever
     * and the splash could only ever leave via the 30 s hard timeout. For a
     * 3D title that is 30 seconds of dead weight at 6 ms/frame, because
     * frame_has_content() below samples vita_screen_pixels and Graphics3D
     * games never write that buffer - so the OTHER exit path could not fire
     * either.
     *
     * Measured in log191 (极品飞车, 7.4 fps): `[PERF] seg ... splash=5.96`
     * every single frame, and prepare=17.2 ms of a 135 ms frame. That 6 ms was
     * us painting a loading screen over a game that was already drawing.
     *
     * New rule, and it needs no knowledge of content: once the MIDlet asks for
     * frames at all, the splash has done its job. A flip we did not request
     * (g_splash_own_flip) IS that signal. The only thing to keep is a short
     * grace period so the initial warm-up flips do not tear the splash away
     * before it is ever visible. */
    if (!g_splash_own_flip) {
        vita_splash_on_foreign_flip();
    }

    /* Blit scaled virtual screen into the BACK framebuffer.
     * Nearest neighbour via fixed point steps. */
    blit_scaled(vita_fb);
    ts = sceKernelGetProcessTimeWide();
    seg.blit_us = (unsigned int)(ts - t0);

    /* v02.85: virtual keypad, after the blit so it sits on top of the
     * game. It uses the letterbox bars when they are wide enough (the
     * common case - a 4:3 or 3:4 game on a 16:9 panel), so normally it
     * costs no game pixels. Skipped while the system keyboard is up:
     * the dialog owns the screen and the buttons would be unreachable
     * anyway. */
    /* v02.93: one frame after the screenshot banner goes away, clean the
     * bars it was drawn over. Must run BEFORE the keypad/button on this
     * frame, or their pixels would be wiped too. */
    {
        int toast_now = vita_shot_toast_active();
        int splash_now = vita_splash_active();
        if (g_toast_was && !toast_now) {
            /* v03.05: two frames, one per CDRAM buffer (see the
             * g_splash_clear comment). */
            g_toast_clear = 2;
        }
        g_toast_was = toast_now;
        /* v02.99: the splash painted the whole panel, the blit above only
         * covered the game rectangle -> wipe what it left in the bars.
         * Before the keypad/button draw so their pixels survive.
         * v03.05: 2 frames so BOTH buffers lose the overlay, otherwise the
         * game flickers: clean bars, stale bars, clean bars, stale bars. */
        if (g_splash_was && !splash_now) {
            g_splash_clear = 2;
        }
        g_splash_was = splash_now;
        if (g_splash_clear > 0 || g_toast_clear > 0) {
            clear_letterbox(vita_fb);
            if (g_splash_clear > 0) {
                g_splash_clear--;
            }
            if (g_toast_clear > 0) {
                g_toast_clear--;
            }
        }
    }
    {
        unsigned long long now = sceKernelGetProcessTimeWide();
        seg.letterbox_us = (unsigned int)(now - ts);
        ts = now;
    }
    if (!vita_ime_is_active()) {
        vita_vkey_draw(vita_fb, dst_x, dst_y, dst_w, dst_h);
        /* v02.92: the camera button lives in the same left bar, just
         * below the virtual left soft key. */
        vita_shot_draw_button(vita_fb, VITA_PHYS_W, VITA_PHYS_H);
    }
    {
        unsigned long long now = sceKernelGetProcessTimeWide();
        seg.vkey_us = (unsigned int)(now - ts);
        ts = now;
    }

    /* v02.92: a queued screenshot captures the buffer that is about to be
     * submitted - game + keypad + camera button, i.e. exactly the frame
     * the user sees (also while the keyboard is up, minus the GXM dialog
     * which is composited after this point). */
    vita_shot_service(vita_fb, VITA_PHYS_W, VITA_PHYS_H);
    /* v02.93: "已保存截图 <file>" / "截图失败" banner. Painted AFTER the
     * capture so the PNG itself stays free of the notice, and only for
     * the ~3 s the shot module keeps it armed. */
    if (!vita_ime_is_active()) {
        toast_drawn = vita_shot_draw_toast(vita_fb, VITA_PHYS_W, VITA_PHYS_H);
    }
    {
        unsigned long long now = sceKernelGetProcessTimeWide();
        seg.shot_us = (unsigned int)(now - ts);
        ts = now;
    }

    /* v02.99: startup splash (P1-2). Last overlay before the submit, so it
     * covers the letterbox, the keypad and the white virtual screen
     * lfjport_ui_init() used to show. It writes only the PHYSICAL buffer,
     * so frame_has_content() (which samples vita_screen_pixels) still
     * measures the real white screen and not our own pixels. */
    vita_splash_draw(vita_fb, VITA_PHYS_W, VITA_PHYS_H);

    /* v03.70: diagnostics overlay (P1-8), last so nothing else can cover it.
     * It samples counters only - vita_perf_get()/vita_membw_get() - so it
     * cannot alter behaviour, and when hidden it is a single int test. */
    vita_diag_draw(vita_fb, VITA_PHYS_W, VITA_PHYS_H);

    /* v02.97: our own CPU work ends here; from this point the frame is
     * either handed to the GXM dialog or to sceDisplaySetFrameBuf. */
    t_prep = sceKernelGetProcessTimeWide();
    seg.splash_us = (unsigned int)(t_prep - ts);

    /* v02.63: paint the system keyboard (if up) over the just-blitted back
     * buffer, then submit it. When the IME is active, the CommonDialog render
     * must go through the GXM display queue (vita_gxm_dialog_frame) rather
     * than a raw sceDisplaySetFrameBuf - the two submit paths must not be
     * mixed, or the GPU-rendered dialog never reaches the screen and the VM
     * stalls in the render path (log109 problem 3). */
    /* v02.67: also take the few frames right after Term through the dialog
     * path (sceCommonDialogUpdate keeps running, as in the SDK sample). */
    if (vita_ime_is_active() || vita_ime_take_post_frame()) {
        g_was_ime = 1;
        /* v02.65: the dialog composites onto whatever is in the target, so
         * start every dialog frame from a clean bar area (the official IME
         * sample clears the whole target each frame for the same reason). */
        clear_letterbox(vita_fb);
        vita_cd_ensure_resources();
        vita_gxm_dialog_frame(vita_fb);
        /* v02.69: pace the dialog at one frame per vblank, exactly like
         * samples/ime (gxm_swap(); sceDisplayWaitVblankStart();). With
         * displayQueueMaxPendingCount = 1 this is also what keeps
         * sceGxmDisplayQueueAddEntry from blocking the VM thread, and it caps
         * the dialog at 60 fps instead of the 16-32 fps log113 showed. */
        sceDisplayWaitVblankStart();
        tmp = vita_fb_front;
        vita_fb_front = vita_fb;
        vita_fb = tmp;
    } else {
        /* v02.65: the keyboard just went away - wipe the bars it left in
         * each of the two CDRAM buffers (one normal frame per buffer). */
        if (g_was_ime) {
            g_was_ime = 0;
            g_post_ime_clear = 2;
        }
        if (g_post_ime_clear > 0) {
            clear_letterbox(vita_fb);
            g_post_ime_clear--;
        }
        memset(&fb, 0, sizeof(fb));
        fb.size = sizeof(SceDisplayFrameBuf);
        fb.base = vita_fb;
        fb.pitch = VITA_PHYS_W;
        fb.pixelformat = SCE_DISPLAY_PIXELFORMAT_A8B8G8R8;
        fb.width = VITA_PHYS_W;
        fb.height = VITA_PHYS_H;
        /* v01.69: NEXTFRAME - IMMEDIATE returns INVALID_UPDATETIMING
         * (0x80290006) on real fw 3.65 (see vita_menu.c menu_flip).
         * v01.70: submit the just-blitted back buffer, then swap; the next
         * blit goes into the old front while the display scans the new
         * one out.
         * v01.77: NEXTFRAME only takes effect at the NEXT vblank; until
         * then scanout still reads the old front buffer. Without a wait
         * here a fast repaint loop could finish blitting into the (just
         * swapped) new back buffer before the flip lands and tear the
         * frame - menu_flip() has always paired the submit with
         * sceDisplayWaitVblankStart(), this side now does the same. It
         * also caps the refresh rate at 60 Hz, which a software blit
         * cannot exceed anyway. */
        rc = sceDisplaySetFrameBuf(&fb, SCE_DISPLAY_SETBUF_NEXTFRAME);
        if (rc < 0) {
            /* once per process: a persistent error would spam 60 lines/s */
            static int reported = 0;
            if (!reported) {
                reported = 1;
                fprintf(stderr, "vita_display: SetFrameBuf rc=0x%08x\n", rc);
            }
            return;
        }
        sceDisplayWaitVblankStart();
        tmp = vita_fb_front;
        vita_fb_front = vita_fb;
        vita_fb = tmp;
    }

    /* v03.01: remember whether the frame just submitted carries the
     * banner. The shot poller reads this when the notice expires and the
     * MIDlet is not repainting: it then takes the banner off itself
     * (vita_display_toast_clear), which is what fixes the "green box stays
     * on screen" report. */
    __atomic_store_n(&g_toast_on_panel, toast_drawn, __ATOMIC_RELEASE);

    /* v02.64: when the last frame was submitted (vita_display_ime_tick). */
    __atomic_store_n(&g_last_flip_us, sceKernelGetProcessTimeWide(),
                     __ATOMIC_RELAXED);

    /* v02.97: one frame's cost -> perf.log (P1-3). Both branches above
     * end here, so an IME frame and a normal frame are measured the same
     * way and the numbers are comparable. */
    {
        unsigned long long t_end =
            __atomic_load_n(&g_last_flip_us, __ATOMIC_RELAXED);
        prepare_us = (unsigned int)(t_prep - t0);
        total_us = (unsigned int)(t_end - t0);
        vita_perf_frame_seg(prepare_us, total_us, &seg);
    }

    /* v02.44: white-screen marks (see above) */
    g_flip_n++;
    if (g_flip_n == 1) {
        vita_timing_markf("first_flip");
    }
    if (!g_content_found && g_flip_n <= 900 && frame_has_content()) {
        g_content_found = 1;
        /* v02.99: real pixels are on screen - stop painting the splash.
         * This is the only correct end point: a round can take 11 s and
         * the stage lines say nothing about when the app is ready. */
        vita_splash_end();
        vita_timing_markf("first_content_flip flip#=%d", g_flip_n);
    }
}

/* v02.99: repaint request from the splash module (preload loop, VM thread).
 * It goes through the SAME flip_to_display() the VM uses - letterbox, blit,
 * overlays, single sceDisplaySetFrameBuf + swap. A second submit path is
 * exactly what broke the IME in log109 (v02.63), so there is none. */
void vita_display_splash_repaint(void) {
    if (!display_ready || vita_fb == NULL) {
        return;
    }
    g_splash_own_flip = 1;
    flip_to_display();
    g_splash_own_flip = 0;
}

/* ------------------------------------------------------------------ */
/* lfjport implementation                                             */
/* ------------------------------------------------------------------ */

/* v02.64: keep the system keyboard alive while the MIDlet is idle.
 * sceCommonDialogUpdate() has to run EVERY frame (the dialog draws and reads
 * its input inside it), but it only ran from flip_to_display(), i.e. when
 * Java repainted. A TextField that just opened the keyboard is a static
 * screen - no repaint, so the dialog stayed RUNNING and never drew (log110:
 * Init rc=0, status=1, VM alive, nothing on screen). Called from the event
 * pump (VM thread, ~94 Hz): while the IME is up and nothing was submitted
 * for 16 ms, resubmit the current screen. No effect when the IME is idle. */
void vita_display_ime_tick(void) {
    /* v02.65: also runs for the two frames after the keyboard closed, so the
     * bars get wiped even if the MIDlet does not repaint by itself. */
    if (!display_ready ||
        (!vita_ime_is_active() && !g_was_ime && g_post_ime_clear == 0 &&
         !vita_ime_post_frames_pending())) {
        return;
    }
    if (sceKernelGetProcessTimeWide() - g_last_flip_us < 16000ULL) {
        return;
    }
    flip_to_display();
}

/* v02.95: redraw so a tap on the virtual keypad is actually SEEN.
 *
 * The keypad is painted as part of flip_to_display(), and that only runs
 * when Java calls lfjport_refresh() - i.e. when the game repaints. A
 * menu-driven game repaints once *after* it received the key, by which
 * time the finger is long gone, so a highlight that only lived while the
 * finger was down could never appear (the report behind this change).
 *
 * vita_vkey now latches a ~260 ms "flash" on every press; while that is
 * open (or a button is held) this re-runs the normal blit + overlay +
 * submit, at most every 33 ms so it stays a 30 fps idle refresh rather
 * than a busy loop. The game's pixels come from vita_screen_pixels, which
 * still holds the last frame Java produced, so nothing is invented.
 *
 * Safe from the VM thread: CLDC green threads mean there is exactly one OS
 * thread touching the virtual screen and the back buffer.
 * Skips entirely while the system keyboard dialog owns the display. */
/* v03.80: is a keypad replay ever showing a screen Java had not finished? Sample 1 pixel in 97 of the virtual
 * screen at each lfjport_refresh (a finished frame) and compare it at each replay: a difference means Java drew
 * into vita_screen_pixels between two refreshes. Diagnostics only. */
static unsigned int screen_fp(void) {
    unsigned int h = 2166136261u;
    int n = virt_w * virt_h;
    int i;
    for (i = 0; i < n; i += 97) {
        h = (h ^ vita_screen_pixels[i]) * 16777619u;
    }
    return h;
}
static unsigned int g_fp_refresh = 0;
static unsigned int g_replay_n = 0;
static unsigned int g_replay_diff = 0;
static unsigned int g_replay_logged = 0;
static unsigned int g_refresh_n = 0;
static int g_replay_sig = -2;           /* held button the last replays drew (-1 none), -2 = none yet */
static unsigned int g_replay_same = 0;  /* replays done for that picture */

/* v04.10: log176/177 measured 83-94% of the keypad-flash repaints showing a screen that differed from the last finished
 * frame, i.e. Java was part-way through drawing the next one. Keep a copy of every finished frame (only while the
 * keypad is on) and let the flash repaint show that copy. If the copy cannot be had the live buffer is used, as before. */
static gxj_pixel_type *g_snap = NULL;
static int g_snap_w = 0, g_snap_h = 0;

static void snap_take(void) {
    size_t bytes;
    if (!vita_vkey_enabled()) {
        return;
    }
    bytes = (size_t)virt_w * (size_t)virt_h * sizeof(gxj_pixel_type);
    if (g_snap == NULL || g_snap_w != virt_w || g_snap_h != virt_h) {
        free(g_snap);
        g_snap_w = 0;
        g_snap_h = 0;
        g_snap = (gxj_pixel_type *)malloc(bytes);
        if (g_snap == NULL) {
            return;
        }
        g_snap_w = virt_w;
        g_snap_h = virt_h;
    }
    memcpy(g_snap, vita_screen_pixels, bytes);
}

/* Time between finished frames, smoothed. 0 = not a continuously animated screen (a gap of 2 s or more is a menu
 * that repaints on input, not a frame period). Read by the virtual-key minimum hold in vita_input.c. */
static unsigned long long g_last_refresh_us = 0;
static unsigned int g_refresh_period_ms = 0;

static void refresh_period_note(void) {
    unsigned long long now = sceKernelGetProcessTimeWide();
    if (g_last_refresh_us != 0) {
        unsigned long long d = (now - g_last_refresh_us) / 1000ULL;
        if (d < 2000ULL) {
            g_refresh_period_ms = (g_refresh_period_ms == 0)
                ? (unsigned int)d : (g_refresh_period_ms * 3U + (unsigned int)d) / 4U;
        } else {
            g_refresh_period_ms = 0;
        }
    }
    g_last_refresh_us = now;
}

unsigned int vita_display_frame_period_ms(void) {
    return g_refresh_period_ms;
}

static void vkey_replay_log(const char *tag) {
    struct timeval tv;
    char line[160];
    int fd;
    gettimeofday(&tv, NULL);
    snprintf(line, sizeof(line), "[VKEY] replay %s n=%u diff=%u refreshes=%u ms=%lld\n", tag, g_replay_n,
             g_replay_diff, g_refresh_n, (long long)tv.tv_sec * 1000 + tv.tv_usec / 1000);
    fd = sceIoOpen("ux0:/data/J2ME00001/input_debug.log", SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0777);
    if (fd >= 0) {
        sceIoWrite(fd, line, strlen(line));
        sceIoClose(fd);
    }
}

void vita_display_vkey_tick(void) {
    static int was_active = 0;
    if (!display_ready || vita_ime_is_active()) {
        return;
    }
    /* v02.99: the loading screen owns the panel; a keypad flash repaint
     * would both draw over it and count as a "foreign" frame. */
    if (vita_splash_owns_screen()) {
        return;
    }
    if (!vita_vkey_flash_active()) {
        if (was_active) {
            was_active = 0;
            vkey_replay_log("end");
        }
        return;
    }
    if (sceKernelGetProcessTimeWide() - g_last_flip_us < 33000ULL) {
        return;
    }
    was_active = 1;
    /* v04.18+: log180 - a replay costs ~32 ms of the VM thread (prepare 12 + flip 20) and v04.10 made a tap last a whole
     * game frame, so 11-16 replays of the same picture ran per tap (306 replays = ~9.8 s of 29 s held). The picture only
     * changes when the held button changes: 2 replays cover the two display buffers, 1 more is spare. The game's own
     * refresh draws the current highlight, so nothing is lost by stopping. */
    {
        int sig = vita_vkey_pressed();
        if (sig != g_replay_sig) {
            g_replay_sig = sig;
            g_replay_same = 0;
        }
        if (g_replay_same >= 3) {
            return;
        }
        g_replay_same++;
    }
    g_replay_n++;
    if (screen_fp() != g_fp_refresh) {
        g_replay_diff++;
        if (g_replay_logged < 40) {
            g_replay_logged++;
            vkey_replay_log("DIFF");
        }
    }
    if (g_snap != NULL && g_snap_w == virt_w && g_snap_h == virt_h) {
        g_blit_src = g_snap;
        flip_to_display();
        g_blit_src = vita_screen_pixels;
    } else {
        flip_to_display();
    }
}

void lfjport_refresh(int hardwareId, int x, int y, int w, int h) {
    (void)hardwareId; (void)x; (void)y; (void)w; (void)h;
    g_fp_refresh = screen_fp();
    g_refresh_n++;
    snap_take();
    refresh_period_note();
    /* The scaled blit covers the whole screen anyway; full flip. */
    flip_to_display();
}
jboolean lfjport_reverse_orientation(int hardwareId) {
    (void)hardwareId;
    return KNI_FALSE;
}

void lfjport_handle_clamshell_event(void) {
    /* PS Vita: no clamshell */
}

jboolean lfjport_get_reverse_orientation(int hardwareId) {
    (void)hardwareId;
    return KNI_FALSE;
}

int lfjport_get_screen_width(int hardwareId) {
    (void)hardwareId;
    return virt_w;
}

int lfjport_get_screen_height(int hardwareId) {
    (void)hardwareId;
    return virt_h;
}

void lfjport_set_fullscreen_mode(int hardwareId, jboolean mode) {
    /* Always fullscreen on Vita */
    (void)hardwareId; (void)mode;
}

void lfjport_gained_foreground(int hardwareId) {
    (void)hardwareId;
    /* Push current buffer so a foreground switch repaints immediately. */
    flip_to_display();
}

/* v02.72: CDRAM bookkeeping per round. log114: the round-1 Init saw 86016K
 * free CDRAM against 90112K in round 0 (-4096K = exactly one framebuffer
 * block), and the crash dump shows a 4096 KB hole at 0x61400000 (round 0's
 * freed block) that the round-1 block did NOT reuse (it went to 0x62800000).
 * These lines give free CDRAM at every alloc / unmap / release so the next
 * log says which step does or does not give the 4 MB back. */
static void vita_fb_log_cdram(const char *what, void *base) {
    SceKernelFreeMemorySizeInfo mi;
    memset(&mi, 0, sizeof(mi));
    mi.size = sizeof(mi);
    sceKernelGetFreeMemorySize(&mi);
    fprintf(stderr, "[fb] %s base=%p cdram free=%dK\n", what, base,
            mi.size_cdram / 1024);
}

int lfjport_ui_init(void) {
    if (display_ready) {
        return 0;
    }
    compute_scaling();
    /* v02.85: hand the game rectangle to the virtual keypad now, so a
     * touch arriving before the first Java frame already hits the right
     * button. */
    vita_vkey_set_geometry(dst_x, dst_y, dst_w, dst_h);

    vita_fb = NULL;
    if (vita_fbmem_alloc(&vita_fb_blk,
            VITA_PHYS_W * VITA_PHYS_H * sizeof(uint32_t) * 2) == 0) {
        vita_fb_front = (uint32_t *)vita_fb_blk.base;
        vita_fb = vita_fb_front + VITA_PHYS_W * VITA_PHYS_H;
        vita_fb_back = vita_fb;
        vita_fb_log_cdram("alloc", vita_fb_blk.base);
    }
    if (vita_fb == NULL) {
        return -1;
    }
    /* v03.45: measure the framebuffer write bandwidth BEFORE the first
     * memset, while both buffers are still uninitialised and nothing can be
     * showing on them. Why here and not in main(): the probe needs real
     * uncached framebuffer memory (that is the whole point) and it needs the
     * virtual screen as a conversion source, and both exist only after the
     * allocation above. The probe costs ~60 ms once per process and its
     * result explains the `prepare=17.1 ms` that perf.log reports on every
     * single frame in log140 - see vita_membw.h. */
    vita_membw_probe(vita_fb, VITA_PHYS_W * VITA_PHYS_H * (int)sizeof(uint32_t),
                     (const uint16_t *)(const void *)vita_screen_pixels,
                     virt_w * virt_h,
                     dst_w * dst_h * (int)sizeof(uint32_t));

    /* Black letterbox background for BOTH buffers. */
    memset(vita_fb_front, 0, VITA_PHYS_W * VITA_PHYS_H * sizeof(uint32_t));
    memset(vita_fb, 0, VITA_PHYS_W * VITA_PHYS_H * sizeof(uint32_t));

    /* v03.05: both buffers are black again, so no overlay residue is
     * pending. (A second round reuses this display layer; a leftover
     * countdown from the previous round would be harmless but wrong.) */
    g_splash_was = 0;
    g_splash_clear = 0;
    g_toast_was = 0;
    g_toast_clear = 0;

    display_ready = 1;

    /* v02.92: start the screenshot poller (pad combo / on-screen button)
     * now that there is a framebuffer for the frozen-VM fallback. */
    vita_shot_init();

    /* Show something immediately. v02.99: the white fill is KEPT on
     * purpose. It is invisible now - the splash overlay (vita_splash.c)
     * covers the whole panel before this frame is submitted - and
     * frame_has_content() uses "not 0xFFFF" to decide when the white
     * screen ended, so changing this colour to black would make every
     * empty frame look like content and end the splash on flip #1. */
    memset(vita_screen_pixels, 0xFF, sizeof(vita_screen_pixels));
    flip_to_display();
    return 0;
}

void lfjport_ui_finalize(void) {
    display_ready = 0;
    /* v02.99: a round that ends while the splash is still up must not leave
     * the overlay armed for the next round's first frames. */
    vita_splash_end();
    /* v02.92: stop the screenshot poller from touching the framebuffer
     * while the CDRAM block is handed back. */
    vita_shot_quiesce();
    if (vita_fb_front != NULL) {
        vita_fb_front = NULL;
        vita_fb_back = NULL;
        vita_fb = NULL;
        /* NOTE (review v01.77, left as-is): the display may still be
         * scanning this block out for up to one more frame when we
         * hand it back - the same window vita_menu.c avoids by keeping
         * its fb allocated. Accepted here because finalize only runs
         * at VM shutdown, right before the menu reclaims the display,
         * so the worst case is a single garbage frame. */
        /* v02.71: unmap from GXM BEFORE the block goes back (log114). */
        {
            void *base = vita_fb_blk.base;
            vita_fb_log_cdram("finalize begin", base);
            vita_cd_unmap_fb();
            vita_fb_log_cdram("after unmap", base);
            vita_fbmem_release(&vita_fb_blk);
            vita_fb_log_cdram("after release", base);
        }
    }
}

jboolean lfjport_direct_flush(int hardwareId, const java_graphics *g,
                              const java_imagedata *offscreen_buffer, int h) {
    /* Let the generic path copy into gxj_system_screen_buffer and call
     * lfjport_refresh; we handle the actual flip there. */
    (void)hardwareId; (void)g; (void)offscreen_buffer; (void)h;
    return KNI_FALSE;
}

jboolean lfjport_is_native_softbutton_layer_supported(void) {
    /* Soft button labels are drawn by the Java chameleon layer. */
    return KNI_FALSE;
}

void lfjport_set_softbutton_label_on_native_layer(unsigned short *label,
                                                  int len, int index) {
    (void)label; (void)len; (void)index;
}

int lfjport_get_current_hardwareId(void) {
    return 0;
}

char *lfjport_get_display_name(int hardwareId) {
    (void)hardwareId;
    return "PS Vita Screen";
}

jboolean lfjport_is_display_primary(int hardwareId) {
    (void)hardwareId;
    return KNI_TRUE;
}

jboolean lfjport_is_display_buildin(int hardwareId) {
    (void)hardwareId;
    return KNI_TRUE;
}

jboolean lfjport_is_display_pen_supported(int hardwareId) {
    /* Vita front touch panel -> MIDP_PEN_EVENT (vita_input.c). */
    (void)hardwareId;
    return KNI_TRUE;
}

jboolean lfjport_is_display_pen_motion_supported(int hardwareId) {
    (void)hardwareId;
    return KNI_TRUE;
}

int lfjport_get_display_capabilities(int hardwareId) {
    /* Return 0 here and Display.setCurrent() throws
     * "This display does not support title" — a Form can never be shown.
     * Bits (DisplayDevice.java): 1=input, 2=commands, 4=forms, 8=ticker,
     * 16=title, 32=alerts, 64=lists, 128=textboxes, 256=tabbedpanes,
     * 512=fileselectors. We support them all. */
    (void)hardwareId;
    return 0x3FF;
}

jint *lfjport_get_display_device_ids(jint *n) {
    /* Contract (fbapp_export.c upstream): return a non-NULL array of ids.
     * Returning NULL with *n=1 made KNI_SetIntArrayElement dereference NULL
     * in DisplayDeviceContainer.getDisplayDevicesIds0 and crash the VM. */
    static jint display_device_ids[] = { 0 };

    if (n != NULL) {
        *n = 1;
    }
    return display_device_ids;
}

void lfjport_display_device_state_changed(int hardwareId, int state) {
    (void)hardwareId; (void)state;
}
