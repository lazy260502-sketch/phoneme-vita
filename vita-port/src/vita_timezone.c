/*
 * vita_timezone.c - "GMT+HH:MM" for the console's local time zone.
 *
 * Why
 * ---
 * phoneME asks the platform for the local time zone from
 * getLocalTimeZone() (phoneme-midp/src/core/timezone/posix/native/
 * midp_timezone_md.c), which derives it from localtime() - gmtime().
 * On this port getenv() is a strong symbol that returns NULL for every
 * name (vita_pcsl.c: it must not expose newlib's environ, see the v04.04
 * note about the unresolved vitalink import), so newlib has no TZ at all
 * and localtime() == gmtime().  The result was GMT+00:00 for every
 * MIDlet: in-game clocks, calendars, countdowns and "daily bonus"
 * timers were all off by the user's offset.
 *
 * What
 * ----
 * Ask the console instead of libc: sceRtcGetCurrentClockLocalTime() is
 * the local wall clock, sceRtcGetCurrentClock(&dt, 0) is the same moment
 * in UTC.  Their difference is the offset in force right now, daylight
 * saving included, exactly what the MIDP contract wants
 * ("GMT-08:00" / "GMT+09:00", at most 11 chars).
 *
 * The value is computed once per process: MIDP caches it as well, and the
 * offset cannot change while the game runs (a system time-zone change
 * reboots the app anyway on this platform).
 */
#include <stdio.h>
#include <time.h>

#include <psp2/rtc.h>

#include "vita_timezone.h"

static char tz_buf[12];
static int tz_ready = 0;

const char *vita_local_timezone(void) {
    SceDateTime local;
    SceDateTime utc;
    time_t t_local = 0;
    time_t t_utc = 0;
    long diff;
    int minutes;
    char sign;

    if (tz_ready) {
        return tz_buf;
    }
    tz_ready = 1;
    tz_buf[0] = '\0';
    snprintf(tz_buf, sizeof(tz_buf), "GMT+00:00");

    if (sceRtcGetCurrentClockLocalTime(&local) < 0 ||
        sceRtcGetCurrentClock(&utc, 0) < 0 ||
        sceRtcGetTime_t(&local, &t_local) < 0 ||
        sceRtcGetTime_t(&utc, &t_utc) < 0) {
        fprintf(stderr, "[TZ] sceRtc query failed, reporting GMT+00:00\n");
        return tz_buf;
    }

    diff = (long)(t_local - t_utc);
    /* Round to the nearest minute: the two calls are microseconds apart,
     * so a sample that straddles a second boundary must not bias us. */
    minutes = (int)((diff >= 0 ? diff + 30 : diff - 30) / 60);
    if (minutes < -12 * 60) {
        minutes = -12 * 60;
    } else if (minutes > 14 * 60) {
        minutes = 14 * 60;
    }

    sign = (minutes < 0) ? '-' : '+';
    if (minutes < 0) {
        minutes = -minutes;
    }
    snprintf(tz_buf, sizeof(tz_buf), "GMT%c%02d:%02d",
             sign, minutes / 60, minutes % 60);
    /* One line per process: enough to confirm the wiring on a real Vita
     * (stderr lands in vm_stderr.log like every other diagnostic). */
    fprintf(stderr, "[TZ] %s (local-UTC %ld s)\n", tz_buf, diff);
    return tz_buf;
}
