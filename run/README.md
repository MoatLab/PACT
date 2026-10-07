```
 ____      _      ____  _____
|  _ \    / \    / ___||_   _|
| |_) |  / _ \  | |     | |
|  __/  / ___ \ | |___  | |
|_|    /_/   \_\ \____| |_|
```
# PACT - Runner

Run a workload under the PACT runtime. This directory assumes the host is
already prepared and the runtime is built:

1. Host set up - see [`../setup/`](../setup/) (Linux 6.3 + modules + env prep).
2. PACT runtime built - see [`../src/`](../src/) (`cd ../src && make`).

## Contents

| File | Description |
|------|-------------|
| [`run-pact.sh`](run-pact.sh) | Main runner - run a single workload under PACT |
| [`workloads.sh`](workloads.sh) | Workload definitions (sourced by `run-pact.sh`) |

## Running a workload

```bash
./run-pact.sh <workload_name>
```

Example:

```bash
./run-pact.sh bc_kron_8t        # any workload name defined in workloads.sh
```

An optional 2nd positional argument, `skip_setup`, skips the cold-start cache
flush (`drop_caches`) and the `vmtouch` slow-tier preload - use it to re-run a
workload without re-priming the machine:

```bash
./run-pact.sh bc_kron_8t skip_setup
```

What the script does:
- Loads the PACT kernel modules from [`../setup/kernel/`](../setup/kernel/)
  (or aborts, telling you to build them).
- Preloads the workload's graph file into the slow-tier page cache with
  `vmtouch --membind 1`.
- Launches the workload CPU-pinned (`numactl -C <cores> --`) with default
  first-touch memory allocation - PACT is the page placer, so the workload's
  memory is not bound to any node.
- Launches `../src/pact` (under `sudo`; PACT needs root for PEBS/PMU) with the
  default PEBS / migration / binning parameters and no core pinning (PACT's
  `--monitor-cpu` / `--migration-cpu` default to -1).
- Saves logs to `results/<workload>/pact/`.

### How the tiering experiment works (read this)

PACT only has work to do when the fast tier is **too small to hold the whole
working set**, so part of it starts on the slow tier. PACT then samples the
slow-tier accesses (PEBS), scores their performance-criticality, promotes the
critical pages to the fast tier, and the kernel demotes cold pages to make room.

- **The split comes from a physically small fast tier - `run-pact.sh` does NOT
  create it.** You must shrink node 0 with a `memmap=` boot parameter BEFORE
  running (setup step 1b; see [`../setup/README.md`](../setup/README.md)). For a
  1:1 split, node-0 usable DRAM = workload RSS / 2. If you skip this, the whole
  RSS fits in local DRAM, nothing lands on the slow tier, and PACT is a no-op
  (`pebs_samples = 0`, `Promotions = 0`).
- **First-touch placement, not a bind.** The workload launches CPU-pinned but
  with default first-touch allocation (`numactl -C <cores> --`, no `--membind`
  or `--preferred`) - the paper's policy for application transparency. Memory
  fills the small fast tier first, then spills to the slow tier; PACT is the
  page placer from there.
- **Not a cgroup cap.** Do not emulate a small fast tier with a cgroup
  `memory.high`/`memory.max`: a memcg limit caps total usage rather than
  fast-tier residency, and with demotion disabled the kernel cannot shrink an
  anonymous working set, so the workload throttles in unreclaimable D-state
  during its allocation phase.
- **Permissions.** `perf_event_paranoid` must be `<= 0` or PACT collects **zero
  PEBS samples** even as root. `setup/env/prepare_environment.sh` sets it to
  `-1`; if you skip env prep, do `sudo sysctl kernel.perf_event_paranoid=-1`.
- **Checking it worked.** A healthy run shows, in `results/<wl>/pact/pact_debug.log`,
  `PAC Updates` and `Promotions (successful)` in the millions and non-zero
  `Demotions`, and `numastat -p <bc-pid>` shows a non-zero node-1 (slow)
  footprint that PACT drains over time. If `PAC Updates = 0`, nothing reached
  the slow tier (node 0 not shrunk, or run too short) or paranoid is too high.

### Tunables (environment variables)

`pebs_period`, `migration_limit`, `bin_count`, `bin_width`, `PACT`, `VMTOUCH`,
`run_setup_config`, `enable_thp`. Example:

```bash
pebs_period=200 bin_count=32 ./run-pact.sh bc_kron_8t
enable_thp=true ./run-pact.sh bc_kron_8t      # switch the system THP policy
```

### Workload data paths

Dataset locations come from environment variables (no machine-specific paths
are baked in). Set them to match your node before running, e.g.:

```bash
export GAPBS_DIR=/path/to/gapbs        # ./bc and graphs
export SPEC_DIR=/path/to/cpu2017       # licensed SPEC CPU 2017
export SILO_DIR=/path/to/silo
export UBENCH_DIR=/path/to/microbenchmarks
```

See [`workloads.sh`](workloads.sh) for the full list of predefined workloads
and the variables each one expects.

### Obtaining the datasets

The artifact does not bundle workload data (some is licensed; the graphs are
large). Build/obtain each from its upstream source:

