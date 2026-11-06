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
#include <psp2/kernel/processmgr.h> /* v02.73: Clear-key long-press timing */
#include <psp2/io/fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h> /* v01.71 heartbeat clock sampling */

#include <midp_logging.h>
#include <midpServices.h>
#include <midpEvents.h>
#include <midpEventUtil.h>
#include <keymap_input.h>
#include <midp_mastermode_port.h> /* checkForSystemSignal contract */
#include "vita_ime.h" /* v02.50: system keyboard bridge */
#include "vita_vkey.h" /* v02.85: on-screen keypad */
#include "vita_shot.h" /* v02.92: SELECT+START / tap camera = screenshot */
#include "vita_diag.h"  /* v03.70: SELECT+L toggles the diagnostics overlay */

#define VITA_INPUT_RING_SIZE 16

/* Map a physical (960x544) touch coordinate to the virtual J2ME screen.
 * Exported by vita_display.c. Returns 0 on hit, -1 in the letterbox. */
extern int vita_display_map_touch(int px, int py, int *vx, int *vy);

/* ---- SPSC ring: producer fills events, VM thread consumes them ----
 * Lock-free correctness relies on there being exactly ONE producer
 * AND one consumer thread (both are the VM thread today: the pump
 * produces in checkForSystemSignal, the scheduler drains it). A
 * second producer would race ring_head; if one is ever added this
 * must become a critical-section ring. */
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

/* Analog sticks are folded into pad bits above the real button bits. */
#define KM_STICK_LL (1u << 16)
#define KM_STICK_LR (1u << 17)
#define KM_STICK_LU (1u << 18)
#define KM_STICK_LD (1u << 19)
#define KM_STICK_RL (1u << 20)
#define KM_STICK_RR (1u << 21)
#define KM_STICK_RU (1u << 22)
#define KM_STICK_RD (1u << 23)
#define KM_STICK_ALL (0xFFu << 16)
#define KM_REAL_MASK 0xFFFFu

typedef struct {
    unsigned int vitaButton;
    int keymapKey;
} VitaKeyMap;

/* Mutable: the menu's 设置 > 按键映射 page rewrites keymapKey at runtime
 * (persisted in keymap.cfg). Order of entries == UI order. */
static VitaKeyMap key_map[] = {
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
    /* stick directions: virtual bits (see stick_bits), unmapped by default */
    {KM_STICK_LL, KEYMAP_KEY_INVALID},
    {KM_STICK_LR, KEYMAP_KEY_INVALID},
    {KM_STICK_LU, KEYMAP_KEY_INVALID},
    {KM_STICK_LD, KEYMAP_KEY_INVALID},
    {KM_STICK_RL, KEYMAP_KEY_INVALID},
    {KM_STICK_RR, KEYMAP_KEY_INVALID},
    {KM_STICK_RU, KEYMAP_KEY_INVALID},
    {KM_STICK_RD, KEYMAP_KEY_INVALID},
    {0, 0}
};

/* ---- User remapping (feature-phone key assigned to each Vita button) ----
 * API used by vita_menu.c (plain ints, no MIDP headers needed there). */
#define KEYMAP_CFG "ux0:/data/J2ME00001/keymap.cfg"
#define KM_BTN_N 20

static const char *km_btn_names[KM_BTN_N] = {
    "UP", "DOWN", "LEFT", "RIGHT", "CROSS(X)", "CIRCLE(O)", "SQUARE",
    "TRIANGLE", "L", "R", "START", "SELECT",
    "左摇杆 左", "左摇杆 右", "左摇杆 上", "左摇杆 下",
    "右摇杆 左", "右摇杆 右", "右摇杆 上", "右摇杆 下"
};

typedef struct { int key; const char *name; } KmChoice;
static const KmChoice km_choices[] = {
    {KEYMAP_KEY_INVALID,  "无"},
    {KEYMAP_KEY_0, "0"}, {KEYMAP_KEY_1, "1"}, {KEYMAP_KEY_2, "2"},
    {KEYMAP_KEY_3, "3"}, {KEYMAP_KEY_4, "4"}, {KEYMAP_KEY_5, "5"},
    {KEYMAP_KEY_6, "6"}, {KEYMAP_KEY_7, "7"}, {KEYMAP_KEY_8, "8"},
    {KEYMAP_KEY_9, "9"},
    {KEYMAP_KEY_ASTERISK, "*"}, {KEYMAP_KEY_POUND, "#"},
    {KEYMAP_KEY_UP, "上"}, {KEYMAP_KEY_DOWN, "下"},
    {KEYMAP_KEY_LEFT, "左"}, {KEYMAP_KEY_RIGHT, "右"},
    {KEYMAP_KEY_SELECT, "确定"},
    {KEYMAP_KEY_SOFT1, "左软键"}, {KEYMAP_KEY_SOFT2, "右软键"},
    {KEYMAP_KEY_SEND, "拨号"}, {KEYMAP_KEY_END, "挂断"},
    {KEYMAP_KEY_CLEAR, "清除"},
    {KEYMAP_KEY_GAMEA, "GAME A"}, {KEYMAP_KEY_GAMEB, "GAME B"},
    {KEYMAP_KEY_GAMEC, "GAME C"}, {KEYMAP_KEY_GAMED, "GAME D"}
};
#define KM_CHOICE_N ((int)(sizeof(km_choices) / sizeof(km_choices[0])))

