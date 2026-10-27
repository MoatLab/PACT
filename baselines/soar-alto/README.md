# Soar/Alto (OSDI '25)

Soar/Alto tiering baseline.
[Paper](https://www.usenix.org/conference/osdi25/presentation/liu) ·
[Full code](https://github.com/MoatLab/SoarAlto).

This directory holds the kernel patches used to reproduce the Soar/Alto
baseline in the PACT evaluation. For the complete Soar/Alto system and its
own run scripts, use the upstream repository linked above.

## Contents
- `colloid-skx.patch`, `colloid-skx-alto.patch` - kernel patches applied on
  top of Linux `v6.3` (Skylake-X target).
- `compile.sh` - checks out `v6.3` in a sibling `linux/` git tree, applies
  three patches (`colloid-skx.patch`, `colloid-skx-alto.patch`, and
  [`../common/colloid-build.patch`](../common/colloid-build.patch)), builds,
  installs modules, runs `make install` and `update-grub`.

> The patch files here are identical to those under
> [`../colloid-tpp/`](../colloid-tpp/): both baselines share the same
> v6.3 Skylake-X kernel base. The directories are kept separate so each
> baseline has its own build. The sysctls select the system: Soar/Alto uses
> the sysctls that `colloid-skx-alto.patch` adds
> (`kernel.numa_balancing_disable_migrate`,
> `kernel.numa_balancing_page_promote_scale`,
> `kernel.numa_balancing_reset_kswapd_failures`,
> `kernel.numa_balancing_disable_pte`, `kernel.numa_balancing_pte_scale`).
> For their values and the NUMA balancing mode, see the upstream repository.
> Colloid uses
> `kernel.numa_balancing=6` with the `colloid-mon` module (see
> [`../colloid-tpp/`](../colloid-tpp/)).

## Build
`compile.sh` expects a Linux git checkout in `./linux` (not bundled). Clone it
first, then build (kernel install + `update-grub` need root):

```bash
cd soar-alto
git clone https://github.com/torvalds/linux.git linux   # v6.3 is a mainline tag
sudo ./compile.sh
```

The script runs: `git checkout v6.3` -> `git apply colloid-skx.patch` ->
`git apply colloid-skx-alto.patch` -> `git apply ../common/colloid-build.patch`
-> `make olddefconfig` -> `make -j` -> `make modules_install` ->
`make install` -> `update-grub`.

## Running a workload
Kernel patches only; refer to the upstream
[SoarAlto](https://github.com/MoatLab/SoarAlto) repository for the runtime and
its run scripts.
