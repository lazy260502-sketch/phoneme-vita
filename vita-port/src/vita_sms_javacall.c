/*
 * PS Vita JSR 120/205 SMS (WMA) javacall implementation
 *
 * Since PS Vita has no cellular modem, SMS operations return failure.
 * This stub layer satisfies the javacall_sms_* API required by jsr120.
 *
 * Functions implemented:
 *   javacall_sms_is_service_available()
 *   javacall_sms_send()
 *   javacall_sms_add_listening_port()
 *   javacall_sms_remove_listening_port()
 *
 * Note: javanotify_incoming_sms / javanotify_sms_send_completed are
 *       called by the Java layer, not by this native code.
 */

#include <javacall_sms.h>
#include <javacall_cbs.h>
#include <javacall_memory.h>
#include <javacall_defs.h>
#include <string.h>

/* Called by platform to notify SMS send completion */
extern void javanotify_sms_send_completed(javacall_result result, int handle);

/* Called by platform to deliver incoming SMS */
extern void javanotify_incoming_sms(
    javacall_sms_encoding   msgType,
    char*                   sourceAddress,
    unsigned char*          msgBuffer,
    int                     msgBufferLen,
    unsigned short          sourcePortNum,
    unsigned short          destPortNum,
    javacall_int64          timeStamp);

/**
 * Check if SMS service is available.
 * PS Vita has no cellular modem, so this returns JAVACALL_FAIL.
 */
javacall_result javacall_sms_is_service_available(void) {
    return JAVACALL_FAIL;
}

/**
 * Send an SMS message.
 * PS Vita has no cellular modem, so this returns JAVACALL_FAIL immediately.
 * The result is NOT notified via javanotify_sms_send_completed()
 * since there is no real hardware to handle the send.
 */
javacall_result javacall_sms_send(
    javacall_sms_encoding   msgType,
    const unsigned char*    destAddress,
    const unsigned char*    msgBuffer,
    int                     msgBufferLen,
    unsigned short          sourcePort,
    unsigned short          destPort,
    int                     handle) {
    (void)msgType;
    (void)destAddress;
    (void)msgBuffer;
    (void)msgBufferLen;
    (void)sourcePort;
    (void)destPort;
    (void)handle;
    return JAVACALL_FAIL;
}

/**
 * Register a port to receive incoming SMS.
 * PS Vita has no cellular modem, so this returns JAVACALL_FAIL.
 */
javacall_result javacall_sms_add_listening_port(unsigned short portNum) {
    (void)portNum;
    return JAVACALL_FAIL;
}

/**
 * Unregister a listening port.
 * PS Vita has no cellular modem, so this returns JAVACALL_FAIL.
 */
javacall_result javacall_sms_remove_listening_port(unsigned short portNum) {
    (void)portNum;
    return JAVACALL_FAIL;
}

/**
 * Calculate number of SMS segments needed to send a message.
 * For a real device, this would calculate GSM concatenate SMS segments.
 * Stub returns 1 (single segment) for ASCII up to 160 chars.
 */
int javacall_sms_get_number_of_segments(
    javacall_sms_encoding   msgType,
    char*                   msgBuffer,
    int                     msgBufferLen,
    javacall_bool           hasPort) {
    int portBytes = hasPort ? 6 : 0;  /* 2-byte source + 2-byte dest + 2-byte length */
    int maxUserData;
    int perSegment;

    (void)msgBuffer;

    if (msgType == JAVACALL_SMS_MSG_TYPE_UNICODE_UCS2) {
        /* GSM 03.38 / UCS-2: 70 chars per segment (134 bytes), 66 with port */
        perSegment = (hasPort ? 66 : 70) * 2;  /* in bytes */
    } else if (msgType == JAVACALL_SMS_MSG_TYPE_ASCII) {
        /* GSM 7-bit: 160 chars, 153 with concatenation header */
        perSegment = hasPort ? 153 : 160;
    } else {
        /* Binary: 140 bytes per segment, 134 with concatenation header */
        perSegment = hasPort ? 134 : 140;
    }

    maxUserData = perSegment - portBytes;
    if (maxUserData <= 0) maxUserData = 1;

    if (msgBufferLen <= maxUserData) {
        return 1;
    }
    /* Concatenated SMS: subsequent segments carry the UDH */
    if (perSegment > 153) perSegment = 153;
    perSegment -= portBytes;
    if (perSegment <= 0) perSegment = 1;
    return (msgBufferLen + perSegment - 1) / perSegment;
}

/*
 * ============================ CBS (Cell Broadcast) ============================
 * PS Vita has no cell broadcast receiver; all operations fail gracefully.
 */

javacall_result javacall_cbs_add_listening_msgID(unsigned short msgID) {
    (void)msgID;
    return JAVACALL_FAIL;
}

javacall_result javacall_cbs_remove_listening_msgID(unsigned short msgID) {
    (void)msgID;
    return JAVACALL_FAIL;
}

/*
 * ============================ Memory helper ==================================
 * Required by jsr120_list_element.c / wmaPushRegistry.c
 */
char* javacall_strdup(const char* str) {
    size_t len;
    char* dup;
    if (str == NULL) {
        return NULL;
    }
    len = strlen(str) + 1;
    dup = (char*)javacall_malloc(len);
    if (dup != NULL) {
        memcpy(dup, str, len);
    }
    return dup;
}
