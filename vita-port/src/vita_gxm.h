/*
 * vita_gxm.h - shared GXM initializer + display-queue submit for the IME
 *
 * This port draws with sceDisplaySetFrameBuf (software blit into a CDRAM
 * framebuffer), so GXM is only used for the system IME (CommonDialog), which
 * renders THROUGH GXM. CommonDialog needs a fully working GXM display queue:
 *   - sceGxmInitialize with a display-queue callback (the callback performs
 *     the actual sceDisplaySetFrameBuf on the vsync path)
 *   - per frame: vita_gxm_dialog_frame() paints the dialog via
 *     sceCommonDialogUpdate() and hands the framebuffer to the display queue
 *     via sceGxmPadHeartbeat + sceGxmDisplayQueueAddEntry
 * A NULL callback (the pre-v02.63 state) left the dialog's render with nowhere
 * to be submitted, so the keyboard never appeared and the VM stalled in the
 * render path (log109 problem 3).
 */
#ifndef VITA_GXM_H
#define VITA_GXM_H

#include <stdint.h>

/* Lazy, idempotent sceGxmInitialize. Returns non-zero once GXM is ready. */
int vita_gxm_ensure_init(void);

/* Render the system IME (CommonDialog) into fb, then submit it through the GXM
 * display queue. Called INSTEAD of sceDisplaySetFrameBuf while the IME is
 * active (the CommonDialog render must go through GXM's swap path). */
void vita_gxm_dialog_frame(uint32_t *fb);

/* v02.68: number of dialog frames submitted since start-up (diagnostics:
 * frame rate of an IME session, and whether the frames after Term ran). */
unsigned vita_gxm_frame_count(void);

#endif /* VITA_GXM_H */
