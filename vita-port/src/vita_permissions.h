/*
 * vita_permissions.h - MIDP permission policy mode (see vita_permissions.c)
 *
 * The port defaults to RELAXED: every permission group is allowed for
 * every domain, so an unsigned jar can use network, files, RMS and media
 * without a modal permission dialog blocking the game (that dialog is what
 * froze titles before v02.90). STRICT restores phoneME's own tables.
 */
#ifndef VITA_PERMISSIONS_H
#define VITA_PERMISSIONS_H

#define VITA_PERMS_RELAXED 0
#define VITA_PERMS_STRICT  1

/* Force the mode (the launcher does this from defaults.cfg / game.cfg
 * before the VM starts). */
void vita_permissions_set_mode(int mode);

/* Current mode, reading defaults.cfg lazily on first query when nothing
 * has been set explicitly. */
int  vita_permissions_mode(void);

/* Parse `perms=relaxed|strict` from `path` (NULL = defaults.cfg).
 * Returns VITA_PERMS_RELAXED when the file or the key is missing. */
int  vita_permissions_load(const char *path);

#endif /* VITA_PERMISSIONS_H */
