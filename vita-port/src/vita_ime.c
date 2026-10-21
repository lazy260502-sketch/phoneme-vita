/*
 * vita_ime.c - PS Vita system IME (sceImeDialog) bridge for phoneME MIDP
 *
 * See vita_ime.h for the design. This file owns the sceImeDialog lifecycle:
 *
 *   vita_ime_show()  -> sceImeDialogInit   (async, never blocks)
 *   vita_ime_poll()  -> sceImeDialogGetStatus / GetResult / Term, then
 *                       build the MIDP IME event from the UTF-16 result
 *
 * The dialog result buffer is SceWChar16 (UTF-16), the same 16-bit width
 * as phoneME's jchar, so the text is handed straight to
 * pcsl_string_convert_from_utf16() and then to the MIDP event queue (which
 * owns and frees it after the Java layer consumes it - see midpEvents.c
 * freeMIDPEventFields).
 */
#include <string.h>
#include <stdio.h>

#include <psp2/ime_dialog.h>
#include <psp2/libime.h>      /* SceImeLanguage, SceImeType */
#include <psp2/common_dialog.h> /* SceCommonDialogStatus */
#include <psp2/apputil.h>     /* v02.69: samples/ime inits AppUtil first   */
#include <psp2/sysmodule.h>   /* v02.69: SCE_SYSMODULE_IME                 */
#include <psp2/kernel/processmgr.h>   /* sceKernelGetProcessTimeWide */
#include <psp2/kernel/sysmem.h>       /* sceKernelGetFreeMemorySize (v02.68 log) */

#include <midpEvents.h>
#include <keymap_input.h>     /* KEYMAP_STATE_IME */
#include <pcsl_string.h>

#include "vita_gxm.h"

/* Result buffer: the largest the system dialog can return. Static so it
 * survives across event-pump polls; pcsl_string_convert_from_utf16 deep
 * copies into a heap pcsl_string, so this buffer is reusable afterwards. */
static SceWChar16 g_ime_buffer[SCE_IME_DIALOG_MAX_TEXT_LENGTH + 1];

/* v02.74: the focused TextBox's current content, handed to the dialog as
 * initialText so the user edits what is already there (before, the keyboard
 * always opened empty and the result was INSERTED at the caret: old text
 * could be neither seen nor removed). Java keeps it in sync through
 * showNativeKeyboard's sibling setNativeKeyboardText(); the SELECT key opens
 * the dialog from native code without going through Java, hence the cache.
 * Separate from g_ime_buffer: the dialog may keep reading initialText while
 * it is up. */
static SceWChar16 g_ime_initial[SCE_IME_DIALOG_MAX_TEXT_LENGTH + 1];
static int g_ime_initial_len = 0;
/* Java writes into the pending copy at any time (also while a dialog is up
 * and reading g_ime_initial); ime_try_init() moves it over right before Init. */
static SceWChar16 g_ime_pending[SCE_IME_DIALOG_MAX_TEXT_LENGTH + 1];
static int g_ime_pending_len = 0;
static int g_ime_max_len = 0;       /* TextBox capacity, 0 = no limit known */

/* Dialog title. UTF-16 literal written as an explicit array so it does not
 * depend on wchar_t width (32-bit on ARM, SceWChar16 is 16-bit). */
static const SceWChar16 g_ime_title[] = {
    'T', 'e', 'x', 't', ' ', 'I', 'n', 'p', 'u', 't', 0
};

static int g_ime_active = 0;
static int g_ime_config_done = 0;
static int g_ime_last_status = -1;   /* v02.64: for change-only status logging */

/* v02.65: closing protocol. sceImeDialogTerm() is only legal once the status
 * is FINISHED (the official sample never calls it earlier). log111: the UC
 * keyboard came up by itself at app start, UC's field lost focus within
 * seconds, hideNativeKeyboard -> vita_ime_abort() called Term() on a RUNNING
 * dialog and cleared g_ime_active. The dialog stayed alive in the system but
 * nobody called sceCommonDialogUpdate() any more: frozen keyboard on screen,
 * input swallowed, "cannot type, cannot close". Now abort only REQUESTS the
 * close (sceImeDialogAbort) and the poll finishes it after FINISHED, while
 * the display keeps driving Update. */
static int g_ime_aborting = 0;                  /* Abort sent, wait for FINISHED */
static int g_ime_reshow = 0;                    /* show() asked while closing   */
static unsigned long long g_ime_show_us = 0;
static unsigned long long g_ime_abort_us = 0;
#define IME_ABORT_TIMEOUT_US 3000000ULL         /* safety net, see poll()       */

