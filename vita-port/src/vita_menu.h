#ifndef VITA_MENU_H
#define VITA_MENU_H

typedef struct {
    char jar[256];   /* absolute path of the game jar */
    char cls[128];   /* MIDlet class name */
    char orient[32]; /* "portrait" or "landscape" */
} VitaGameSel;

/* Runs the native game menu. Returns 1 when a game was picked (fills
 * *out), 0 when the user quit without a selection. */
int vita_menu_run(VitaGameSel *out);

#endif
