#!/bin/bash
# Apply the upstream host-tool fix needed by pre-5.17 baseline kernels.
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
[[ $# -eq 1 ]] || { echo "Usage: $0 KERNEL_DIR" >&2; exit 2; }
KERNEL_DIR="$1"
PATCH="$SCRIPT_DIR/libsubcmd-realloc.patch"
if git -C "$KERNEL_DIR" apply --reverse --check "$PATCH" >/dev/null 2>&1; then
    echo "libsubcmd realloc fix already present"
else
    git -C "$KERNEL_DIR" apply --check "$PATCH"
    git -C "$KERNEL_DIR" apply "$PATCH"
fi
