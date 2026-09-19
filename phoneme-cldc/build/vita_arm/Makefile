#
# Master Makefile for PS Vita (ARMv7-A)
#

# Must set ENABLE_JAVA_DEBUGGER and ENABLE_ISOLATES before including root.make,
# otherwise root.make's JAVA_FILES find command prunes DebuggerInvoke.java and
# the isolate directory, causing the VM bootstrap to fail at runtime when
# trying to load these classes.
export ENABLE_JAVA_DEBUGGER := true
export ENABLE_ISOLATES := true

IsSelfHosted = false
include ../share/root.make
