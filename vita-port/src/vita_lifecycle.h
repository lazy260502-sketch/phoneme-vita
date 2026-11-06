/*
 * vita_lifecycle.h - suspend/resume handling (see vita_lifecycle.c).
 */
#ifndef VITA_LIFECYCLE_H
#define VITA_LIFECYCLE_H

/* Start the resume listener once (idempotent). */
void vita_lifecycle_init(void);

/* Number of resumes handled so far (diagnostics / diag overlay). */
int vita_lifecycle_resumes(void);

#endif /* VITA_LIFECYCLE_H */
