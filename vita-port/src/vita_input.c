/*
 * vita_input.c - PS Vita key + touch input for phoneME MIDP
 *
 * Replaces phoneme-midp's mastermode_export.o (whose checkForSystemSignal
 * was an empty stub — the reason no key ever reached a MIDlet). The CMake
 * build deletes that member from libobj_no_main.a and links this file.
 *
 * phoneME calls checkForSystemSignal() from midp_check_events(), the
 * JVMSPI periodic callback, every time Java threads block. To deliver a
 * key we fill the MidpEvent and set pNewSignal->waitingFor = UI_SIGNAL;
 * the caller then routes the event to the foreground MIDlet's queue
 * (midpStoreEventAndSignalForeground). Event values follow
 * events/input_port/fb/native/fb_handle_input.c:
 *   type   = MIDP_KEY_EVENT
 *   CHR    = KEYMAP_KEY_* code (digits '0'-'9', UP=-1..SELECT=-5,
 *            SOFT1/2=-6/-7, GAMEA..D=-13..-16)
 *   ACTION = KEYMAP_STATE_PRESSED / KEYMAP_STATE_RELEASED
 *
 * Touch (SceTouch, clipped to the virtual screen and mapped back from
 * the letterboxed 960x544 display) is delivered as MIDP_PEN_EVENT:
 *   type   = MIDP_PEN_EVENT
 *   X_POS  = virtual x (intParam2)
 *   Y_POS  = virtual y (intParam3)
 *   ACTION = KEYMAP_STATE_PRESSED/DRAGGED/RELEASED (intParam1)
 * This mirrors qvfb_handle_input.c so DisplayEventListener delivers
 * Canvas.pointerPressed/pointerReleased.
 *
 * SceCtrl/SceTouch are sampled on every callback; edge detection turns
 * level state into press/release events. A small SPSC ring buffers
 * bursts (the VM thread is the only consumer).
 */

#include <kni.h>
#include <psp2/ctrl.h>
#include <psp2/touch.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/io/fcntl.h>
#include <stdio.h>
#include <string.h>

#include <midp_logging.h>
#include <midpServices.h>
#include <midpEvents.h>
#include <midpEventUtil.h>
#include <keymap_input.h>
#include <midp_mastermode_port.h> /* checkForSystemSignal contract */

#define VITA_INPUT_RING_SIZE 16

/* Map a physical (960x544) touch coordinate to the virtual J2ME screen.
 * Exported by vita_display.c. Returns 0 on hit, -1 in the letterbox. */
extern int vita_display_map_touch(int px, int py, int *vx, int *vy);

/* ---- SPSC ring: producer fills events, VM thread consumes them ---- */
static MidpEvent event_ring[VITA_INPUT_RING_SIZE];
static volatile int ring_head = 0; /* written by producer */
static volatile int ring_tail = 0; /* written by consumer */

static void ring_push(const MidpEvent *evt) {
    int next = (ring_head + 1) % VITA_INPUT_RING_SIZE;
    if (next == ring_tail) {
        /* Overflow: drop the oldest pending event to keep latency low. */
        ring_tail = (ring_tail + 1) % VITA_INPUT_RING_SIZE;
    }
    event_ring[ring_head] = *evt;
    ring_head = next;
}

static int ring_pop(MidpEvent *out) {
    if (ring_tail == ring_head) {
        return 0;
    }
    *out = event_ring[ring_tail];
    ring_tail = (ring_tail + 1) % VITA_INPUT_RING_SIZE;
    return 1;
}

/* ---- Key mapping: Vita buttons -> KEYMAP_KEY_* codes ---- */

typedef struct {
    unsigned int vitaButton;
    int keymapKey;
} VitaKeyMap;

