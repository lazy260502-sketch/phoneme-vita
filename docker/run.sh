#!/bin/bash
# Run commands inside the vitasdk build container
# Usage:
#   ./run.sh                    -> interactive shell in /workspace/j2me
#   ./run.sh <command...>       -> run command inside container
set -e

J2ME_DIR="$(cd "$(dirname "$0")/.." && pwd)"

if [ $# -eq 0 ]; then
    exec docker run -it --rm \
        -v "$J2ME_DIR":/workspace/j2me \
        -v /home/zyb/tools/jdk8u502-b07:/opt/jdk8:ro \
        -e JDK_DIR=/opt/jdk8 \
        -u $(id -u):$(id -g) \
        vitasdk-build /bin/bash
else
    exec docker run --rm \
        -v "$J2ME_DIR":/workspace/j2me \
        -v /home/zyb/tools/jdk8u502-b07:/opt/jdk8:ro \
        -e JDK_DIR=/opt/jdk8 \
        -u $(id -u):$(id -g) \
        vitasdk-build "$@"
fi
