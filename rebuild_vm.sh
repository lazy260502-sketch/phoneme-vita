#!/bin/bash
#==============================================================================
# rebuild_vm.sh - Rebuild the CLDC VM (libcldc_vm.a) for PS Vita
#
# Verified recipe from the 2026-09-02 rebuild campaign. See VM_BUILD.md
# for the reasoning behind every step. Run after changing any
# phoneme-cldc/src/**.cpp|.hpp.
#
# Usage:
#   ./rebuild_vm.sh            # target VM + repack library
#   ./rebuild_vm.sh tools      # rebuild host loopgen/romgen only (if broken)
#   ./rebuild_vm.sh clean      # remove target objects (library kept)
#==============================================================================
set -eo pipefail

CLDC=/home/zyb/vitasdk/samples/j2me/phoneme-cldc
BD=$CLDC/build/vita_arm
L=$BD/dist/lib/libcldc_vm.a
AR=/home/zyb/.local/vitasdk/bin/arm-vita-eabi-ar
FLAVOR=release            # debug=AZZERT gaps; product=compiler member; release=OK
# 2026-09-03: FLAVOR=release gives CPP_DEF_FLAGS_release="" (jvm.make:834),
# so -DPRODUCT is NOT added automatically. The library baseline is a
# PRODUCT-ABI build (no Traps* symbols, no ...Ev signatures). Every target
# object MUST be compiled with the same ABI: -DPRODUCT -DROMIZING=1.
# Without it the new objects are ABI-incompatible with the old library
# members they replace (fillInStackTrace crash 2026-09-03).

# Objects that belong ONLY to the cldc_vm executable, never to the library.
# jvm.make LIB_OBJS (lines 1030-1048) explicitly subst's these out:
# Main_vita has main/module_start, ROMImage clashes with the MIDP-side
# ROMImage, the rest corrupt symbol pull-in (pte_osInit/ANI_Initialize
# link failures seen 2026-09-02).
# InterpreterSkeleton.o / OopMapsSkeleton.o are HOST loopgen/romgen stub
# tables (empty interpreter_dispatch_table() etc.) - they must never enter
# the Vita library (v01.28: repack pulled them in and the C-interpreter
# guard caught interpreter_dispatch_table leaking).
EXCLUDE="AsmStubs_x86_64.o|Interpreter_arm.o|InterpreterSkeleton.o|OopMapsSkeleton.o|ani.o|ani_bsd_socket.o|os_port.o|poolthread.o|BSDSocket.o|Main_vita.o|NativesTable.o|ROMImage.o|ReflectNatives.o|jvmspi.o"

export JVMWorkSpace=$CLDC
export JVMBuildSpace=$CLDC/build
export JDK_DIR=/home/zyb/tools/jdk8u502-b07
export TOOLS_DIR=/home/zyb/vitasdk/samples/j2me/phoneme_source/phoneME/tools

die() { echo "FAIL: $*" >&2; exit 1; }

build_tools() {
    echo "== host loopgen/romgen (32-bit x86 via FORCE_GCC in cfg) =="
    cd $BD
    make BUILD_DIR_NAME=vita_arm IsLoopGen=true CLDCBUILD=fp \
         ENABLE_ENABLING_CHECK=false -j"$(nproc)" loopgen
    [ -x $BD/loopgen/app/loopgen ] || die "loopgen missing"
    make BUILD_DIR_NAME=vita_arm IsLoopGen=true ROMGENERATOR_DIR=romgen/app \
         CLDCBUILD=fp ENABLE_ENABLING_CHECK=false -j"$(nproc)" romgen
    [ -x $BD/dist/bin/romgen ] || die "romgen missing"
    echo "== host tools OK =="
}

