#!/bin/bash
# Re-run the FP regression against the ELF in vita-port/build/cmake (or FPELF=...).
#   debug/tools/fpmodel/run.sh            -> calibration + 40 interpreter FP bytecode cases
# Needs only the vitasdk binutils/gcc and python3 (no emulator). Read README.md before trusting a result.
set -e
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../../.." && pwd)"
# v04.54: VITASDK_BIN comes from build_env.sh when this runs inside build.sh;
# the fallback keeps a bare `./run.sh` working on the usual install path.
V="${VITASDK_BIN:-/usr/local/vitasdk/bin}/arm-vita-eabi"
export FPELF="${FPELF:-$ROOT/vita-port/build/cmake/midp_vita}"
export FPDIS=/tmp/fpmodel_elf.dis
cd "$HERE"
$V-objdump -d "$FPELF" > "$FPDIS"
$V-gcc -marm -march=armv7-a -mtune=cortex-a9 -mfloat-abi=hard -mfpu=vfpv3 -O2 -c soft_ref.c -o soft_ref.o
echo "ELF: $FPELF ($(strings -a "$FPELF" | grep -m1 'J2ME Player v'))"
echo "--- calibration (model vs GCC soft-float references)"; python3 armcal.py | tail -1
echo "--- interpreter FP bytecode cases"; python3 fpreg.py none /tmp/fpmodel_cases.out | grep -E 'WRONG|SUMMARY'
echo "--- Math natives (sqrt/floor/ceil/sin/cos/tan, incl. |x| > pi/4 which goes through jvm_d2i)"; python3 mathfull.py | grep -E "WRONG|ABORT|TOTAL"
echo "--- hard-float jvm_d2i/jvm_f2i (C++ callers) vs Java (int) cast, incl. NaN/inf/saturation"; python3 d2ihf.py | tail -3
echo "--- KNI float/double bit natives (result must be in r0 / r0:r1 for the interpreter)"; python3 bitsnat.py | grep -E "WRONG|ABORT|TOTAL"
