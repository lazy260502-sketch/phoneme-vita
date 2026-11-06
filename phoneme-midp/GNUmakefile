#
# Top-level Makefile for building MIDP on PS Vita
# Uses phoneME's GMK build system
#

# Source directory
MIDP_DIR := $(CURDIR)

# Output directory
MIDP_OUTPUT_DIR := $(CURDIR)/build/vita_arm

# Platform configuration
HOST_PLATFORM := linux
HOST_OS := linux
HOST_CPU := x86_64
HOST_COMPILER := gcc

TARGET_PLATFORM := vita_arm
TARGET_OS := vita
TARGET_CPU := arm
TARGET_COMPILER := gcc
TARGET_VM := cldc_vm
TARGET_DEVICE := vita

# Include the phoneME build system
include $(MIDP_DIR)/build/common/Top.gmk

# v02.91: rules that need variables Top.gmk defines (OBJ_DIR and friends).
# The variable-only overlay is loaded through MAKEFILES before this file;
# anything that declares a *rule* has to come after, because make expands
# its target immediately and $(OBJ_DIR) would still be empty.
-include $(MIDP_DIR)/build/vita_arm/vita_overlay_rules.mk