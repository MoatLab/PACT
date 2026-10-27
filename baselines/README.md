# Tiering systems

This directory contains kernel patches and build scripts for the baseline tiered memory management systems of the PACT evaluation.

## Supported systems

| System | Description |
|--------|-------------|
| **TPP** | Transparent Page Placement - ASPLOS'23 |
| **NBT** | Linux NUMA-Balancing-Tiering (v5.18) |
| **Nomad** | Non-exclusive Memory Tiering - OSDI'24 |
| **Colloid** | Access Latency is Key! - SOSP'24 |
| **Memtis** | Hardware-counter-driven tiered memory using PEBS for per-page access profiling and frequency-based migration - SOSP'23. [memtis/](memtis/) |
| **Soar/Alto** | AOL-based Layered Tiering Orchestration and Static Object Allocation based on Ranking - OSDI'25. [Paper](https://www.usenix.org/conference/osdi25/presentation/liu) · [Code](https://github.com/MoatLab/SoarAlto) |

## Per-system directories

Each baseline lives in its own subdirectory with a kernel patch, a build
script (`compile.sh`, or `setup_memtis.sh` for Memtis), and a README
describing the exact kernel tag and build steps:

| System | Directory | Kernel base |
|--------|-----------|-------------|
| TPP | [`tpp/`](tpp/) | v5.15 |
| NBT | [`nbt/`](nbt/) | v5.18 |
| Nomad | [`nomad/`](nomad/) | v5.13-rc6 |
| Colloid-tpp | [`colloid-tpp/`](colloid-tpp/) | v6.3 |
| Memtis | [`memtis/`](memtis/) | v5.15.19 |
| Soar/Alto | [`soar-alto/`](soar-alto/) | v6.3 |

## Notes

* `Colloid-tpp` is Colloid on the TPP path of Linux v6.3. The Colloid
  paper implements Colloid on top of TPP in Linux v6.3, and also integrates
  it with HeMem and MEMTIS. This directory holds only the TPP version, as a
  standalone kernel without per-cgroup caps.
* To run Colloid on the same kernel as Tierce and PACT, with per-cgroup
  fast-tier caps, build the add-on in [`../setup/kernel/`](../setup/kernel/)
  (`KERNEL_BASELINES=1`) and load `colloid-mon` from
  [`colloid-tpp/colloid-mon/`](colloid-tpp/colloid-mon/). Load
  `colloid-mon` after the last Tierce or PACT run of the boot: a runtime run
  reprograms the CHA counters that `colloid-mon` sets up at load. Unload it
  only after `kernel.numa_balancing` is 0. Its counter setup is for
  Skylake-SP.
* These directories ship the kernel patches and build scripts. They do not
  include per-system workload run wrappers; boot the patched kernel and drive
  workloads with your own launcher (PACT's own runner is in [`../run/`](../run/)).

## CPU use

The Colloid helpers use fixed CPUs. Keep workloads off them in your
launcher. To change a CPU, edit the source and rebuild.

| Helper | CPU | Where it is set |
|---|---|---|
| `colloid-mon` | 1 | `CORE_MON` in `colloid-mon.c` |
| `kswapdrst` (Colloid) | 8 | `CORE` in `kswapdrst.c` |
| `tierinit` `offline_cpus.sh` | takes a fixed CPU list offline | the loop in `offline_cpus.sh` and `online_cpus.sh`. Edit both to the slow-tier node of your host (`numactl -H`). |

With `run.sh`, the runtime and workloads use the CPUs in
[`../run/README.md`](../run/README.md#cpu-layout) (CPU layout).

## Build patches

The build scripts of TPP, Nomad and Memtis also apply
[`common/libsubcmd-realloc.patch`](common/libsubcmd-realloc.patch), upstream
Linux commit `52a9dab6d892763b2a8334a568bd4e2c1a6fde66`, so that GCC 12 and
newer build the kernel host tools. The patch retains its upstream authorship
and the affected file's GPL-2.0 license. Colloid-tpp and Soar/Alto also
apply `common/colloid-build.patch`, so that `CONFIG_WERROR=y` builds
succeed.
