#!/bin/bash
# patch_kni_fp.sh <libcldc_vm.a>
#
# Overlay for Float.intBitsToFloat / Double.longBitsToDouble (see vita_kni_fp_overlay.S for the full story):
#   1. in the archive member that defines the C bodies, objcopy --redefine-sym X=X_c
#   2. assemble vita_kni_fp_overlay.S and add it to the archive as its own member (defines X, result in r0 / r0:r1)
#   3. nm check: X and X_c are each defined exactly once in the whole archive.
# Idempotent: when X_c already exists step 1 is skipped; step 2 replaces the member of the same name.
set -eo pipefail
L=$1
[ -f "$L" ] || { echo "usage: $0 libcldc_vm.a" >&2; exit 2; }
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
: "${VITASDK:?VITASDK not set (source build_env.sh)}"
P="$VITASDK/bin/arm-vita-eabi"
L="$(cd "$(dirname "$L")" && pwd)/$(basename "$L")"
SYMS="Java_java_lang_Float_intBitsToFloat Java_java_lang_Double_longBitsToDouble"

# NOT "nm | grep -q" (SIGPIPE + pipefail, see BUILD_QUICKSTART pitfall 3): store the output, then count.
NM_ALL="$("$P-nm" -A "$L" 2>/dev/null || true)"
T=$(mktemp -d); trap "rm -rf '$T'" EXIT

for s in $SYMS; do
    if [ "$(printf '%s\n' "$NM_ALL" | grep -c " T ${s}_c\$" || true)" -ge 1 ]; then
        continue                                    # already renamed (idempotent)
    fi
    # member that defines the C body, never the overlay member itself
    mem="$(printf '%s\n' "$NM_ALL" | grep " T ${s}\$" | grep -v 'vita_kni_fp_overlay.o' | head -1 | sed -E 's/^[^:]*:([^:]*):.*/\1/' || true)"
    [ -n "$mem" ] || { echo "patch_kni_fp: no member defines $s" >&2; exit 1; }
    ( cd "$T" && "$P-ar" x "$L" "$mem" && "$P-objcopy" --redefine-sym "$s=${s}_c" "$mem" && "$P-ar" r "$L" "$mem" ) \
        || { echo "patch_kni_fp: rename in $mem failed" >&2; exit 1; }
    NM_ALL="$("$P-nm" -A "$L" 2>/dev/null || true)"
done

"$P-gcc" -c -march=armv7-a -mfloat-abi=hard -mfpu=vfpv3 "$HERE/vita_kni_fp_overlay.S" -o "$T/vita_kni_fp_overlay.o"
( cd "$T" && "$P-ar" r "$L" vita_kni_fp_overlay.o ) || { echo "patch_kni_fp: ar r overlay failed" >&2; exit 1; }

NM_ALL="$("$P-nm" "$L" 2>/dev/null || true)"
for s in Java_java_lang_Float_intBitsToFloat Java_java_lang_Float_intBitsToFloat_c \
         Java_java_lang_Double_longBitsToDouble Java_java_lang_Double_longBitsToDouble_c; do
    n=$(printf '%s\n' "$NM_ALL" | grep -c " T ${s}\$" || true)
    [ "$n" -eq 1 ] || { echo "patch_kni_fp: $s defined $n times (expected 1)" >&2; exit 1; }
done
echo "   patch_kni_fp: OK (intBitsToFloat / longBitsToDouble now return in r0 / r0:r1)"
