#ifndef VITA_KERNEL_FORENSICS_H
#define VITA_KERNEL_FORENSICS_H

#ifdef __cplusplus
extern "C" {
#endif

/*
 * v02.18 kernel-side stall forensics (see vita_kernel_forensics.c).
 * Appends to the given log path from the CALLING thread (watchdog).
 * Degrades to a "kubridge absent" line when the plugin is missing.
 *
 * v02.20: vita_kf_precheck() - boot-time probe so a round's kernel
 * visibility is decidable from boot_log.txt ALONE (not only in the
 * HANG-time kernel_forensics.log, which needs a freeze to appear).
 */
void vita_kf_dump(const char *path);
const char *vita_kf_precheck(void); /* "present" / "ABSENT" / "err" */
void vita_kf_list_modules(const char *path); /* v02.22: module table dump */
void vita_kf_boot_resolve(void); /* v02.25: BOOT-time taiHEN resolve;
                                  * MUST run on the main thread before
                                  * the VM starts - the HANG path only
                                  * consumes the cached address */

#ifdef __cplusplus
}
#endif

#endif /* VITA_KERNEL_FORENSICS_H */
