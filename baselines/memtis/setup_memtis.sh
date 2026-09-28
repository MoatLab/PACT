#!/bin/bash
# Build script for the Memtis kernel (Linux v5.15.19 + memtis.patch).
# Clones v5.15.19, applies memtis.patch, builds with the provided config.

set -e

# ---------------------------------------------------------------------------
# Paths
# ---------------------------------------------------------------------------
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PATCH_FILE="$SCRIPT_DIR/memtis.patch"
BASE_CONFIG="$SCRIPT_DIR/config"
KERNEL_DIR="$SCRIPT_DIR/linux"

# ---------------------------------------------------------------------------
# Sanity checks
# ---------------------------------------------------------------------------
if [ ! -f "$PATCH_FILE" ]; then
    echo "ERROR: patch file not found: $PATCH_FILE"
    exit 1
fi

if [ ! -f "$BASE_CONFIG" ]; then
    echo "ERROR: base config not found: $BASE_CONFIG"
    exit 1
fi

# ---------------------------------------------------------------------------
# 1. Clone Linux v5.15.19
# ---------------------------------------------------------------------------
echo "==> Fetching Linux v5.15.19 source..."
# v5.15.19 is a STABLE point release: it lives in linux-stable, NOT in
# torvalds/linux (which only tags mainline releases like v5.15). Clone the
# stable tree.
if [ ! -d "$KERNEL_DIR/.git" ]; then
    git clone --depth=1 --branch v5.15.19 \
        https://git.kernel.org/pub/scm/linux/kernel/git/stable/linux.git "$KERNEL_DIR"
else
    echo "    Repo already exists, skipping clone."
fi

cd "$KERNEL_DIR"

echo "==> Checking out v5.15.19..."
git checkout v5.15.19

# ---------------------------------------------------------------------------
# 2. Apply Memtis patch
# ---------------------------------------------------------------------------
echo "==> Verifying patch applies cleanly (dry run)..."
patch -p1 --dry-run < "$PATCH_FILE"

echo "==> Applying memtis.patch..."
patch -p1 < "$PATCH_FILE"
echo "    Patch applied successfully."
git apply "$SCRIPT_DIR/memtis-vmstat.patch"
git apply "$SCRIPT_DIR/memtis-pte-allocation.patch"
bash "$SCRIPT_DIR/../common/apply-build-fixes.sh" "$KERNEL_DIR"

# ---------------------------------------------------------------------------
# 3. Kernel configuration
# ---------------------------------------------------------------------------
echo "==> Installing base kernel config..."
cp "$BASE_CONFIG" .config

echo "==> Running make olddefconfig to resolve any new symbols..."
make olddefconfig

echo "==> Enabling CONFIG_INTEL_UNCORE_FREQ_CONTROL as built-in (*)..."
./scripts/config --enable CONFIG_INTEL_UNCORE_FREQ_CONTROL
make olddefconfig

if grep -q "^CONFIG_INTEL_UNCORE_FREQ_CONTROL=y" .config; then
    echo "    CONFIG_INTEL_UNCORE_FREQ_CONTROL=y  confirmed."
else
    echo "WARNING: CONFIG_INTEL_UNCORE_FREQ_CONTROL may not be set — check .config manually."
fi

# ---------------------------------------------------------------------------
# 4. Build
# ---------------------------------------------------------------------------
NPROC=$(nproc)
echo "==> Building kernel with $NPROC parallel jobs..."
echo "    (this will take several minutes)"
make -j"$NPROC"

# ---------------------------------------------------------------------------
# 5. Done
# ---------------------------------------------------------------------------
echo ""
echo "==> Build complete."
echo ""
echo "    Kernel image  : $KERNEL_DIR/arch/x86/boot/bzImage"
echo "    Kernel version: $(make kernelrelease 2>/dev/null)"
echo ""
echo "    To install (requires root):"
echo "      sudo make -C $KERNEL_DIR modules_install"
echo "      sudo make -C $KERNEL_DIR install"
echo "      sudo update-grub"
