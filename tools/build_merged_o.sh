#!/bin/bash
# build_merged_o.sh - recompile a _MergedSrcNNN.cpp from the CURRENT source
# tree with the exact PRODUCT-ABI flags, as a drop-in replacement member for
# the hybrid libcldc_vm.a. Usage: build_merged_o.sh 003 /tmp/out.o
set -eo pipefail
N="$1"; OUT="$2"
GEN=/home/zyb/vitasdk/samples/j2me/phoneme-cldc/build/vita_arm/loopgen/generated
SRC=/home/zyb/vitasdk/samples/j2me/phoneme-cldc/src
cd $GEN
/home/zyb/.local/vitasdk/bin/g++ -O2 -Wuninitialized \
  -DPRODUCT -DROMIZING=1 -DARM -DVITA -D__PSP2__ \
  -Wno-narrowing -fpermissive \
  -marm -march=armv7-a -mtune=cortex-a9 -mfloat-abi=hard -mfpu=vfpv3 \
  -DSUPPORTS_MEMORY_MAPPED_FILES=0 -DSUPPORTS_ADJUSTABLE_MEMORY_CHUNK=0 \
  -DSUPPORTS_TIMER_THREAD=1 -DSUPPORTS_TIMER_INTERRUPT=0 \
  -DUSE_VM_EXCEPTIONS=0 -DUSE_BSD_SOCKET=1 \
  -fno-gnu-keywords -fno-operator-names -fno-exceptions \
  -fno-optional-diags -fno-rtti \
  -I. -I"$SRC/vm/share/compiler" -I"$SRC/vm/share/debugger" \
  -I"$SRC/vm/share/handles" -I"$SRC/vm/share/memory" \
  -I"$SRC/vm/share/interpreter" -I"$SRC/vm/share/isolate" \
  -I"$SRC/vm/share/natives" -I"$SRC/vm/share/reflection" \
  -I"$SRC/vm/share/runtime" -I"$SRC/vm/share/utilities" \
  -I"$SRC/vm/share/ROM" -I"$SRC/vm/share/verifier" \
  -I"$SRC/vm/share/float" -I"$SRC/vm/os/utilities" \
  -I"$SRC/vm/share/memoryprofiler" -I"$SRC/vm/os/vita" \
  -I"$SRC/midp" -I"$SRC/vm/cpu/arm" -I"$SRC/vm/cpu/c" \
  -I"$SRC/vm/cpu/i386" -I"$SRC/vm/cpu/sh" -I"$SRC/vm/cpu/thumb" \
  -I"$SRC/vm/cpu/thumb2" -I"$SRC/anilib/share" -I"$SRC/anilib/vita" \
  -I"$SRC/tools/ads_extender" \
  -c "_MergedSrc${N}.cpp" -o "$OUT"
echo "OK: $OUT ($(stat -c%s "$OUT") bytes)"
