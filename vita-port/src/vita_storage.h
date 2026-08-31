#ifndef VITA_STORAGE_H
#define VITA_STORAGE_H

/*
 * Derive the per-game appdb directory path from the game JAR path.
 * Games under "games/<name>/" get a unique appdb_<TAG> path;
 * Hello.jar (bundled, no games/ prefix) uses the shared "appdb" path.
 *
 *   game_jar: e.g. "games/PocketMonster/game.jar" or "Hello.jar"
 *   out_buf:  caller-supplied buffer (≥80 bytes)
 *   buf_sz:   size of out_buf
 * Returns 0 always.
 */
int get_per_game_appdb(const char *game_jar,
                       char *out_buf, size_t buf_sz);

#endif /* VITA_STORAGE_H */
