/*
 * JVM_vita.cpp: PS Vita-specific VM startup and shutdown routines.
 *
 * Based on JVM_linux.cpp.
 */

#include "incls/_precompiled.incl"
#include "incls/_JVM_vita.cpp.incl"

#if defined(VITA)
/* Hook-style bootstrap breadcrumbs: every line is written with its own
 * fopen/fclose pair. Under Vita3K the host-side stdio buffer of an
 * always-open FILE* is lost when the guest is killed, but a per-line
 * open/append/close forces the host to flush to disk (this is why the
 * pcsl/midp_run hook logs survive while midp_stdout.log comes up empty).
 * The VM froze silently inside JVM::start() in v01.27 with no way to see
 * how far bootstrap got - these breadcrumbs close that gap. The launcher
 * chdir()'d into the data dir, so a relative path is enough. */
#include <stdio.h>
#include <stdarg.h>

void vita_boot_log(const char *fmt, ...) {
  va_list ap;
  FILE *f = fopen("vm_boot.log", "a");
  if (f == NULL) {
    return;
  }
  va_start(ap, fmt);
  vfprintf(f, fmt, ap);
  va_end(ap);
  fputc('\n', f);
  fclose(f);
}
#endif

extern "C" int JVM_Start(const JvmPathChar *classpath,
                         char *main_class, int argc,
                         char **argv) {
#if defined(VITA)
  vita_boot_log("[JVM_Start] classpath=%s main=%s argc=%d",
                classpath ? classpath : "(null)",
                main_class ? main_class : "(null)", argc);
#endif
  JVM::set_arguments(classpath, main_class, argc, argv);
#if defined(VITA)
  vita_boot_log("[JVM_Start] set_arguments done, calling JVM::start");
#endif

  int result = 0;
  for (int i = 0; i < ExecutionLoops; i++) {
    if (Verbose) {
      TTY_TRACE_CR(("\t***Starting VM***"));
    }
#if defined(VITA)
    vita_boot_log("[JVM_Start] JVM::start() loop %d enter", i);
#endif
    result = JVM::start();
#if defined(VITA)
    vita_boot_log("[JVM_Start] JVM::start() loop %d returned %d", i, result);
#endif
  }
  return result;
}

extern "C" int JVM_Start2(const JvmPathChar *classpath,
                          char *main_class, int argc,
                          jchar **u_argv) {
#if defined(VITA)
  vita_boot_log("[JVM_Start2] calling set_arguments2");
#endif
  JVM::set_arguments2(classpath, main_class, argc, NULL, u_argv, true);

  int result = 0;
  for (int i = 0; i < ExecutionLoops; i++) {
    if (Verbose) {
      TTY_TRACE_CR(("\t***Starting VM***"));
    }
#if defined(VITA)
    vita_boot_log("[JVM_Start2] JVM::start() loop %d enter", i);
#endif
    result = JVM::start();
#if defined(VITA)
    vita_boot_log("[JVM_Start2] JVM::start() loop %d returned %d", i, result);
#endif
  }
  return result;
}
