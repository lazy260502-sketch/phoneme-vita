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
# Paths (J2ME_ROOT / JDK_DIR / VITASDK) come from build_env.sh.
. "$PORT_DIR/../build_env.sh"
J2ME_DIR="$J2ME_ROOT"

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
# Step 0.5: rebuild obj/arm/runMidlet.o (v04.27)
#
# WHY THIS IS HERE AND NOT IN THE MAKEFILE:
#   runMidlet.c is the ONLY file in this tree that is hand-compiled. The MIDP
#   build lists it as AMS_RUNMIDLET_OBJ_FILES in
#   src/ams/example/jams/lib.gmk, but that variable is only consumed when the
#   `runMidlet` EXECUTABLE is a build goal - which it never is here, because
#   the launcher links it into midp_vita instead. Nothing in the tree has a
#   pattern rule for obj/arm/runMidlet.o, so make never refreshes it.
#
#   The cost of that, measured 2026-11-04: the file sat at its 11-02 06:18
#   timestamp while runMidlet.c had been edited on 11-04 03:34. Every Vita
#   change that went through runMidlet.c - the v04.19 heap_mb override and the
#   v04.24 JIT timing fix - was therefore DEAD CODE in the shipped VPK.
#   The symptom was maddening and cost a whole debugging session: the settings
#   page wrote ux0:/data/J2ME00001/heap_mb, runMidlet never read it, the heap
#   stayed at 32 MiB, and the JIT kept reporting "no executable memory" no
#   matter what the user selected. `strings` on the SELF showed zero
#   occurrences of "heap_mb" - the proof that the code was not in the binary.
#
#   The include list below is the CMake one plus the three directories that
#   midpMalloc.h / commandLineUtil_md.h / ams_params.h live in (they are
#   pulled in by the MIDP makefile from its own vpath, which CMake does not
#   reproduce).
# ---------------------------------------------------------------------------
RM_SRC="$J2ME_DIR/phoneme-midp/src/ams/example/jams/native/runMidlet.c"
RM_OBJ="$J2ME_DIR/phoneme-midp/build/vita_arm/obj/arm/runMidlet.o"
if [ -f "$RM_SRC" ]; then
    if [ ! -f "$RM_OBJ" ] || [ "$RM_SRC" -nt "$RM_OBJ" ]; then
        echo "==> runMidlet.o is older than runMidlet.c - rebuilding it"
        RM_INC=""
        RM_CMAKE_DIR="$PORT_DIR/build/cmake"
        if [ -f "$RM_CMAKE_DIR/CMakeFiles/midp_vita.dir/flags.make" ]; then
            RM_INC=$(grep 'C_INCLUDES' \
                "$RM_CMAKE_DIR/CMakeFiles/midp_vita.dir/flags.make" \
                | sed 's/^C_INCLUDES = //')
        fi
        RM_EXTRA=""
        for d in \
            src/core/memory/include \
            src/ams/example/ams_common_port/include \
            src/ams/example/ams_params/include \
            src/ams/example/jams/native
        do
            RM_EXTRA="$RM_EXTRA -I$J2ME_DIR/phoneme-midp/$d"
        done
        arm-vita-eabi-gcc -c -O2 -Wall -Wno-unused-parameter \
            -fomit-frame-pointer -marm -march=armv7-a -mfpu=vfpv3 \
            -mfloat-abi=hard -DVITA -D__PSP2__ \
            -DVITA_VM_CPU_AFFINITY_MASK=0 \
            $RM_INC $RM_EXTRA "$RM_SRC" -o "$RM_OBJ" || {
            echo "ERROR: could not compile $RM_SRC"
            exit 1
        }
        # Guard: the whole point of this step. If the object does not contain
        # the launcher-facing strings, the link would silently use stale code.
        if ! strings "$RM_OBJ" | grep -q 'heap_mb'; then
            echo "WARNING: runMidlet.o rebuilt but has no heap_mb string,"
            echo "         the source may have lost the VITA block."
        fi
    fi