static int km_default[KM_BTN_N];
static int km_loaded = 0;

static int km_choice_index(int key) {
    int i;
    for (i = 0; i < KM_CHOICE_N; i++) {
        if (km_choices[i].key == key) return i;
    }
    return -1;
}

int vita_keymap_count(void) { return KM_BTN_N; }
const char *vita_keymap_btn_name(int i) {
    return (i >= 0 && i < KM_BTN_N) ? km_btn_names[i] : "";
}

static void km_load(void) {
    SceUID fd;
    if (km_loaded) return;
    km_loaded = 1;
    {
        int i;
        for (i = 0; i < KM_BTN_N; i++) km_default[i] = key_map[i].keymapKey;
    }
    fd = sceIoOpen(KEYMAP_CFG, SCE_O_RDONLY, 0);
    if (fd >= 0) {
        char buf[256];
        int n = sceIoRead(fd, buf, sizeof(buf) - 1);
        sceIoClose(fd);
        if (n > 0) {
            char *p, *nl;
            buf[n] = '\0';
            for (p = buf; p != NULL; p = (nl != NULL) ? nl + 1 : NULL) {
                int idx, key;
                nl = strpbrk(p, "\r\n");
                if (nl != NULL) *nl = '\0';
                if (sscanf(p, "%d=%d", &idx, &key) == 2 &&
                    idx >= 0 && idx < KM_BTN_N &&
                    km_choice_index(key) >= 0) {
                    key_map[idx].keymapKey = key;
                }
            }
        }
    }
}

int vita_keymap_get(int i) {
    km_load();
    return (i >= 0 && i < KM_BTN_N) ? key_map[i].keymapKey : 0;
}

const char *vita_keymap_key_name(int key) {
    int c = km_choice_index(key);
    return (c >= 0) ? km_choices[c].name : "?";
}

/* dir = +1 / -1: step to the next / previous choice. */
void vita_keymap_cycle(int i, int dir) {
    int c;
    km_load();
    if (i < 0 || i >= KM_BTN_N) return;
    c = km_choice_index(key_map[i].keymapKey);
    if (c < 0) c = 0;
    c = (c + dir + KM_CHOICE_N) % KM_CHOICE_N;
    key_map[i].keymapKey = km_choices[c].key;
}

void vita_keymap_reset(void) {
    int i;
    km_load();
    for (i = 0; i < KM_BTN_N; i++) key_map[i].keymapKey = km_default[i];
}

void vita_keymap_save(void) {
    char buf[256];
    int i, off = 0;
    SceUID fd;
    km_load();
    for (i = 0; i < KM_BTN_N; i++) {
        off += snprintf(buf + off, sizeof(buf) - off, "%d=%d\n", i,
                        key_map[i].keymapKey);
    }
    sceIoRemove(KEYMAP_CFG); /* Vita3K ignores O_TRUNC */
    fd = sceIoOpen(KEYMAP_CFG, SCE_O_WRONLY | SCE_O_CREAT, 0777);
    if (fd >= 0) {
        sceIoWrite(fd, buf, off);
        sceIoClose(fd);
    }
}

static unsigned int prev_buttons = 0;
static int input_initialized = 0;

/* Real buttons + stick direction bits; hysteresis keeps edges clean. */
static unsigned int stick_bits(const SceCtrlData *pad, unsigned int prev) {
    static const struct { int axis; unsigned int neg, pos; } ax[4] = {
        {0, KM_STICK_LL, KM_STICK_LR}, {1, KM_STICK_LU, KM_STICK_LD},
        {2, KM_STICK_RL, KM_STICK_RR}, {3, KM_STICK_RU, KM_STICK_RD}
    };
    unsigned int out = pad->buttons & KM_REAL_MASK;
    int i;
    for (i = 0; i < 4; i++) {
        int v = (ax[i].axis == 0) ? pad->lx : (ax[i].axis == 1) ? pad->ly
              : (ax[i].axis == 2) ? pad->rx : pad->ry;
        int d = v - 128;
        int on = (prev & ax[i].neg) ? 48 : 64; /* release < press */
        int op = (prev & ax[i].pos) ? 48 : 64;
        if (d <= -on) out |= ax[i].neg;
        else if (d >= op) out |= ax[i].pos;
    }
    return out;
}

