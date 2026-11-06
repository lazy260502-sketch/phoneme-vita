#!/bin/bash
# Build MIDP for PS Vita with all boolean options set to false.
# Paths come from build_env.sh (J2ME_ROOT / JDK_DIR / VITASDK), all
# configurable via environment variables.

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"   # phoneme-midp
. "$SCRIPT_DIR/../build_env.sh"

TOOLS_DIR="$J2ME_ROOT/phoneme_source/phoneME/tools"
CLDC_DIST_DIR="$J2ME_ROOT/phoneme-cldc/build/vita_arm/dist"
GNU_TOOLS_BINDIR="$VITASDK/bin/arm-vita-eabi-"
JSR_135_DIR="$J2ME_ROOT/phoneme_source/phoneME/jsr135"
JSR_120_DIR="$J2ME_ROOT/phoneme_source/phoneME/jsr120"
JSR_75_DIR="$J2ME_ROOT/jsr75"
JSR_184_DIR="$J2ME_ROOT/jsr184"
ABSTRACTIONS_DIR="$SCRIPT_DIR/src/abstractions"
OVERLAY_MK="$SCRIPT_DIR/build/vita_arm/vita_overlay.mk"
OVERLAY_RULES_MK="$SCRIPT_DIR/build/vita_arm/vita_overlay_rules.mk"

mkdir -p "$SCRIPT_DIR/build/vita_arm"

# The overlay is GENERATED here, not checked in: `phoneme-midp/build/vita_arm/*`
# is git-ignored (it is the output directory), so a hand-written overlay would
# be lost by a fresh clone and by `clean`, and the build would then fail with
# "vita_sms_javacall.c: No such file" or a missing jpegdecoder.h. It is
# injected through MAKEFILES, which GNU make reads before GNUmakefile, so it
# can append vpath/CFLAGS without touching a single upstream .gmk.
cat > "$OVERLAY_MK" <<'OVERLAY_EOF'
# vita_overlay.mk - Vita platform overlay for phoneME MIDP build (GENERATED)
#
# Written by phoneme-midp/build_vita.sh on every run - edit that script, not
# this file. Injected via MAKEFILES.
#
#   1. vpath so native sources from vita-port/src resolve by plain name
#   2. vita_sms_javacall.c  - JSR 120 (WMA) javacall implementation
#   3. vita_jpeg.c          - JPEG_To_RGB_* seam (stb_image), used when
#                             USE_JPEG=true defines ENABLE_JPEG=1

VITA_PORT_SRC_DIR = ../vita-port/src

vpath % $(VITA_PORT_SRC_DIR)

# imgdcd_image_util.c does `#include <jpegdecoder.h>`, which lives in
# vita-port/src. No header there shares a name with a phoneME header, so this
# cannot shadow anything.
EXTRA_INCLUDES += -I$(VITA_PORT_SRC_DIR)

JTWI_NATIVE_FILES += vita_sms_javacall.c vita_jpeg.c

