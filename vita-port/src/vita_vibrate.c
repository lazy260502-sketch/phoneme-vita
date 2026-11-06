/*
 * vita_vibrate.c - Display.vibrate() backend for phoneME MIDP on PS Vita
 *
 * Replaces phoneme-midp's annunciator/stubs/native/anc_st_vibrate.c,
 * whose anc_start_vibrate() always returns KNI_FALSE - so every J2ME
 * game that calls Display.vibrate() got "not supported" and the feature
 * silently did nothing. The CMake build drops anc_st_vibrate.o from
 * libobj.a and links this file instead, the same mechanism already used
 * for lfjport_fb_export.o and mastermode_export.o.
 *
 * Mechanism: sceCtrlSetActuator(port, &state) with both motor fields at
 * full strength. vitaSDK declares the port as a plain int - "1" means the
 * first paired controller - so a PS TV with a DualShock is the only
 * configuration where rumble is physically felt; the handheld Vita has no
 * motor and the call fails (logged once, then the feature switches itself
 * off so games get a clean "unsupported" answer). Vita3K does not emulate
 * rumble either, so there it is always a no-op.
 *
 * Gating: ux0:/data/J2ME00001/defaults.cfg's "vibrate=0" disables it, so a
 * player who dislikes rumble can switch it off from 设置 without touching
 * the game. The setting is read once per launch (before the VM starts) -
 * the file is not re-read on every vibrate call.
 */
#include <kni.h>
#include <psp2/ctrl.h>
#include <stdio.h>
#include <string.h>

#include "vita_vibrate.h"

/* sceCtrlSetActuator() takes the port as an int: 1 = first paired pad.
 * There is no SCE_CTRL_PORT_* constant in vitaSDK. */
#define VIB_PORT 1

static int g_allowed = 1;      /* set from defaults.cfg before the VM */
static int g_active = 0;       /* a vibrate is currently on          */
static int g_supported = -1;   /* -1 unknown, 0 no actuator, 1 works  */

void vita_vibrate_set_allowed(int allowed) {
    g_allowed = allowed ? 1 : 0;
    if (!g_allowed) {
        vita_vibrate_stop();
    }
}

int vita_vibrate_allowed(void) {
    return g_allowed;
}

void vita_vibrate_start(int duration_ms) {
    SceCtrlActuator act;
    int rc;

    (void)duration_ms;   /* the MIDP layer sends the stop itself (TimerTask) */
    if (!g_allowed) {
        return;
    }
    memset(&act, 0, sizeof(act));
    /* The two fields are the small and the large motor. Some firmware
     * versions only honour one of them, so drive both. */
    act.small = 255;
    act.large = 255;
    rc = sceCtrlSetActuator(VIB_PORT, &act);
    if (rc < 0) {
        /* No actuator on this device (handheld Vita / Vita3K). Latch it so
         * anc_stop_vibrate() can answer "unsupported" from now on, and log
         * only once - a game that vibrates in a loop would spam. */
        g_supported = 0;
        fprintf(stderr, "[VIB] SetActuator rc=0x%08x (no actuator)\n", rc);
        return;
    }
    g_supported = 1;
    g_active = 1;
    fprintf(stderr, "[VIB] start\n");
}

void vita_vibrate_stop(void) {
    SceCtrlActuator act;
    if (!g_active) {
        return;
    }
    memset(&act, 0, sizeof(act));
    sceCtrlSetActuator(VIB_PORT, &act);
    g_active = 0;
    fprintf(stderr, "[VIB] stop\n");
}

int vita_vibrate_supported(void) {
    if (!g_allowed) {
        return 0;
    }
    return g_supported != 0;
}

/* ---- the two symbols anc_st_vibrate.o used to provide ---- */

jboolean anc_start_vibrate(void) {
    if (!vita_vibrate_supported()) {
        return KNI_FALSE;
    }
    vita_vibrate_start(-1);
    return vita_vibrate_supported() ? KNI_TRUE : KNI_FALSE;
}

jboolean anc_stop_vibrate(void) {
    if (!vita_vibrate_supported()) {
        return KNI_FALSE;
    }
    vita_vibrate_stop();
    return KNI_TRUE;
}
