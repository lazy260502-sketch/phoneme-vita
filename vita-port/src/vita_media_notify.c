/*
 * vita_media_notify.c - javanotify_on_media_notification() for the Vita port
 *
 * The upstream implementation lives in javanotify_midp_jsr.c, which belongs
 * to the 'javacall_application' subsystem - only compiled when PLATFORM ==
 * javacall. The vita_arm MIDP build never includes it, and the whole
 * midp_jc_event_send() pipeline is absent from the link closure too.
 *
 * Instead of resurrecting that pipeline we reuse the pattern already
 * established by vita_input.c: push a MidpEvent into the SPSC ring that
 * checkForSystemSignal() (VM thread) drains, and set waitingFor =
 * MEDIA_EVENT_SIGNAL. midp_master_mode_events.c then routes it:
 *   case MEDIA_EVENT_SIGNAL:
 *       StoreMIDPEventInVmThread(newMidpEvent, newMidpEvent.MM_ISOLATE);
 *       eventUnblockJavaThread(..., MEDIA_EVENT_SIGNAL, ...);
 * which is exactly what the upstream javacall slavemode bridge does for
 * MIDP_JC_EVENT_MULTIMEDIA.
 *
 * Field mapping mirrors midp_slavemode_javacall.c lines 314-319:
 *   MM_PLAYER_ID  = playerId
 *   MM_DATA       = (int)data
 *   MM_ISOLATE    = appId
 *   MM_EVT_TYPE   = javacall_media_notification_type (Java MMEventListener
 *                   consumes the same numeric values)
 *   MM_EVT_STATUS = status
 */

#include <midpEvents.h>
#include <midpServices.h>
#include <javacall_defs.h>
#include <javanotify_multimedia.h>

/* Same ring discipline as vita_input.c (kept independent on purpose:
 * media events come from the audio thread, input events from the VM
 * thread itself, and sharing one ring would need extra locking). */
#define VITA_MEDIA_RING_SIZE 16

static MidpEvent media_ring[VITA_MEDIA_RING_SIZE];
static volatile int media_head = 0; /* written by producers */
static volatile int media_tail = 0; /* written by VM thread */

static void media_ring_push(const MidpEvent *evt) {
    int next = (media_head + 1) % VITA_MEDIA_RING_SIZE;
    if (next == media_tail) {
        /* Ring full: drop the oldest event so a stuck consumer cannot
         * wedge the audio thread. */
        media_tail = (media_tail + 1) % VITA_MEDIA_RING_SIZE;
    }
    media_ring[media_head] = *evt;
    media_head = next;
}

int vita_media_poll(MidpEvent *out) {
    if (media_tail == media_head) {
        return 0;
    }
    *out = media_ring[media_tail];
    media_tail = (media_tail + 1) % VITA_MEDIA_RING_SIZE;
    return 1;
}

void javanotify_on_media_notification(javacall_media_notification_type type,
                                      int appId,
                                      int playerId,
                                      javacall_result status,
                                      void *data) {
    MidpEvent evt;

    MIDP_EVENT_INITIALIZE(evt);
    evt.type         = MMAPI_EVENT;
    evt.MM_PLAYER_ID = playerId;
    evt.MM_DATA      = (int)(intptr_t)data;
    evt.MM_ISOLATE   = appId;
    evt.MM_EVT_TYPE  = (int)type;
    evt.MM_EVT_STATUS = (int)status;

    media_ring_push(&evt);
}
