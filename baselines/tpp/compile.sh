#!/bin/bash
# Build the TPP baseline kernel: Linux v5.15 + tpp.patch.
# Expects a Linux git checkout at ./linux (clone it there first; not bundled).
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
KERNEL_DIR="$SCRIPT_DIR/linux"
PATCH="$SCRIPT_DIR/tpp.patch"
LOGF="$SCRIPT_DIR/log"

[[ -d "$KERNEL_DIR" ]] || { echo "ERROR: kernel tree not found at $KERNEL_DIR (clone Linux there first)"; exit 1; }
[[ -e "$PATCH" ]]      || { echo "ERROR: patch not found: $PATCH"; exit 1; }

cd "$KERNEL_DIR"
git checkout v5.15
echo "Applying $(basename "$PATCH") ..."
# Idempotent: skip if the patch is already applied (so a re-run after a
# later failure does not abort at `git apply`).
if git apply --reverse --check "$PATCH" >/dev/null 2>&1; then
    echo "  (already applied, skipping)"
else
    git apply "$PATCH"
fi

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
