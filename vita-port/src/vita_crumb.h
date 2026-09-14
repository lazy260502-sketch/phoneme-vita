/*
 * vita_crumb.h - stdio-free diagnostic breadcrumbs (v01.57 crash hunt).
 *
 * See vita_crumb.c for the rationale: the faulting PC sits inside
 * pte_osSemaphoreCreate (newlib FILE-lock init) on the VM's exception
 * path, so any fprintf(stderr) breadcrumb would itself risk detonating
 * the bug. These helpers write straight to
 * ux0:/data/J2ME00001/crumb.log via sceIo, with no FILE*, no locks and
 * no allocation on the diagnostic path.
 */

#ifndef VITA_CRUMB_H
#define VITA_CRUMB_H

#ifdef __cplusplus
extern "C" {
#endif

/* Append "==== label ====" section bar. */
void crumb_marker(const char *label);

/* Append one printf-style line (+ CRLF). Never touches stdio. */
void crumb_printf(const char *fmt, ...)
    __attribute__((format(printf, 1, 2)));

/* Generic append channel: per-path cached handle, sceIo only. Used by
 * the VM output hooks (JVMSPI_PrintRaw / pcsl_print_chars). */
void crumb_append(const char *path, const char *s, int len);

/* Close the log handle so every byte written so far is on disk. */
void crumb_flush(void);

/* Convenience: marker + printf in one call. */
#define CRUMB(...)      crumb_printf(__VA_ARGS__)
#define CRUMB_SEC(l)    crumb_marker(l)

#ifdef __cplusplus
}
#endif

#endif /* VITA_CRUMB_H */
