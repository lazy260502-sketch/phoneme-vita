/*
 * vita_diag.h - on-screen diagnostics overlay (v03.70, P1-8).
 *
 * Why: every performance question in this port has cost a device round trip -
 * copy perf.log off the card, read it, guess. The data is already in memory:
 * vita_perf_get() has fps / period / prepare / flip and the per-stage split,
 * vita_membw_get() has the measured framebuffer bandwidth, and the VM has the
 * clock the firmware actually allows (333 MHz - the 444 request is refused
 * with 0x802b0000, so this number matters and is easy to forget).
 *
 * The overlay is drawn as one more layer inside flip_to_display(), the same
 * way the virtual keypad, the screenshot banner and the startup splash are -
 * there is exactly ONE submit path in this port (the IME lesson, log109), and
 * a second one is not introduced here.
 *
 * It is ON by default but tiny (a two-line strip in the top-left corner over
 * the letterbox bar when there is one, so it normally covers no game pixel),
 * and it can be switched at runtime:
 *
 *   ux0:/data/J2ME00001/diag.cfg
 *       on=1       1 = show at boot, 0 = hidden (combo toggles either way)
 *       detail=0   0 = one line, 1 = the full breakdown
 *
 *   Physical: SELECT + L trigger toggles it (the screenshot combo is
 *   SELECT + START; SELECT alone is already "clear clipboard" inside some
 *   MIDlets, so a second combination key is used rather than a single one.)
 */

#ifndef VITA_DIAG_H
#define VITA_DIAG_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Read diag.cfg once. Called early in boot (vita_main.c). */
void vita_diag_init(void);

/* Toggle by the physical combo: call from the input/event path with 1 when
 * SELECT+L is held. Edge-triggered by the caller. */
void vita_diag_toggle(void);

int vita_diag_visible(void);

/* v03.79: live settings, so the launcher's 设置 page can drive this without
 * anyone editing diag.cfg by hand (which needs a card reader). The setters
 * take effect on the next frame; vita_diag_save() persists all three to
 * ux0:/data/J2ME00001/diag.cfg so the choice survives a restart. */
void vita_diag_set_visible(int on);
void vita_diag_set_detail(int on);
void vita_diag_set_pos(int pos);      /* 0 TR, 1 TL, 2 BR, 3 BL */
int  vita_diag_get_detail(void);
int  vita_diag_get_pos(void);
void vita_diag_save(void);

/* Overlay painter. `fb` is the PHYSICAL back buffer, w/h its geometry. Must be
 * called after every other overlay so it is never covered, and before the
 * submit. Cheap: two text rows plus an opaque backing box.
 *
 * Position (v03.78): top-RIGHT by default, because the Vita's own frame-rate
 * overlay occupies the top-left corner and the virtual keypad fills the left
 * letterbox bar. Override with "pos=" in diag.cfg:
 *     0 = top-right (default)   1 = top-left
 *     2 = bottom-right          3 = bottom-left
 * The box width is measured from the text, not fixed, so it cannot reach into
 * the game area further than it has to. */
void vita_diag_draw(uint32_t *fb, int w, int h);

#ifdef __cplusplus
}
#endif

#endif /* VITA_DIAG_H */