fi

# ---------------------------------------------------------------------------
# Step 1 (optional): rebuild phoneme-midp
# ---------------------------------------------------------------------------
if [ -n "$FORCE_MIDP" ]; then
    # Vendor classes (com.nokia.*, com.siemens.*, ...) MUST be compiled and
    # put into classes.zip BEFORE the ROM is generated. romgen reads
    # classes.zip, but build_nokia_stubs.sh (Step 2b) only writes classes/ and
    # used to run AFTER the ROM was made, so the ROM always held the previous
    # build's vendor classes (log123: RingTone's pitch/cache fix was in
    # classes/ and midp_system.jar but the ROM still had the 9721-byte old
    # one, and the ROM copy wins). Same failure family as the libobj.a guard
    # below. A stale zip is fine as the stubs' bootclasspath: they only use
    # the MIDP API, which does not change here.
    CLASSES_DIR="$J2ME_DIR/phoneme-midp/build/vita_arm/classes"
    CLASSES_ZIP="$J2ME_DIR/phoneme-midp/build/vita_arm/classes.zip"
    if [ -f "$CLASSES_ZIP" ]; then
        echo "==> Pre-building vendor classes so the ROM sees them..."
        bash "$PORT_DIR/build_nokia_stubs.sh"
        VENDOR_PKGS="$(cd "$CLASSES_DIR" && ls -d com/*/ 2>/dev/null | grep -v '^com/sun/' | tr -d '\r')"
        if [ -n "$VENDOR_PKGS" ]; then
            # make decides "recompile .java" by comparing sources with classes.zip's mtime;
            # jar uf would bump it and hide every Java edit made since the last build.
            ZIP_MTIME_REF="$(mktemp)"
            touch -r "$CLASSES_ZIP" "$ZIP_MTIME_REF"
            # shellcheck disable=SC2086
            (cd "$CLASSES_DIR" && jar uf "$CLASSES_ZIP" $VENDOR_PKGS)
            touch -r "$ZIP_MTIME_REF" "$CLASSES_ZIP"
            rm -f "$ZIP_MTIME_REF"
        fi
    fi

    echo "==> Rebuilding phoneme-midp (build_vita.sh)..."
    bash "$J2ME_DIR/phoneme-midp/build_vita.sh"

    # Guard (log121): after a Java-side change ROMImage.o was rebuilt but
    # libobj.a - the archive the link actually uses - kept the OLD member, and
    # build_vita.sh still exited 0. ROMized classes (all of com.sun.midp.* and
    # the vendor stubs) are loaded from the ROM, not from midp_system.jar, so
    # the VPK shipped the previous Java code and the device log looked like
    # the fix had done nothing (v02.98: no [ALERT-LAYER], RingTone still null).
    #
    # Why: build_vita.sh runs make with no goal, i.e. `all` = verify display
    # midp varexport, and `midp` does not depend on libobj.a (only
    # bin/arm/runMidlet does, which this port never links). Measured: three
    # plain runs left libobj.a untouched; naming the archive as the goal
    # refreshed it. So ask for it explicitly, then verify the member.
    OBJ_ARM="$J2ME_DIR/phoneme-midp/build/vita_arm/obj/arm"
    ROM_O="$OBJ_ARM/ROMImage.o"
    rom_member_matches() {
        local tmp rc
        tmp="$(mktemp -d)"
        (cd "$tmp" && arm-vita-eabi-ar x "$MIDP_LIBOBJ" ROMImage.o 2>/dev/null)
        cmp -s "$ROM_O" "$tmp/ROMImage.o"
        rc=$?
        rm -rf "$tmp"
        return $rc
    }
    if ! rom_member_matches; then
        echo "==> libobj.a holds a stale ROMImage.o; building the archive goal..."
        MIDP_EXTRA_GOAL="$MIDP_LIBOBJ" bash "$J2ME_DIR/phoneme-midp/build_vita.sh"
        if ! rom_member_matches; then
            echo "ERROR: libobj.a ROMImage.o != obj/arm/ROMImage.o after building libobj.a."
            echo "       The VPK would contain stale ROMized Java classes."
            exit 1
        fi
    fi
