#!/bin/bash
# Build test MIDlets (HelloMIDlet, CanvasTest, InputTest) into Hello.jar
# Layout and preverify steps follow midp-vita/build_jar.sh.

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC_DIR="$SCRIPT_DIR/midlets"
BUILD_DIR="$SCRIPT_DIR/build/midlet"

# J2ME class library
CLDC_ZIP="/home/zyb/vitasdk/samples/j2me/phoneme-cldc/build/vita_arm/dist/lib/cldc_classes.zip"
MIDP_ZIP="/home/zyb/vitasdk/samples/j2me/phoneme-midp/build/vita_arm/classes.zip"
BOOTCP="$CLDC_ZIP:$MIDP_ZIP"

mkdir -p "$BUILD_DIR/classes" "$BUILD_DIR/preverified" "$BUILD_DIR/jar"

# CLDC preverify tool (32-bit x86 host binary bundled with phoneME)
PREVERIFY="/home/zyb/vitasdk/samples/j2me/phoneme_source/phoneME/cldc/build/share/bin/linux_i386/preverify"

echo "Compiling test MIDlets..."
javac -source 1.3 -target 1.3 \
    -bootclasspath "$BOOTCP" \
    -d "$BUILD_DIR/classes" \
    "$SRC_DIR/HelloMIDlet.java" \
    "$SRC_DIR/CanvasTest.java" \
    "$SRC_DIR/InputTest.java" \
    "$SRC_DIR/ToneTest.java"

echo "Preverifying classes..."
rm -rf "$BUILD_DIR/preverified"/*
"$PREVERIFY" \
    -classpath "$BOOTCP" \
    -d "$BUILD_DIR/preverified" \
    "$BUILD_DIR/classes"

echo "Creating JAR..."
cd "$BUILD_DIR/preverified"
jar cfm "$BUILD_DIR/jar/Hello.jar" "$SRC_DIR/MANIFEST.MF" *.class

# Normalize the zip layout: the JDK 'jar' tool writes STREAMED entries
# (local header crc/sizes = 0 + data descriptors). phoneME's
# JarFileParser re-derives offsets from the central directory and its
# handling of streamed local headers is fragile - v01.15's jar (9460B)
# parsed, v01.16's (9452B, 8 bytes different deflate output) threw
# ClassNotFoundException for every class. Rewriting the zip with
# deterministic NON-streamed local headers removes that variable.
python3 - "$BUILD_DIR/jar/Hello.jar" <<'NORMALIZE'
import sys, zipfile, shutil, os
path = sys.argv[1]
tmp = path + '.norm'
with zipfile.ZipFile(path) as zin, \
     zipfile.ZipFile(tmp, 'w', zipfile.ZIP_DEFLATED) as zout:
    for info in zin.infolist():
        if info.filename.endswith('/'):
            continue  # drop directory stubs
        zi = zipfile.ZipInfo(info.filename, date_time=(2000, 1, 1, 0, 0, 0))
        zi.compress_type = zipfile.ZIP_DEFLATED
        zi.external_attr = info.external_attr
        zout.writestr(zi, zin.read(info.filename))
shutil.move(tmp, path)
print('normalized (non-streamed, no dir stubs):', path, os.path.getsize(path), 'bytes')
NORMALIZE

echo "JAR created at: $BUILD_DIR/jar/Hello.jar"
ls -la "$BUILD_DIR/jar/Hello.jar"
