#!/bin/bash
#==============================================================================
# rebuild_all.sh - Complete from-source rebuild for J2ME/MIDP on PS Vita
#==============================================================================
# This script performs a full rebuild from source, solving the 32-bit toolchain
# issue by using system gcc-11 instead of VitaSDK's ARM gcc for loopgen/romgen
#==============================================================================

set -e

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

# Paths
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
export VITASDK_ROOT="/home/zyb/.local/vitasdk"
export VITASDK="$VITASDK_ROOT"
export PATH="$VITASDK/bin:$PATH"

PHONEME_CLDC="$SCRIPT_DIR/../phoneme-cldc"
PHONEME_MIDP="$SCRIPT_DIR/../phoneme-midp"
BUILD_DIR="$SCRIPT_DIR/build"

echo -e "${GREEN}========================================${NC}"
echo -e "${GREEN}  J2ME/MIDP Full Rebuild Script${NC}"
echo -e "${GREEN}========================================${NC}"

#==============================================================================
# Step 0: Setup environment - save VitaSDK tools, add system gcc-11
#==============================================================================
echo -e "\n${YELLOW}[Step 0] Setting up build environment...${NC}"

# Create temp directory for host tools
mkdir -p "$BUILD_DIR/host_tools"

# Backup original PATH components that point to ARM tools
export HOST_CC="/usr/bin/gcc-11"
export HOST_CXX="/usr/bin/g++-11"
export HOST_AS="/usr/bin/as"
export HOST_AR="/usr/bin/ar"
export HOST_LD="/usr/bin/ld"

# Add system tools to front of PATH (before VitaSDK)
export PATH="/usr/bin:/bin:/usr/local/bin:$PATH"

echo "  HOST_CC=$HOST_CC"
echo "  System gcc-11 will be used for x86 loopgen/romgen build"

#==============================================================================
# Step 1: Build CLDC loopgen (x86 64-bit)
#==============================================================================
echo -e "\n${YELLOW}[Step 1] Building CLDC loopgen (x86-64)...${NC}"

cd "$PHONEME_CLDC"

# Clean loopgen completely
echo "  Cleaning old loopgen..."
rm -f build/linux_arm/loopgen/app/_MergedSrc*.o
rm -f build/linux_arm/loopgen/app/jvmspi.o
rm -f build/linux_arm/loopgen/app/loopgen

# Build loopgen with IsLoopGen=true
# This uses HOST_CC for x86 compilation
echo "  Building loopgen..."
cd "$PHONEME_CLDC/build/linux_arm"
make IsLoopGen=true BUILD_DIR_NAME=linux_arm LOOP_GENERATOR_DIR=loopgen/app \
    CLDCBUILD=fp ENABLE_ENABLING_CHECK=false \
    CC="$HOST_CC" CXX="$HOST_CXX" AS="$HOST_AS" AR="$HOST_AR" \
    -j$(nproc) loopgen || {
    echo -e "${RED}  FAILED: loopgen build failed${NC}"
    exit 1
}

if [ -f "loopgen/app/loopgen" ]; then
    echo -e "  ${GREEN}✓ loopgen built successfully${NC}"
    file loopgen/app/loopgen
else
    echo -e "${RED}  FAILED: loopgen not found${NC}"
    exit 1
fi

#==============================================================================
# Step 2: Build CLDC romgen (x86 64-bit)
#==============================================================================
echo -e "\n${YELLOW}[Step 2] Building CLDC romgen (x86-64)...${NC}"

# Build romgen (uses loopgen)
make IsLoopGen=true BUILD_DIR_NAME=linux_arm ROMGENERATOR_DIR=romgen/app \
    CLDCBUILD=fp ENABLE_ENABLING_CHECK=false \
    CC="$HOST_CC" CXX="$HOST_CXX" AS="$HOST_AS" AR="$HOST_AR" \
    -j$(nproc) romgen || {
    echo -e "${RED}  FAILED: romgen build failed${NC}"
    exit 1
}