/* v02.67: log112 (v02.66) - the close protocol works (Abort rc=0, status 2,
 * Term rc=0) but EVERY later sceImeDialogInit returned 0x8002047f
 * (UNEXPECTED_FATAL), 13 times, so the keyboard showed once and never again.
 * The cause is not known. Two cheap, testable guesses are built in, plus
 * logging that separates them:
 *  - the SDK sample keeps calling sceCommonDialogUpdate after Term; we
 *    stopped at once -> run a few more Update frames (g_ime_post_frames);
 *  - Init may fail only while the system is still tearing the old dialog
 *    down -> a failed Init is retried every 100 ms for 3 s (g_ime_retry)
 *    instead of being dropped. */
static int g_ime_retry = 0;
static int g_ime_retry_n = 0;
static unsigned long long g_ime_retry_us = 0;
static unsigned long long g_ime_term_us = 0;    /* when Term last ran           */
static int g_ime_post_frames = 0;
static unsigned g_ime_frames_at_show = 0;   /* v02.68 dialog-frame counters   */
static unsigned g_ime_frames_at_term = 0;
/* v02.68: log113 ruled the timing guess out (still failing 84 s after Term),
 * so a retry window of 3 s only costs failed Init calls: 3 tries now. */
#define IME_RETRY_INTERVAL_US 100000ULL
#define IME_RETRY_MAX         3
#define IME_POST_FRAMES       6

/* v02.64: does the UI currently have a focused text component? Set by the
 * Java->native bridge (showNativeKeyboard / hideNativeKeyboard in
 * lfj_native_input_mode.c, i.e. TextField/TextBox focus gain/loss). The
 * SELECT key only opens the keyboard while this is set; otherwise it is the
 * ordinary '#' key again. log110: SELECT used to open the keyboard
 * unconditionally, so pressing it in a game (where '#' is a function key)
 * popped the system IME. */
static int g_text_wanted = 0;

void vita_ime_set_text_wanted(int on) {
    on = on ? 1 : 0;
    /* v02.67: log the transitions with a process-relative timestamp. This is
     * the Java side's show/hide rhythm (UC took the keyboard away 3.1 s after
     * showing it, with no input from the user) and it was invisible before. */
    if (on != g_text_wanted) {
        fprintf(stderr, "[IME] text_wanted -> %d (t=%llu ms)\n", on,
                sceKernelGetProcessTimeWide() / 1000ULL);
    }
    g_text_wanted = on;
}

int vita_ime_post_frames_pending(void) {
    return g_ime_post_frames > 0;
}

int vita_ime_take_post_frame(void) {
    if (g_ime_post_frames > 0) {
        g_ime_post_frames--;
        return 1;
    }
    return 0;
}

int vita_ime_text_wanted(void) {
    return g_text_wanted;
}

void vita_ime_set_initial_text(const unsigned short *text, int len,
                               int max_len) {
    /* Writes the pending copy only; the dialog's own buffer (g_ime_initial)
     * is refreshed in ime_try_init(). Java also calls this with null/0 when
     * the TextBox goes away, so a stale text never leaks into a TextField
     * keyboard (which inserts instead of replacing). */
    g_ime_max_len = max_len > 0 ? max_len : 0;
    if (text == NULL || len < 0) {
        len = 0;
    }
    if (len > SCE_IME_DIALOG_MAX_TEXT_LENGTH) {
        len = SCE_IME_DIALOG_MAX_TEXT_LENGTH;
    }
    if (len > 0) {
        memcpy(g_ime_pending, text, (size_t)len * sizeof(SceWChar16));
    }
    g_ime_pending[len] = 0;
    g_ime_pending_len = len;
}

int vita_ime_is_active(void) {
    return g_ime_active;
}

/* v02.67: one attempt to open the dialog (was the body of vita_ime_show).
 * Returns the sceImeDialogInit result (>= 0 = dialog is up) or -1 when GXM
 * could not be initialised. */
