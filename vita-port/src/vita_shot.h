/*
 * vita_shot.h - screenshot capture for the PS Vita J2ME port (v02.92)
 *
 * Why: diagnosing a J2ME title on the Vita is often "what exactly was on
 * screen at that moment" (white screen, garbled Alert, missing sprite,
 * frozen frame). A PNG on ux0 is far more useful than a description.
 *
 * Two triggers, as requested:
 *   - physical pad : SELECT + START held together
 *   - virtual key  : the small camera button in the left letterbox bar
 *
 * The capture itself is queued and written on the render thread right
 * before the frame is submitted, so the PNG is exactly the frame the
 * user sees (game + virtual keypad + camera button). A low-priority
 * poller thread watches the pad/touch on its own, so a shot can still be
 * taken when the VM thread has stopped pumping events (the classic
 * freeze case): in that case it falls back to the last submitted frame.
 *
 * Files land in ux0:/data/J2ME00001/shots/shot_YYYYMMDD_HHMMSS.png and
 * every attempt is logged to ux0:/data/J2ME00001/shot.log.
 *
 * Config ux0:/data/J2ME00001/shot.cfg (all optional):
 *   on=1       master switch (0 = no screenshots at all)
 *   button=1   1 = draw the on-screen camera button
 *   combo=1    1 = SELECT+START takes a screenshot
 */
#ifndef VITA_SHOT_H
#define VITA_SHOT_H

/* Load shot.cfg, create the shots directory and start the poller thread.
 * Idempotent; called once per VM round from lfjport_ui_init(). */
void vita_shot_init(void);

/* Config accessors (loaded on first use). */
int  vita_shot_enabled(void);
void vita_shot_set_enabled(int on);
int  vita_shot_button_enabled(void);
int  vita_shot_combo_enabled(void);

/* Queue a capture (debounced). Ignored while disabled. */
void vita_shot_request(const char *why);

/* 1 when a request is waiting for a frame. */
int  vita_shot_pending(void);

/* Render-thread service: capture `fb` (w x h, A8B8G8R8, pitch == w) and
 * clear the pending flag. Call right before the frame is submitted. */
void vita_shot_service(const unsigned int *fb, int w, int h);

/* Poller-thread fallback: capture the last submitted frame when the
 * render thread never serviced the request (VM frozen). Returns 1 when
 * the request was consumed. */
int  vita_shot_capture_stale(void);

/* Drop a pending request and wait for an in-progress write to finish.
 * Called before the display frees its CDRAM framebuffers. */
void vita_shot_quiesce(void);

/* On-screen camera button (drawn over the blitted frame). */
void vita_shot_draw_button(unsigned int *fb, int w, int h);
int  vita_shot_button_hit(int px, int py);

/* Result banner shown after an attempt ("saved" / "failed" + file name).
 * Drawn by the display layer right after the frame was submitted, so it
 * never ends up in the PNG itself. Returns 1 when a banner was actually
 * painted (the caller records that the panel now carries it). */
int  vita_shot_draw_toast(unsigned int *fb, int w, int h);
int  vita_shot_toast_active(void);

/* SELECT+START combo state, owned by the input layer. */
void vita_shot_set_combo(int held);
int  vita_shot_combo_held(void);

#endif /* VITA_SHOT_H */