static void vita_input_init(void) {
    if (input_initialized) {
        return;
    }
    km_load();
    sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG);
    SceCtrlData pad;
    sceCtrlPeekBufferPositive(0, &pad, 1);
    prev_buttons = stick_bits(&pad, 0);
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

/* v02.73: a Clear (backspace) key while a TextBox/TextField is being edited.
 * log115: after the system keyboard committed text there was no way to delete
 * it - no Vita button was mapped to KEYMAP_KEY_CLEAR (-8), although phoneME
 * already handles it (TextFieldLFImpl.handleClearKey: a short press deletes
 * the char before the cursor, a second event for the SAME key before its
 * release = "long press" empties the whole field).
 * Context-sensitive like SELECT (v02.64): only while a text component has
 * focus (vita_ime_text_wanted) and the keyboard is not up, TRIANGLE sends
 * Clear instead of GAME_A. Outside text editing (games) nothing changes. It
 * stands down if the user already mapped any button to Clear, or moved
 * TRIANGLE away from its default GAME_A in 设置 > 按键映射. */
#define CLEAR_LONGPRESS_US 700000ULL
static int tri_is_clear = 0;                 /* current TRIANGLE press = Clear */
static int tri_long_sent = 0;
static unsigned long long tri_down_us = 0;

static int clear_context_active(void) {
    int i, tri = -1;
    if (!vita_ime_text_wanted() || vita_ime_is_active()) {
        return 0;
    }
    for (i = 0; key_map[i].vitaButton != 0; i++) {
        if (key_map[i].keymapKey == KEYMAP_KEY_CLEAR) {
            return 0;                        /* the user has a Clear key */
        }
        if (key_map[i].vitaButton == SCE_CTRL_TRIANGLE) {
            tri = i;
        }
    }
    return tri >= 0 && key_map[tri].keymapKey == KEYMAP_KEY_GAMEA;
}