static int ime_try_init(void) {
    SceImeDialogParam param;
    int rc, st;
    unsigned long long now = sceKernelGetProcessTimeWide();

    /* The Vita CommonDialog framework (which sceImeDialog is built on)
     * needs a one-time config before any dialog can run. Without it
     * sceImeDialogInit can fail with SCE_COMMON_DIALOG_ERROR_NOT_CONFIGURED
     * (0x80020407). MAX_VALUE = "use the system language / enter-button
     * assignment".
     *
     * v02.69: two omissions vs samples/ime found while chasing log113's
     * "first dialog works, every later Init returns 0x8002047f
     * (SCE_COMMON_DIALOG_ERROR_UNEXPECTED_FATAL)":
     *   1. SCE_SYSMODULE_IME was never loaded. ime_dialog.h's usage line
     *      names it, and the sample's module is resident for the whole
     *      process. If the system auto-loads it for the first dialog and
     *      releases it on Term, the next Init fails - exactly the observed
     *      shape (status before=0, fails even 84 s after Term).
     *   2. sceAppUtilInit was never called; the sample does it before
     *      sceCommonDialogSetConfigParam (CommonDialog reads app info).
     * Both are idempotent one-time steps, logged so the next run shows them. */
    if (!g_ime_config_done) {
        SceCommonDialogConfigParam cfg;
        int rc_mod = sceSysmoduleLoadModule(SCE_SYSMODULE_IME);
        int rc_app = sceAppUtilInit(&(SceAppUtilInitParam){0},
                                    &(SceAppUtilBootParam){0});
        fprintf(stderr, "[IME] sysmodule IME load rc=0x%08x, apputil init "
                "rc=0x%08x\n", (unsigned)rc_mod, (unsigned)rc_app);
        sceCommonDialogConfigParamInit(&cfg);
        sceCommonDialogSetConfigParam(&cfg);
        g_ime_config_done = 1;
    }

    /* v02.54: GXM must be initialized BEFORE sceImeDialogInit, otherwise
     * Init fails with SCE_COMMON_DIALOG_ERROR_GXM_IS_UNINITIALIZED
     * (0x80020436). log104 showed exactly this: GXM init lived inside the
     * per-frame update path which only runs while a dialog is active, but
     * the dialog can never become active without GXM - a chicken-and-egg
     * deadlock. Init it up front, idempotently. */
    if (!vita_gxm_ensure_init()) {
        fprintf(stderr, "[IME] GXM init failed, aborting show\n");
        return -1;
    }

    sceImeDialogParamInit(&param);
    /* Default input method (0) lets the user pick; supportedLanguages
     * enumerates the on-screen keyboards offered. English + simplified /
     * traditional Chinese + Japanese cover the J2ME games in scope. */
    param.supportedLanguages =
        SCE_IME_LANGUAGE_ENGLISH |
        SCE_IME_LANGUAGE_SIMPLIFIED_CHINESE |
        SCE_IME_LANGUAGE_TRADITIONAL_CHINESE |
        SCE_IME_LANGUAGE_JAPANESE;
    param.languagesForced = SCE_FALSE;
    param.type = SCE_IME_TYPE_DEFAULT;
    param.dialogMode = SCE_IME_DIALOG_DIALOG_MODE_WITH_CANCEL;
    param.textBoxMode = SCE_IME_DIALOG_TEXTBOX_MODE_WITH_CLEAR;
    param.title = g_ime_title;
    param.maxTextLength = SCE_IME_DIALOG_MAX_TEXT_LENGTH;
    /* v02.74: honour the TextBox capacity so the result always fits (a
     * setString over the limit throws and would lose the whole edit). */
    if (g_ime_max_len > 0 && g_ime_max_len < SCE_IME_DIALOG_MAX_TEXT_LENGTH) {
        param.maxTextLength = (SceUInt32)g_ime_max_len;
    }
    param.inputTextBuffer = g_ime_buffer;
    /* v02.74: open with the TextBox's current content (see g_ime_initial). */
    memcpy(g_ime_initial, g_ime_pending,
           ((size_t)g_ime_pending_len + 1) * sizeof(SceWChar16));
    g_ime_initial_len = g_ime_pending_len;
    param.initialText = g_ime_initial;

    g_ime_buffer[0] = 0;

    /* v02.67: what the system thinks before Init, and how long ago our last
     * Term was - separates "still tearing down" from "stuck for good". */
    st = sceImeDialogGetStatus();
    rc = sceImeDialogInit(&param);
    if (rc < 0 && g_ime_retry_n != 0) {
        return rc;          /* only the first attempt of a request is logged */
    }
    {
        /* v02.68: free memory at Init time (UNEXPECTED_FATAL could be an
         * allocation failure inside the IME) and how many dialog frames ran
         * after the last Term (proves whether the post-Term Update ran). */
        SceKernelFreeMemorySizeInfo mi;
        memset(&mi, 0, sizeof(mi));
        mi.size = sizeof(mi);
        sceKernelGetFreeMemorySize(&mi);
        fprintf(stderr, "[IME] show: Init rc=0x%08x (status before=%d, "
                "%lld ms since last Term, %u dialog frames since Term, "
                "attempt %d, t=%llu ms, initial=%d chars, "
                "free user=%dK cdram=%dK phycont=%dK)\n",
                rc, st,
                g_ime_term_us ? (long long)((now - g_ime_term_us) / 1000ULL) : -1LL,
                g_ime_term_us ? vita_gxm_frame_count() - g_ime_frames_at_term : 0u,
                g_ime_retry_n + 1, now / 1000ULL, g_ime_initial_len,
                mi.size_user / 1024, mi.size_cdram / 1024,
                mi.size_phycont / 1024);
    }
    if (rc < 0) {
        return rc;
    }
    g_ime_frames_at_show = vita_gxm_frame_count();
    g_ime_active = 1;
    g_ime_aborting = 0;
    g_ime_reshow = 0;
    g_ime_last_status = -1;
    g_ime_show_us = sceKernelGetProcessTimeWide();
    return rc;
}

