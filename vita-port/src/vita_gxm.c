/*
 * vita_gxm.c - shared GXM initializer + display-queue submit (see vita_gxm.h)
 */
#include <string.h>
#include <stdio.h>

#include <psp2/display.h>
#include <psp2/gxm.h>
#include <psp2/common_dialog.h>
#include <psp2/kernel/processmgr.h>

#include "vita_gxm.h"

/* Framebuffer geometry - must match vita_display.c's physical screen. */
#define GXM_FB_W 960
#define GXM_FB_H 544
#define GXM_FB_STRIDE 960

static int vita_gxm_ready = 0;

/* Display-queue swap chain state (double buffered, 2 sync objects).
 * v02.69: ONE SceGxmColorSurface PER BUFFER, initialised once, like the
 * official samples/ime (dbuf[].surf). Re-running sceGxmColorSurfaceInit on a
 * single shared surface every frame rewrites the description the GPU may
 * still be reading for the previous frame. */
static SceGxmColorSurface g_surf[2];
static uint32_t *g_surf_fb[2];   /* buffer each surface was built for (NULL=no)*/
static SceGxmSyncObject *g_sync[2];
static uint32_t *g_fb_data[2];   /* stable callbackData slots (not overwritten
                                    before the async swap callback reads them) */
static int g_fb_idx = 0;
/* v02.68: total dialog-path frames submitted (see vita_gxm_frame_count). */
static unsigned g_dialog_frames_total = 0;

unsigned vita_gxm_frame_count(void) {
    return g_dialog_frames_total;
}

/* Display-queue swap callback: runs on the GXM display-queue thread once the
 * GPU has finished rendering `fb` (signalled via the sync object). This is the
 * GXM path's equivalent of flip_to_display()'s sceDisplaySetFrameBuf. */
static void gxm_display_cb(const void *cb_data) {
    uint32_t *fb = *(uint32_t * const *)cb_data;
    SceDisplayFrameBuf f;
    memset(&f, 0, sizeof(f));
    f.size = sizeof(SceDisplayFrameBuf);
    f.base = fb;
    f.pitch = GXM_FB_STRIDE;
    f.pixelformat = SCE_DISPLAY_PIXELFORMAT_A8B8G8R8;
    f.width = GXM_FB_W;
    f.height = GXM_FB_H;
    sceDisplaySetFrameBuf(&f, SCE_DISPLAY_SETBUF_NEXTFRAME);
}

int vita_gxm_ensure_init(void) {
    SceGxmInitializeParams gxm_params;
    int i;

    if (vita_gxm_ready) {
        return 1;
    }
    memset(&gxm_params, 0, sizeof(gxm_params));
    gxm_params.flags = 0;
    /* v02.69: 1 pending swap, like samples/ime (DISPLAY_MAX_PENDING_SWAPS).
     * With 2, sceGxmDisplayQueueAddEntry can block the VM thread waiting for
     * a queue slot, and two frames in flight made the dialog's own state
     * harder to reason about (log113: 16-32 fps). */
    gxm_params.displayQueueMaxPendingCount = 1;
    gxm_params.displayQueueCallback = gxm_display_cb;
    gxm_params.displayQueueCallbackDataSize = sizeof(void *);
    gxm_params.parameterBufferSize = SCE_GXM_DEFAULT_PARAMETER_BUFFER_SIZE;
    if (sceGxmInitialize(&gxm_params) < 0) {
        fprintf(stderr, "[GXM] sceGxmInitialize failed\n");
        return 0;
    }

    /* Two sync objects back the double-buffered swap chain. */
    for (i = 0; i < 2; i++) {
        g_surf_fb[i] = NULL;
        if (sceGxmSyncObjectCreate(&g_sync[i]) < 0) {
            fprintf(stderr, "[GXM] sync object create failed (%d)\n", i);
        }
    }
    g_fb_idx = 0;

    vita_gxm_ready = 1;
    fprintf(stderr, "[GXM] initialized (display queue)\n");
    return 1;
}

void vita_gxm_dialog_frame(uint32_t *fb) {
    SceCommonDialogUpdateParam up;
    int idx;
    int rc_up, rc_pad, rc_q;
    /* v02.64 observability: log110 had no record of whether the per-frame
     * CommonDialog calls ran or what they returned. Log the first frames of
     * each IME session (a >1 s gap starts a new session) and every 300th. */
    static unsigned frame_n = 0;
    static unsigned long long last_us = 0;
    unsigned long long now_us = sceKernelGetProcessTimeWide();

    if (!vita_gxm_ready) {
        return;
    }
    if (now_us - last_us > 1000000ULL) {
        frame_n = 0;
    }
    last_us = now_us;
    idx = g_fb_idx;
    g_fb_data[idx] = fb;

    /* Paint the system IME over the just-blitted back buffer, synced to the
     * sync object this frame will signal. */
    memset(&up, 0, sizeof(up));
    up.renderTarget.depthSurfaceData = NULL;
    up.renderTarget.colorSurfaceData = fb;
    up.renderTarget.surfaceType = SCE_GXM_COLOR_SURFACE_LINEAR;
    up.renderTarget.colorFormat = SCE_GXM_COLOR_FORMAT_A8B8G8R8;
    up.renderTarget.width = GXM_FB_W;
    up.renderTarget.height = GXM_FB_H;
    up.renderTarget.strideInPixels = GXM_FB_STRIDE;
    up.displaySyncObject = g_sync[idx];
    rc_up = sceCommonDialogUpdate(&up);

    /* v02.69: describe each buffer's surface ONCE (samples/ime does the same
     * in gxm_init). g_surf_fb tracks which framebuffer a slot was built for,
     * so the normal double-buffer ping-pong costs a pointer compare. */
    if (g_surf_fb[idx] != fb) {
        sceGxmColorSurfaceInit(&g_surf[idx],
                               SCE_GXM_COLOR_FORMAT_A8B8G8R8,
                               SCE_GXM_COLOR_SURFACE_LINEAR,
                               SCE_GXM_COLOR_SURFACE_SCALE_NONE,
                               SCE_GXM_OUTPUT_REGISTER_SIZE_32BIT,
                               GXM_FB_W, GXM_FB_H, GXM_FB_STRIDE, fb);
        g_surf_fb[idx] = fb;
    }

    /* GPU renders this surface -> signal g_sync[idx]; the display queue waits
     * for the previous frame's sync before swapping to this one. */
    rc_pad = sceGxmPadHeartbeat(&g_surf[idx], g_sync[idx]);
    rc_q = sceGxmDisplayQueueAddEntry(g_sync[1 - idx], g_sync[idx],
                                      &g_fb_data[idx]);
    g_fb_idx = 1 - idx;

    g_dialog_frames_total++;
    frame_n++;
    if (frame_n <= 4 || (frame_n % 300) == 0) {
        fprintf(stderr, "[GXM] dialog frame #%u update=0x%08x pad=0x%08x "
                "queue=0x%08x fb=%p\n", frame_n, (unsigned)rc_up,
                (unsigned)rc_pad, (unsigned)rc_q, (void *)fb);
    }
}
