#!/bin/bash
# Build the vendor-API class libraries (Nokia UI/Sound under nokia_src/, other
# vendors' classes under nokia_src/vendor/) into the MIDP classes dir, so games
# that import com.nokia.mid.*, com.samsung.util.*, com.siemens.mp.* etc. can
# load. Classes are preverified like app classes (the CLDC VM requires it for
# jar/dir classes).
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
. "$SCRIPT_DIR/../build_env.sh"
SRC_DIR="$SCRIPT_DIR/nokia_src"
# v04.36: optional JSR packages we implement ourselves (JSR-179 location today)
# live here, in their real javax/... tree, so the sources read like the spec.
JSR_DIR="$SCRIPT_DIR/jsr_src"
OUT_CLASSES="$SCRIPT_DIR/nokia_classes"
CLS="$J2ME_ROOT/phoneme-midp/build/vita_arm/classes"
CLDC_ZIP="$J2ME_ROOT/phoneme-cldc/build/vita_arm/dist/lib/cldc_classes.zip"
MIDP_ZIP=$CLS.zip
PREVERIFY="$J2ME_ROOT/phoneme_source/phoneME/cldc/build/share/bin/linux_i386/preverify"

mkdir -p "$SRC_DIR"
# Stale classes from a removed/renamed source would otherwise be preverified
# and shipped; always start from an empty output directory.
rm -rf "$OUT_CLASSES"
mkdir -p "$OUT_CLASSES/raw"

# Sources: com.nokia.* live flat in nokia_src/, the others in vendor/<pkg path>/
# inside nokia_src/, and our own JSR packages in jsr_src/javax/...
SOURCES="$( { find "$SRC_DIR" -name '*.java'; \
              find "$JSR_DIR" -name '*.java' 2>/dev/null; } | sort)"
[ -n "$SOURCES" ] || { echo "no sources under $SRC_DIR or $JSR_DIR"; exit 1; }

BOOTCP="$CLDC_ZIP:$MIDP_ZIP"
# -encoding: sources are ASCII, but javac falls back to the platform charset
javac -encoding UTF-8 -source 1.3 -target 1.3 -bootclasspath "$BOOTCP" \
      -d "$OUT_CLASSES/raw" $SOURCES

$PREVERIFY -classpath "$BOOTCP" -d "$OUT_CLASSES/preverified" "$OUT_CLASSES/raw"

# Install every preverified package under com/ into the MIDP classes dir
# (packaged into midp_system.jar by the CMake build). Remove the old copies of
# exactly these packages first so a deleted class does not linger.
# v04.36: com.sun.* is MIDP's own code and must not be replaced wholesale, but
# one of our packages legitimately lives there: the Connector protocol handler
# for the sensor: scheme, which the MIDP Connector looks up by class name
# (com.sun.midp.io.j2me.<scheme>.Protocol).
SUN_ALLOW="com/sun/midp/io/j2me/sensor com/sun/midp/io/j2me/btspp"
for top in "$OUT_CLASSES/preverified/com/"*/; do
    name="$(basename "$top")"
    if [ "$name" = "sun" ]; then
        extra="$(cd "$OUT_CLASSES/preverified" && \
                 find com/sun -name '*.class' 2>/dev/null | \
                 grep -vE '^com/sun/midp/io/j2me/(sensor|btspp)/' || true)"
        if [ -n "$extra" ]; then
            echo "ERROR: stub sources define MIDP's own classes:"
            echo "$extra"
            exit 1
        fi
        continue
    fi
    # com/nokia/mid/** was installed by older versions of this script as well;
    # replacing only the packages we ship keeps anything else in com/nokia
    rm -rf "$CLS/com/$name"
done
for allow in $SUN_ALLOW; do
    rm -rf "$CLS/$allow"
done
# v04.36: our own javax packages. Whitelist only, because MIDP's own
# javax.microedition packages (lcdui, media, io, rms, midlet, pki, ...) are
# ROMized - a jar copy would shadow them.
# Full package paths: JSR-82 lives in javax/bluetooth, the others in
# javax/microedition/<pkg>.
JSR_PKGS="javax/microedition/location javax/microedition/sensor javax/bluetooth com/vodafone/v10/graphics/j3d"
for pkg in $JSR_PKGS; do
    rm -rf "$CLS/$pkg"
done
(cd "$OUT_CLASSES/preverified" && find com -name '*.class') | while read -r f; do
    mkdir -p "$CLS/$(dirname "$f")"
    cp "$OUT_CLASSES/preverified/$f" "$CLS/$f"
done
for pkg in $JSR_PKGS; do
    (cd "$OUT_CLASSES/preverified" && \
     find "$pkg" -name '*.class' 2>/dev/null) | while read -r f; do
        mkdir -p "$CLS/$(dirname "$f")"
        cp "$OUT_CLASSES/preverified/$f" "$CLS/$f"
    done
done
# com/sun/** belongs to MIDP itself and must never be touched
echo "Vendor stubs installed into $CLS:"
(cd "$OUT_CLASSES/preverified" && find com -name '*.class' | sort | sed 's/^/  /')
for pkg in $JSR_PKGS; do
    (cd "$OUT_CLASSES/preverified" && \
     find "$pkg" -name '*.class' 2>/dev/null | sort | sed 's/^/  /')
done
