# Colloid (SOSP '24)

Colloid tiering baseline: **Colloid-tpp**, Colloid built on the TPP path of
Linux v6.3, as in the Colloid paper. This directory builds a standalone kernel
without per-cgroup caps. The parent [README](../README.md) names the second
way to run Colloid, on the Tierce kernel.

## Contents
- `colloid-skx.patch`, `colloid-skx-alto.patch` - kernel patches applied on
  top of Linux `v6.3` (Skylake-X target).
- `compile.sh` - checks out `v6.3` in a sibling `linux/` git tree, applies
  three patches (`colloid-skx.patch`, `colloid-skx-alto.patch`, and
  [`../common/colloid-build.patch`](../common/colloid-build.patch)), builds,
  installs modules, runs `make install` and `update-grub`.
- `colloid-mon/` - Colloid monitoring kernel module.
- `tierinit/` - tier-initialization helper (CPU on/offline).
- `kswapdrst/` - kswapd reset helper.

## Build
`compile.sh` expects a Linux git checkout in `./linux` (not bundled). Clone it
first, then build (kernel install + `update-grub` need root):

```bash
cd colloid-tpp
git clone https://github.com/torvalds/linux.git linux   # v6.3 is a mainline tag
sudo ./compile.sh
```

The script runs: `git checkout v6.3` -> `git apply colloid-skx.patch` ->
`git apply colloid-skx-alto.patch` -> `git apply ../common/colloid-build.patch`
-> `make olddefconfig` -> `make -j` -> `make modules_install` ->
`make install` -> `update-grub`.

This kernel is the same kernel as the one of [`../soar-alto/`](../soar-alto/):
the two directories hold the same patches. The sysctls select the system.
For Colloid, set `kernel.numa_balancing=6` (memory tiering mode `0x2` plus
the Colloid bit `0x4`) and load `colloid-mon`. Soar/Alto uses the sysctls
that `colloid-skx-alto.patch` adds (see [`../soar-alto/`](../soar-alto/)).

Build the helper tools (`colloid-mon/`, `tierinit/`, `kswapdrst/`) with the
`make` target in each subdirectory; see their own READMEs.

## Running a workload
Kernel + helpers only; no Colloid-specific top-level run wrapper is shipped.
Boot into the patched kernel, start the monitor/helpers, and drive a workload
with your own launcher. See [`../README.md`](../README.md#cpu-use) (CPU use)
for the CPUs the helpers use.
