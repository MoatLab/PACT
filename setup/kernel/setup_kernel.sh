#!/bin/bash
# Build the kernel for PACT and Tierce: Linux v6.3 with tierce-v6.3.patch,
# packaged as Debian packages. README.md describes what the patch adds and
# how to install the packages and set up GRUB.
#
# Usage:
#   ./setup_kernel.sh
#
# Optional environment variables:
#   KERNEL_DIR          Linux git tree. If it does not exist, the script
#                       clones tag v6.3 into it. Default: ./linux next to
#                       this script.
#   KERNEL_CONFIG       Base .config. Default: config-6.3.0-tierce next to
#                       this script.
#   KERNEL_LOCALVERSION Kernel release suffix. Default: -tierce (release
#                       6.3.0-tierce), or -tierce-baselines (release
#                       6.3.0-tierce-baselines) with KERNEL_BASELINES=1, so
#                       that uname -r tells the two builds apart.
#   KERNEL_PKGVERSION   Debian package version. Default: 1.
#   KERNEL_BASELINES    1: also apply baselines-v6.3.patch after
#                       tierce-v6.3.patch, so that the same kernel can run
#                       the NUMA-balancing and Colloid baselines (see
#                       README.md). Default: 0 (tierce-v6.3.patch only).
#   JOBS                Parallel make jobs. Default: nproc.
#
# Output: the linux-image and linux-headers packages and build.log, in the
# parent directory of KERNEL_DIR. The script installs nothing.
set -euo pipefail
set -o errtrace

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
KERNEL_DIR="${KERNEL_DIR:-$SCRIPT_DIR/linux}"
KERNEL_CONFIG="${KERNEL_CONFIG:-$SCRIPT_DIR/config-6.3.0-tierce}"
KERNEL_BASELINES="${KERNEL_BASELINES:-0}"
if [[ "$KERNEL_BASELINES" == "1" ]]; then
    KERNEL_LOCALVERSION="${KERNEL_LOCALVERSION:--tierce-baselines}"
else
    KERNEL_LOCALVERSION="${KERNEL_LOCALVERSION:--tierce}"
fi
KERNEL_PKGVERSION="${KERNEL_PKGVERSION:-1}"
JOBS="${JOBS:-$(nproc)}"
PATCH="$SCRIPT_DIR/tierce-v6.3.patch"
ADDON="$SCRIPT_DIR/baselines-v6.3.patch"
LINUX_URL="https://github.com/torvalds/linux.git"
TAG="v6.3"

die()
{
    echo "ERROR: $*" >&2
    exit 1
}

[[ -d "$(dirname "$KERNEL_DIR")" ]] ||
    die "parent directory of KERNEL_DIR does not exist: $KERNEL_DIR"
OUT_DIR="$(cd "$(dirname "$KERNEL_DIR")" && pwd)"
LOGF="$OUT_DIR/build.log"

on_error()
{
    echo "ERROR: command failed at line $1. Last lines of $LOGF:" >&2
    tail -n 20 "$LOGF" >&2
}
trap 'on_error $LINENO' ERR

# Fail if .config does not contain the exact line given.
require_config()
{
    grep -qx -- "$1" .config || die ".config lacks '$1' after olddefconfig"
}

[[ -f "$PATCH" ]] || die "patch not found: $PATCH"
[[ "$KERNEL_BASELINES" == "0" || "$KERNEL_BASELINES" == "1" ]] ||
    die "KERNEL_BASELINES must be 0 or 1: $KERNEL_BASELINES"
PATCHES=("$PATCH")
if [[ "$KERNEL_BASELINES" == "1" ]]; then
    [[ -f "$ADDON" ]] || die "add-on patch not found: $ADDON"
    PATCHES+=("$ADDON")
fi
[[ -f "$KERNEL_CONFIG" ]] || die "config not found: $KERNEL_CONFIG"
KERNEL_CONFIG="$(cd "$(dirname "$KERNEL_CONFIG")" && pwd)/$(basename "$KERNEL_CONFIG")"
[[ "$JOBS" =~ ^[1-9][0-9]*$ ]] || die "JOBS must be a positive integer: $JOBS"
for tool in git make gcc flex bison bc perl rsync cpio fakeroot dpkg-buildpackage; do
    command -v "$tool" >/dev/null || die "missing build tool: $tool (see README.md)"
