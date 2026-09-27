#ifndef VITA_KERNEL_FORENSICS_H
#define VITA_KERNEL_FORENSICS_H

#ifdef __cplusplus
extern "C" {
#endif

/*
 * v02.18 kernel-side stall forensics (see vita_kernel_forensics.c).
 * Appends to the given log path from the CALLING thread (watchdog).
 * Degrades to a "kubridge absent" line when the plugin is missing.
 */
void vita_kf_dump(const char *path);

#ifdef __cplusplus
}
#endif

#endif /* VITA_KERNEL_FORENSICS_H */