build_target() {
    echo "== target VM objects (ARM, flavor=$FLAVOR) =="
    cd $BD
    mkdir -p target/$FLAVOR
    # Preseed host-object AsmStubs so vpath never feeds it to the ARM assembler
    cp romgen/app/AsmStubs_x86_64.o target/$FLAVOR/
    touch -d "2026-08-25" target/$FLAVOR/AsmStubs_x86_64.o

    # Triple-clear on the command line is the core of this recipe:
    # FORCE_GCC=        else cfg's romgen branch swaps ALL compiler roles to
    #                   the host g++-11 -> "-marm unrecognized" on ARM sources
    # GNU_TOOLS_DIR=    else the cross prefix is lost -> bare g++, crt0 not found
    # CPP_DEF_FLAGS=    else "-B/usr/bin -m32 -DCROSS_GENERATOR=1" leaks in
    #
    # 2026-09-03 FIX: the bare "CPP_DEF_FLAGS=" was wrong in a subtle way.
    # A command-line variable in GNU make CANNOT be appended to by += in the
    # makefiles, so ALL of vita_arm.cfg's target-section CPP_DEF_FLAGS were
    # silently dropped: -DARM -DVITA -D__PSP2__, -Wno-narrowing -fpermissive,
    # -marm -march=armv7-a -mtune=cortex-a9 -mfloat-abi=hard -mfpu=vfpv3,
    # -DSUPPORTS_MEMORY_MAPPED_FILES=0 -DSUPPORTS_ADJUSTABLE_MEMORY_CHUNK=0
    # -DSUPPORTS_TIMER_THREAD=1 -DSUPPORTS_TIMER_INTERRUPT=0 -DUSE_VM_EXCEPTIONS=0
    # -DUSE_BSD_SOCKET=1. Result: objects were compiled as Thumb without
    # PRODUCT/ROMIZING -> mixed-generation library -> crash. The command line
    # must therefore carry the FULL flag set from vita_arm.cfg lines 232-254
    # plus -DPRODUCT (FLAVOR=release adds nothing) and -DROMIZING=1 (cfg only
    # exports ROMIZING=true as a make variable, not a -D).
    CPP_DEF_FLAGS_TARGET="-DPRODUCT -DROMIZING=1 -DARM -DVITA -D__PSP2__ \
        -Wno-narrowing -fpermissive \
        -marm -march=armv7-a -mtune=cortex-a9 -mfloat-abi=hard -mfpu=vfpv3 \
        -DSUPPORTS_MEMORY_MAPPED_FILES=0 -DSUPPORTS_ADJUSTABLE_MEMORY_CHUNK=0 \
        -DSUPPORTS_TIMER_THREAD=1 -DSUPPORTS_TIMER_INTERRUPT=0 \
        -DUSE_VM_EXCEPTIONS=0 -DUSE_BSD_SOCKET=1"
    # 2026-09-04 FIX: NEVER pass ENABLE_C_INTERPRETER=true. It compiles
    # Interpreter_c.cpp into _MergedSrc006, which brings a second, BSS
    # (zero-initialized) definition of jvm_fast_globals that silently
    # wins over Interpreter_arm.o's .data GP pointer table under
    # --allow-multiple-definition -> every r10-based GP-table access in
    # the fast interpreter reads garbage -> wild jumps into ROM bytecode
    # (the 0902-0904 launch-crash regression, root cause closed in v01.08).
    # It also moves SNI_*/StackmapGenerator*/fplib definitions into 006,
    # splitting the MergedSrc grouping away from the 8-31 baseline.
    make BUILD_DIR_NAME=vita_arm IsLoopGen=true \
         LOOP_GENERATOR_DIR=../../linux_arm/loopgen/app \
         ENABLE_ENABLING_CHECK=false \
         FORCE_GCC= GNU_TOOLS_DIR=/home/zyb/.local/vitasdk \
         CPP_DEF_FLAGS="$CPP_DEF_FLAGS_TARGET" \
         -j"$(nproc)" _$FLAVOR || true   # final exe link fails on crt0: expected

    local n; n=$(ls target/$FLAVOR/*.o 2>/dev/null | wc -l)
    [ "$n" -ge 30 ] || die "only $n objects produced (expect ~31)"
    echo "== $n ARM objects OK =="
}

pack_lib() {
    echo "== repack $L =="
    [ -f $L ] || die "library not found"
    cp $L $L.bak

    # 2026-09-03: NEVER repack from the old KNOWN_GOOD baseline anymore.
    # That re-seeds 20 old-generation members and only the replaced ones get
    # updated -> mixed-generation library -> fillInStackTrace crash. All
    # members that exist in target/$FLAVOR are replaced below, in one pass,
    # so the whole library is one compile generation.
    echo "   (full-generation repack: every matching member replaced)"

    cd $BD/target/$FLAVOR
    local m
    for m in *.o; do
        echo "$m" | grep -qE "^($EXCLUDE)$" && continue
        $AR r $L "$m" || die "ar r $m"
    done

    # Interpreter_arm.o must come from the OLD library: the regenerated .s
    # lost the jvm_f2i/jvm_d2i float stubs (GP table + fast globals only).
    # NOTE: once the regenerated interpreter gains working jvm_f2i/jvm_d2i,
    # remove this and use the fresh member instead.
    # SAFETY: after packing, verify the library has EXACTLY ONE definition
    #   arm-vita-eabi-nm $L | grep jvm_fast_globals   -> single "D" line
    # and zero C-interpreter symbols (g_jpc/g_jsp/interpreter_dispatch_table).
    cd /tmp
    $AR p $L.bak Interpreter_arm.o > Interpreter_arm.o 2>/dev/null \
        || die "cannot extract Interpreter_arm.o from backup"
    $AR r $L Interpreter_arm.o

    # ar chains fail silently - verify members are non-empty
    local sz; sz=$($AR p $L Interpreter_arm.o 2>/dev/null | wc -c)
    [ "$sz" -gt 1000 ] || die "Interpreter_arm.o member is empty ($sz bytes)"

    # 2026-09-04: hard guards against the double-definition regression.
    local dcount; dcount=$(/home/zyb/.local/vitasdk/bin/arm-vita-eabi-nm $L 2>/dev/null | grep -c " D jvm_fast_globals")
    [ "$dcount" -eq 1 ] || die "jvm_fast_globals: expected exactly 1 D definition, got $dcount"
    local ccount; ccount=$(/home/zyb/.local/vitasdk/bin/arm-vita-eabi-nm $L 2>/dev/null | grep -cE "g_jpc|g_jsp|interpreter_dispatch_table")
    [ "$ccount" -eq 0 ] || die "C-interpreter symbols leaked into library ($ccount)"

    local cnt; cnt=$($AR t $L | wc -l)
    echo "== library packed: $cnt members =="
}

case "${1:-build}" in
    tools) build_tools ;;
    clean) rm -rf $BD/target; echo "target objects removed" ;;
    build)
        build_target
        pack_lib
        echo ""
        echo "Next: cd vita-port && git commit ... && cmake --build build -j8"
        echo "(commit BEFORE building so the version string matches)"
        ;;
    *) echo "usage: $0 [build|tools|clean]"; exit 1 ;;
esac