static const VitaKeyMap key_map[] = {
    {SCE_CTRL_UP,       KEYMAP_KEY_UP},      /* -1 -> Canvas.UP   */
    {SCE_CTRL_DOWN,     KEYMAP_KEY_DOWN},    /* -2 -> Canvas.DOWN */
    {SCE_CTRL_LEFT,     KEYMAP_KEY_LEFT},    /* -3 -> Canvas.LEFT */
    {SCE_CTRL_RIGHT,    KEYMAP_KEY_RIGHT},   /* -4 -> Canvas.RIGHT*/
    {SCE_CTRL_CROSS,    KEYMAP_KEY_SELECT},  /* -5 -> FIRE        */
    {SCE_CTRL_CIRCLE,   KEYMAP_KEY_SOFT1},   /* -6 -> left soft   */
    {SCE_CTRL_SQUARE,   KEYMAP_KEY_SOFT2},   /* -7 -> right soft  */
    {SCE_CTRL_TRIANGLE, KEYMAP_KEY_GAMEA},   /* -13               */
    {SCE_CTRL_LTRIGGER, KEYMAP_KEY_GAMEB},   /* -14               */
    {SCE_CTRL_RTRIGGER, KEYMAP_KEY_GAMEC},   /* -15               */
    {SCE_CTRL_START,    KEYMAP_KEY_ASTERISK},/* '*' menu/pause    */
    {SCE_CTRL_SELECT,   KEYMAP_KEY_POUND},   /* '#'               */
    {0, 0}
};

static unsigned int prev_buttons = 0;
static int input_initialized = 0;

static void vita_input_init(void) {
    if (input_initialized) {
        return;
    }
    sceCtrlSetSamplingMode(SCE_CTRL_MODE_DIGITAL);
    SceCtrlData pad;
    sceCtrlPeekBufferPositive(0, &pad, 1);
    prev_buttons = pad.buttons;
    input_initialized = 1;
}

static void emit_key_event(int keymapKey, int state) {
    MidpEvent evt;
    MIDP_EVENT_INITIALIZE(evt);
    evt.type = MIDP_KEY_EVENT;
    evt.CHR = keymapKey;
    evt.ACTION = state;
    ring_push(&evt);
}

/* Sample the pad once and enqueue press/release events for edges. */
static void vita_input_poll(void) {
    SceCtrlData pad;
    unsigned int changed, i;

    if (sceCtrlPeekBufferPositive(0, &pad, 1) < 0) {
        return;
    }

    changed = pad.buttons ^ prev_buttons;
    if (changed == 0) {
        return;
    }

    /* DEBUG: log every button change to verify the VM event pump is
     * still running (if the VM is frozen on a blocking native call,
     * checkForSystemSignal never fires and this log stays empty). */
    {
        char dbg[64];
        snprintf(dbg, sizeof(dbg), "[INPUT] buttons=0x%08X\n", pad.buttons);
        int fd = sceIoOpen("ux0:/data/J2ME00001/input_debug.log",
                           SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0777);
        if (fd >= 0) {
            sceIoWrite(fd, dbg, strlen(dbg));
            sceIoClose(fd);
        }
    }

    for (i = 0; key_map[i].vitaButton != 0; i++) {
        unsigned int mask = key_map[i].vitaButton;
        if (changed & mask) {
            int pressed = (pad.buttons & mask) ? 1 : 0;
            emit_key_event(key_map[i].keymapKey,
                           pressed ? KEYMAP_STATE_PRESSED
                                   : KEYMAP_STATE_RELEASED);
        }
    }
    prev_buttons = pad.buttons;
}

/* ---- Touch (pointer) input ----
 * SceTouch reports the physical front panel (960x544). We map that
 * coordinate into the virtual J2ME screen (240x320 / 320x240) and emit
 * MIDP_PEN_EVENT. Track the previous touch position so a held+move
 * finger turns into KEYMAP_STATE_DRAGGED instead of press/release spam. */
static int touch_initialized = 0;
static int touch_x = 0, touch_y = 0;      /* last virtual position */
static int touch_down = 0;                /* finger currently down */

static void vita_touch_init(void) {
    if (touch_initialized) {
        return;
    }
    /* Sample once to establish the idle (no-touch) baseline. */
    SceTouchData touch;
    sceTouchPeek(SCE_TOUCH_PORT_FRONT, &touch, 1);
    touch_down = 0;
    touch_x = touch_y = 0;
    touch_initialized = 1;
}

