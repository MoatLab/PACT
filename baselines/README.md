# Tiering systems

This directory contains runtime configuration scripts and run scripts for baseline tiered memory management systems used in PACT evaluation.

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

Each baseline lives in its own subdirectory with a kernel patch, a
`compile.sh`, and a README describing the exact kernel tag and build steps:

| System | Directory | Kernel base |
|--------|-----------|-------------|
| TPP | [`tpp/`](tpp/) | v5.15 |
| NBT | [`nbt/`](nbt/) | v5.18 |
| Nomad | [`nomad/`](nomad/) | 5.13-rc6 |
| Colloid-tpp | [`colloid-tpp/`](colloid-tpp/) | v6.3 |
| Memtis | [`memtis/`](memtis/) | v5.15.19 |
| Soar/Alto | [`soar-alto/`](soar-alto/) | v6.3 |

## Notes

* `Colloid` in our work refers to `Colloid-tpp`: we reproduce Colloid on the
  TPP path at Linux v6.3. (The original Colloid paper evaluates on NBT, NUMA
  Balancing Tiering.)
* These directories ship the kernel patches and build scripts. They do not
  include per-system workload run wrappers; boot the patched kernel and drive
  workloads with your own launcher (PACT's own runner is in [`../`](../)).

## Older-kernel build compatibility

TPP, Nomad and Memtis builds also apply
[`common/libsubcmd-realloc.patch`](common/libsubcmd-realloc.patch), upstream Linux
commit `52a9dab6d892763b2a8334a568bd4e2c1a6fde66`. It fixes the host-tool
`realloc(ptr, 0)` use-after-free diagnosed by GCC 12 and newer. The patch retains
its upstream authorship and the affected file's GPL-2.0 license. It does not
change kernel tiering policy. The helper skips the fix when already present
and stops if neither patch direction applies; it does not suppress compiler
warnings. NBT and the Linux 6.3 baselines already contain this fix.

Colloid-tpp and Soar/Alto also apply `common/colloid-build.patch`, which removes
an unused declaration in the tier-reset helper so `CONFIG_WERROR=y` builds
succeed. It leaves the helper's operations and tiering policy unchanged.
