/*
 * vita_checkevents.c - strong JVMSPI_CheckEvents for the Vita port
 *
 * ROOT CAUSE (v01.20 "no keys, no EOM"): libcldc_vm_ani.a contains a
 * STRONG definition of JVMSPI_CheckEvents (anilib/share/ani_bsd_socket.cpp)
 * whose body only handles the debugger and calls
 * ANI_WaitForThreadUnblocking() - it NEVER calls midp_check_events().
 * The version in midp/ams/ams_base_cldc/reference/native/midp_run.c that
 * bridges into the MIDP event pump is __attribute__((weak)), so the ANI
 * strong symbol silently won the link (same class of bug as the v01.15
 * stub-archive shadowing). Result: checkForSystemSignal() never ran -
 * keys and END_OF_MEDIA died in the native rings, while Java-internal
 * events (started/stopped/closed, which never cross native) kept
 * working. That exact symptom pattern is the fingerprint.
 *
 * FIX: this translation unit is compiled as a CMake object of this
 * project. CMake always places its own objects BEFORE all libraries on
 * the linker line, so with --allow-multiple-definition this strong
 * definition is seen first and wins over ani_bsd_socket.cpp's.
 *
 * Semantics: pump MIDP events first (input/touch/media rings -> midp
 * event queue, unblocking waiting Java threads), then give ANI its
 * window. The ANI wait is clamped to <= 50 ms so the pump keeps
 * running at a responsive rate even when the VM asks for a long block.
 */

#include <jvmspi.h>
#include <midp_check_events.h>
#include <ani.h>

void JVMSPI_CheckEvents(JVMSPI_BlockedThreadInfo *blocked_threads,
                        int blocked_threads_count,
                        jlong timeout) {
    /* 1. MIDP event pump: checkForSystemSignal samples the pad and the
     * touch panel, drains the media ring, and routes anything found to
     * the foreground queue / waiting threads. Must run with a 0
     * (non-blocking) timeout so the ANI wait below gets its turn. */
    midp_check_events(blocked_threads, blocked_threads_count, 0);

    /* 2. ANI pool wait - replaces ani_bsd_socket.cpp's call. Clamped so
     * the event pump runs at least every 50 ms even when the scheduler
     * asks to wait forever (-1) or for a long time. */
    if (timeout < 0 || timeout > 50) {
        timeout = 50;
    }
    ANI_WaitForThreadUnblocking(blocked_threads, blocked_threads_count,
                                timeout);
}