- **GAPBS** ([github.com/sbeamer/gapbs](https://github.com/sbeamer/gapbs)) -
  `make` produces `bc` and `converter`. The `bc_kron_*` workloads read a
  serialized Kronecker graph at `${GAPBS_GRAPH_DIR}/kron.sg`; generate it with
  the `converter`:

  ```bash
  cd gapbs && make
  # -g <scale>: 2^scale vertices, -k <degree>: avg degree, -b: serialized .sg
  # Paper's bc-kron = scale 27, degree 16 (134.2M vertices, ~18 GB, ~19.5 GB RSS):
  ./converter -g27 -k16 -b benchmark/graphs/kron.sg
  export GAPBS_DIR=$PWD
  ```

  The `bc_kron_8t_rss` value in `workloads.sh` is a reference figure you use to
  size the fast tier by hand (usable node-0 memory = `_rss`/2 for a 1:1 split); the runner
  does not read it. It assumes this scale-27 graph, so if you generate a
  different scale, re-measure the RSS and update `_rss` (see the comment in
  `workloads.sh`) and your `memmap`.

  The graph must be sized to **exceed** your fast tier so it spills to the slow
  tier (that is what gives PACT work). Scale 27 (~19.5 GB RSS) with a ~10 GB
  fast tier is the 1:1 point used above. On a machine with more or less DRAM,
  pick `scale` so the RSS is about twice your intended fast-tier size, then set
  `memmap` and `_rss` to match.

- **SPEC CPU 2017** (`603.bwaves_s`, `649.fotonik3d_s`) - a **licensed**
  benchmark; install it yourself and point `SPEC_DIR` at the run tree. The
  workloads invoke the per-benchmark run command / `cmd.sh` inside each
  benchmark directory.

- **Silo** ([github.com/stephentu/silo](https://github.com/stephentu/silo)) -
  build the `out-perf.masstree` target; `SILO_DIR` points at the build root
  (the runner expects `out-perf.masstree/benchmarks/dbtest`).

- **Microbenchmarks** (`ptr_chase`, `seq_array`) - point `UBENCH_DIR` at the
  directory containing those two binaries.

### Full machine preparation

For controlled measurements, prepare the machine first (uncore-frequency
pinning, CXL/NUMA layout, governor, disable turbo/THP/KSM/NUMA-balancing) -
see [`../setup/env/`](../setup/env/). You can also fold it into a run:

```bash
run_setup_config=true ./run-pact.sh bc_kron_8t
```

## Verifying a run (kick-the-tires)

A run writes to `results/<workload>/pact/`:

| File | What it shows |
|------|---------------|
| `vmstat.txt` | `pgmigrate*` (PACT promotions) / `pgdemote*` (demotions) counters sampled each second |
| `numastat.log` | the workload's per-node (node0 = fast, node1 = slow) resident memory over time |
| `pact_debug.log` | PACT runtime log (PAC sampling, binning, migration decisions) |
| `workload.output` | the workload's own stdout/stderr |

You don't need a golden number to confirm the pipeline works - confirm
**movement** instead. During a healthy run:

```bash
# migration/demotion counters should be increasing (pages moving between tiers):
tail -f results/bc_kron_8t/pact/vmstat.txt

# the workload's node0 (fast-tier) footprint should grow as PACT promotes
# hot pages (data starts on node1 after the vmtouch preload):
tail -f results/bc_kron_8t/pact/numastat.log
```

Watch `pgmigrate_success` (PACT's `move_pages` promotions) and `pgdemote_kswapd`
(demotions): both should climb. `pgpromote_success` stays **zero** under PACT by
design - it counts only the kernel's own NUMA-balancing promotion path, which the
runner disables; PACT promotes with `move_pages` instead. If `pgmigrate_success`
stays flat at zero, tiering is not active - re-check the kernel modules
(`lsmod | grep -E 'tierinit|kswapdrst'`) and the CXL topology
(`numactl --hardware`; node1 should be CPU-less). Absolute slowdown numbers
depend on the graph size, tier latency, and DRAM budget, so compare PACT against
a baseline run on the *same* machine rather than to a fixed target.

### A validation point (rough, for sanity only)

For a rough idea of what a working run looks like, here are approximate
`bc_kron_8t` numbers on the paper's hardware (CloudLab `c220g5`, scale-27
Kronecker graph, 8 threads, ~1:1 fast:slow split, THP off). Runtime is
`bc`'s reported average iteration time:

| Configuration | Runtime | Slowdown vs local DRAM |
|---|---|---|
| All local DRAM (no split) | ~86 s | 1.00x (reference) |
| **PACT** (1:1 split) | ~101 s | **1.16x** |
| No tiering (1:1 split, pages stay where first-touched) | ~143 s | 1.65x |

So on this machine PACT runs ~1.4x faster than leaving pages untiered and
recovers about three-quarters of the gap back to all-local DRAM. **These are
machine-specific and only a sanity anchor** - graph size, tier latency, and
DRAM budget all shift them, so reproduce the *comparison* (PACT vs no-tiering
on your own hardware), not these exact numbers.

## Quick-start checklist

- [ ] Booted into the PACT kernel (`uname -r` shows `6.3.0`) - [`../setup/kernel/`](../setup/kernel/)
- [ ] Kernel modules built (`tierinit.ko`, `kswapdrst.ko`) - [`../setup/kernel/`](../setup/kernel/)
- [ ] Machine prepared - [`../setup/env/`](../setup/env/)
- [ ] `../src/pact` binary compiled - [`../src/`](../src/)
- [ ] Workload name and data paths set (see `workloads.sh` / env vars above)
- [ ] Run `./run-pact.sh <workload_name>`
