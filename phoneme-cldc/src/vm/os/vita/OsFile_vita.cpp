/*
 * OsFile_vita.cpp - PS Vita file I/O implementation
 *
 * newlib on Vita maps stdio to sceIo* already, so most of the
 * standard file API works. We use jvm_fopen/jvm_fclose wrappers
 * same as Linux.
 */

#include "incls/_precompiled.incl"
#include "incls/_OsFile_vita.cpp.incl"

#ifdef __cplusplus
extern "C" {
#endif

#if !ENABLE_PCSL
OsFile_Handle OsFile_open(const JvmPathChar *filename, const char *mode) {
  return (OsFile_Handle)jvm_fopen(filename, mode);
}

int OsFile_close(OsFile_Handle handle) {
  return jvm_fclose(handle);
}

int OsFile_flush(OsFile_Handle handle) {
  return jvm_fflush(handle);
}

size_t OsFile_read(OsFile_Handle handle,
                   void *buffer, size_t size, size_t count) {
  return jvm_fread(buffer, size, count, handle);
}

size_t OsFile_write(OsFile_Handle handle,
                    const void *buffer, size_t size, size_t count) {
  return jvm_fwrite(buffer, size, count, handle);
}

long OsFile_length(OsFile_Handle handle) {
  jvm_fseek(handle, 0, SEEK_END);
  long res = jvm_ftell(handle);
  jvm_fseek(handle, 0, SEEK_SET);
  return res;
}

bool OsFile_exists(const JvmPathChar *name) {
  struct stat buf;
  if (jvm_stat(name, &buf) == 0) {
    if (S_ISREG(buf.st_mode)) {
       return true;
    }
  }
  return false;
}

long OsFile_seek(OsFile_Handle handle, long offset, int origin) {
  return jvm_fseek(handle, offset, origin);
}

int OsFile_eof(OsFile_Handle handle) {
  return jvm_feof(handle);
}

bool OsFile_rename(const JvmPathChar *from, const JvmPathChar *to) {
  jvm_rename(from, to);
  return true;
}

int OsFile_remove(const JvmPathChar *filename) {
  return jvm_remove(filename);
}
#endif // !ENABLE_PCSL

#ifdef __cplusplus
}
#endif
