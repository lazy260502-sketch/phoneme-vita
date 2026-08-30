#!/bin/bash
#==============================================================================
# midp-vita Build Script v3 (Modular)
#==============================================================================
# Modular build script for J2ME/MIDP on PS Vita
#
# Usage:
#   ./build_midp_vita.sh              # Full build
#   ./build_midp_vita.sh clean        # Clean and exit
#   ./build_midp_vita.sh jar          # Build Hello.jar only
#   ./build_midp_vita.sh cldc         # Rebuild CLDC only
#   ./build_midp_vita.sh rom          # Generate ROM only
#   ./build_midp_vita.sh midp         # Rebuild MIDP only
#   ./build_midp_vita.sh vpk          # Build VPK only
#
# Modules:
#   build_midp_vita.common            # Common utilities
#   build_midp_vita.step0_jar         # Build Hello.jar
#   build_midp_vita.step1_cldc        # Rebuild CLDC library
#   build_midp_vita.step1_5_rom       # Generate ROM image
#   build_midp_vita.step2_midp        # Rebuild MIDP libobj
#   build_midp_vita.step3_vpk         # Build VPK
#==============================================================================

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

# Banner
echo -e "\033[0;32m========================================\033[0m"
echo -e "\033[0;32m  midp-vita Build Script v3 (Modular)\033[0m"
echo -e "\033[0;32m========================================\033[0m"

# Set paths before sourcing modules
VITASDK_ROOT="/home/zyb/.local/vitasdk"
CLDC_DIR="$SCRIPT_DIR/../phoneme-cldc"
MIDP_DIR="$SCRIPT_DIR/../phoneme-midp"
BUILD_DIR="$SCRIPT_DIR/build"
SRC_DIR="$SCRIPT_DIR/src"
CLDC_MARKER="$BUILD_DIR/.cldc_built"
ROM_MARKER="$BUILD_DIR/.rom_built"
VPK_OUTPUT="$BUILD_DIR/midp_vita.vpk"

# Source Vita SDK
[ -f "$VITASDK_ROOT/vitasdk.sh" ] && source "$VITASDK_ROOT/vitasdk.sh"

# Source common utilities
source "$SCRIPT_DIR/build_midp_vita.common"

# Clean command
if [ "$1" = "clean" ]; then
    echo -e "\n\033[1;33mCleaning build artifacts...\033[0m"
    rm -f "$CLDC_MARKER" "$ROM_MARKER"
    rm -f "$BUILD_DIR"/.cldc_*.md5
    rm -f "$BUILD_DIR"/.midp_*.md5
    rm -f "$BUILD_DIR"/.rom_*.md5
    rm -f "$BUILD_DIR"/.hello_jar.md5
    [ -d "$BUILD_DIR" ] && find "$BUILD_DIR" -maxdepth 1 -name "midp_vita*" -delete
    echo -e "  \033[0;32m✓ Cleaned\033[0m"
    exit 0
fi

# Define build steps
declare -A STEPS=(
    ["jar"]="build_midp_vita.step0_jar"
    ["rom"]="build_midp_vita.step1_5_rom"
    ["cldc"]="build_midp_vita.step1_cldc"
    ["midp"]="build_midp_vita.step2_midp"
    ["vpk"]="build_midp_vita.step3_vpk"
)

# If specific step requested
if [ -n "$1" ] && [ -n "${STEPS[$1]}" ]; then
    # Always run jar first as dependency
    [ "$1" != "jar" ] && source "$SCRIPT_DIR/${STEPS[jar]}"
    source "$SCRIPT_DIR/${STEPS[$1]}"
    exit 0
fi

# Full build (in dependency order)
source "$SCRIPT_DIR/${STEPS[jar]}"
source "$SCRIPT_DIR/${STEPS[rom]}"
source "$SCRIPT_DIR/${STEPS[cldc]}"
source "$SCRIPT_DIR/${STEPS[midp]}"
source "$SCRIPT_DIR/${STEPS[vpk]}"
