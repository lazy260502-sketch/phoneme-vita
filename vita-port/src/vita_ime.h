/*
 * vita_ime.h - PS Vita system IME (sceImeDialog) bridge for phoneME MIDP
 *
 * phoneME ships a built-in "input method event" channel that is unused in
 * the Vita port: a MIDP_KEY_EVENT whose ACTION == KEYMAP_STATE_IME (4)
 * carries the typed text in stringParam1, and DisplayEventListener routes
 * it to DisplayEventConsumer.handleInputMethodEvent(), which inserts the
 * string into the focused TextBox. This is exactly the hook a batch
 * (whole-string) system keyboard needs - Vita's sceImeDialog returns the
 * finished text in one shot, unlike the per-key WINCE soft keyboard.
 *
 * The Vita port has NO digit keys, so phoneME's multi-tap InputMode chain
 * (digits -> characters) can never fire: that is why J2ME programs have no
 * way to enter text. SELECT triggers the system keyboard (a pragmatic MVP;
 * auto-pop on TextField focus needs a Java-side InputMode change and is
 * left as a follow-up), and the finished text is fed back through the IME
 * event. TextBox is covered; TextField needs the same event routed in the
 * Java layer (follow-up).
 */
#ifndef VITA_IME_H
#define VITA_IME_H

#include <midpEvents.h>

/* Open the system keyboard. Idempotent while one is already up. */
void vita_ime_show(void);

/* Event-pump poll. When the user finished typing, fills *out with the IME
 * event (MIDP_KEY_EVENT, ACTION=IME, stringParam1=typed text) and returns
 * 1. Returns 0 otherwise. The returned stringParam1's ownership is handed
 * to the MIDP event queue (it will free it). */
int vita_ime_poll(MidpEvent *out);

/* Dismiss the keyboard without committing (best-effort). */
void vita_ime_abort(void);

/* Non-zero while a system-keyboard dialog is up. The display layer uses
 * this to decide whether to drive sceCommonDialogUpdate() each frame. */
int vita_ime_is_active(void);

/* v02.64: a text component (TextField/TextBox) currently has focus. SELECT
 * opens the keyboard only while this is set; otherwise it sends '#'. */
void vita_ime_set_text_wanted(int on);
int vita_ime_text_wanted(void);

/* v02.74: the focused TextBox's current content (UTF-16, len chars), used as
 * the dialog's initialText. Ignored while a dialog is up. */
void vita_ime_set_initial_text(const unsigned short *text, int len,
                               int max_len);

/* v02.67: after the dialog is Term'd the display keeps running
 * sceCommonDialogUpdate for a few more frames (the SDK sample calls it every
 * frame, also right after Term). _pending peeks, _take consumes one frame. */
int vita_ime_post_frames_pending(void);
int vita_ime_take_post_frame(void);

#endif /* VITA_IME_H */
