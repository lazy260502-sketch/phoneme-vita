/*
 * Main_vita.cpp: PS Vita entry point for CLDC-HI VM
 *
 * Based on Main_linux.cpp, with Vita-specific initialization.
 */

#include "incls/_precompiled.incl"
#include "incls/_Main_vita.cpp.incl"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <psp2/kernel/processmgr.h>
#include "os_port.h"

/*
 * This file implements the launching of the stand-alone VM
 * on the PS Vita platform.
 */

void JVMSPI_PrintRaw(const char* s, int length) {
  jvm_printf("%.*s", length, s);
  jvm_fflush(stdout);
}

void JVMSPI_Exit(int code) {
  ::jvm_exit(code);
}

#if ENABLE_DYNAMIC_RESTRICTED_PACKAGE
jboolean JVMSPI_IsRestrictedPackage(const char* pkg_name, int name_length) {
  GUARANTEE(UseROM, "sanity");
  (void)pkg_name;
  (void)name_length;
  return false;
}
#endif

int main(int argc, char **argv) {
  int code = 0;

  // Call this before any other Jvm_ functions.
  JVM_Initialize();

  // Ignore arg[0] -- the name of the program.
  argc--;
  argv++;

  while (true) {
    int n = JVM_ParseOneArg(argc, argv);
    if (n < 0) {
      JVMSPI_DisplayUsage(NULL);
      code = -1;
      goto end;
    } else if (n == 0) {
      break;
    }
    argc -= n;
    argv += n;
  }

  if (JVM_GetConfig(JVM_CONFIG_SLAVE_MODE) == KNI_FALSE) {
    // Run the VM in regular mode -- JVM_Start won't return until
    // the VM completes execution.
    code = JVM_Start(NULL, NULL, argc, argv);
  } else {
    JVM_Start(NULL, NULL, argc, argv);

    for (;;) {
      long timeout = JVM_TimeSlice();
      if (timeout <= -2) {
        break;
      } else {
        int blocked_threads_count;
        JVMSPI_BlockedThreadInfo * blocked_threads;

        blocked_threads = SNI_GetBlockedThreads(&blocked_threads_count);
        JVMSPI_CheckEvents(blocked_threads, blocked_threads_count, timeout);
      }
    }

    code = JVM_CleanUp();
  }

end:
  return code;
}
