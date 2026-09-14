#!/bin/bash
# Regenerate the bitmap font bank used by vita_font.c.
# MUST run the host gcc with -B/usr/bin: the vitasdk bin dir earlier in
# PATH contains the ARM cross as/ld, which reject x86 objects.
# Inputs: ../config/font.ttf (CJK face) and ../config/font-latin.ttf
# (Latin face, proportional + anti aliased); output packaged at
# ../config/fontbitmap.bin
set -e
cd "$(dirname "$0")"
# No gcc-11 on this host; plain gcc is fine, -B/usr/bin is what matters.
CC=${CC:-/usr/bin/gcc}
"$CC" -B/usr/bin -O2 -o fontgen fontgen.c -lm
./fontgen ../config/font.ttf ../config/font-latin.ttf fontbitmap.bin
cp fontbitmap.bin ../config/fontbitmap.bin
echo "updated config/fontbitmap.bin"