/* Sample the pad once and enqueue press/release events for edges. */
static void vita_input_poll(void) {
    SceCtrlData pad;
    unsigned int changed, i, cur;

    if (sceCtrlPeekBufferPositive(0, &pad, 1) < 0) {
        return;
    }
    cur = stick_bits(&pad, prev_buttons);

    /* v02.65: the system keyboard owns the buttons while it is up. Track the
     * pad state (so nothing fires on the way out) but forward nothing to the
     * MIDlet - otherwise a key meant for the dialog also drives the UI
     * behind it (log111: UC lost focus and the keyboard was torn down). */
    if (vita_ime_is_active()) {
        prev_buttons = cur;
        return;
    }

    /* v02.73: held Clear key -> one "repeated" event = long press = Java
     * empties the field. Must run before the no-change early return. */
    if (tri_is_clear && !tri_long_sent && (cur & SCE_CTRL_TRIANGLE) &&
        sceKernelGetProcessTimeWide() - tri_down_us >= CLEAR_LONGPRESS_US) {
        tri_long_sent = 1;
        fprintf(stderr, "[IME] Clear key held -> long press (empty field)\n");
        emit_key_event(KEYMAP_KEY_CLEAR, KEYMAP_STATE_REPEATED);
    }

    changed = cur ^ prev_buttons;
    if (changed == 0) {
        return;
    }

    /* v02.92: screenshot combo, SELECT + START held together.
     * State is kept in the shot module because the poller thread uses the
     * same flag to tell "combo in progress" from "ordinary key press".
     * Both keys are swallowed while the combo is down (see the key loop
     * below) - otherwise the game receives '#' and '*'. */
    if (vita_shot_combo_enabled()) {
        const unsigned int combo_mask = SCE_CTRL_SELECT | SCE_CTRL_START;
        unsigned int combo_now = cur & combo_mask;
        if (combo_now == combo_mask) {
            if (!vita_shot_combo_held()) {
                vita_shot_set_combo(1);
                vita_shot_request("keys");
            }
        } else if (combo_now == 0) {
            vita_shot_set_combo(0);
        }
    }

    /* v03.70: diagnostics overlay, SELECT + L held together. Separate combo
     * from the screenshot's SELECT+START, and not a single key: SELECT alone
     * is '#' for the MIDlet and, with a text component focused, opens the
     * system keyboard, so it cannot be repurposed. State is a module static
     * here rather than in the overlay because the overlay has no polling
     * thread - this is the only place that sees the raw pad. */
    {
        static int diag_combo_held = 0;
        const unsigned int diag_mask = SCE_CTRL_SELECT | SCE_CTRL_LTRIGGER;
        unsigned int diag_now = cur & diag_mask;
        if (diag_now == diag_mask) {
            if (!diag_combo_held) {
                diag_combo_held = 1;
                vita_diag_toggle();
            }
        } else if (diag_now == 0) {
            diag_combo_held = 0;
        }
    }

    /* DEBUG: log every button change to verify the VM event pump is
     * still running (if the VM is frozen on a blocking native call,
     * checkForSystemSignal never fires and this log stays empty). */
    {
        char dbg[96];
        struct timeval tvi;
        gettimeofday(&tvi, NULL);
        snprintf(dbg, sizeof(dbg), "[INPUT] buttons=0x%08X ms=%lld\n", pad.buttons,
                 (long long)((jlong)tvi.tv_sec * 1000 + tvi.tv_usec / 1000));
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
            int pressed = (cur & mask) ? 1 : 0;
            /* v02.92: while SELECT+START is held it owns both keys. Presses
             * are dropped; a release is still delivered so a key whose press
             * went out before the second one went down cannot stay stuck. */
            if ((mask & (SCE_CTRL_SELECT | SCE_CTRL_START)) &&
                vita_shot_combo_held()) {
                if (!pressed && key_map[i].keymapKey != KEYMAP_KEY_INVALID) {
                    emit_key_event(key_map[i].keymapKey,
                                   KEYMAP_STATE_RELEASED);
                }
                continue;
            }
            /* v02.50: SELECT opens the system IME instead of emitting
             * '#' - the port has no digit keys, so this is the only way
             * a MIDlet gets text input. MVP trigger; auto-pop on TextField
             * focus is a follow-up. */
            /* v02.64: only while a TextField/TextBox has focus. In a game
             * (no text component) SELECT falls through to its key_map entry
             * ('#') like before v02.50 - log110: pressing SELECT in a game
             * popped the system keyboard. */
            if (mask == SCE_CTRL_SELECT && vita_ime_text_wanted()) {
                if (pressed) {
                    vita_ime_show();
                }
                continue;
            }
            /* v02.73: TRIANGLE as Clear while editing text; the release
             * always mirrors how the press was sent. */
            if (mask == SCE_CTRL_TRIANGLE) {
                if (pressed && clear_context_active()) {
                    tri_is_clear = 1;
                    tri_long_sent = 0;
                    tri_down_us = sceKernelGetProcessTimeWide();
                    emit_key_event(KEYMAP_KEY_CLEAR, KEYMAP_STATE_PRESSED);
                    continue;
                }
                if (!pressed && tri_is_clear) {
                    tri_is_clear = 0;
                    emit_key_event(KEYMAP_KEY_CLEAR, KEYMAP_STATE_RELEASED);
                    continue;
                }
            }
            if (key_map[i].keymapKey == KEYMAP_KEY_INVALID) {
                continue; /* user unmapped this button */
            }
            emit_key_event(key_map[i].keymapKey,
                           pressed ? KEYMAP_STATE_PRESSED
                                   : KEYMAP_STATE_RELEASED);
        }
    }
    prev_buttons = cur;
}

/* ---- Touch (pointer) input ----
 * SceTouch reports the physical front panel (960x544). We map that
 * coordinate into the virtual J2ME screen (240x320 / 320x240) and emit
 * MIDP_PEN_EVENT. Track the previous touch position so a held+move
 * finger turns into KEYMAP_STATE_DRAGGED instead of press/release spam. */
static int touch_initialized = 0;
static int touch_x = 0, touch_y = 0;      /* last virtual position */
static int touch_down = 0;                /* finger currently down */
/* v02.85: virtual keypad press state. Only one virtual button can be
 * held at a time (the panel reports a single primary finger), so a
 * single slot is enough. -1 = none. */
static int vkey_down_btn = -1;
static int vkey_down_key = 0;

/* v03.80: one line per virtual-key edge with the same wall clock as [INPUT]/ptime, so the finger time can be
 * compared with the game's ~0.8 s frame period. */
