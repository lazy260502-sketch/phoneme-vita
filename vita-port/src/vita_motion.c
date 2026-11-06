/*
 * vita_motion.c - six-axis samples for the JSR-256 sensor classes.
 *
 * WHY A FILE INSTEAD OF A NATIVE METHOD
 * ------------------------------------
 * The JSR-256 implementation lives in midp_system.jar (a dynamically loaded
 * class set), and this port builds the VM with dynamic native methods off
 * ("Dynamic library loading (disabled on Vita - all natives static)", see
 * phoneme-cldc/src/vm/os/vita/OS_vita.cpp). A class that is not ROMized
 * therefore cannot have a native method: its native would never resolve.
 *
 * So the launcher samples sceMotion itself and publishes the result as 24
 * bytes that the Java side reads back through JSR-75 FileConnection. Cost is
 * one 24-byte write per 20 ms and (in Java) at most one small read per frame.
 *
 * Wire format (little endian, 6 x float32):
 *      ax ay az   accelerometer, in g   (Java multiplies by 9.81 -> m/s^2)
 *      gx gy gz   gyroscope, in rad/s
 *
 * A single 24-byte write, no truncate: a reader sees either the previous
 * sample or the new one, never a torn half. Not truncating also keeps this
 * working on Vita3K, which ignores SCE_O_TRUNC (v01.16 lesson).
 */
#include <psp2/motion.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/io/fcntl.h>
#include <stdio.h>
#include <string.h>

#include "vita_motion.h"

#define MOTION_PATH      "ux0:/data/J2ME00001/motion.bin"
#define MOTION_PERIOD_US (20 * 1000)     /* 50 Hz, same as the JSR-256 stub */

static SceUID g_thread = -1;

static int motion_thread(SceSize args, void *argp) {
    (void)args;
    (void)argp;

    for (;;) {
        SceMotionSensorState st;
        float v[6];

        memset(&st, 0, sizeof(st));
        if (sceMotionGetSensorState(&st, 1) >= 0) {
            v[0] = st.accelerometer.x;
            v[1] = st.accelerometer.y;
            v[2] = st.accelerometer.z;
            v[3] = st.gyro.x;
            v[4] = st.gyro.y;
            v[5] = st.gyro.z;
            {
                SceUID fd = sceIoOpen(MOTION_PATH,
                                      SCE_O_WRONLY | SCE_O_CREAT, 0777);
                if (fd >= 0) {
                    sceIoWrite(fd, v, sizeof(v));
                    sceIoClose(fd);
                }
            }
        }
        sceKernelDelayThread(MOTION_PERIOD_US);
    }

    return 0;
}

void vita_motion_start(void) {
    if (g_thread >= 0) {
        return;
    }
    /* Start sampling before the first read, otherwise the first few samples
     * are zeros (which a game reads as "device in free fall"). */
    sceMotionStartSampling();

    g_thread = sceKernelCreateThread("j2me_motion", motion_thread,
                                     0x10000100, 0x2000, 0, 0, NULL);
    if (g_thread < 0) {
        fprintf(stderr, "[MOTION] thread create failed: 0x%08x\n", g_thread);
        g_thread = -1;
        return;
    }
    sceKernelStartThread(g_thread, 0, 0);
    fprintf(stderr, "[MOTION] six-axis publishing to %s\n", MOTION_PATH);
}
