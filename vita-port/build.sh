#!/bin/bash
# vita-port top-level build.
#
# Usage:
#   ./build.sh            # link + package (uses existing CLDC/MIDP artifacts)
#   ./build.sh midp       # + rebuild phoneme-midp library first
#   ./build.sh clean      # + remove cmake build dir
#
# phoneme trees are NOT touched by this script except running their own
# builds inside build/vita_arm (their canonical configs).

set -e

PORT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
J2ME_DIR="$PORT_DIR/.."
export VITASDK="${VITASDK:-/home/zyb/.local/vitasdk}"

MODE="${1:-all}"

if [ "$MODE" = "clean" ]; then
    rm -rf "$PORT_DIR/build/cmake"
    MODE=all
    FORCE_MIDP=1
fi
if [ "$MODE" = "midp" ]; then
    FORCE_MIDP=1
    MODE=all
fi

# ---------------------------------------------------------------------------
# Step 0: sanity check CLDC/MIDP artifacts
# ---------------------------------------------------------------------------
CLDC_LIB="$J2ME_DIR/phoneme-cldc/build/vita_arm/dist/lib/libcldc_vm.a"
MIDP_LIBOBJ="$J2ME_DIR/phoneme-midp/build/vita_arm/obj/arm/libobj.a"

if [ ! -f "$CLDC_LIB" ]; then
    echo "ERROR: $CLDC_LIB missing."
    echo "Build phoneme-cldc first: cd phoneme-cldc/build/vita_arm && make"
    exit 1
fi
if [ ! -f "$MIDP_LIBOBJ" ]; then
    echo "ERROR: $MIDP_LIBOBJ missing."
    echo "Build phoneme-midp first: bash phoneme-midp/build_vita.sh"
    exit 1
fi

# ---------------------------------------------------------------------------
# Step 1 (optional): rebuild phoneme-midp
# ---------------------------------------------------------------------------
if [ -n "$FORCE_MIDP" ]; then
    echo "==> Rebuilding phoneme-midp (build_vita.sh)..."
    bash "$J2ME_DIR/phoneme-midp/build_vita.sh"
fi

# ---------------------------------------------------------------------------
# Step 2: test MIDlets
# ---------------------------------------------------------------------------
echo "==> Building test MIDlets..."
bash "$PORT_DIR/build_jar.sh"

# ---------------------------------------------------------------------------
# Step 3: cmake + make
# ---------------------------------------------------------------------------
echo "==> Configuring and linking..."
BUILD_DIR="$PORT_DIR/build/cmake"
mkdir -p "$BUILD_DIR"
cmake -S "$PORT_DIR" -B "$BUILD_DIR" \
      -DCMAKE_TOOLCHAIN_FILE="$VITASDK/share/vita.toolchain.cmake" \
      -DCMAKE_BUILD_TYPE=Release > /tmp/vita_port_cmake.log 2>&1 || {
    echo "cmake failed, log tail:"
    tail -30 /tmp/vita_port_cmake.log
    exit 1
}
# pipefail: without it the exit status of `make | tail` comes from tail,
# so a FAILED make still printed "Build successful" and shipped a stale
# VPK (2026-09-09 05:16 incident: 05:16 build failed, the 05:24 VPK was
# rebuilt, but the pattern stays a trap).
set -o pipefail
make -C "$BUILD_DIR" -j4 2>&1 | tail -20 || {
    echo "make failed, full log:"
    make -C "$BUILD_DIR" 2>&1 | tail -40
    exit 1
}

# ---------------------------------------------------------------------------
# Step 4: verify our overrides actually won the link
# ---------------------------------------------------------------------------
ELF="$BUILD_DIR/midp_vita"
echo "==> Verifying overrides in $(basename $ELF)..."
arm-vita-eabi-objdump -h "$ELF" > /dev/null 2>&1

# ---------------------------------------------------------------------------
# Done
# ---------------------------------------------------------------------------
echo ""
echo "=============================================="
echo "  Build successful"
echo "  VPK: $BUILD_DIR/midp_vita.vpk"
# 2026-09-05: canonical artifact path is build/cmake/midp_vita.vpk (the
# bare build/midp_vita.vpk is a STALE leftover from the old layout - the
# 0904 v0109 mixup archived that stale file and shipped the wrong binary).
echo "  NOTE: always archive from $BUILD_DIR/midp_vita.vpk"
echo "=============================================="
echo ""
echo "Deploy: copy VPK to Vita/Vita3K, then to switch games create"
echo "  ux0:/data/J2ME00001/launch.cfg with two lines:"
echo "    ux0:/data/J2ME00001/<game>.jar"
echo "    <MIDletClassName>"
echo "  Built-in test MIDlets: HelloMIDlet, CanvasTest, InputTest"