static void vkey_edge_log(const char *what, int key, int btn, long long held_ms) {
    struct timeval tv;
    char line[112];
    int fd;
    int n;
    gettimeofday(&tv, NULL);
    n = snprintf(line, sizeof(line), "[VKEY] %s key=%d btn=%d ms=%lld", what, key, btn,
                 (long long)tv.tv_sec * 1000 + tv.tv_usec / 1000);
    if (held_ms >= 0 && n > 0 && n < (int)sizeof(line)) {
        n += snprintf(line + n, sizeof(line) - (size_t)n, " held=%lld", held_ms);
    }
    if (n > 0 && n < (int)sizeof(line) - 1) {
        line[n++] = '\n';
        line[n] = '\0';
    }
    fd = sceIoOpen("ux0:/data/J2ME00001/input_debug.log", SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0777);
    if (fd >= 0) {
        sceIoWrite(fd, line, strlen(line));
        sceIoClose(fd);
    }
}

/* v04.10: minimum time a virtual key stays down. log176/177: 33 of 34 and 37 of 54 taps lasted under 200 ms (median
 * ~120 ms) while a slow game samples its key state once per frame, ~0.8 s, so most taps fell between two samples.
 * A tap now lasts at least one frame period of the running game (smoothed time between finished frames, see
 * vita_display_frame_period_ms; 0 for a menu that only repaints on input), capped at VKEY_HOLD_AUTO_MAX_MS.
 * defaults.cfg: vkeyhold=0 turns it off, vkeyhold=<ms> forces a fixed time, anything else / absent = automatic. */
#define VKEY_HOLD_AUTO_MAX_MS 1000
#define VKEY_HOLD_FIXED_MAX_MS 2000
extern unsigned int vita_display_frame_period_ms(void);

static unsigned long long vkey_press_us = 0;
static unsigned long long vkey_release_due_us = 0;   /* 0 = no release is waiting */
/* v04.33: held virtual key repeats. Text fields (name entry) and list
 * scrolling need it; 450 ms is the usual "this is a repeat, not a second
 * tap" delay and 90 ms ≈ 11 chars/s afterwards. The event is
 * KEYMAP_STATE_REPEATED, which MIDP delivers as Canvas.keyRepeated() - the
 * same state the Clear long-press already uses. */
#define VKEY_REPEAT_DELAY_US  (450ULL * 1000ULL)
#define VKEY_REPEAT_PERIOD_US  (90ULL * 1000ULL)
static unsigned long long vkey_repeat_next_us = 0;   /* 0 = no repeat armed */

/* -2 not read yet, -1 automatic, >= 0 fixed milliseconds. */
static int vkey_hold_cfg(void) {
    static int cached = -2;
    if (cached == -2) {
        char buf[512];
        SceUID fd = sceIoOpen("ux0:/data/J2ME00001/defaults.cfg", SCE_O_RDONLY, 0);
        int n = 0;
        cached = -1;
        if (fd >= 0) {
            n = sceIoRead(fd, buf, sizeof(buf) - 1);
            sceIoClose(fd);
        }
        if (n > 0) {
            const char *p;
            buf[n] = '\0';
            for (p = buf; (p = strstr(p, "vkeyhold=")) != NULL; p += 9) {
                if (p == buf || p[-1] == '\n') {
                    const char *v = p + 9;
                    if (v[0] >= '0' && v[0] <= '9') {
                        int ms = 0;
                        while (*v >= '0' && *v <= '9' && ms < VKEY_HOLD_FIXED_MAX_MS) {
                            ms = ms * 10 + (*v - '0');
                            v++;
                        }
                        cached = ms > VKEY_HOLD_FIXED_MAX_MS ? VKEY_HOLD_FIXED_MAX_MS : ms;
                    }
                    break;
                }
            }
        }
    }
    return cached;
}

static unsigned long long vkey_min_hold_us(void) {
    int cfg = vkey_hold_cfg();
    unsigned int ms;
    if (cfg >= 0) {
        ms = (unsigned int)cfg;
    } else {
        ms = vita_display_frame_period_ms();
        if (ms > VKEY_HOLD_AUTO_MAX_MS) {
            ms = VKEY_HOLD_AUTO_MAX_MS;
        }
    }
    return (unsigned long long)ms * 1000ULL;
}

/* Release whatever virtual button is held, now. Safe to call when none is. */
static void vkey_release_now(void) {
    if (vkey_down_btn >= 0) {
        long long held = (long long)((sceKernelGetProcessTimeWide() - vkey_press_us) / 1000ULL);
        vkey_edge_log("release", vkey_down_key, vkey_down_btn, held);
        emit_key_event(vkey_down_key, KEYMAP_STATE_RELEASED);
        vkey_down_btn = -1;
        vkey_down_key = 0;
        vkey_release_due_us = 0;
        vkey_repeat_next_us = 0;
        /* v02.89: clear the visual pressed state too. */
        vita_vkey_set_pressed(-1);
    }
}

