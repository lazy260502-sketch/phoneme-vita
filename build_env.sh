# build_env.sh - shared path resolution for the phoneME Vita build scripts.
#
# All paths are configurable via environment variables; defaults are
# auto-detected, so no hard-coded machine path (e.g. /home/zyb) survives.
# Source this from a build script:  . "$(dirname "$0")/build_env.sh"
#
# Overridable variables:
#   J2ME_ROOT   workspace root (contains phoneme-cldc, phoneme-midp,
#               phoneme_source, jsr75, vita-port)
#   JDK_DIR     JDK 8 home
#   VITASDK     VitaSDK root (contains bin/arm-vita-eabi-*)

# Workspace root: this file lives at the repo root.
if [ -z "${J2ME_ROOT:-}" ]; then
    J2ME_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
fi

# JDK 8
if [ -z "${JDK_DIR:-}" ] || [ ! -d "$JDK_DIR" ]; then
    for _d in /usr/lib/jvm/java-8-openjdk-amd64 "${JAVA_HOME:-}"; do
        [ -n "$_d" ] && [ -d "$_d" ] && { JDK_DIR="$_d"; break; }
    done
fi

# VitaSDK
if [ -z "${VITASDK:-}" ] || [ ! -d "$VITASDK" ]; then
    for _d in /usr/local/vitasdk "$HOME/.local/vitasdk"; do
        [ -d "$_d" ] && { VITASDK="$_d"; break; }
    done
fi

[ -d "$JDK_DIR" ] || { echo "ERROR: JDK_DIR not found (set JDK_DIR)" >&2; return 1 2>/dev/null || exit 1; }
[ -d "$VITASDK" ] || { echo "ERROR: VITASDK not found (set VITASDK)" >&2; return 1 2>/dev/null || exit 1; }

export J2ME_ROOT JDK_DIR VITASDK
