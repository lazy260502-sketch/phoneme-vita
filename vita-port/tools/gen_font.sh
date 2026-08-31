#!/bin/bash
# Regenerate the 1bpp bitmap font bank used by vita_font.c.
# MUST run the host gcc with -B/usr/bin: the vitasdk bin dir earlier in
# PATH contains the ARM cross as/ld, which reject x86 objects.
# Input font: ../config/font.ttf ; output packaged at ../config/fontbitmap.bin
set -e
cd "$(dirname "$0")"
/usr/bin/gcc-11 -B/usr/bin -O2 -o fontgen fontgen.c -lm
./fontgen ../config/font.ttf fontbitmap.bin
cp fontbitmap.bin ../config/fontbitmap.bin
echo "updated config/fontbitmap.bin"
