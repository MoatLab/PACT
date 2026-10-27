# PACT: A Criticality-First Design for Tiered Memory

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)

This repository holds the implementation of two papers:

- **PACT: A Criticality-First Design for Tiered Memory** (ASPLOS 2026), run
  with `--mode pact`.
- **Tierce: Observability-Driven Tiered Memory Management for Colocated
  Workloads** (SOSP 2026), run with `--mode tierce`.

Both modes share one runtime and one patched Linux 6.3 kernel. The overview
below is PACT's. Tierce is described in
[Two modes: PACT and Tierce](#two-modes-pact-and-tierce).

**Overview.** Our **key insight** is that access frequency is a poor proxy for
performance: a page's true cost is its *criticality* - how much it stalls the
CPU - which depends on access pattern, memory-level parallelism (MLP), and tier
latency, not on how often it is touched. PACT therefore makes online,
page-granular performance criticality a first-class placement signal. Its **key
technical contributions** are: (1) **Per-page Access Criticality (PAC)**, a
metric that attributes per-tier CPU stall time to individual pages online,
estimated from an analytical model over just **four standard CPU performance
counters** with per-tier MLP decomposition from hardware queue-occupancy
counters; and (2) a **PAC-centric promotion policy**, *adaptive promotion*,
which uses statistical sampling with adaptive binning to identify high-PAC
pages without global sorting or hand-tuned thresholds. PACT realizes both as a
runtime that places pages by their PAC, transparently and without application
changes.

> **Note:** Compared to the PACT system presented in the paper at the ASPLOS'26
> conference, we have updated PACT with several changes, most
> notably, we redesigned the multi-threaded sampling and migration into a
> coroutine-based scheme, and we added batched migration of memory pages. We
> also moved from a custom Linux 5.15 kernel to the tiering support of Linux
> 6.3, with a patch that adds a per-cgroup fast-tier cap (see
> [`setup/kernel/`](setup/kernel/)).

## How PACT works

PACT is a **standalone userspace runtime**. It attaches to an unmodified,
already-running workload (no recompilation or library changes) and ranks the
workload's pages by **performance criticality (PAC)**: how many CPU stall cycles
each page is responsible for, *independent of how often it is accessed*. It then
drives the kernel's tiering interface to keep the most critical pages in fast
memory. Everything below runs on each ~20 ms cycle:

<p align="center">
  <img src="pact-arch.svg" alt="PACT architecture: per-cycle sample, score (PAC), bin, and migrate pipeline running as a single-core coroutine loop, with adaptive promotion of critical pages to the fast tier" width="760">
</p>

<p align="center"><sub>PACT's per-cycle pipeline: sample → score (PAC) → bin → migrate, on a single-core coroutine loop.</sub></p>

1. **Sample** - PEBS records the pages the workload loads from the slow tier;
   CHA/uncore counters measure memory-level parallelism (MLP).
2. **Score** - each page's PAC ≈ the CPU stall cycles attributable to it,
   computed from per-tier LLC misses weighted by tier latency and divided by
   MLP: `LLC-stalls = k · LLC-misses / MLP`. High PAC means the page stalls the
   CPU, *independent of how often it is accessed*.
3. **Bin** - pages are grouped into PAC bins whose width self-tunes
   (Freedman-Diaconis) to the live PAC distribution, keeping promotion
   selective even under skewed (power-law) workloads.
4. **Migrate** - pages in the highest-criticality bin are promoted to the fast
   tier; cold pages fall back to the slow tier.

Demotion on the patched kernel of this repository: each workload's cgroup has
a cap on its fast-tier memory. When a promotion needs room under the cap, the
kernel demotes cold pages of the same cgroup, so demotions follow promotions.
In both modes the demotions run in the cgroup's kernel thread `kdemotiond`.

## Two modes: PACT and Tierce

The runtime has two modes in one binary. `--mode pact` (the default) runs
PACT. `--mode tierce` runs Tierce, the design of the SOSP 2026 paper
*"Tierce: Observability-Driven Tiered Memory Management for Colocated
Workloads."* Both modes manage up to 16 colocated workloads, and both run on
the same kernel. Each workload runs in its own cgroup with a cap on its
fast-tier memory, and the kernel's per-cgroup demotion thread (`kdemotiond`)
enforces each cap.

| | PACT mode | Tierce mode |
|---|---|---|
| Scope | One global instance over all workloads | One domain per workload |
| PAC table, threshold, promotion queue | One of each, shared by all workloads | One of each per domain |
| MLP | One global slow-tier MLP: all CHAs, TID filter off, one ratio of sums | Per-workload MLP (wMLP): one paired CHA per core with the TID filter on, the mean of the per-core MLP over the domain's cores |
| Promotion admission | Top bin of the adaptive PAC bins | 0.90 percentile of the domain's PAC values |
| Migration | One queue, drained on each wake | Round robin over the domain queues |
| Demotion | `kdemotiond` enforces each cap | `kdemotiond` enforces each cap |
| Optional parts | none | LDLAT latency weighting, LDLAT-Dyn, LLC way control (CAT) |
| Default PEBS period | 400 | 100 (LDLAT: 5) |

**PACT mode** runs the PACT pipeline over all workloads in one global
instance. The workloads share one PAC table (keyed by workload and page), one
score distribution, one promotion threshold and one promotion queue. The
global MLP counts the slow-tier TOR requests of every core of the socket, so
the TID filter is off. The LLC misses of the stall formula are the sum over
the threads of all workloads.

**Tierce mode** isolates each workload (a *domain*) along the whole pipeline:

1. **wMLP.** The MLP of each core comes from the CHA paired with that
   core, with the TID filter set to that core. The domain's MLP is the
   mean over its cores.
2. **Domain PAC.** Each 20 ms epoch, the base stall per sample is
   `S_w = k · LLCmiss_w / (MLP_w · N_w · P)`.
3. **Admission.** A page enters the domain's promotion queue when its PAC
   is at or above the 0.90 percentile of the domain's PAC values.
4. **Migration.** One migration worker visits the domain queues round
   robin.
5. **LDLAT (optional, `--use-ldlat`).** Each slow-tier sample scales the
   domain's stall by its load latency: `S_p = S_w · l_p / k_slow`. A gate
   drops the samples below 700 cycles. `--ldlat-gate 350` turns the gate
   off.
6. **LDLAT-Dyn (optional, `--ldlat-dyn`).** The gate moves between 350
   and 700 cycles with the reclaim pressure of the domains.
7. **LLC way control (optional, `--use-cat`).** The first `--workload` is
   the foreground (FG). It keeps `W_min..W_max` LLC ways through Intel CAT
   (`resctrl`). The other workloads share the other ways.

The [runner](run/README.md) has one PACT arm and five Tierce arms: `wmlp`
(wMLP only), `wmlp-ldlat` (wMLP + LDLAT, gate 350), `wmlp-cat` (static
split), `tierce` (wMLP + LDLAT with gate 700 + static split) and
`wmlp-dcat` (dynamic split).

## Repository layout

```
PACT/
├── src/             # the runtime (C source + Makefile), PACT and Tierce modes
├── setup/           # host bring-up - do this first
│   ├── kernel/      #   build/install the patched Linux 6.3 + the tierinit & kswapdrst modules
│   ├── env/         #   uncore-freq pinning, CXL/NUMA layout, governor, THP/KSM off
│   └── tools/       #   offline CHA TID discovery for new CPUs (Tierce mode)
├── run/             # run workloads under the runtime (run.sh, workloads.sh)
├── baselines/       # baseline tiering systems (TPP, NBT, Nomad, Colloid, Memtis, Soar/Alto)
└── modeling/        # PAC modeling scripts (Figure 2 of the PACT paper, ASPLOS'26)
```

**`src/` is the runtime - a standalone userspace process.** It performs
PEBS sampling, computes per-page performance criticality, and drives page
migration through the kernel's tiering interface entirely from user space. It
is **independent of the target workload**: it attaches to running workloads
and needs no source changes, recompilation, or library linking on the
workload's side. The `run/` scripts put each workload into its own capped
cgroup, start it, and start the `src/` runtime next to it on separate cores.

The top-level directories follow the order you use them: **`setup/` → build
`src/` → `run/`**. `baselines/` and `modeling/` are independent and only needed
to reproduce specific paper results.

## Requirements

- **Hardware:** an Intel server with a tiered memory layout - Skylake-X /
  Cascade Lake, Sapphire Rapids, or Emerald Rapids (see
  [Supported CPUs](#supported-cpus)).
  The remote NUMA node emulates a slower (CXL-like) tier. PEBS and CHA uncore
  counters are required for PAC sampling. LLC way control needs Intel CAT.
- **Kernel:** the patched **Linux 6.3** kernel `6.3.0-tierce` for both modes
  (built by [`setup/kernel/`](setup/kernel/)). The patch adds a per-cgroup
  fast-tier cap (`memory.max_at_node0`) and per-cgroup demotion
  (`kdemotiond`). Baselines use their own kernel versions - see
  [`baselines/`](baselines/).
- **Software:** `gcc`, `make`, `libnuma`, `numactl`, `vmtouch`, `gnuplot`
  (plotting), and `perf` (the PAC modeling scripts in [`modeling/`](modeling/)
  read counters via stock `perf stat`). The runtime samples the PMU directly
  through `perf_event_open` and needs no external `perf` binary.
- **Privileges:** kernel install, module loading, cgroup setup, and the
  environment-prep scripts require root.

> **Scope.** This is a single-node release. It reproduces the PAC modeling
> experiment end-to-end, builds the runtime and all baseline kernels, and
> runs the single-command workloads. The paper's Redis-YCSB result uses a
> two-node SSH client/server harness that is not part of this release.

## Supported CPUs

| CPU | LLC ways | Static split (FG / BG) |
|-----|----------|------------------------|
| Skylake-SP / Cascade Lake | 11 | 8 / 3 |
| Sapphire Rapids / Emerald Rapids | 20 | 15 / 5 |

The runtime takes these values from
[`src/pmu-platform.c`](src/pmu-platform.c) or computes them at start:

- **CHA and TID tables (Tierce mode).** The compiled tables hold the CPU
  numbering of a two-socket Xeon Silver 4114 (Skylake-SP, 10 cores per
  socket) and a Xeon Gold 6530 (Emerald Rapids). For another core count or
  numbering, measure a new TID table with [`setup/tools/`](setup/tools/),
  and supply a new CHA-to-core table yourself.
- **MLP bounds:** 1..16 on Skylake-SP, 1..20 on Sapphire/Emerald Rapids.
- **PAC slow-tier constant `k_cxl`:** 771 on both.
- **LLC way range:** `W_min = (ways+2)/4`, `W_max = 3*ways/4`. The static
  split gives the FG `W_max` ways. The runtime prints the range at start.
- **`l2lat` CAT signal:** needs more than 4 general-purpose counters per
  CPU (CPUID leaf 0xA). Skylake-SP has 4, so the runtime refuses `l2lat`
  there.
- An LLC split needs a BG workload. The runner refuses the CAT arms with
  one workload.

## Getting started

Follow these steps top to bottom on a fresh machine. Start each code
block from the repository root.

The scripts below set up and run the evaluation on a two-socket Intel
Skylake-SP host (CloudLab c220g5), with node 1 used as a CPU-less slow tier.
On other hardware, read each script before you run it: CPU lists, NUMA nodes,
uncore frequencies, kernel paths and dataset paths are set for that host and
may need changes.

### 1. Set up the host - [`setup/`](setup/)

```bash
# 1a. Build the patched Linux 6.3 packages and install them
#     (see setup/kernel/README.md for the options and prerequisites).
cd setup/kernel
./setup_kernel.sh            # clone v6.3 + apply the patch + configure + build .deb packages
sudo dpkg -i linux-image-6.3.0-tierce_1_amd64.deb linux-headers-6.3.0-tierce_1_amd64.deb

# 1b. In /etc/default/grub, make 6.3.0-tierce the default kernel and add the
#     boot arguments below to GRUB_CMDLINE_LINUX (see setup/kernel/README.md):
#       rdt=cmt,mbmtotal,mbmlocal,l3cat systemd.unified_cgroup_hierarchy=yes mitigations=off
sudo update-grub && sudo reboot

# 1c. After the reboot, from the repository root, build the modules.
cd setup/kernel
./build_modules.sh           # builds tierinit.ko and kswapdrst.ko

# 1d. Prepare the machine (uncore pinning, CXL/NUMA layout, governor,
#     turbo/THP/KSM/NUMA-balancing off). Requires root. Re-run after
#     EVERY reboot.
cd ../env
sudo ./prepare_environment.sh
```

> **Required kernel modules.** **`tierinit`** registers the far NUMA node as
> a slow tier and sets up the demotion targets (without it there is no tier
> structure to migrate across). **`kswapdrst`** keeps `kswapd` from
> permanently backing off, so demotion does not stall. `run.sh` loads both
> and refuses to start if either is missing. Leave `tierinit` loaded until the
> next reboot: unloading it may lock up the CPUs. See
> [`setup/kernel/README.md`](setup/kernel/README.md).

The fast/slow split comes from a cap per workload, not from the boot setup.
See [`setup/README.md`](setup/README.md) for the cap sizes and for the
uncore-frequency tuning of step 1d.

### 2. Build the runtime - [`src/`](src/)

The runtime is a self-contained userspace program with no workload-side
dependencies.

```bash
cd src
make                 # produces ./pact
make check-format    # optional: clang-format style check (needs clang-format >= 16)
```

See [`src/CODING_STYLE.md`](src/CODING_STYLE.md) for style conventions.

### 3. Run a workload - [`run/`](run/)

```bash
cd run
# Point the dataset paths in workloads.sh at your GAPBS / SPEC / Silo installs:
#   export GAPBS_DIR=/path/to/gapbs   SPEC_DIR=/path/to/cpu2017  ...
make -C tools                                        # restart library for w_649_4t_long

# PACT mode: one workload with a 1:1 fast:slow split.
sudo -E ./run.sh bc_kron_8t

# Tierce mode, arm tierce (wMLP, LDLAT with gate 700, static LLC split),
# bc_kron_4t (FG) with w_649_4t_long (BG).
sudo -E ./run.sh --mode tierce --arm tierce bc_kron_4t w_649_4t_long

grep '^check_' results/*/*/summary.txt               # no check may read FAIL
```

`run.sh` loads the kernel modules from `setup/kernel/`, checks the host,
creates one cgroup per workload with the cap `RSS x 0.5` in
`memory.max_at_node0`, starts each workload inside its cgroup, and starts the
runtime with one `--workload PID CPUS NAME CGROUP` per workload. See
[`run/README.md`](run/README.md) for the arms, the options, the outputs, and
the list of predefined workloads.

#### Running the runtime directly

The runtime **attaches to already-running, CPU-pinned workloads** by PID and
runs as root (it needs PMU/PEBS access). Each workload must already run in
its own cgroup with a finite `memory.max_at_node0`, and it must join that
cgroup before it allocates memory. An example (adapt the paths, CPUs and
cgroup to your setup):

```bash
gapbs=/path/to/gapbs
cg=/sys/fs/cgroup/bc_kron_8t_0
sudo mkdir $cg
echo enabled | sudo tee $cg/memory.mmctl_enabled $cg/memory.demotion
echo 10000M  | sudo tee $cg/memory.max_at_node0
sudo bash -c "echo \$\$ > $cg/cgroup.procs && exec numactl -C 2-9 -- $gapbs/bc -f $gapbs/benchmark/graphs/kron.sg -i4 -n4" &
# Wait until the shell has executed bc, then attach to it. The pattern
# matches only the bc process, not its launcher shell.
until pid=$(pgrep -n -f "^$gapbs/bc -f "); do sleep 0.1; done
sudo ./src/pact --mode pact --monitor-cpu 1 --migration-cpu 1 \
    --workload "$pid" 2-9 bc_kron_8t bc_kron_8t_0
```

After the run, stop the cgroup's demotion thread and then remove the
cgroup:

```bash
echo disabled | sudo tee $cg/memory.demotion   # stops the thread kdemotiond_bc_kron_8t_0_0
pgrep -l kdemotiond                              # lists the demotion threads that still run
sudo rmdir $cg
```

The runtime stops at start if a workload's `memory.max_at_node0` holds no
finite byte count, or if `memory.mmctl_enabled` or `memory.demotion` of
its cgroup is not `enabled`. The cgroup files and the demotion thread are
described in [`setup/kernel/README.md`](setup/kernel/README.md).

### Runtime options

`--mode` selects the mode. A flag of the other mode is an error. The table
lists the main options. Run `./src/pact --help` for the full list.

| Option | Mode | Meaning | Default |
|--------|------|---------|---------|
| `--mode pact\|tierce` | both | runtime mode | `pact` |
| `--workload PID CPUS NAME CGROUP` | both | process to manage, repeat for up to 16 workloads. `CGROUP` must have a finite `memory.max_at_node0`. In PACT mode `CPUS` can be `auto`: the runtime then reads the process affinity. The first workload is the FG of LLC way control. | *required* |
| `--pebs-period N` | both | PEBS sample period (1 sample per N slow-tier loads) | `400` (PACT), `100` (Tierce) |
| `--max-migrations-per-cycle N` | PACT | most pages per `numa_move_pages` call | `4096` |
| `--bin-count N` | PACT | number of PAC bins. The top bin is promoted. | `20` |
| `--monitor-cpu` / `--migration-cpu` | both | pin the event loop / migration thread to one core | none |
| `--use-ldlat`, `--ldlat-gate 350\|700` | Tierce | LDLAT sampling and its gate | off, `700` |
| `--ldlat-dyn` | Tierce | LDLAT-Dyn (needs `--use-ldlat`) | off |
| `--use-cat`, `--cat-signal NAME` | Tierce | LLC way control. Signals: `static`, `mpki`, `l2lat`, `probe`, `bw` | off, `mpki` |

With `--use-ldlat`, `--ldlat-period N` sets the period (default 5), and
`--pebs-period` is an error. `--ldlat-gate 350` turns the gate off. With
`--ldlat-dyn` the runtime sets the gate itself, and `--ldlat-gate 700` is
an error. A build with `make DIAG=1` adds diagnostic options (see
`--help`).

## Baseline tiering systems

[`baselines/`](baselines/) holds kernel patches and build scripts for TPP,
NBT, Nomad, Colloid-tpp, Memtis, and Soar/Alto, each with its own README
documenting the exact kernel tag and build steps.

## Limitations and future directions

This is a research artifact built to reproduce the papers' results. A few
constraints of the current implementation are worth knowing up front:

- **Intel server uarchs only (SKX, SPR, EMR).** PACT has PMU descriptors for
  Skylake-X / Cascade Lake and Sapphire Rapids / Emerald Rapids. The core
  DRD and CHA-TOR event encodings for all three are validated on real
  silicon. See [Supported CPUs](#supported-cpus).
  The SPR/EMR PAC latency constant (`k_cxl`) currently reuses the SKX value
  pending per-uarch calibration, so absolute PAC magnitudes on SPR/EMR are
  approximate (tiering decisions, which depend on relative PAC, are
  unaffected). The runtime **stops on
  an unrecognized CPU**. In PACT mode, the environment variable
  `PACT_ALLOW_UNKNOWN_PLATFORM=1` makes it run with the Skylake-X values
  instead, and the results can then be invalid. Tierce mode does not accept
  this variable. Other microarchitectures (Granite Rapids, AMD) are not
  supported.
- **Two tiers, single node.** PACT assumes one fast tier (local DRAM) and one
  CXL-like slow tier on a 2-NUMA-node host. More than two tiers, multi-socket
  fan-out, and multi-node setups are out of scope for this release.
- **Hardware sampling required.** PAC profiling depends on PEBS plus CHA/uncore
  occupancy counters (read directly via `perf_event_open`), and the runtime must
  run as root.
- **Attaches to running, pre-pinned workloads in capped cgroups.** The
  runtime manages existing processes by PID. It does not start, CPU-pin, or
  cap workloads itself. The `run/` scripts do that around it.
- **Tierce mode: known CPUs only.** Tierce mode needs a TID table entry for
  every CPU of a workload. The runtime has tables for the CPUs in
  [Supported CPUs](#supported-cpus) (see [`setup/tools/`](setup/tools/)
  for another CPU).

Natural directions for future work (and good places to contribute): calibrating
the PAC model for additional CPUs, generalizing placement beyond two tiers,
alternative access-sampling backends, and a self-contained workload launcher.

**Contributions are welcome.** Issues and pull requests, whether bug fixes,
new platform calibrations, or documentation improvements, are appreciated.
Please keep changes consistent with [`src/CODING_STYLE.md`](src/CODING_STYLE.md)
and make sure `make` and `make check-format` pass before opening a PR.

## Citation

If you use this work, please cite our papers:

```bibtex
@inproceedings{pact.asplos26,
  title     = {PACT: A Criticality-First Design for Tiered Memory},
  author    = {Hadian, Hamid and Liu, Jinshu and Xu, Hanchen and Idden, Hansen and
               Li, Huaicheng},
  booktitle = {International Conference on Architectural Support for Programming
               Languages and Operating Systems (ASPLOS '26)},
  year      = {2026},
  doi       = {10.1145/3779212.3790198}
}

@inproceedings{tierce.sosp26,
  title     = {Tierce: Observability-Driven Tiered Memory Management for
               Colocated Workloads},
  author    = {Xu, Hanchen and Inceisci, Berkay and Li, Hao and Zhang, Zhenyu and
               Li, Huaicheng},
  booktitle = {Symposium on Operating Systems Principles (SOSP '26)},
  year      = {2026},
  doi       = {10.1145/3830418.3843905}
}
```

## Contact

**Maintainers:**
- Hamid Hadian - [hamidhadian@vt.edu](mailto:hamidhadian@vt.edu)
- Hanchen Xu - [hanry@vt.edu](mailto:hanry@vt.edu)
- Berkay Inceisci - [berkayinceisci@vt.edu](mailto:berkayinceisci@vt.edu)
- Huaicheng Li - [huaicheng@vt.edu](mailto:huaicheng@vt.edu)

For questions about the research or implementation, please open an issue or
contact a maintainer.

## License

The PACT runtime is released under the [MIT License](LICENSE). Vendored
third-party headers, kernel patches (GPL-2.0), and documentation/data carry
their own licenses - see [LICENSE](LICENSE) and [NOTICE](NOTICE).
