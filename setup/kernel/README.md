# Kernel setup (Linux 6.3)

PACT and Tierce run on one kernel: Linux 6.3 with the patch in this folder.
Each workload runs in its own cgroup, and that cgroup has a cap on the
workload's fast-tier (node 0) memory. Linux 6.3 has no per-node memory limit
for a cgroup and no per-cgroup demotion. The patch adds both. This folder also
has the two out-of-tree kernel modules that the tiering subsystem needs.

## Contents

| File | Purpose |
|------|---------|
| `tierce-v6.3.patch` | Patch against the Linux tag `v6.3`: 11 files, +1602 / -164 lines |
| `baselines-v6.3.patch` | Optional add-on on top of `tierce-v6.3.patch` for the NUMA-balancing and Colloid baselines: 9 files, +155 / -6 lines. Not applied by default (see [Optional add-on](#optional-add-on-for-the-numa-balancing-and-colloid-baselines)) |
| `config-6.3.0-tierce` | Kernel configuration (x86_64), with debug information turned off |
| `setup_kernel.sh` | Gets `v6.3`, applies the patch, configures, and builds `.deb` packages |
| `tierinit/` | Module: puts node 1 in a lower memory tier than node 0 |
| `kswapdrst/` | Module: resets the kswapd failure count of node 0 every 5 s |
| `build_modules.sh` | Builds both modules |

## What the patch adds

### Build options

- `CONFIG_MMCTL` ("Enable cgroup based memory capacity control", depends on
  `MEMCG` and `NUMA`). It must be `y`.
- `CONFIG_LRU_GEN` must be off. The per-cgroup demotion thread scans the
  active and inactive LRU lists and stops with `BUG()` if the
  multi-generational LRU is enabled.

### cgroup v2 files

The memory controller of every non-root cgroup gets these files:

| File | Values | Effect |
|------|--------|--------|
| `memory.mmctl_enabled` | `enabled`, `disabled` (default) | Turns on the per-node limits of this cgroup. |
| `memory.max_at_node<N>` | bytes (suffixes `K`, `M`, `G` accepted) or `max` (default: no limit) | Limit on the pages of this cgroup on node `N`. There is one file per memory node. |
| `memory.demotion` | `enabled`, `disabled` (default) | Per-cgroup demotion. |
| `memory.promotion_counter` | read-only: `cgroup_promotion:<n>` | Pages that NUMA balancing promoted for this cgroup. |

The usage of a cgroup on a node is the number of its pages on the LRU lists
of that node. A page counts only if it is charged to the cgroup. A process
must therefore join the cgroup before it allocates memory (for example
`echo $$ > /sys/fs/cgroup/<name>/cgroup.procs && exec <command>`).

When `memory.mmctl_enabled` and `memory.demotion` are both `enabled` and
`memory.max_at_node<N>` holds a limit, the kernel starts a kernel thread
named `kdemotiond_<cgroup>_<N>` on the CPUs of node `N`. The thread wakes
up when a promotion into node `N` is refused because of the limit. It then
demotes the coldest pages of the cgroup from node `N` to the next lower
tier. Writing `max` to the limit, `disabled` to either switch, or removing
the cgroup stops the thread.

Write `disabled` to `memory.demotion` before you remove a cgroup, so the
thread stops at a known point. A write that cannot start the thread fails
with an error. Without the thread, the cgroup refuses every promotion into
a full limit. `pgrep -l kdemotiond` lists the threads (names cut to 15
characters).

### Statistics

`memory.stat` of each cgroup gets `pgdemote_kswapd`, `pgdemote_direct` and
`pgdemote_khugepaged` (pages of this cgroup demoted by kswapd or by
`kdemotiond`, by direct reclaim, and by khugepaged) and their sum `pgdemote`.
The runtime reads these counters.

### Placement and migration in a cgroup with a limit

- **Page faults.** Let `N` be the node of the CPU that takes the fault.
  While the cgroup is below its limit on `N`, a new page of a mapping comes
  from `N`. At the limit, it comes from the next lower tier. Pin workloads
  to CPUs of node 0.
- **`move_pages(2)`.** The kernel does not move a page into node `N` when
  the free space of its cgroup under the limit is too low. The status of
  that page is `-ENOMEM`, and the kernel wakes `kdemotiond`.
- **NUMA balancing.** The kernel refuses a NUMA-balancing promotion into a
  full limit. PACT and Tierce promote with `move_pages(2)` and run with
  `kernel.numa_balancing=0`.
- **Demotion.** Per-cgroup demotion does not depend on the global switch
  `/sys/kernel/mm/numa/demotion_enabled`.

## Build

The build produces Debian packages. Install the build dependencies first, for
example on Ubuntu:

```bash
sudo apt-get install git build-essential bc bison flex libelf-dev libssl-dev \
    debhelper rsync kmod cpio fakeroot dpkg-dev
```

Then run:

```bash
./setup_kernel.sh
```

`setup_kernel.sh` clones `v6.3` into `./linux` (or reuses that tree), applies
`tierce-v6.3.patch`, copies `config-6.3.0-tierce` to `.config`, sets the
options that the patch and the modules need (for example `CONFIG_MMCTL=y`,
`CONFIG_KPROBES=y`, `CONFIG_X86_CPU_RESCTRL=y`), runs `make olddefconfig`,
checks these options, and runs
`make bindeb-pkg LOCALVERSION=-tierce KDEB_PKGVERSION=1`. The script installs
nothing. The packages appear next to `./linux`:

- `linux-image-6.3.0-tierce_1_amd64.deb`
- `linux-headers-6.3.0-tierce_1_amd64.deb`

Optional environment variables: `KERNEL_DIR` (kernel tree), `KERNEL_CONFIG`
(another base configuration), `KERNEL_LOCALVERSION` (release suffix, default
`-tierce`, or `-tierce-baselines` with `KERNEL_BASELINES=1`),
`KERNEL_PKGVERSION` (package version, default `1`),
`KERNEL_BASELINES` (`1` also applies `baselines-v6.3.patch`, default `0`),
`JOBS` (parallel jobs, default: all CPUs). The build log is `build.log` next
to the packages.

## Optional add-on for the NUMA-balancing and Colloid baselines

`baselines-v6.3.patch` applies on top of `tierce-v6.3.patch`. With it, the
same kernel with per-cgroup caps also runs two baselines: upstream NUMA
balancing in memory tiering mode (`kernel.numa_balancing=2`) and Colloid
(`kernel.numa_balancing=6` with the Colloid monitor module). The default
build does not apply it. To build one kernel with both patches:

```bash
KERNEL_BASELINES=1 ./setup_kernel.sh
```

This build has the release name `6.3.0-tierce-baselines` (packages
`linux-image-6.3.0-tierce-baselines_1_amd64.deb` and
`linux-headers-6.3.0-tierce-baselines_1_amd64.deb`), so `uname -r` tells it
apart from the default build. Use that name in the GRUB entry below. To build
the other variant in the same tree, reset the tree first
(`git reset --hard v6.3` and `git clean -f mm/vmscan.h` in `./linux`): the
core patch adds the file `mm/vmscan.h`, and a reset keeps it.

With the add-on, `mm/memory-tiers.c` differs from `v6.3`. The modules in
this folder build against both kernels. To see whether a running kernel has
the add-on, run `grep -w colloid_nid_of_interest /proc/kallsyms`: it prints a
line only with the add-on.

## Install

```bash
sudo dpkg -i linux-image-6.3.0-tierce_1_amd64.deb linux-headers-6.3.0-tierce_1_amd64.deb
```

### GRUB

Make the new kernel the default and add three boot arguments. In
`/etc/default/grub`, set:

```
GRUB_DEFAULT="Advanced options for Ubuntu>Ubuntu, with Linux 6.3.0-tierce"
GRUB_CMDLINE_LINUX="<existing arguments> rdt=cmt,mbmtotal,mbmlocal,l3cat systemd.unified_cgroup_hierarchy=yes mitigations=off"
```

The menu entry names above are the names on Ubuntu. On another distribution,
take the names from `/boot/grub/grub.cfg`. Then run:

```bash
sudo update-grub
sudo reboot
```

Why each boot argument is necessary:

- `rdt=cmt,mbmtotal,mbmlocal,l3cat`: on Skylake-SP processors, Linux 6.3
  turns off L3 cache allocation (CAT) by default, and on some steppings also
  cache and memory bandwidth monitoring. Tierce's LLC way control needs CAT.
  This argument turns these features on.
- `systemd.unified_cgroup_hierarchy=yes`: the cgroup files of the patch exist
  only in cgroup v2, and every workload runs in its own cgroup.
- `mitigations=off`: turns off the CPU vulnerability mitigations of the
  kernel, which avoids their performance cost. Use it only on machines that
  do not run untrusted code.

After the reboot, check:

```bash
uname -r                    # 6.3.0-tierce (6.3.0-tierce-baselines with the add-on)
cat /proc/cmdline           # contains the three arguments above
sudo dmesg | grep resctrl   # "L3 allocation detected"
# The memory controller must be on in /sys/fs/cgroup/cgroup.subtree_control
# (systemd turns it on).
sudo mkdir /sys/fs/cgroup/test && ls /sys/fs/cgroup/test/memory.max_at_node0
sudo rmdir /sys/fs/cgroup/test
```

## Modules

Build the modules after the new kernel boots, with its headers package
installed. Each Makefile builds against `/lib/modules/$(uname -r)/build`. To
build against another kernel tree, add `KDIR=<kernel build directory>` to the
`make` command.

```bash
./build_modules.sh
```

or build each module separately (from `setup/kernel/`):

```bash
make -C tierinit          # -> tierinit/tierinit.ko
make -C kswapdrst         # -> kswapdrst/kswapdrst.ko
```

Load both modules once after each boot, before the first run:

```bash
sudo insmod tierinit/tierinit.ko
sudo insmod kswapdrst/kswapdrst.ko
lsmod | grep -E 'tierinit|kswapdrst'
cat /sys/devices/virtual/memory_tiering/memory_tier4/nodelist    # 0
cat /sys/devices/virtual/memory_tiering/memory_tier45/nodelist   # 1
sudo dmesg | grep -E 'tierinit|kswapdrst'
```

### `tierinit`: register the slow tier and the demotion targets

Linux 6.3 puts the far-memory NUMA node (node 1) in the same memory tier as
node 0, so it builds no demotion path from the fast tier (node 0) to the slow
tier. `tierinit` fixes this at load time. It gives node 1 an abstract
distance ten times that of DRAM, which puts node 1 in memory tier 45, below
node 0 in tier 4. It then rebuilds the demotion targets and sets the top-tier
boundary so that node 1 is not a top-tier node, even if node 1 has CPUs. After
that, `next_demotion_node(0)` is 1. `tierinit` resolves the internal
`memory-tiers` symbols with `kprobes`, so the kernel needs `CONFIG_KPROBES=y`
and `CONFIG_KALLSYMS_ALL=y` (the configuration in this folder has both).
`tierinit` assumes two memory nodes: node 0 is the fast tier and node 1 the
slow tier. Leave it loaded until the next reboot.

### `kswapdrst`: keep demotion alive under sustained pressure

The kernel's reclaim and demotion daemon (`kswapd`) increments a per-node
`kswapd_failures` counter and stops after repeated reclaim failures, which
stalls demotion during long tiering runs. `kswapdrst` resets
`kswapd_failures` of node 0 every 5 s from a high-priority workqueue on CPU 0,
so demotion continues. (Derived from the Colloid memory-tiering artifact.) Do
not run workloads on CPU 0 (`CORE` in `kswapdrst.c`). Remove the module with
`sudo rmmod kswapdrst`.

## Next step

Prepare the host with [`../env/`](../env/).