/* Called from checkForSystemSignal before the event ring is drained. */
static void vita_touch_poll(void) {
    SceTouchData touch;
    int n;
    int px, py, vx, vy;
    int state;

    if (!touch_initialized) {
        vita_touch_init();
    }

    n = sceTouchPeek(SCE_TOUCH_PORT_FRONT, &touch, 1);
    if (n < 0) {
        return;
    }

    if (touch.reportNum <= 0) {
        /* No finger on the panel. */
        if (touch_down) {
            /* Emit release at the last known position. */
            MidpEvent evt;
            MIDP_EVENT_INITIALIZE(evt);
            evt.type = MIDP_PEN_EVENT;
            evt.ACTION = KEYMAP_STATE_RELEASED;
            evt.X_POS = touch_x;
            evt.Y_POS = touch_y;
            ring_push(&evt);
            touch_down = 0;
        }
        return;
    }

    /* First report = primary finger. */
    px = touch.report[0].x;
    py = touch.report[0].y;
    if (vita_display_map_touch(px, py, &vx, &vy) != 0) {
        return; /* touch in the letterbox - ignore */
    }

    if (!touch_down) {
        state = KEYMAP_STATE_PRESSED;
    } else if (vx != touch_x || vy != touch_y) {
        state = KEYMAP_STATE_DRAGGED;
    } else {
        state = 0; /* no change */
    }

    if (state != 0) {
        MidpEvent evt;
        MIDP_EVENT_INITIALIZE(evt);
        evt.type = MIDP_PEN_EVENT;
        evt.ACTION = state;
        evt.X_POS = vx;
        evt.Y_POS = vy;
        ring_push(&evt);
    }

    touch_x = vx;
    touch_y = vy;
    touch_down = 1;
}

/* ---- Exported: also called from vita_main.c after VM start ---- */
void vita_input_reset(void) {
    ring_tail = ring_head;
    input_initialized = 0;
    vita_input_init();
}

/* Exported by vita_media_notify.c: drains the media event ring */
extern int vita_media_poll(MidpEvent *out);

/*
 * The platform event pump. Called by midp_check_events() in the VM thread.
 * Semantics per mastermode_port contract: timeout >0/0/-1 — we are a
 * polling source (no blockable fd on Vita), so always non-blocking.
 */
void checkForSystemSignal(MidpReentryData *pNewSignal,
                          MidpEvent *pNewMidpEvent,
                          jlong timeout) {
    (void)timeout;

    if (!input_initialized) {
        vita_input_init();
    }

    /* DEBUG: heartbeat - logs every N-th call to prove the VM thread
     * still reaches the event pump. If this stops while a key press
     * still produces input_debug.log entries, the VM froze after
     * sampling the pad (i.e. inside a blocked native call). */
    static unsigned int hb = 0;
    if ((hb++ & 0x3FF) == 0) {
        char dbg[64];
        snprintf(dbg, sizeof(dbg), "[HEARTBEAT] checkForSystemSignal #%u\n", hb);
        int fd = sceIoOpen("ux0:/data/J2ME00001/input_debug.log",
                           SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0777);
        if (fd >= 0) {
            sceIoWrite(fd, dbg, strlen(dbg));
            sceIoClose(fd);
        }
    }

    vita_input_poll();
    vita_touch_poll();

    /* Media events first: a blocked MMAPI Java thread waits on
     * MEDIA_EVENT_SIGNAL, and END_OF_MEDIA must not be starved by
     * input traffic. */
    if (vita_media_poll(pNewMidpEvent)) {
        pNewSignal->waitingFor = MEDIA_EVENT_SIGNAL;
        return;
    }

    if (ring_pop(pNewMidpEvent)) {
        pNewSignal->waitingFor = UI_SIGNAL;
    }
    /* Otherwise leave waitingFor == NO_SIGNAL (caller zeroed it). */
}
