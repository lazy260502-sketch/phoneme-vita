/*
 * vita_vibrate.h - Display.vibrate() backend (see vita_vibrate.c).
 */
#ifndef VITA_VIBRATE_H
#define VITA_VIBRATE_H

/* Enable/disable from defaults.cfg (vibrate=0|1). Call before the VM
 * starts; disabling also stops any running pattern. */
void vita_vibrate_set_allowed(int allowed);
int  vita_vibrate_allowed(void);

void vita_vibrate_start(int duration_ms);
void vita_vibrate_stop(void);

/* 0 when disabled by the player or when the device has no actuator (the
 * handheld Vita has no rumble motor; only a PS TV with a paired pad does). */
int  vita_vibrate_supported(void);

#endif /* VITA_VIBRATE_H */
