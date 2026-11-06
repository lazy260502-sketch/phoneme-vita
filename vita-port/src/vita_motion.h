/*
 * vita_motion.h - six-axis publisher for JSR-256 (see vita_motion.c).
 */
#ifndef VITA_MOTION_H
#define VITA_MOTION_H

/* Start sceMotion sampling and the 50 Hz publisher thread (idempotent). */
void vita_motion_start(void);

#endif /* VITA_MOTION_H */
