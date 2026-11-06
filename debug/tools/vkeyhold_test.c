/* Host test for the virtual-key minimum hold in vita-port/src/vita_input.c (v04.10).
 * The state machine below is a LOGIC COPY of vkey_min_hold_us / vkey_release_now / vkey_release / vkey_hold_service
 * and of the press/slide/same-key branches of vkey_touch, with the Vita calls replaced by a fake clock. It checks the
 * races (short tap, long press, finger back on a held-back key, another key, off, no period); it does not compile the
 * real file. Build: PATH=/usr/bin:/bin gcc -Wall -o /tmp/vkeyhold_test debug/tools/vkeyhold_test.c */
#include <stdio.h>

typedef unsigned long long u64;
static u64 now_us;
static u64 sceKernelGetProcessTimeWide(void) { return now_us; }

#define VKEY_HOLD_AUTO_MAX_MS 1000
static int cfg = -1;                 /* -1 auto, >=0 fixed ms */
static unsigned int period_ms = 800; /* vita_display_frame_period_ms() */

static int vkey_down_btn = -1, vkey_down_key = 0;
static u64 vkey_press_us = 0, vkey_release_due_us = 0;
static int n_press = 0, n_release = 0;
static u64 last_release_ms = 0, last_held_ms = 0;

static u64 vkey_min_hold_us(void) {
    unsigned int ms;
    if (cfg >= 0) {
        ms = (unsigned int)cfg;
    } else {
        ms = period_ms;
        if (ms > VKEY_HOLD_AUTO_MAX_MS) ms = VKEY_HOLD_AUTO_MAX_MS;
    }
    return (u64)ms * 1000ULL;
}
static void vkey_release_now(void) {
    if (vkey_down_btn >= 0) {
        last_held_ms = (sceKernelGetProcessTimeWide() - vkey_press_us) / 1000ULL;
        last_release_ms = sceKernelGetProcessTimeWide() / 1000ULL;
        n_release++;
        vkey_down_btn = -1;
        vkey_down_key = 0;
        vkey_release_due_us = 0;
    }
}
static void vkey_release(void) {
    u64 t, need;
    if (vkey_down_btn < 0 || vkey_release_due_us != 0) return;
    need = vkey_min_hold_us();
    t = sceKernelGetProcessTimeWide();
    if (need == 0 || t - vkey_press_us >= need) { vkey_release_now(); return; }
    vkey_release_due_us = vkey_press_us + need;
}
static void vkey_hold_service(void) {
    if (vkey_release_due_us != 0 && sceKernelGetProcessTimeWide() >= vkey_release_due_us) vkey_release_now();
}
#define FIRE_BTN 9
static int n_fire_press = 0;
static void touch_hit(int btn) {
    /* v04.21: LOGIC COPY of the FIRE guard in vkey_touch */
    if (btn == FIRE_BTN && vkey_down_btn >= 0 && vkey_release_due_us == 0 && btn != vkey_down_btn) return;
    if (btn == vkey_down_btn) { vkey_release_due_us = 0; return; }
    vkey_release_now();
    vkey_down_btn = btn;
    vkey_down_key = btn + 100;
    vkey_press_us = sceKernelGetProcessTimeWide();
    n_press++;
    if (btn == FIRE_BTN) n_fire_press++;
}
static void touch_miss(void) { if (vkey_down_btn >= 0) vkey_release(); }
static void finger_up(void) { vkey_release(); }

static void reset(int c, unsigned int p) {
    cfg = c; period_ms = p;
    vkey_down_btn = -1; vkey_down_key = 0; vkey_press_us = 0; vkey_release_due_us = 0;
    n_press = n_release = 0; last_release_ms = last_held_ms = 0; now_us = 1000000ULL;
}
/* one poll cycle at +ms */
static void poll(unsigned int ms) { now_us += (u64)ms * 1000ULL; vkey_hold_service(); }

static int fails = 0;
#define CHECK(c, msg) do { if (!(c)) { printf("FAIL: %s\n", msg); fails++; } else printf("ok:   %s\n", msg); } while (0)

