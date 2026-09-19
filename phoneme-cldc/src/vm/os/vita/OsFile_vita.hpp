/*
 * OsFile_vita.hpp:
 * File I/O definitions for PS Vita.
 * Vita uses newlib which provides standard file API via sceIo.
 */

#ifdef __cplusplus
extern "C" {
#endif

#if !ENABLE_PCSL
typedef FILE *OsFile_Handle;
#endif

const char OsFile_separator_char      = '/';
const char OsFile_path_separator_char = ':';

#ifdef __cplusplus
}
#endif