# v02.91: the port's own MIDP-level Java classes (com.sun.midp.ssl.*), which
# provide SSLStreamConnection for the https/ssl protocols. They MUST be
# compiled in the SAME javac run as the rest of MIDP - upstream
# https/Protocol.java imports com.sun.midp.ssl.SSLStreamConnection, so
# building them separately and later (as build_nokia_stubs.sh does for the
# vendor APIs) is a chicken-and-egg problem.
#
# Appending here works because this file is read BEFORE protocol/lib.gmk and
# protocol/subsystem.gmk: those use += and then fold the result into
# SUBSYSTEM_PROTOCOL_JAVA_FILES, which becomes a prerequisite of
# classes.zip. (Appending from a later rule does not work - the javac run
# already happened by then.)
VITA_VENDOR_JAVA := $(wildcard ../vita-port/vendor_src/com/sun/midp/ssl/*.java)
ifneq ($(VITA_VENDOR_JAVA),)
SUBSYSTEM_HTTPS_JAVA_FILES += $(VITA_VENDOR_JAVA)
endif
OVERLAY_EOF

# Rules go in a second file, included AFTER Top.gmk by GNUmakefile: make
# expands a rule's target as soon as it reads it, and $(OBJ_DIR) is not
# defined yet when MAKEFILES is parsed.
cat > "$OVERLAY_RULES_MK" <<'OVERLAY_RULES_EOF'
# vita_overlay_rules.mk - Vita platform overlay, rules half (GENERATED)
#
# v02.91: MIDP.gmk (already patched in this tree) makes libmidp.so depend on
# $(OBJ_DIR)/vita_stubs.o, but nothing adds src/vita_stubs.c to the object
# list, so the rule does not exist. That never surfaced while libmidp.so was
# up to date; flipping USE_SSL changes its file list and forces a relink.
#
# Only the build rule is defined here. The source is deliberately NOT added
# to JTWI_NATIVE_FILES: those stubs collide with the real pcsl_file_* /
# javacall_* implementations in vita-port/src/vita_pcsl.c, and with
# --whole-archive the winner would be whatever the archive order happens to
# be. -Wno-error because this file predates the -Werror this build enables
# and was never compiled before, so its snprintf truncation warnings would
# otherwise block the build; it is not part of the shipping image.
$(OBJ_DIR)/vita_stubs.o: $(MIDP_DIR)/src/vita_stubs.c
	@echo " ... $@ " $(LOG)
	$(A)$(CC) $(CFLAGS) -Wno-error -c $(CC_OUTPUT)$@ $(MIDP_DIR)/src/vita_stubs.c $(LOG)
OVERLAY_RULES_EOF

cd "$SCRIPT_DIR"

# NOTE (v02.91): CREATE_MIDP_SHARED_LIB is false. libmidp.so needs
# vita_ime_* / lfjport_* symbols that live in vita-port/src and are
# linked into the final eboot, so it cannot link on its own; the port
# consumes obj/arm/libobj.a and never loads the .so. It only appeared
# to work while the target was already up to date - flipping USE_SSL
# changed its file list, forced the link, and exposed the bug.

# Build the command
CMD="make -f GNUmakefile \
  JDK_DIR=$JDK_DIR \
  TOOLS_DIR=$TOOLS_DIR \
  PROJECT_TOOLS_DIR=$(pwd)/src/tool \
  PCSL_OUTPUT_DIR=$(pwd)/build/vita_arm/pcsl \
  CLDC_DIST_DIR=$CLDC_DIST_DIR \
  GNU_TOOLS_BINDIR=$GNU_TOOLS_BINDIR \
  g= \
  USE_CLDC_RELEASE=true \
  USE_NATIVE_APP_MANAGER=false \
  USE_NATIVE_INSTALLER=false \
  USE_NATIVE_SUITE_STORAGE=false \
  USE_NATIVE_RMS=false \
  USE_NAMS_TEST_SERVICE=false \
  USE_RAW_AMS_IMAGES=false \
  USE_RESTRICTED_CRYPTO=false \
  USE_SSL=true \
  USE_BINARY_CRYPTO=false \
  USE_FIXED=false \
  USE_CLDC_11=true \
  USE_MONET=false \
  USE_STATIC_PROPERTIES=false \
  USE_GCC=false \
  CREATE_MIDP_SHARED_LIB=false \
  USE_VERBOSE_MAKE=true \
  CMDLINE_CFLAGS=-marm -Wno-error -Wno-error=redefined-conditional-interpretation -Wno-error=unused-parameter -Wno-error=char-subscripts -Wno-error=format-overflow= -Wno-error=maybe-uninitialized=unused-parameter -Wno-error=char-subscripts -Wno-error=format-overflow= \
  USE_I3_TEST=false \
  USE_CONTROL_ARGS_FROM_JAD=false \
  USE_GCOV=false \
  USE_MIDP_MALLOC=false \
  USE_MULTIPLE_ISOLATES=false \
  USE_NATIVE_PROFILER=false \
  USE_NETWORK_INDICATOR=false \
  USE_NUTS_FRAMEWORK=false \
  USE_RMS_TREE_INDEX=false \
  USE_MIDP_ABB=false \
  USE_JPEG=true \
  JPEG_DIR=$J2ME_ROOT/phoneme_source/phoneME/jpeg \
  USE_GCI=false \
  USE_VERBOSE_MAKE=false \
  USE_MIDP_MALLOC=false \
  USE_JSR_135=true \
  JSR_135_DIR=$JSR_135_DIR \
  PROJECT_JSR_135_DIR=$JSR_135_DIR \
  USE_ABSTRACTIONS=true \
  ABSTRACTIONS_DIR=$ABSTRACTIONS_DIR \
  USE_JSR_75=true \
  JSR_75_DIR=$JSR_75_DIR \
  PROJECT_JSR_75_DIR=$JSR_75_DIR \
  USE_JSR_177=false \
  USE_JSR_179=false \
  USE_JSR_180=false \
  USE_JSR_184=true \
  JSR_184_DIR=$JSR_184_DIR \
  PROJECT_JSR_184_DIR=$JSR_184_DIR \
  USE_JSR_205=false \
  USE_JSR_211=false \
  USE_JSR_226=false \
  USE_JSR_229=false \
  USE_JSR_230=false \
  USE_JSR_234=false \
  USE_JSR_238=false \
  USE_JSR_239=false \
  USE_JSR_256=false \
  USE_JSR_257=false \
  USE_JSR_258=false \
  USE_JSR_280=false \
  USE_JSR_281=false \
  USE_JSR_290=false \
  USE_JSR_293=false \
  USE_CMS=false \
  USE_PISCES=false \
  USE_IMAGE_CACHE=false \
  USE_FONT_CACHE=false \
  USE_ICON_CACHE=false \
  USE_MIDP_MALLOC=false \
  USE_MULTIPLE_ISOLATES=false \
  USE_DEBUG=false \
  USE_GCOV=false \
  USE_NATIVE_PROFILER=false \
  USE_NETWORK_INDICATOR=false \
  USE_NUTS_FRAMEWORK=false \
  USE_RMS_TREE_INDEX=false \
  USE_MIDP_ABB=false \
  USE_JSR_177=false \
  USE_JSR_75=true \
  JSR_75_DIR=$JSR_75_DIR \
  PROJECT_JSR_75_DIR=$JSR_75_DIR \
  USE_JSR_82=false \
  USE_JSR_120=true \
  JSR_120_DIR=$JSR_120_DIR \
  PROJECT_JSR_120_DIR=$JSR_120_DIR \
  USE_JSR_172=false \
  USE_JSR_226=false \
  USE_PISCES=false \
  USE_JSR_179=false \
  USE_JSR_293=false \
  USE_JSR_180=false \
  USE_JSR_184=true \
  JSR_184_DIR=$JSR_184_DIR \
  PROJECT_JSR_184_DIR=$JSR_184_DIR \
  USE_JSR_205=false \
  USE_JSR_211=false \
  USE_JSR_229=false \
  USE_JSR_230=false \
  USE_JSR_238=false \
  USE_JSR_239=false \
  USE_JSR_280=false \
  USE_JSR_256=false \
  USE_JSR_257=false \
  USE_JSR_258=false \
  USE_JSR_281=false \
  USE_JSR_290=false \
  USE_CMS=false \
  USE_JAVA_DEBUGGER=true \
  USE_JAVA_PROFILER=false \
  USE_CONTROL_ARGS_FROM_JAD=false \
  USE_JPEG=false \
  USE_GCI=false \
  USE_DIRECTDRAW=false \
  USE_FILE_SYSTEM=false \
  USE_NATIVE_PTI=false \
  USE_MULTIPLE_DISPLAYS=false \
  USE_VERIFY_ONCE=false \
  USE_EXTENDED_ATTRIBUTES=false \
  USE_VM_PROFILES=false \
  USE_I18N_VITA=true \
  USE_SERVER_SOCKET=true \
  USE_ON_DEVICE_DEBUG=false \
  USE_WTK_DEBUG=false \
  USE_SYSTEM_CLASSES_DEBUG=false \
  USE_OCSP=false \
  USE_DYNAMIC_COMPONENTS=false \
  USE_AMS_FOLDERS=false \
  USE_AUTOTESTER=false \
  USE_PUBLIC_AMS_API=false"

echo "Running build..."
# Inject the Vita overlay generated above (extra vpath + the two Vita native
# sources). MAKEFILES is read by GNU make before GNUmakefile; the overlay only
# appends to variables, so no upstream makefile is touched.
export MAKEFILES="$OVERLAY_MK"
# MIDP_EXTRA_GOAL (set by vita-port/build.sh): an additional make goal. The
# default goal (`all`) never refreshes obj/arm/libobj.a - see build.sh.
eval $CMD $MIDP_EXTRA_GOAL 2>&1 | grep -v "gawk: cmd" | tee /tmp/midp_build.log | tail -30