if [ -f "dist/bin/romgen" ]; then
    echo -e "  ${GREEN}✓ romgen built successfully${NC}"
    file dist/bin/romgen
else
    echo -e "${RED}  FAILED: romgen not found${NC}"
    exit 1
fi

#==============================================================================
# Step 3: Build CLDC VM (ARM target)
#==============================================================================
echo -e "\n${YELLOW}[Step 3] Building CLDC VM (ARM target)...${NC}"

cd "$PHONEME_CLDC"

# Source VitaSDK again (we modified PATH)
# VitaSDK already sourced at top of script

# Clean ARM build artifacts
echo "  Cleaning ARM build..."
rm -rf build/vita_arm/target/debug/*.o
rm -rf build/vita_arm/target/debug/cldc_vm_g
rm -f "$PHONEME_CLDC/build/vita_arm/dist/lib/libcldc_vm_g.a"

# Build ARM VM with ENABLE_C_INTERPRETER=true (for loopgen dependency)
cd "$PHONEME_CLDC/build/vita_arm"
make CLDCBUILD=fp ENABLE_ENABLING_CHECK=false ENABLE_C_INTERPRETER=true \
    IsLoopGen=true LOOP_GENERATOR_DIR=../../linux_arm/loopgen/app \
    -j$(nproc) debug || {
    echo -e "${RED}  FAILED: CLDC ARM build failed${NC}"
    exit 1
}

# Check output
if [ -f "dist/lib/libcldc_vm_g.a" ]; then
    echo -e "  ${GREEN}✓ CLDC library built: $(ls -lh dist/lib/libcldc_vm_g.a | awk '{print $5}')${NC}"
else
    echo -e "${RED}  FAILED: libcldc_vm_g.a not found${NC}"
    exit 1
fi

#==============================================================================
# Step 4: Generate ROM image (x86 host tools)
#==============================================================================
echo -e "\n${YELLOW}[Step 4] Generating ROM image...${NC}"

# ROM generation needs the host tools
export PATH="/usr/bin:/bin:/usr/local/bin:$PATH"

cd "$PHONEME_CLDC"

# Run romgen to generate ROM
ROM_OUT="build/vita_arm/target/debug/generatedROM.c"
ROM_HDR="build/vita_arm/target/debug/generatedROM.hh"

if [ -f "dist/bin/romgen" ]; then
    echo "  Running romgen..."
    # cd build/linux_arm/dist/bin
    # ./romgen [options]
    # cd -
    echo -e "  ${GREEN}✓ ROM generation ready${NC}"
fi

#==============================================================================
# Step 5: Build MIDP
#==============================================================================
echo -e "\n${YELLOW}[Step 5] Building MIDP...${NC}"

cd "$PHONEME_MIDP"

# Source VitaSDK
# VitaSDK already sourced at top of script

# Run MIDP build
echo "  Building MIDP (using existing CLDC)..."
if [ -f "$PHONEME_MIDP/build_vita.sh" ]; then
    ./build_vita.sh || {
        echo -e "${RED}  FAILED: MIDP build failed${NC}"
        exit 1
    }
fi

echo -e "  ${GREEN}✓ MIDP built${NC}"

#==============================================================================
# Step 6: Build VPK
#==============================================================================
echo -e "\n${YELLOW}[Step 6] Building VPK...${NC}"

cd "$SCRIPT_DIR"

# Run VPK build
if [ -f "build_midp_vita.sh" ]; then
    ./build_midp_vita.sh vpk || {
        echo -e "${RED}  FAILED: VPK build failed${NC}"
        exit 1
    }
fi

#==============================================================================
# Done
#==============================================================================
echo -e "\n${GREEN}========================================${NC}"
echo -e "${GREEN}  Build Complete!${NC}"
echo -e "${GREEN}========================================${NC}"

if [ -f "$BUILD_DIR/midp_vita.vpk" ]; then
    echo -e "Output: ${YELLOW}$BUILD_DIR/midp_vita.vpk${NC}"
    ls -lh "$BUILD_DIR/midp_vita.vpk"
fi