int main(void) {
    int i;

    reset(-1, 800);                                  /* short tap, slow game: release held back to 800 ms */
    touch_hit(3); poll(60); finger_up();
    CHECK(n_release == 0, "60 ms tap, period 800: release not sent yet");
    for (i = 0; i < 100; i++) { poll(10); finger_up(); }
    CHECK(n_release == 1 && last_held_ms >= 800 && last_held_ms < 820, "released once at ~800 ms held");

    reset(-1, 800);                                  /* long press: released immediately on lift */
    touch_hit(3); poll(1500); finger_up();
    CHECK(n_release == 1 && last_held_ms == 1500, "1500 ms press released at once");

    reset(-1, 800);                                  /* finger returns to the same key while held back */
    touch_hit(3); poll(60); finger_up(); poll(100); touch_hit(3);
    CHECK(n_press == 1 && n_release == 0 && vkey_release_due_us == 0, "same key again: still one press, no release, pending cleared");
    poll(900); finger_up();
    CHECK(n_release == 1, "then lifted after the period: released");

    reset(-1, 800);                                  /* another key while one is held back */
    touch_hit(3); poll(60); finger_up(); poll(100); touch_hit(4);
    CHECK(n_release == 1 && n_press == 2 && vkey_down_btn == 4, "other key: release of the first, press of the second, in order");

    reset(0, 800);                                   /* vkeyhold=0 */
    touch_hit(3); poll(60); finger_up();
    CHECK(n_release == 1 && last_held_ms == 60, "vkeyhold=0: released on lift");

    reset(-1, 0);                                    /* menu: no frame period known */
    touch_hit(3); poll(60); finger_up();
    CHECK(n_release == 1 && last_held_ms == 60, "period 0: released on lift");

    reset(-1, 5000);                                 /* cap */
    touch_hit(3); poll(60); finger_up();
    for (i = 0; i < 200; i++) { poll(10); finger_up(); }
    CHECK(n_release == 1 && last_held_ms >= 1000 && last_held_ms < 1020, "period 5000 capped at 1000 ms");

    reset(300, 800);                                 /* fixed ms from the cfg */
    touch_hit(3); poll(60); finger_up();
    for (i = 0; i < 100; i++) { poll(10); finger_up(); }
    CHECK(n_release == 1 && last_held_ms >= 300 && last_held_ms < 320, "vkeyhold=300 fixed");

    reset(-1, 800);                                  /* slid off the key */
    touch_hit(3); poll(60); touch_miss();
    CHECK(n_release == 0, "slid off: held back too");
    poll(800); 
    CHECK(n_release == 1, "slid off: released by the poll once due");

    reset(-1, 800);                                  /* a second lift call must not move the due time */
    touch_hit(3); poll(60); finger_up();
    { u64 due = vkey_release_due_us; poll(100); finger_up(); CHECK(vkey_release_due_us == due, "repeated lift keeps the due time"); }

    reset(-1, 800); n_fire_press = 0;                /* held UP, finger slides across the centre */
    touch_hit(1); poll(30); touch_hit(FIRE_BTN); poll(30); touch_hit(FIRE_BTN);
    CHECK(n_fire_press == 0 && n_press == 1 && n_release == 0 && vkey_down_btn == 1, "slide across FIRE: not fired, UP stays down");
    poll(30); touch_hit(2);
    CHECK(n_fire_press == 0 && n_press == 2 && n_release == 1 && vkey_down_btn == 2, "UP -> centre -> DOWN: FIRE never pressed");

    reset(-1, 800); n_fire_press = 0;                /* fresh tap on the centre */
    touch_hit(FIRE_BTN);
    CHECK(n_fire_press == 1 && vkey_down_btn == FIRE_BTN, "first touch on FIRE: pressed");

    reset(-1, 800); n_fire_press = 0;                /* UP tapped, release held back, then a NEW tap on FIRE */
    touch_hit(1); poll(60); finger_up(); poll(100); touch_hit(FIRE_BTN);
    CHECK(n_fire_press == 1 && n_release == 1 && vkey_down_btn == FIRE_BTN, "new tap on FIRE while UP's release is held back: fires");

    reset(-1, 800); n_fire_press = 0;                /* FIRE held, finger slides to UP */
    touch_hit(FIRE_BTN); poll(30); touch_hit(1);
    CHECK(n_fire_press == 1 && n_release == 1 && vkey_down_btn == 1, "FIRE -> UP: FIRE released, UP pressed");

    {   /* replay budget: LOGIC COPY of the signature check in vita_display_vkey_tick (v04.18+) */
        int sig = -2, replays = 0, k;
        unsigned int same = 0;
#define REPLAY_TICK(pressed) do { int p_ = (pressed); \
            if (p_ != sig) { sig = p_; same = 0; } \
            if (same < 3) { same++; replays++; } } while (0)
        for (k = 0; k < 100; k++) REPLAY_TICK(3);
        CHECK(replays == 3, "held 100 ticks on one button: 3 replays");
        for (k = 0; k < 100; k++) REPLAY_TICK(-1);
        CHECK(replays == 6, "button released: 3 more");
        for (k = 0; k < 100; k++) REPLAY_TICK(4);
        CHECK(replays == 9, "other button: 3 more");
        for (k = 0; k < 100; k++) REPLAY_TICK(4);
        CHECK(replays == 9, "same button again without a change: none");
        REPLAY_TICK(3); REPLAY_TICK(3); REPLAY_TICK(3); REPLAY_TICK(3);
        CHECK(replays == 12, "switch back to 3: exactly 3");
    }

    printf("%s (%d failures)\n", fails ? "FAILED" : "PASSED", fails);
    return fails ? 1 : 0;
}
