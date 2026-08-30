#!/bin/bash
# Build Nokia UI API stubs (FullCanvas/DirectUtils) into the MIDP classes
# dir so games depending on com.nokia.mid.ui.* can load. Classes are
# preverified like app classes (CLDC VM requires it for jar/dir classes).
set -e

SRC_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/nokia_src"
OUT_CLASSES="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/nokia_classes"
CLS=/home/zyb/vitasdk/samples/j2me/phoneme-midp/build/vita_arm/classes
CLDC_ZIP=/home/zyb/vitasdk/samples/j2me/phoneme-cldc/build/vita_arm/dist/lib/cldc_classes.zip
MIDP_ZIP=$CLS.zip
PREVERIFY=/home/zyb/vitasdk/samples/j2me/phoneme_source/phoneME/cldc/build/share/bin/linux_i386/preverify

mkdir -p "$SRC_DIR" "$OUT_CLASSES"
ls "$SRC_DIR"/*.java > /dev/null

BOOTCP="$CLDC_ZIP:$MIDP_ZIP"
javac -source 1.3 -target 1.3 -bootclasspath "$BOOTCP" -d "$OUT_CLASSES" "$SRC_DIR"/*.java

rm -rf "$OUT_CLASSES/preverified"
$PREVERIFY -classpath "$BOOTCP" -d "$OUT_CLASSES/preverified" "$OUT_CLASSES"

# install preverified classes into the MIDP classes dir (packaged into
# midp_system.jar by the CMake build)
mkdir -p "$CLS/com/nokia/mid/ui"
cp "$OUT_CLASSES/preverified/com/nokia/mid/ui/"*.class "$CLS/com/nokia/mid/ui/"
echo "Nokia stubs installed into $CLS"