/* The finger left the button: release it, but not before the minimum hold has passed. */
static void vkey_release(void) {
    unsigned long long now, need;
    if (vkey_down_btn < 0 || vkey_release_due_us != 0) {
        return;     /* nothing held, or its release is already scheduled */
    }
    need = vkey_min_hold_us();
    now = sceKernelGetProcessTimeWide();
    if (need == 0 || now - vkey_press_us >= need) {
        vkey_release_now();
        return;
    }
    vkey_release_due_us = vkey_press_us + need;
}

/* Called every poll: sends a release that was held back once its time has come. */
static void vkey_hold_service(void) {
    if (vkey_release_due_us != 0 && sceKernelGetProcessTimeWide() >= vkey_release_due_us) {
        vkey_release_now();
    }
}

/* Returns 1 when the touch was consumed by a virtual button. */
static int vkey_touch(int px, int py) {
    int key = 0, btn = -1;

    if (!vita_vkey_enabled()) {
        return 0;
    }
    if (!vita_vkey_hit(px, py, &key, &btn)) {
        if (vkey_down_btn >= 0) {
            /* slid off the button: release it and let the caller decide
             * whether this position means a pointer press instead */
            vkey_release();
        }
        return 0;
    }
    /* v04.21: the d-pad centre is FIRE. A finger already down on another key that slides across the centre on its way to
     * the next direction must not fire it; a fresh touch (nothing held, or its release already scheduled) may. */
    if (key == KEYMAP_KEY_SELECT && vkey_down_btn >= 0 && vkey_release_due_us == 0 && btn != vkey_down_btn) {
        return 1;
    }
    if (btn == vkey_down_btn) {
        unsigned long long now;

        vkey_release_due_us = 0;    /* finger is back on a key whose release was held back: it stays down */
        /* v04.33: a held key repeats (see VKEY_REPEAT_DELAY_US). Only the
         * same button that is still down can repeat, so sliding onto
         * another key never produces a repeat for the old one. */
        if (vkey_repeat_next_us != 0) {
            now = sceKernelGetProcessTimeWide();
            if (now >= vkey_repeat_next_us) {
                emit_key_event(vkey_down_key, KEYMAP_STATE_REPEATED);
                vkey_repeat_next_us = now + VKEY_REPEAT_PERIOD_US;
            }
        }
        return 1;   /* same button: no new press event */
    }
    vkey_release_now();
    vkey_down_btn = btn;
    vkey_down_key = key;
    vkey_press_us = sceKernelGetProcessTimeWide();
    vkey_repeat_next_us = vkey_press_us + VKEY_REPEAT_DELAY_US;
    /* v02.89: highlight it until the finger lifts (see vita_vkey_draw). */
    vita_vkey_set_pressed(btn);
    vkey_edge_log("press", key, btn, -1);
    emit_key_event(key, KEYMAP_STATE_PRESSED);
    return 1;
}
/* Front panel report space: real HW reports in a 1920x1088 grid while
 * the display is 960x544 (the SDK touch sample uses "y >= 1000" as a
 * swipe threshold, proving the 2x scale). Read the active area from
 * the panel info instead of hardcoding, so Vita3K quirks also work. */
static int touch_max_x = 1919, touch_max_y = 1087;

