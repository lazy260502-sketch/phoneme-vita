#
#   
#
# Copyright  1990-2007 Sun Microsystems, Inc. All Rights Reserved.
# DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER
# 
# This program is free software; you can redistribute it and/or
# modify it under the terms of the GNU General Public License version
# 2 only, as published by the Free Software Foundation.
# 
# This program is distributed in the hope that it will be useful, but
# WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
# General Public License version 2 for more details (a copy is
# included at /legal/license.txt).
# 
# You should have received a copy of the GNU General Public License
# version 2 along with this work; if not, write to the Free Software
# Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA
# 02110-1301 USA
# 
# Please contact Sun Microsystems, Inc., 4150 Network Circle, Santa
# Clara, CA 95054 or visit www.sun.com if you need additional
# information or have any questions.
#
######################################################################
#
# Makefile for building MIDP native layer for PS Vita
# Uses x86 host compiler with -m32 flag to generate 32-bit code
# matching the 32-bit ARM target (sizeof(void*)==4)
#
######################################################################

# Compiler - use host g++ with -m32 to generate 32-bit code
# This makes sizeof(void*)==4, sizeof(jobject)==4, matching the ARM target
CC      = /usr/bin/gcc-13 -m32
CXX     = /usr/bin/g++-13 -m32

# Include paths - MIDP source and phoneme-cldc headers
INCLUDES = -I/home/zyb/vitasdk/samples/j2me/phoneme-midp/src \
           -I/home/zyb/vitasdk/samples/j2me/phoneme-midp/build/vita_arm \
           -I/home/zyb/vitasdk/samples/j2me/phoneme-cldc/build/vita_arm/dist/include \
           -I/usr/include

# Compiler flags for Vita 32-bit target
CFLAGS   = -m32 -march=i686 -mtune=generic -fPIC
CXXFLAGS = -m32 -march=i686 -mtune=generic -fPIC -std=c++17

# Define macros for Vita port
CPPFLAGS = -DLINUX=1 -DVITA=1 -DUSE_MIDP=1

# Object file suffix (using .o for compatibility)
OBJ_SUFFIX = o

# Source files (MIDP Vita native layer)
VITA_SRCS = \
    src/porting/vita/lcdui_display_vita.c \
    src/porting/vita/lcdui_input_vita.c \
    src/porting/vita/gxj_graphics_vita.c \
    src/porting/vita/midpNativeAppManagerPeer_vita.c \
    src/porting/vita/platform_vita.h

# Object files (derived from source files)
VITA_OBJS = $(VITA_SRCS:%.c=%.$(OBJ_SUFFIX)) $(VITA_SRCS:%.cpp=%.$(OBJ_SUFFIX))

# Default target
all: $(VITA_OBJS)

# Compile C files
%.c.$(OBJ_SUFFIX): %.c
	$(CC) $(CFLAGS) $(INCLUDES) $(CPPFLAGS) -c $< -o $@

# Compile C++ files
%.cpp.$(OBJ_SUFFIX): %.cpp
	$(CXX) $(CXXFLAGS) $(INCLUDES) $(CPPFLAGS) -c $< -o $@

# Clean target
clean:
	rm -f *.$(OBJ_SUFFIX)

# Show help
help:
	@echo "MIDP Vita Native Build System"
	@echo "============================"
	@echo "Compiles MIDP native layer for PS Vita using x86 host with -m32"
	@echo ""
	@echo "Usage: make [target]"
	@echo ""
	@echo "Targets:"
	@echo "  all      - Build all native object files"
	@echo "  clean    - Remove all object files"
	@echo "  help     - Show this help message"