fi

# ---------------------------------------------------------------------------
# Step 2: test MIDlets
# ---------------------------------------------------------------------------
echo "==> Building test MIDlets..."
bash "$PORT_DIR/build_jar.sh"

# ---------------------------------------------------------------------------
# Step 2b: Nokia UI stubs (FullCanvas/DirectUtils) into the MIDP classes dir,
# so CMake packs them into midp_system.jar. Without this, games depending on
# com.nokia.mid.ui.* die with NoClassDefFoundError (log116).
# ---------------------------------------------------------------------------
echo "==> Building Nokia UI stubs..."
bash "$PORT_DIR/build_nokia_stubs.sh"

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
# Step 4b: FP gate. v04.29-v04.49 shipped a C++ math library that read the wrong half of every double (sqrt(1) came
# out as -1.49) and nothing noticed for six builds, because nothing ever ran the math after linking. This runs the
# host-side ARM model (debug/tools/fpmodel) on the ELF that was just linked: calibration first (a model that does not
# match GCC proves nothing), then Math natives, Double bit natives and the hard-float d2i/f2i. Any WRONG or ABORT
# fails the build; an ABORT means the model could not run an instruction, which is NOT a pass.
# FP_GATE=0 skips it (for a build that cannot change FP code); a missing python3 is an error, not a silent skip.
# ---------------------------------------------------------------------------
if [ "${FP_GATE:-1}" != "0" ]; then
    echo "==> FP gate (fpmodel) on $(basename "$ELF")..."
    command -v python3 > /dev/null || { echo "ERROR: python3 not found; FP gate cannot run (FP_GATE=0 to skip)"; exit 1; }
    FPDIR="$J2ME_DIR/debug/tools/fpmodel"
    [ -f "$FPDIR/mathfull.py" ] || { echo "ERROR: $FPDIR missing (see debug/tools/fpmodel/README.md; FP_GATE=0 to skip)"; exit 1; }
    # v04.54: fpmodel is tracked in git now, and its scripts find the toolchain
    # through VITASDK_BIN instead of hard-coding /usr/local/vitasdk. armcal.py
    # also builds soft_ref.o itself - it used to be a manual step, so a fresh
    # clone failed this gate with FileNotFoundError on soft_ref.o.
    export VITASDK_BIN="$VITASDK/bin"
    fp_fail=0
    fp_run() {   # fp_run <label> <script> <must-match regex> -- any WRONG/ABORT line also fails
        local label="$1" script="$2" want="$3" out
        out=$(cd "$FPDIR" && FPELF="$ELF" timeout 900 python3 "$script" 2>&1) || true
        if echo "$out" | grep -q -E 'WRONG|ABORT|Traceback' || ! echo "$out" | grep -q -E "$want"; then
            echo "  FAIL  $label"; echo "$out" | grep -E 'WRONG|ABORT|Traceback|TOTAL|SUMMARY|CALIBRATION|bad' | head -8 | sed 's/^/        /'
            fp_fail=1
        else
            echo "  ok    $label: $(echo "$out" | grep -E "$want" | tail -1)"
        fi
    }
    fp_run "calibration"            armcal.py   'CALIBRATION PASSED \(0 mismatches\)'
    fp_run "Math natives"           mathfull.py 'TOTAL [0-9]+: ok=[0-9]+ wrong=0 abort=0'
    fp_run "Double/Float bit natives" bitsnat.py 'TOTAL [0-9]+: ok=[0-9]+ wrong=0 abort=0'
    fp_run "hard-float d2i/f2i"     d2ihf.py    'cases, 0 bad'
    [ "$fp_fail" -eq 0 ] || { echo "ERROR: FP gate failed - do not ship this ELF"; exit 1; }
fi

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
