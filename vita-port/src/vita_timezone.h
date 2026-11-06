/*
 * vita_timezone.h - local time zone for MIDP (see vita_timezone.c).
 *
 * Returns a static "GMT+HH:MM" string in the format
 * midp_timezone_md.c's getLocalTimeZone() contract asks for.  The string
 * is owned by this module and must not be freed.
 */
#ifndef VITA_TIMEZONE_H
#define VITA_TIMEZONE_H

const char *vita_local_timezone(void);

#endif /* VITA_TIMEZONE_H */