done
: > "$LOGF"

# ---------------------------------------------------------------------------
# 1. Linux v6.3 source
# ---------------------------------------------------------------------------
if [[ ! -e "$KERNEL_DIR" ]]; then
    echo "==> Cloning Linux $TAG into $KERNEL_DIR ..."
    git clone --depth 1 --branch "$TAG" "$LINUX_URL" "$KERNEL_DIR"
elif [[ ! -e "$KERNEL_DIR/.git" ]]; then
    die "$KERNEL_DIR exists but is not a git checkout"
fi
KERNEL_DIR="$(cd "$KERNEL_DIR" && pwd)"
cd "$KERNEL_DIR"

if ! git rev-parse -q --verify "refs/tags/$TAG" >/dev/null; then
    echo "==> Fetching tag $TAG ..."
    if [[ "$(git rev-parse --is-shallow-repository)" == "true" ]]; then
        git fetch --depth 1 "$LINUX_URL" "refs/tags/$TAG:refs/tags/$TAG"
    else
        git fetch "$LINUX_URL" "refs/tags/$TAG:refs/tags/$TAG"
    fi
fi
TAG_COMMIT="$(git rev-parse "$TAG^{commit}")"

# ---------------------------------------------------------------------------
# 2. Apply the patches (skipped if the tree already holds exactly them)
# ---------------------------------------------------------------------------
# True if HEAD is the tag, the files that the patches touch equal the tag
# with the patches applied in order, and no other tracked file is modified.
# The expected files are built in a temporary index, because "git apply
# --check" tests each patch against the tree as it is, not after the patch
# before it.
tree_has_patches()
{
    local idx p touched others
    [[ "$(git rev-parse HEAD)" == "$TAG_COMMIT" ]] || return 1
    idx="$(mktemp)"
    if ! GIT_INDEX_FILE="$idx" git read-tree "$TAG"; then
        rm -f "$idx"
        return 1
    fi
    for p in "${PATCHES[@]}"; do
        if ! GIT_INDEX_FILE="$idx" git apply --cached "$p" 2>/dev/null; then
            rm -f "$idx"
            return 1
        fi
    done
    mapfile -t touched < <(for p in "${PATCHES[@]}"; do
        git apply --numstat "$p"; done | awk '{print $3}' | sort -u)
    if ! GIT_INDEX_FILE="$idx" git diff --quiet -- "${touched[@]}"; then
        rm -f "$idx"
        return 1
    fi
    rm -f "$idx"
    others="$(git status --porcelain --untracked-files=no | awk '{print $2}' |
        grep -vxF -f <(printf '%s\n' "${touched[@]}") || true)"
    [[ -z "$others" ]]
}

if tree_has_patches; then
    echo "==> Already applied, skipping: ${PATCHES[*]##*/}"
else
    [[ -z "$(git status --porcelain --untracked-files=no)" ]] ||
        die "$KERNEL_DIR has local changes (for example the patches of a build with another KERNEL_BASELINES value); reset it to $TAG first (git reset --hard $TAG; git clean -f mm/vmscan.h)"
    # The core patch adds mm/vmscan.h. A reset to the tag keeps that
    # untracked file, and git apply then stops on it. A HEAD that tracks the
    # file is fine: the checkout of the tag removes it.
    if [[ -e mm/vmscan.h ]] && ! git ls-files --error-unmatch mm/vmscan.h >/dev/null 2>&1; then
        die "$KERNEL_DIR/mm/vmscan.h is left from an earlier apply of the patches; remove it (git clean -f mm/vmscan.h) and run the script again"
    fi
    git checkout --quiet --detach "$TAG"
    for p in "${PATCHES[@]}"; do
        echo "==> Applying $(basename "$p") ..."
        git apply --check "$p"
        git apply "$p"
    done
fi

