#!/bin/bash
# Build the Nomad baseline kernel: Linux 5.13-rc6 + nomad.patch + nomad-alto.patch.
# Expects a Linux git checkout at ./linux (clone it there first; not bundled).
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
KERNEL_DIR="$SCRIPT_DIR/linux"
LOGF="$SCRIPT_DIR/log"
PATCHES=("$SCRIPT_DIR/nomad.patch" "$SCRIPT_DIR/nomad-alto.patch")

[[ -d "$KERNEL_DIR" ]] || { echo "ERROR: kernel tree not found at $KERNEL_DIR (clone Linux there first)"; exit 1; }
for p in "${PATCHES[@]}"; do
    [[ -e "$p" ]] || { echo "ERROR: patch not found: $p"; exit 1; }
done

cd "$KERNEL_DIR"
git checkout 5.13-rc6
for p in "${PATCHES[@]}"; do
    echo "Applying $(basename "$p") ..."
    git apply "$p"
done

bash "$SCRIPT_DIR/../common/apply-build-fixes.sh" "$KERNEL_DIR"

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
