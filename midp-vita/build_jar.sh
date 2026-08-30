#!/bin/bash
# Build the test MIDlet JAR file

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC_DIR="$SCRIPT_DIR/src"
BUILD_DIR="$SCRIPT_DIR/build/midlet"

# J2ME class library
CLDC_ZIP="/home/zyb/vitasdk/samples/j2me/phoneme-cldc/build/vita_arm/dist/lib/cldc_classes.zip"
MIDP_ZIP="/home/zyb/vitasdk/samples/j2me/phoneme-midp/build/vita_arm/classes.zip"
BOOTCP="$CLDC_ZIP:$MIDP_ZIP"

mkdir -p "$BUILD_DIR/classes"
mkdir -p "$BUILD_DIR/preverified"
mkdir -p "$BUILD_DIR/jar"

# CLDC preverify tool (32-bit x86 host binary bundled with phoneME)
PREVERIFY="/home/zyb/vitasdk/samples/j2me/phoneme_source/phoneME/cldc/build/share/bin/linux_i386/preverify"

# Compile the MIDlet
echo "Compiling HelloMIDlet.java..."
javac -source 1.3 -target 1.3 \
    -bootclasspath "$BOOTCP" \
    -d "$BUILD_DIR/classes" \
    "$SRC_DIR/HelloMIDlet.java"

# Preverify the classes (CLDC VM rejects non-preverified classes with
# ClassFormatError: verification_error)
echo "Preverifying classes..."
rm -rf "$BUILD_DIR/preverified"/*
"$PREVERIFY" \
    -classpath "$BOOTCP" \
    -d "$BUILD_DIR/preverified" \
    "$BUILD_DIR/classes"

# Create the JAR with manifest (from PREVERIFIED classes!)
echo "Creating JAR..."
cd "$BUILD_DIR/preverified"
jar cfm "$BUILD_DIR/jar/Hello.jar" "$SRC_DIR/MANIFEST.MF" *.class

echo "JAR created at: $BUILD_DIR/jar/Hello.jar"
ls -la "$BUILD_DIR/jar/Hello.jar"
