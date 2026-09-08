#ifndef VITA_STORAGE_H
#define VITA_STORAGE_H

/*
 * Derive the appdb directory path for a game from its JAR path.
 * All suite storage lives under "rms/": games under "games/<name>/"
 * get a unique rms/appdb_<TAG>; Hello.jar (bundled, no games/ prefix)
 * uses the shared "rms/appdb".
 *
 *   game_jar: e.g. "games/PocketMonster/game.jar" or "Hello.jar"
 *   out_buf:  caller-supplied buffer (≥96 bytes)
 *   buf_sz:   size of out_buf
 * Returns 0 always.
 */
int get_per_game_appdb(const char *game_jar,
                       char *out_buf, size_t buf_sz);

#endif /* VITA_STORAGE_H */