static void vita_touch_init(void) {
    if (touch_initialized) {
        return;
    }
    /* MANDATORY: the front panel starts in the INACTIVE sampling state.
     * Without this call sceTouchPeek succeeds but always reports
     * reportNum == 0, so touch silently looks "not supported". */
    SceTouchSamplingState ss = SCE_TOUCH_SAMPLING_STATE_STOP;
    int ssr = sceTouchGetSamplingState(SCE_TOUCH_PORT_FRONT, &ss);
    if (ss != SCE_TOUCH_SAMPLING_STATE_START) {
        ssr = sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT,
                                       SCE_TOUCH_SAMPLING_STATE_START);
    }
    {
        SceTouchPanelInfo panel;
        memset(&panel, 0, sizeof(panel));
        if (sceTouchGetPanelInfo(SCE_TOUCH_PORT_FRONT, &panel) == 0 &&
            panel.maxAaX > 0 && panel.maxAaY > 0) {
            touch_max_x = panel.maxAaX;
            touch_max_y = panel.maxAaY;
        }
    }
    /* DEBUG: record the sampling-state setup for field diagnosis. */
    {
        char dbg[128];
        snprintf(dbg, sizeof(dbg),
                 "[TOUCH] init: state=%d set=%d max=(%d,%d)\n",
                 (int)ss, ssr, touch_max_x, touch_max_y);
        int fd = sceIoOpen("ux0:/data/J2ME00001/input_debug.log",
                           SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0777);
        if (fd >= 0) {
            sceIoWrite(fd, dbg, strlen(dbg));
            sceIoClose(fd);
        }
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
    /* DEBUG: log a touch lifecycle line only on state transitions so
     * the file stays readable (poll runs every pump cycle). */
    static int dbg_down = -1; /* -1 = unknown, 1 = on panel, 0 = off */

    if (!touch_initialized) {
        vita_touch_init();
    }

    vkey_hold_service();

    n = sceTouchPeek(SCE_TOUCH_PORT_FRONT, &touch, 1);
    if (n < 0) {
        if (dbg_down != -2) {
            char dbg[64];
            snprintf(dbg, sizeof(dbg), "[TOUCH] peek FAILED n=%d\n", n);
            int fd = sceIoOpen("ux0:/data/J2ME00001/input_debug.log",
                               SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND,
                               0777);
            if (fd >= 0) {
                sceIoWrite(fd, dbg, strlen(dbg));
                sceIoClose(fd);
            }
            dbg_down = -2;
        }
        return;
    }

    /* v02.65: same for the front panel. While the system keyboard is up its
     * taps must not reach the MIDlet behind it. If a finger DOES show up here
     * the dialog is not capturing touch - log the first few (evidence for
     * "does the CommonDialog own the panel?"). A finger that was down when
     * the keyboard opened is released so the MIDlet is not left in a drag. */
    {
        static int ime_was_active = 0;
        static int ime_touch_seen = 0;
        if (vita_ime_is_active()) {
            if (!ime_was_active) {
                ime_touch_seen = 0;
            }
            ime_was_active = 1;
            if (touch.reportNum > 0 && ime_touch_seen < 3) {
                ime_touch_seen++;
                fprintf(stderr, "[IME] touch visible to the app while the "
                        "dialog is up: raw (%d,%d)\n",
                        touch.report[0].x, touch.report[0].y);
            }
            if (touch_down) {
                MidpEvent evt;
                MIDP_EVENT_INITIALIZE(evt);
                evt.type = MIDP_PEN_EVENT;
                evt.ACTION = KEYMAP_STATE_RELEASED;
                evt.X_POS = touch_x;
                evt.Y_POS = touch_y;
                ring_push(&evt);
                touch_down = 0;
            }
            /* v02.85: a virtual button held when the dialog opened must
             * not stay pressed in the MIDlet. */
            vkey_release_now();
            return;
        }
        ime_was_active = 0;
    }

    if (touch.reportNum <= 0) {
        /* No finger on the panel. */
        vkey_release();
        if (dbg_down != 0) {
            char dbg[48];
            snprintf(dbg, sizeof(dbg), "[TOUCH] up\n");
            int fd = sceIoOpen("ux0:/data/J2ME00001/input_debug.log",
                               SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND,
                               0777);
            if (fd >= 0) {
                sceIoWrite(fd, dbg, strlen(dbg));
                sceIoClose(fd);
            }
            dbg_down = 0;
        }
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

    /* First report = primary finger. Reports arrive in the panel's own
     * space (1920x1088 on HW) - normalize into the 960x544 display
     * space the display mapping expects. */
    px = touch.report[0].x * 960 / (touch_max_x + 1);
    py = touch.report[0].y * 544 / (touch_max_y + 1);

    /* v02.92: the screenshot camera button. The shot poller thread does
     * the actual hit test and capture (so it also works while the VM is
     * frozen); here we only have to make sure the tap does not become a
     * pointer event for the MIDlet. */
    if (vita_shot_button_hit(px, py)) {
        if (dbg_down != 4) {
            char dbg[48];
            snprintf(dbg, sizeof(dbg), "[SHOT] touch button\n");
            int fd = sceIoOpen("ux0:/data/J2ME00001/input_debug.log",
                               SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND,
                               0777);
            if (fd >= 0) {
                sceIoWrite(fd, dbg, strlen(dbg));
                sceIoClose(fd);
            }
            dbg_down = 4;
        }
        if (touch_down) {
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

    /* v02.85: the keypad lives in the letterbox bars, i.e. OUTSIDE the
     * game rectangle - so it has to be tested before the map_touch()
     * rejection below. A hit consumes the touch: no pointer event is
     * generated, otherwise games that handle pointerPressed would react
     * to every virtual key press. */
    if (vkey_touch(px, py)) {
        if (dbg_down != 3) {
            char dbg[80];
            snprintf(dbg, sizeof(dbg), "[VKEY] touch phys=(%d,%d)\n", px, py);
            int fd = sceIoOpen("ux0:/data/J2ME00001/input_debug.log",
                               SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND,
                               0777);
            if (fd >= 0) {
                sceIoWrite(fd, dbg, strlen(dbg));
                sceIoClose(fd);
            }
            dbg_down = 3;
        }
        /* keep the pen state clean: a pointer press must not survive
         * the finger wandering onto the keypad */
        if (touch_down) {
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

    if (vita_display_map_touch(px, py, &vx, &vy) != 0) {
        /* Finger on the panel but in the letterbox - log only the first
         * contact so a resting finger does not spam the log. */
        if (dbg_down != 1) {
            char dbg[80];
            snprintf(dbg, sizeof(dbg),
                     "[TOUCH] down OUTSIDE (phys %d,%d)\n", px, py);
            int fd = sceIoOpen("ux0:/data/J2ME00001/input_debug.log",
                               SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND,
                               0777);
            if (fd >= 0) {
                sceIoWrite(fd, dbg, strlen(dbg));
                sceIoClose(fd);
            }
            dbg_down = 1;
        }
        return; /* touch in the letterbox - ignore */
    }

    if (!touch_down) {
        state = KEYMAP_STATE_PRESSED;
    } else if (vx != touch_x || vy != touch_y) {
        state = KEYMAP_STATE_DRAGGED;
    } else {
        state = 0; /* no change */
    }

    if (dbg_down != 2) {
        char dbg[96];
        snprintf(dbg, sizeof(dbg),
                 "[TOUCH] down INSIDE phys=(%d,%d) virt=(%d,%d)\n",
                 px, py, vx, vy);
        int fd = sceIoOpen("ux0:/data/J2ME00001/input_debug.log",
                           SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0777);
        if (fd >= 0) {
            sceIoWrite(fd, dbg, strlen(dbg));
            sceIoClose(fd);
        }
        dbg_down = 2;
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
     * sampling the pad (i.e. inside a blocked native call).
     * v01.71: also sample the wall clock (gettimeofday, the exact
     * source behind Os::java_time_millis). If the count advances but
     * the timestamp freezes, the time source itself died (H2) and no
     * timed waiter on it can ever wake - which would freeze every
     * Java thread while the pump keeps running. */
    static unsigned int hb = 0;
    static jlong hb_last_ms = 0;
    /* v01.81: 0x3FF -> 0x3F. At 1024 pumps/sample (~40 s) every real
     * hang died between sample #1 and #2, so the clock liveness data
     * this heartbeat exists for was never captured. 64 pumps ~= 1-3 s:
     * a frozen gettimeofday now shows up as ms= stuck across two (or
     * more) consecutive lines before the log goes silent. */
    if ((hb++ & 0x3F) == 0) {
        struct timeval tv;
        gettimeofday(&tv, NULL);
        jlong ms = (jlong)tv.tv_sec * 1000 + tv.tv_usec / 1000;
        /* vita_checkevents.c diagnostics counters (v01.71) */
        extern volatile unsigned int vita_ce_count;
        extern volatile unsigned int vita_ani_enter;
        extern volatile unsigned int vita_ani_exit;
        char dbg[128];
        snprintf(dbg, sizeof(dbg),
                 "[HEARTBEAT] #%u ms=%lld d=%lld CE=%u ani=%u/%u\n",
                 hb, (long long)ms, (long long)(ms - hb_last_ms),
                 vita_ce_count, vita_ani_enter, vita_ani_exit);
        hb_last_ms = ms;
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

    /* v02.64: drive the CommonDialog while the IME is up even if Java is
     * not repainting (see vita_display.c). No-op when the IME is idle. */
    {
        extern void vita_display_ime_tick(void);
        vita_display_ime_tick();
    }

    /* v02.95: repaint while a virtual-keypad tap is still flashing, so the
     * "pressed" highlight is visible even in a game that only repaints
     * after it receives the key (see vita_display_vkey_tick). No-op when
     * nothing was tapped. */
    {
        extern void vita_display_vkey_tick(void);
        vita_display_vkey_tick();
    }

    /* v02.50: system IME result. Delivered straight into pNewMidpEvent
     * (not the ring) because stringParam1's ownership passes to the MIDP
     * event queue; a ring copy would alias that pointer. */
    if (vita_ime_poll(pNewMidpEvent)) {
        pNewSignal->waitingFor = UI_SIGNAL;
        return;
    }

    if (ring_pop(pNewMidpEvent)) {
        pNewSignal->waitingFor = UI_SIGNAL;
    }
    /* Otherwise leave waitingFor == NO_SIGNAL (caller zeroed it). */
}
