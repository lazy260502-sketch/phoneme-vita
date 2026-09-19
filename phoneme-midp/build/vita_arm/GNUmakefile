#
# Makefile for building and packaging.
#

# Definition of platform to build on
ifeq (${OS}, Windows_NT)
HOST_PLATFORM           = win32
HOST_OS                 = win32
else
HOST_PLATFORM           = linux
HOST_OS                 = linux
endif
HOST_CPU                = i386
HOST_COMPILER           = ads

# Definition of platform to run on
TARGET_PLATFORM         = vita_arm
TARGET_OS               = armsd
TARGET_CPU              = arm
TARGET_COMPILER         = ads
TARGET_VM               = cldc_vm

# file/directory name separator
FN_SEP=/

# Workspace directory
MIDP_DIR                = $(CURDIR)/../..

# Build output directory
ifndef MIDP_OUTPUT_DIR
MIDP_OUTPUT_DIR         = $(CURDIR)
endif

# Build options should be include before shared gmk
include Options.gmk

# All rules are defined in shared makefiles
include $(MIDP_DIR)/build/common/Top.gmk