#!/bin/bash
# Build the Vita SDK Docker image
set -e

cd "$(dirname "$0")"

echo "==> Building vitasdk-build image (Ubuntu 24.04 + latest vitasdk toolchain)..."
docker build -t vitasdk-build .

echo ""
echo "==> Done! Usage:"
echo "    ./run.sh                # enter build shell"
echo "    ./run.sh 'make ...'     # run a command inside container"