# ---------------------------------------------------------------------------
# 3. Configuration
# ---------------------------------------------------------------------------
echo "==> Configuring from $KERNEL_CONFIG ..."
cp -f "$KERNEL_CONFIG" .config
# Options that the patch, the modules and ../env need. The default config
# already has all of them. They matter when KERNEL_CONFIG points to another
# config. CONFIG_LRU_GEN must stay off: the per-cgroup demotion thread scans
# the classic active/inactive LRU lists. Module compression stays off because
# Linux 6.3.0 can fault when it loads a compressed module without a signature.
./scripts/config --file .config \
    --enable MMCTL \
    --disable LRU_GEN \
    --enable NUMA_BALANCING \
    --enable MEMCG \
    --enable X86_CPU_RESCTRL \
    --enable KPROBES \
    --enable KALLSYMS_ALL \
    --enable INTEL_UNCORE_FREQ_CONTROL \
    --enable MODULES \
    --enable MODULE_UNLOAD \
    --disable MODULE_SIG \
    --enable MODULE_COMPRESS_NONE \
    --disable MODULE_COMPRESS_GZIP \
    --disable MODULE_COMPRESS_XZ \
    --disable MODULE_COMPRESS_ZSTD \
    --set-str SYSTEM_TRUSTED_KEYS "" \
    --set-str SYSTEM_REVOCATION_KEYS "" \
    --disable LOCALVERSION_AUTO \
    --set-str LOCALVERSION ""
make olddefconfig >> "$LOGF" 2>&1

require_config 'CONFIG_MMCTL=y'
require_config '# CONFIG_LRU_GEN is not set'
require_config 'CONFIG_NUMA_BALANCING=y'
require_config 'CONFIG_MEMCG=y'
require_config 'CONFIG_MIGRATION=y'
require_config 'CONFIG_X86_CPU_RESCTRL=y'
require_config 'CONFIG_KPROBES=y'
require_config 'CONFIG_KALLSYMS_ALL=y'
require_config 'CONFIG_INTEL_UNCORE_FREQ_CONTROL=y'
require_config 'CONFIG_MODULE_UNLOAD=y'
require_config 'CONFIG_MODULE_COMPRESS_NONE=y'

KREL_EXPECTED="6.3.0${KERNEL_LOCALVERSION}"
KREL="$(make -s kernelrelease LOCALVERSION="$KERNEL_LOCALVERSION")"
[[ "$KREL" == "$KREL_EXPECTED" ]] ||
    die "kernel release is '$KREL', expected '$KREL_EXPECTED'"

# ---------------------------------------------------------------------------
# 4. Build the Debian packages
# ---------------------------------------------------------------------------
echo "==> Building $KREL packages with $JOBS jobs (log: $LOGF) ..."
make -j"$JOBS" bindeb-pkg \
    LOCALVERSION="$KERNEL_LOCALVERSION" \
    KDEB_PKGVERSION="$KERNEL_PKGVERSION" >> "$LOGF" 2>&1

IMAGE_DEB="$OUT_DIR/linux-image-${KREL}_${KERNEL_PKGVERSION}_amd64.deb"
HEADERS_DEB="$OUT_DIR/linux-headers-${KREL}_${KERNEL_PKGVERSION}_amd64.deb"
[[ -f "$IMAGE_DEB" ]] || die "package not found: $IMAGE_DEB"
[[ -f "$HEADERS_DEB" ]] || die "package not found: $HEADERS_DEB"

cat <<EOF

==> Build complete: kernel release $KREL
    Patches: ${PATCHES[*]##*/}
    $IMAGE_DEB
    $HEADERS_DEB

Next steps (details in README.md):
  1. sudo dpkg -i $IMAGE_DEB $HEADERS_DEB
  2. In /etc/default/grub, make $KREL the default kernel and add these
     boot arguments to GRUB_CMDLINE_LINUX:
       rdt=cmt,mbmtotal,mbmlocal,l3cat systemd.unified_cgroup_hierarchy=yes mitigations=off
     Then run 'sudo update-grub' and reboot.
  3. After the reboot, check that 'uname -r' shows $KREL. Then build and
     load the modules: ./build_modules.sh, then
     'sudo insmod tierinit/tierinit.ko' and
     'sudo insmod kswapdrst/kswapdrst.ko'.
EOF
