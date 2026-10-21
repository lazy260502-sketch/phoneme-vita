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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <gxj_putpixel.h>
#include <lfjport_export.h>

#include "vita_fbmem.h"
#include "vita_ime.h"
#include "vita_gxm.h"

/* ------------------------------------------------------------------ */
/* Virtual phone screen (what MIDP games see) vs Vita physical screen.
 *
 * Most J2ME games are portrait 240x320; that is the default. Landscape
 * games (320x240) select the other orientation via
 * vita_display_set_orientation(), called from the launcher after reading
 * launch.cfg, before the VM and hence the Java UI layer start. On the
 * 960x544 panel a portrait screen is shown as a centered 407x543 column. */
#define VITA_MAX_W 320
#define VITA_MAX_H 320

#ifndef VITA_DEFAULT_LANDSCAPE
#define VITA_DEFAULT_LANDSCAPE 0
#endif

#define VITA_PHYS_W 960
#define VITA_PHYS_H 544

static int virt_w = VITA_DEFAULT_LANDSCAPE ? 320 : 240;
static int virt_h = VITA_DEFAULT_LANDSCAPE ? 240 : 320;

/* Static buffers for the system screen buffer, sized for the largest
 * orientation (either way it is 320x240 = 76800 pixels). */
static gxj_pixel_type vita_screen_pixels[VITA_MAX_W * VITA_MAX_H];
static gxj_alpha_type vita_screen_alpha[VITA_MAX_W * VITA_MAX_H];

gxj_screen_buffer gxj_system_screen_buffer = {
    VITA_DEFAULT_LANDSCAPE ? 320 : 240,
    VITA_DEFAULT_LANDSCAPE ? 240 : 320,
    vita_screen_pixels,
    vita_screen_alpha
};

/* Switch the virtual screen orientation. Must be called before the VM
 * starts (the Java UI layer reads the size once at init). */
void vita_display_set_orientation(int landscape) {
    if (landscape) {
        virt_w = 320;
        virt_h = 240;
    } else {
        virt_w = 240;
        virt_h = 320;
    }
    gxj_system_screen_buffer.width = virt_w;
    gxj_system_screen_buffer.height = virt_h;
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
/* v02.64: time of the last submitted frame (see vita_display_ime_tick). */
static unsigned long long g_last_flip_us = 0;
/* v02.65: letterbox hygiene around the system keyboard. The blit only writes
 * the scaled MIDlet rectangle; the bars around it were never touched. The
 * keyboard is full-screen wide, so whatever it painted into the bars stayed
 * there for good ("overlaps the UC screen", log111). g_was_ime = the last
 * frame went through the dialog path; g_post_ime_clear = number of normal
 * frames that still have to wipe the bars (one per CDRAM buffer). */
static int g_was_ime = 0;
static int g_post_ime_clear = 0;

/* Computed scaling: virtual screen scaled to fit physical, centered. */
static int dst_w, dst_h, dst_x, dst_y;
/* Fixed point 16.16 step per destination pixel. */
static int step_x, step_y;

static void compute_scaling(void) {
    /* Fit the virtual screen inside 960x544 keeping aspect ratio:
     * scale = min(PHYS_W/VIRT_W, PHYS_H/VIRT_H). */
    int sx = (VITA_PHYS_W << 12) / virt_w;
    int sy = (VITA_PHYS_H << 12) / virt_h;
    int scale = (sx < sy) ? sx : sy;
    dst_w = (virt_w * scale) >> 12;
    dst_h = (virt_h * scale) >> 12;
    if (dst_w > VITA_PHYS_W) dst_w = VITA_PHYS_W;
    if (dst_h > VITA_PHYS_H) dst_h = VITA_PHYS_H;
    dst_x = (VITA_PHYS_W - dst_w) / 2;
    dst_y = (VITA_PHYS_H - dst_h) / 2;
    step_x = (virt_w << 16) / dst_w;
    step_y = (virt_h << 16) / dst_h;
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

static void flip_to_display(void) {
    SceDisplayFrameBuf fb;
    int x, y;
    int rc;
    uint32_t *tmp;

    if (vita_fb == NULL) {
        return;
    }

    /* Blit scaled virtual screen into the BACK framebuffer.
     * Nearest neighbour via fixed point steps. */
    for (y = 0; y < dst_h; y++) {
        const gxj_pixel_type *src_row =
            &vita_screen_pixels[((y * step_y) >> 16) * virt_w];
        uint32_t *dst_row = &vita_fb[(dst_y + y) * VITA_PHYS_W + dst_x];
        int sx = 0;
        for (x = 0; x < dst_w; x++) {
            dst_row[x] = rgb565_to_abgr(src_row[sx >> 16]);
            sx += step_x;
        }
    }

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

    /* v02.64: when the last frame was submitted (vita_display_ime_tick). */
    g_last_flip_us = sceKernelGetProcessTimeWide();

    /* v02.44: white-screen marks (see above) */
    g_flip_n++;
    if (g_flip_n == 1) {
        vita_timing_markf("first_flip");
    }
    if (!g_content_found && g_flip_n <= 900 && frame_has_content()) {
        g_content_found = 1;
        vita_timing_markf("first_content_flip flip#=%d", g_flip_n);
    }
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

void lfjport_refresh(int hardwareId, int x, int y, int w, int h) {
    (void)hardwareId; (void)x; (void)y; (void)w; (void)h;
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
    /* Black letterbox background for BOTH buffers. */
    memset(vita_fb_front, 0, VITA_PHYS_W * VITA_PHYS_H * sizeof(uint32_t));
    memset(vita_fb, 0, VITA_PHYS_W * VITA_PHYS_H * sizeof(uint32_t));

    display_ready = 1;

    /* Show something immediately (white virtual screen). */
    memset(vita_screen_pixels, 0xFF, sizeof(vita_screen_pixels));
    flip_to_display();
    return 0;
}

void lfjport_ui_finalize(void) {
    display_ready = 0;
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
