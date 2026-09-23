/*
 * vita_thread_audit.h - v02.02 OS-side thread forensics interface
 */

#ifndef VITA_THREAD_AUDIT_H
#define VITA_THREAD_AUDIT_H

/* Called by the watchdog after a HANG: appends the registered-thread
 * table (status/wait/priority/cpu via sceKernelGetThreadInfo) plus the
 * per-core run status to the given log path. Uses sceIo* only - same
 * discipline as the site ring (never stdio inside forensics). */
void vita_thread_audit_dump(const char *path);

/* Appends the "vm tid=" marker line for log cross-referencing. */
void vita_thread_audit_dump_vm_marker(const char *path);

#endif /* VITA_THREAD_AUDIT_H */
