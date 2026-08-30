#!/bin/bash
#==============================================================================
# rebuild_cldc.sh - Rebuild CLDC from source for PS Vita
#==============================================================================
# This script performs a complete rebuild of CLDC VM from source.
# It uses existing x86 loopgen/romgen tools and ARM toolchain for target.
#
# Usage:
#   ./rebuild_cldc.sh              # Full rebuild
#   ./rebuild_cldc.sh clean        # Clean only
#   ./rebuild_cldc.sh loopgen      # Rebuild loopgen only
#   ./rebuild_cldc.sh arm          # Build ARM VM only
#==============================================================================

set -e

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

# Paths
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PHONEME_CLDC="$SCRIPT_DIR/../phoneme-cldc"
VITASDK_ROOT="/home/zyb/.local/vitasdk"

# Toolchain
export VITASDK="$VITASDK_ROOT"
export PATH="$VITASDK/bin:$PATH"

echo -e "${BLUE}========================================${NC}"
echo -e "${BLUE}  CLDC Rebuild Script${NC}"
echo -e "${BLUE}========================================${NC}"

cd "$PHONEME_CLDC"

#==============================================================================
# Clean
#==============================================================================
clean() {
    echo -e "\n${YELLOW}[Clean] Removing build artifacts...${NC}"
    
    # ARM target
    rm -f build/vita_arm/_MergedSrc*.o
    rm -f build/vita_arm/*.o
    rm -f build/vita_arm/cldc_vm_g
    rm -f build/vita_arm/*.velf
    rm -f build/vita_arm/*.self
    rm -f build/vita_arm/*.a
    rm -rf build/vita_arm/target
    rm -rf build/vita_arm/dist
    
    # loopgen artifacts (keep existing tools)
    rm -f build/vita_arm/loopgen/app/_MergedSrc*.o
    rm -f build/vita_arm/loopgen/app/*.o
    rm -f build/vita_arm/loopgen/app/loopgen
    
    # linux_arm loopgen artifacts
    rm -f build/linux_arm/loopgen/app/_MergedSrc*.o
    rm -f build/linux_arm/loopgen/app/*.o
    rm -f build/linux_arm/loopgen/app/loopgen
    
    echo -e "  ${GREEN}✓ Cleaned${NC}"
}

#==============================================================================
# Build x86 loopgen
#==============================================================================
build_loopgen() {
    echo -e "\n${YELLOW}[Loopgen] Building x86 loopgen...${NC}"
    
    # Check if existing loopgen is usable
    if [ -x "build/linux_arm/loopgen/app/loopgen" ]; then
        file build/linux_arm/loopgen/app/loopgen | grep -q "x86-64" && {
            echo -e "  ${GREEN}✓ Using existing x86-64 loopgen${NC}"
            return 0
        }
    fi
    
    # Need to rebuild loopgen
    echo "  Building loopgen..."
    cd build/linux_arm
    
    # Set up x86 compilers (avoid VitaSDK ARM as)
    export PATH="/usr/bin:/bin:$PATH"
    export CC="/usr/bin/gcc-11"
    export CXX="/usr/bin/g++-11"
    
    make IsLoopGen=true BUILD_DIR_NAME=linux_arm LOOP_GENERATOR_DIR=loopgen/app \
        CLDCBUILD=fp ENABLE_ENABLING_CHECK=false \
        -j$(nproc) loopgen
    
    cd ../..
    
    if [ -x "build/linux_arm/loopgen/app/loopgen" ]; then
        file build/linux_arm/loopgen/app/loopgen | grep -q "x86-64" && {
            echo -e "  ${GREEN}✓ loopgen built successfully${NC}"
            return 0
        }
    fi
    
    echo -e "${RED}  FAILED: loopgen build failed${NC}"
    return 1
}

#==============================================================================
# Build x86 romgen
#==============================================================================
build_romgen() {
    echo -e "\n${YELLOW}[Romgen] Building x86 romgen...${NC}"
    
    # Check if existing romgen is usable
    if [ -x "build/linux_arm/dist/bin/romgen" ]; then
        file build/linux_arm/dist/bin/romgen | grep -q "x86-64" && {
            echo -e "  ${GREEN}✓ Using existing x86-64 romgen${NC}"
            return 0
        }
    fi
    
    # Build romgen
    echo "  Building romgen..."
    cd build/linux_arm
    
    export PATH="/usr/bin:/bin:$PATH"
    export CC="/usr/bin/gcc-11"
    export CXX="/usr/bin/g++-11"
    
    make IsLoopGen=true BUILD_DIR_NAME=linux_arm ROMGENERATOR_DIR=romgen/app \
        CLDCBUILD=fp ENABLE_ENABLING_CHECK=false \
        -j$(nproc) romgen
    
    cd ../..
    
    if [ -x "build/linux_arm/dist/bin/romgen" ]; then
        echo -e "  ${GREEN}✓ romgen built successfully${NC}"
        return 0
    fi
    
    echo -e "${RED}  FAILED: romgen build failed${NC}"
    return 1
}

#==============================================================================
# Build ARM VM
#==============================================================================
build_arm() {
    echo -e "\n${YELLOW}[ARM] Building CLDC VM for Vita...${NC}"
    
    # Clean ARM artifacts
    echo "  Cleaning ARM build..."
    rm -f build/vita_arm/_MergedSrc*.o
    rm -f build/vita_arm/*.o
    rm -f build/vita_arm/cldc_vm_g
    
    # Set up ARM toolchain
    export PATH="$VITASDK/bin:$PATH"
    export CC="arm-vita-eabi-gcc"
    export CXX="arm-vita-eabi-g++"
    export AS="arm-vita-eabi-as"
    export AR="arm-vita-eabi-ar"
    export LD="arm-vita-eabi-ld"
    
    cd build/vita_arm
    
    # Build ARM VM with loopgen
    echo "  Building ARM VM (this may take a while)..."
    make BUILD_DIR_NAME=vita_arm \
        IsLoopGen=true \
        LOOP_GENERATOR_DIR=../../linux_arm/loopgen/app \
        CLDCBUILD=fp \
        ENABLE_ENABLING_CHECK=false \
        ENABLE_C_INTERPRETER=true \
        -j$(nproc) debug 2>&1 | tail -50
    
    cd ../..
    
    # Check result
    if [ -f "build/vita_arm/cldc_vm_g" ]; then
        echo -e "  ${GREEN}✓ CLDC VM built successfully${NC}"
        ls -lh build/vita_arm/cldc_vm_g
        return 0
    fi
    
    echo -e "${RED}  FAILED: CLDC VM build failed${NC}"
    return 1
}

#==============================================================================
# Link library
#==============================================================================
link_library() {
    echo -e "\n${YELLOW}[Library] Creating static library...${NC}"
    
    cd build/vita_arm
    
    # Create library from objects
    rm -f libcldc_vm_g.a
    arm-vita-eabi-ar rcs libcldc_vm_g.a *.o _MergedSrc*.o 2>/dev/null || true
    
    # Alternative: link full executable
    rm -f cldc_vm_g
    arm-vita-eabi-g++ -o cldc_vm_g \
        -Wl,--gc-sections \
        -Wl,-z,max-page-size=0x1000 \
        -march=armv7-a -mtune=cortex-a9 -mfloat-abi=hard -mfpu=vfpv3 \
        *.o _MergedSrc*.o \
        -L"$VITASDK/arm-vita-eabi/lib" \
        -lpthread -lm \
        -nostartfiles -nodefaultlibs
    
    cd ../..
    
    if [ -f "build/vita_arm/cldc_vm_g" ]; then
        echo -e "  ${GREEN}✓ Library/Executable created${NC}"
        ls -lh build/vita_arm/cldc_vm_g
        return 0
    fi
    
    # If linking failed, at least we have objects
    if [ -d "build/vita_arm" ]; then
        obj_count=$(ls build/vita_arm/*.o 2>/dev/null | wc -l)
        echo -e "  ${YELLOW}⚠ Linked executable failed, but $obj_count object files exist${NC}"
        return 0
    fi
    
    echo -e "${RED}  FAILED: No output produced${NC}"
    return 1
}

#==============================================================================
# Main
#==============================================================================
case "${1:-}" in
    clean)
        clean
        ;;
    loopgen)
        build_loopgen
        build_romgen
        ;;
    arm)
        build_arm
        ;;
    "")
        # Full rebuild
        clean
        build_loopgen
        build_romgen
        build_arm
        link_library
        ;;
    *)
        echo "Usage: $0 [clean|loopgen|arm]"
        exit 1
        ;;
esac

echo -e "\n${GREEN}========================================${NC}"
echo -e "${GREEN}  Done!${NC}"
echo -e "${GREEN}========================================${NC}"
