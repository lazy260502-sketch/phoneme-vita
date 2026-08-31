/*
 * vita_storage.c - per-game RMS namespace isolation for phoneME MIDP.
 *
 * The phoneME MIDP storage path is built as:
 *   sRoot[0] + suiteId(8 hex) + "/" + RecordStoreName + ".db"
 *
 * To keep each game isolated, we override the appdb path that main.c
 * passes to midpSetAppDir(). Each installed game gets its own
 *   ux0:/data/J2ME00001/appdb_<TAG>/
 *   ux0:/data/J2ME00001/appdb_<TAG>/_suites.dat
 *   ux0:/data/J2ME00001/appdb_<TAG>/00000001/<rsname>.db
 *
 * Tag derivation: lower 16 bits of CRC32("games/<name>") as 4 hex chars.
 * For the bundled Hello.jar (no games/ prefix), the default shared
 * "ux0:/data/J2ME00001/appdb" path is used.
 *
 * No modifications to libobj.a are needed: this file only exposes a
 * helper called from main.c BEFORE midpSetAppDir().
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

/* Derive a 4-char hex tag from the JAR path. */
static unsigned int crc16_of_game_name(const char *jar_path) {
    const char *p;
    const char *name_start = NULL;
    size_t name_len = 0;
    unsigned int crc;

    /* Look for "/games/<name>/" segment */
    p = strstr(jar_path, "/games/");
    if (p != NULL) {
        const char *name = p + 7; /* skip "/games/" */
        const char *slash = strchr(name, '/');
        if (slash == NULL) {
            name_len = strlen(name);
        } else {
            name_len = slash - name;
        }
        name_start = name;
    } else {
        /* No "games/" prefix: use last path component */
        const char *slash = strrchr(jar_path, '/');
        if (slash == NULL) slash = strrchr(jar_path, '\\');
        if (slash != NULL) {
            name_start = slash + 1;
            name_len = strlen(name_start);
        } else {
            name_start = jar_path;
            name_len = strlen(jar_path);
        }
        /* strip ".jar" extension */
        if (name_len > 4 &&
            name_start[name_len - 4] == '.' &&
            name_start[name_len - 3] == 'j' &&
            name_start[name_len - 2] == 'a' &&
            name_start[name_len - 1] == 'r') {
            name_len -= 4;
        }
    }

    crc = crc32(0L, Z_NULL, 0);
    if (name_len > 0) {
        crc = crc32(crc, (const Bytef *)name_start, (uInt)name_len);
    }
    return crc & 0xFFFFu;
}

static void hex4(unsigned int val, char *out) {
    const char *hex = "0123456789ABCDEF";
    out[0] = hex[(val >> 12) & 0xF];
    out[1] = hex[(val >> 8) & 0xF];
    out[2] = hex[(val >> 4) & 0xF];
    out[3] = hex[val & 0xF];
    out[4] = '\0';
}

/*
 * Get the per-game appdb path.
 *   game_jar: path to the game jar (e.g. "games/PocketMonster/game.jar"
 *             or "Hello.jar")
 *   out_buf:  output buffer, must be at least 80 bytes
 * Returns 0 on success.
 */
int get_per_game_appdb(const char *game_jar,
                       char *out_buf, size_t buf_sz) {
    unsigned int tag;
    char tag_str[5];
    const char *data_dir = "ux0:/data/J2ME00001";

    /* Bundled Hello.jar or any path NOT under games/ -> shared appdb.
     * This keeps the existing behaviour for the default demo. */
    if (strstr(game_jar, "/games/") == NULL) {
        snprintf(out_buf, buf_sz, "%s/appdb", data_dir);
        return 0;
    }

    tag = crc16_of_game_name(game_jar);
    hex4(tag, tag_str);
    snprintf(out_buf, buf_sz, "%s/appdb_%s", data_dir, tag_str);
    return 0;
}
