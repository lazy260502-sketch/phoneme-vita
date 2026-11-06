/*
 * vita_lifecycle.c - app suspend/resume handling for the J2ME port.
 *
 * Why
 * ---
 * When the user presses the PS button the console suspends the process;
 * on return sceAppMgr posts SCE_APPMGR_SYSTEMEVENT_ON_RESUME.  Until now
 * nothing listened, and two things broke across a suspend/resume cycle:
 *
 *   1. Audio.  sceAudioOutReleasePort/OpenPort are tied to the suspend
 *      cycle: a port handle opened before the suspend keeps returning an
 *      error from sceAudioOutOutput afterwards, and the port is only
 *      opened lazily once (g_audio_port >= 0), so ALL sound stayed dead
 *      for the rest of the session.
 *   2. Clock.  The 444/222 MHz request made at boot (vita_perf.c) is not
 *      guaranteed to survive the resume - the console re-applies its own
 *      power profile, and the game silently drops back to 333 MHz.
 *
 * What
 * ----
 * One thread blocks in sceAppMgrReceiveSystemEvent() and, on a resume,
 * reopens the audio port and re-issues the clock requests the boot code
 * had successfully applied.  Nothing here touches the VM or MIDP state,
 * so it cannot make a round loop less stable.
 *
 * KNOWN GAP (documented in PROJECT_MEMORY.md): MIDlet.pauseApp() /
 * resumeApp() are NOT called yet.  Doing that needs the AMS state
 * machine (MIDletStateHandler) from a native thread, which is a separate
 * piece of work; this module only restores the platform side.
 */
#include <stdio.h>
#include <string.h>

#include <psp2/appmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/power.h>

#include "vita_lifecycle.h"
#include "vita_perf.h"

/* vida_audio_javacall.c: a port from before the suspend is unusable. */
extern void vita_audio_after_resume(void);
/* v04.35: mute while the system UI covers us (see vita_lifecycle.c). */
extern void vita_audio_set_suspended(int suspended);

#define LIFECYCLE_PRIO  0x10000100
#define LIFECYCLE_STACK 0x2000

static SceUID g_thread = -1;
static SceUID g_poll_thread = -1;
static volatile int g_resumes = 0;

/* Reopen the audio port and re-issue the clock that took effect at boot.
 * Shared by the ON_RESUME event and the system-UI detector: both mean "we are
 * back, but the platform state from before may be gone". */
static void lifecycle_resume_now(const char *why) {
    int arm;
    int bus;

    g_resumes++;
    vita_audio_set_suspended(0);
    fprintf(stderr, "[LIFE] resume #%d (%s): reopening audio port\n",
            g_resumes, why);
    vita_audio_after_resume();

    /* Re-issue only what actually took effect at boot; a failed 444 MHz
     * request must not be retried here (v03.06 shows the kernel can accept
     * the call and ignore it). */
    arm = vita_perf_clock_arm();
    bus = vita_perf_clock_bus();
    if (arm > 0) {
        scePowerSetArmClockFrequency(arm);
    }
    if (bus > 0) {
        scePowerSetBusClockFrequency(bus);
    }
    fprintf(stderr, "[LIFE] resume #%d: clock arm=%d bus=%d\n",
            g_resumes, arm, bus);
}

static int lifecycle_thread(SceSize args, void *argp) {
    (void)args;
    (void)argp;

    for (;;) {
        SceAppMgrSystemEvent ev;

        memset(&ev, 0, sizeof(ev));
        if (sceAppMgrReceiveSystemEvent(&ev) < 0) {
            /* No event queue (Vita3K, or an event we are not allowed to
             * receive): do not spin, retry once a second. */
            sceKernelDelayThread(1000 * 1000);
            continue;
        }
        if (ev.systemEvent == SCE_APPMGR_SYSTEMEVENT_ON_RESUME) {
            lifecycle_resume_now("ON_RESUME");
        }
    }

    return 0;
}

/* v04.35: the console has no "going to background" event, but it does say when
 * its own UI (PS-button menu / live area) covers the app. That is exactly when
 * audio must stop: the ports are taken away, so a track pushed into them either
 * stalls the render thread or returns as a pop when we come back. One syscall
 * every 200 ms, and the call cannot block, so this cannot cost a frame. */
static int lifecycle_poll_thread(SceSize args, void *argp) {
    int overlaid = 0;
    int warned = 0;

    (void)args;
    (void)argp;
    for (;;) {
        SceAppMgrAppState st;

        memset(&st, 0, sizeof(st));
        if (_sceAppMgrGetAppState(&st, sizeof(st), 0x1000000) >= 0) {
            int now = st.isSystemUiOverlaid ? 1 : 0;
            if (now != overlaid) {
                overlaid = now;
                if (now) {
                    vita_audio_set_suspended(1);
                    fprintf(stderr, "[LIFE] system UI overlaid -> audio muted\n");
                } else {
                    lifecycle_resume_now("system UI gone");
                }
            }
        } else if (!warned) {
            warned = 1;
            fprintf(stderr, "[LIFE] _sceAppMgrGetAppState unavailable; "
                            "only the ON_RESUME event is watched\n");
        }
        sceKernelDelayThread(200 * 1000);
    }

    return 0;
}

void vita_lifecycle_init(void) {
    if (g_thread >= 0) {
        return;
    }
    g_thread = sceKernelCreateThread("j2me_lifecycle", lifecycle_thread,
                                     LIFECYCLE_PRIO, LIFECYCLE_STACK,
                                     0, 0, NULL);
    if (g_thread < 0) {
        fprintf(stderr, "[LIFE] thread create failed: 0x%08x\n", g_thread);
        g_thread = -1;
        return;
    }
    sceKernelStartThread(g_thread, 0, 0);
    fprintf(stderr, "[LIFE] resume listener started (uid=0x%08x)\n", g_thread);

    /* v04.35: separate thread because the event one blocks. A failure here is
     * not fatal - the ON_RESUME path above still works. */
    g_poll_thread = sceKernelCreateThread("j2me_lifeui", lifecycle_poll_thread,
                                          LIFECYCLE_PRIO, LIFECYCLE_STACK,
                                          0, 0, NULL);
    if (g_poll_thread >= 0) {
        sceKernelStartThread(g_poll_thread, 0, 0);
        fprintf(stderr, "[LIFE] system-UI watch started (uid=0x%08x)\n",
                g_poll_thread);
    } else {
        fprintf(stderr, "[LIFE] system-UI watch thread failed: 0x%08x\n",
                g_poll_thread);
        g_poll_thread = -1;
    }
}

int vita_lifecycle_resumes(void) {
    return g_resumes;
}
