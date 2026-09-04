#!/bin/bash
# Symbol-closure check: extract all members of a library, verify every
# undefined symbol is satisfied by another member or by known external
# (sce*, GCC runtime, vita-port side provides the rest at final link).
set -u
LIB="$1"
DIR=$(mktemp -d)
cd "$DIR"
/home/zyb/.local/vitasdk/bin/arm-vita-eabi-ar x "$LIB" || { echo "ar x failed"; exit 1; }
arm-vita-eabi-nm *.o 2>/dev/null > all.nm
grep " U " all.nm | awk '{print $2}' | sort -u > undef.txt
grep -E "^[0-9a-f]+ [A-Za-z]" all.nm | awk '{print $3}' | sort -u > def.txt
# leak = undefined minus defined, minus compiler/runtime/sce externals
comm -23 undef.txt def.txt \
  | grep -vE '^(sce[A-Z]|_GLOBAL_|__aeabi|__gnu|_Unwind|__cxa|__gxx)' \
  > leak.txt
echo "members: $(ls *.o | wc -l)  undef: $(wc -l < undef.txt)  leak: $(wc -l < leak.txt)"
cat leak.txt
echo "---"
echo "DIR=$DIR"
