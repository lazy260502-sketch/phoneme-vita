#!/bin/bash
# patch_interp.sh <in Interpreter_arm.o> <out Interpreter_arm.o>
#
# Builds the FP-ABI-fixed interpreter object (see vita_fp_overlay.S header for the full story):
#   1. objcopy --redefine-sym jvm_X=jvm_X_isoft for the 28 helpers the interpreter calls with integer registers,
#      plus jvm_d2i / jvm_f2i (the interpreter's own integer-protocol bodies, also referenced by its GP table);
#   2. assemble vita_fp_overlay.S and merge it into the same object with ld -r.
# The result defines jvm_d2i / jvm_f2i with the hard-float C ABI (for the C++ callers) and every *_isoft symbol.
#
# Idempotent: if the input already contains jvm_d2i_isoft it is copied unchanged (pack_lib seeds its input from the
# previous library, which may already be patched).
set -eo pipefail
IN=$1; OUT=$2
[ -f "$IN" ] && [ -n "$OUT" ] || { echo "usage: $0 in.o out.o" >&2; exit 2; }
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
mkdir -p "$(dirname "$OUT")"
: "${VITASDK:?VITASDK not set (source build_env.sh)}"
P="$VITASDK/bin/arm-vita-eabi"

# NOT "nm | grep -q": grep -q exits on the first match, nm then dies of SIGPIPE (rc 141) and under "set -o pipefail"
# the test is randomly false -> the overlay was merged twice ("multiple definition of jvm_l2f_isoft"). Count instead.
if [ "$("$P-nm" "$IN" | grep -c ' T jvm_d2i_isoft$' || true)" -ge 1 ]; then
    cp "$IN" "$OUT"; echo "   patch_interp: input already patched, copied"; exit 0
fi

HELPERS="dadd dsub dmul ddiv drem fadd fsub fmul fdiv frem dcmpl dcmpg fcmpl fcmpg sin cos tan sqrt ceil floor
         i2d i2f f2d d2f l2d l2f f2l d2l d2i f2i"
RENAME=()
for h in $HELPERS; do RENAME+=(--redefine-sym "jvm_$h=jvm_${h}_isoft"); done

T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
"$P-objcopy" "${RENAME[@]}" "$IN" "$T/interp_renamed.o"
"$P-gcc" -c -march=armv7-a -mfloat-abi=hard -mfpu=vfpv3 "$HERE/vita_fp_overlay.S" -o "$T/overlay.o"
"$P-ld" -r "$T/interp_renamed.o" "$T/overlay.o" -o "$OUT"

# Every renamed symbol must be defined exactly once (a still-undefined one means a missing adapter).
for h in $HELPERS; do
    n=$("$P-nm" "$OUT" | grep -c " T jvm_${h}_isoft\$" || true)
    [ "$n" -eq 1 ] || { echo "patch_interp: jvm_${h}_isoft defined $n times" >&2; exit 1; }
done
for h in d2i f2i; do
    n=$("$P-nm" "$OUT" | grep -c " T jvm_$h\$" || true)
    [ "$n" -eq 1 ] || { echo "patch_interp: hard-float jvm_$h defined $n times" >&2; exit 1; }
done
echo "   patch_interp: OK (28 *_isoft adapters + hard-float jvm_d2i/jvm_f2i)"
