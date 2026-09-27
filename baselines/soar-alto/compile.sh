#!/bin/bash
# Build the Soar/Alto baseline kernel: Linux v6.3 + colloid-skx.patch + colloid-skx-alto.patch.
# Expects a Linux git checkout at ./linux (clone it there first; not bundled).
# See README.md and the upstream repo (github.com/MoatLab/SoarAlto) for the full system.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
KERNEL_DIR="$SCRIPT_DIR/linux"
LOGF="$SCRIPT_DIR/log"
PATCHES=("$SCRIPT_DIR/colloid-skx.patch" "$SCRIPT_DIR/colloid-skx-alto.patch" "$SCRIPT_DIR/../common/colloid-build.patch")

[[ -d "$KERNEL_DIR" ]] || { echo "ERROR: kernel tree not found at $KERNEL_DIR (clone Linux there first)"; exit 1; }
for p in "${PATCHES[@]}"; do
    [[ -e "$p" ]] || { echo "ERROR: patch not found: $p"; exit 1; }
done

cd "$KERNEL_DIR"
git checkout v6.3
for p in "${PATCHES[@]}"; do
    echo "Applying $(basename "$p") ..."
    git apply "$p"
done

# Base the config on the running kernel, then resolve new symbols
# non-interactively. (Do NOT pipe `yes` into `make oldconfig`: under
# `set -o pipefail`, `yes` dies with SIGPIPE and aborts the build.)
echo "configuring (olddefconfig) ..."
if [[ -f "/boot/config-$(uname -r)" ]]; then
    cp "/boot/config-$(uname -r)" .config
fi
make olddefconfig > "$LOGF" 2>&1
echo "make ..."
make -j "$(nproc)" >> "$LOGF" 2>&1
echo "make INSTALL_MOD_STRIP=1 modules_install ..."
make INSTALL_MOD_STRIP=1 modules_install >> "$LOGF" 2>&1
echo "make install ..."
make install >> "$LOGF" 2>&1
echo "update-grub ..."
update-grub

echo "Build log: $LOGF"
