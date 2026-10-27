#!/bin/bash
# Build both PACT kernel modules (tierinit, kswapdrst) in one go.
# Each module has its own Makefile; this just runs `make` in both.
#
# Usage:
#   ./build_modules.sh          # build both
#   ./build_modules.sh clean    # remove built artifacts

set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TARGET="${1:-}"

for m in tierinit kswapdrst; do
    echo "==> make ${TARGET:-modules} in $m"
    make -C "$SCRIPT_DIR/$m" ${TARGET:+"$TARGET"}
done