void vita_ime_show(void) {
    if (g_ime_active) {
        /* v02.65: a show request while the old dialog is still closing is
         * replayed once it has finished (poll), if the UI still wants text. */
        if (g_ime_aborting) {
            g_ime_reshow = 1;
        }
        return; /* already up */
    }
    if (g_ime_retry) {
        return;             /* a retry loop is already working on it */
    }
    g_ime_retry_n = 0;
    if (ime_try_init() < 0) {
        /* v02.67: do not drop the request - try again from the event pump. */
        g_ime_retry = 1;
        g_ime_retry_us = sceKernelGetProcessTimeWide();
    }
}

int vita_ime_poll(MidpEvent *out) {
    SceImeDialogResult result;
    int status;
    int len;
    pcsl_string_status rc;

    if (!g_ime_active) {
        /* v02.67: pending retry of a failed Init (see g_ime_retry). Gives up
         * after IME_RETRY_MAX tries, or at once if the UI no longer wants
         * text (hide arrived meanwhile). */
        if (g_ime_retry) {
            unsigned long long now = sceKernelGetProcessTimeWide();
            if (!g_text_wanted) {
                g_ime_retry = 0;
            } else if (now - g_ime_retry_us >= IME_RETRY_INTERVAL_US) {
                g_ime_retry_us = now;
                g_ime_retry_n++;
                if (ime_try_init() >= 0) {
                    fprintf(stderr, "[IME] Init succeeded on retry %d\n",
                            g_ime_retry_n);
                    g_ime_retry = 0;
                } else if (g_ime_retry_n >= IME_RETRY_MAX) {
                    fprintf(stderr, "[IME] Init still failing after %d "
                            "retries - giving up\n", g_ime_retry_n);
                    g_ime_retry = 0;
                }
            }
        }
        return 0;
    }

    status = sceImeDialogGetStatus();
    /* v02.64: log every status CHANGE (was: only the first one). RUNNING (1)
     * = dialog up, FINISHED (2) = user done; NONE (0) = never rendered.
     * v02.65: with the elapsed time since show(). */
    if (status != g_ime_last_status) {
        g_ime_last_status = status;
        fprintf(stderr, "[IME] status -> %d (+%llu ms)\n", status,
                (sceKernelGetProcessTimeWide() - g_ime_show_us) / 1000ULL);
    }
    if (status != SCE_COMMON_DIALOG_STATUS_FINISHED) {
        /* Safety net: an abort that never reaches FINISHED (e.g. nobody is
         * driving sceCommonDialogUpdate) must not wedge the UI for good. */
        if (g_ime_aborting &&
            sceKernelGetProcessTimeWide() - g_ime_abort_us > IME_ABORT_TIMEOUT_US) {
            int trc = sceImeDialogTerm();
            fprintf(stderr, "[IME] abort not finished after 3 s (status=%d): "
                    "forced Term rc=0x%08x\n", status, trc);
            g_ime_term_us = sceKernelGetProcessTimeWide();
            g_ime_post_frames = IME_POST_FRAMES;
            g_ime_active = 0;
            g_ime_aborting = 0;
            g_ime_reshow = 0;
        }
        return 0;
    }

    /* v02.68: the result struct was never cleared and GetResult's return value
     * never looked at, so a failed GetResult left stack garbage in `button`
     * and the dialog looked "dismissed". Clear it, check it, and look at what
     * is really in the text buffer before deciding anything (log113: 18 s of
     * typing ended as button=0 = SCE_IME_DIALOG_BUTTON_NONE, text dropped). */
    memset(&result, 0, sizeof(result));
    {
        int grc = sceImeDialogGetResult(&result);
        int trc = sceImeDialogTerm();
        int was_abort = g_ime_aborting;
        unsigned frames = vita_gxm_frame_count() - g_ime_frames_at_show;
        unsigned long long dur_ms =
            (sceKernelGetProcessTimeWide() - g_ime_show_us) / 1000ULL;
        int n = 0;
        while (n < SCE_IME_DIALOG_MAX_TEXT_LENGTH && g_ime_buffer[n] != 0) {
            n++;
        }
        fprintf(stderr, "[IME] finished: GetResult rc=0x%08x result=%d "
                "button=%d (0=NONE 1=CLOSE 2=ENTER) text=%d chars first=U+%04X "
                "Term rc=0x%08x%s (+%llu ms, %u dialog frames = %llu fps)\n",
                grc, result.result, result.button, n,
                n > 0 ? (unsigned)g_ime_buffer[0] : 0u, trc,
                was_abort ? " [after abort]" : "", dur_ms, frames,
                dur_ms ? (unsigned long long)frames * 1000ULL / dur_ms : 0ULL);
        g_ime_frames_at_term = vita_gxm_frame_count();
        g_ime_term_us = sceKernelGetProcessTimeWide();
        g_ime_post_frames = IME_POST_FRAMES;   /* keep Update running a bit */
        g_ime_active = 0;
        g_ime_aborting = 0;
        if (was_abort) {
            /* We closed it ourselves: no text event. If the UI asked for the
             * keyboard again while it was closing, open it now. */
            if (g_ime_reshow) {
                g_ime_reshow = 0;
                if (g_text_wanted) {
                    vita_ime_show();
                }
            }
            return 0;
        }
    }

    len = 0;
    while (g_ime_buffer[len] != 0 && len < SCE_IME_DIALOG_MAX_TEXT_LENGTH) {
        len++;
    }

    /* ENTER commits; CLOSE (the explicit cancel) discards. v02.68: NONE with
     * text in the buffer also commits - a dialog the user typed into and
     * confirmed, which the system reports as NONE, must not lose the text
     * (log113). NONE with an empty buffer is still just "nothing happened". */
    if (result.button != SCE_IME_DIALOG_BUTTON_ENTER &&
        !(result.button == SCE_IME_DIALOG_BUTTON_NONE && len > 0)) {
        fprintf(stderr, "[IME] dismissed (button=%d, %d chars dropped)\n",
                result.button, len);
        return 0;
    }
    if (result.button != SCE_IME_DIALOG_BUTTON_ENTER) {
        fprintf(stderr, "[IME] button=NONE but %d chars typed - committing\n",
                len);
    }
    /* v02.74: the result is the WHOLE new content (the dialog opened with the
     * old one), and Java replaces the text with it. So an empty result is
     * real - the user deleted everything - and must be delivered. */
    fprintf(stderr, "[IME] committed %d chars%s\n", len,
            len == 0 ? " (empty: clears the text)" : "");

    MIDP_EVENT_INITIALIZE(*out);
    out->type = MIDP_KEY_EVENT;
    out->ACTION = KEYMAP_STATE_IME; /* 4 = EventConstants.IME */
    /* Empty result: leave stringParam1 as PCSL_STRING_NULL; Java reads null
     * as "". */
    if (len > 0) {
        rc = pcsl_string_convert_from_utf16((const jchar *)g_ime_buffer,
                                            len, &out->stringParam1);
        if (rc != PCSL_STRING_OK) {
            /* v02.74: OOM. The event used to go out empty; with replace
             * semantics that would wipe the user's text, so drop it. */
            out->stringParam1 = PCSL_STRING_NULL;
            fprintf(stderr, "[IME] commit dropped: out of memory\n");
            return 0;
        }
    }
    return 1;
}

void vita_ime_abort(void) {
    int rc;
    g_ime_retry = 0;           /* v02.67: a hide cancels a pending re-Init */
    if (!g_ime_active) {
        return;
    }
    if (g_ime_aborting) {
        g_ime_reshow = 0;      /* a later hide cancels a queued re-show */
        return;
    }
    /* v02.65: request the close, do NOT Term here (see g_ime_aborting). The
     * event pump (vita_ime_poll) and the display tick keep running until the
     * status becomes FINISHED. */
    rc = sceImeDialogAbort();
    g_ime_aborting = 1;
    g_ime_abort_us = sceKernelGetProcessTimeWide();
    fprintf(stderr, "[IME] abort requested after %llu ms: Abort rc=0x%08x\n",
            (g_ime_abort_us - g_ime_show_us) / 1000ULL, rc);
}
