```
 ____      _      ____  _____
|  _ \    / \    / ___||_   _|
| |_) |  / _ \  | |     | |
|  __/  / ___ \ | |___  | |
|_|    /_/   \_\ \____| |_|
```
# PACT - Runner

Run one workload, or one foreground (FG) workload with background (BG)
workloads, under the PACT runtime in PACT mode or in Tierce mode. Before you
use this directory, set up the host and build the runtime:

1. Host set up - see [`../setup/`](../setup/) (patched Linux 6.3, boot
   arguments, `tierinit` and `kswapdrst` modules, environment preparation).
2. Runtime built - see [`../src/`](../src/) (`make -C ../src`).

## Contents

| File | Description |
|------|-------------|
| [`run.sh`](run.sh) | Main runner: one run in PACT mode or in Tierce mode |
| [`workloads.sh`](workloads.sh) | Workload definitions (sourced by `run.sh`) |
| [`tools/restart_loop.c`](tools/restart_loop.c) | Restart library for long background workloads (`make -C tools`) |

## Requirements

- A two-socket Intel server. Node 0 is the fast tier. Node 1 is the slow
  tier (CPUs offline, low uncore frequency). See the supported CPUs in
  [`../README.md`](../README.md#supported-cpus).
- The patched kernel `6.3.0-tierce`, booted with
  `rdt=cmt,mbmtotal,mbmlocal,l3cat systemd.unified_cgroup_hierarchy=yes mitigations=off`
  ([`../setup/kernel/README.md`](../setup/kernel/README.md)).
- The modules `tierinit` and `kswapdrst`, built in `../setup/kernel/`.
- The runtime binary `../src/pact`.
- `numactl`, `numastat`, `vmtouch`, root, and `gcc` for the restart library.
- Workload data (see [Workload data paths](#workload-data-paths)).

## Running a workload

```bash
sudo -E ./run.sh [options] <workload> [<workload> ...] [-- <runtime flags>]
```

`-E` keeps the data path variables of `workloads.sh` (for example
`GAPBS_DIR`). If your sudo policy does not allow `-E`, use
`sudo env GAPBS_DIR=... ./run.sh ...`.

The first workload is the foreground (FG). The runner times it. In the CAT
arms, the FG gets the protected LLC ways. Every later workload is a
background (BG) workload. The BG workloads share the remaining LLC ways, and
the runner stops them when the FG finishes. The runtime manages at most 16
workloads.

### Modes and arms

`--mode` selects the runtime mode (default `pact`). `--arm` selects the arm
of that mode (default `pact` in PACT mode and `wmlp` in Tierce mode). An arm
of the other mode is an error.

| Mode | Arm | Runtime flags that the arm adds | Needs a BG |
|------|-----|---------------------------------|------------|
| `pact` | `pact` | `--mode pact` | no |
| `tierce` | `wmlp` | `--mode tierce` | no |
| `tierce` | `wmlp-ldlat` | `--mode tierce --use-ldlat --ldlat-gate 350` | no |
| `tierce` | `wmlp-cat` | `--mode tierce --use-cat --cat-signal static` | yes |
| `tierce` | `tierce` | `--mode tierce --use-ldlat --ldlat-gate 700 --use-cat --cat-signal static` | yes |
| `tierce` | `wmlp-dcat` | `--mode tierce --use-cat --cat-signal mpki` | yes |

The static split gives the FG `W_max` LLC ways (8 of 11 on Skylake-SP,
15 of 20 on Emerald Rapids) and the BG workloads the other ways. Change it
with `--cat-wmax`. The runner option `--ldlat-gate 350|700` replaces the
gate of `wmlp-ldlat` or `tierce`.

### Options

Run `./run.sh --help` for every option and its default. The runtime
options of the runner (`--pebs-period`, `--ldlat-period`, `--ldlat-gate`,
`--cat-signal`, `--cat-wmax`) are valid only for the arms that use them.
More runtime flags go after `--`, for example
`-- --max-migrations-per-cycle 1024`. The main runner options:

- `--ratio R` (default 0.5): fast-tier cap as a share of each workload's RSS.
- `--cpus LIST` (default `2-9`): node-0 CPUs for the workloads, one per thread.
- `--runtime-cpu N` (default 1): CPU of the runtime.
- `--prepare-env`: run `../setup/env/prepare_environment.sh` first.
- `--thp`: transparent huge pages `always` for the run.

Environment variables: `PACT` (runtime binary, default `../src/pact`),
`VMTOUCH`, `TIERINIT_KO`, `KSWAPDRST_KO`, and the data paths of
`workloads.sh`.

### CPU layout

CPU 0 stays free for login shells and for the `kswapdrst` work. CPU 1 runs
the runtime (`--runtime-cpu`). CPUs 2-9 run the workloads, one thread per
CPU. The FG gets the first CPUs and each BG the next ones. With two 4-thread
workloads, the FG runs on CPUs 2-5 and the BG on CPUs 6-9. A solo 8-thread
workload runs on CPUs 2-9. The runner stops if a CPU is not on node 0, if
CPU 0 is in the list, or if the runtime CPU is also a workload CPU.

### Fast-tier cap

Each workload gets its own cgroup (`<workload>_<index>` under
`/sys/fs/cgroup`) with the cap `RSS x ratio` in `memory.max_at_node0`. RSS
is the `<name>_rss` value in `workloads.sh` (MB). With the default ratio
0.5, half of each workload's memory fits on node 0. The kernel allocates on
node 0 up to the cap, then on node 1.

**A workload must be in its cgroup before it allocates memory.** The cap
counts only the pages that are charged to the cgroup. The runner starts
each workload through a shell that joins the cgroup and then executes the
workload (`exec`). Do not start a workload first and move it into its
cgroup later.

### What the script does

- Loads `tierinit` and `kswapdrst` if they are not loaded.
- Checks the host settings that `prepare_environment.sh` makes, and stops if
  one is wrong (`--prepare-env` runs the script first). Sets NUMA balancing
  and the global demotion switch to 0.
- Preloads each input file into the page cache on node 1 with `vmtouch`.
- Creates the cgroups, starts the workloads in them, and starts the runtime
  on `--runtime-cpu`.
- Waits for the FG, stops the BG workloads and the runtime, removes the
  cgroups, writes back the switch values that it read at start, and writes
  `summary.txt`.

### Safety

- Leave `tierinit` loaded until the next reboot: unloading it may lock up
  the CPUs. The runner never unloads it.
- Do not run other PEBS users (for example `perf record`) during a run.
- The runner stops the runtime only with SIGINT. Do not send SIGKILL to the
  runtime.
- `run.sh` cleans up on normal completion, Ctrl+C, SIGTERM and SIGHUP. Run
  long runs in `tmux` or `screen`. If `run.sh` was killed with SIGKILL,
  clean up by hand:
  1. Stop the workloads: `pkill -TERM -s <launcher PID>` for each workload
     (the launcher PIDs are in `run.log`).
  2. Wait until the runtime exits. If it still runs after a minute, send it
     one SIGINT.
  3. For each workload cgroup: write `disabled` to `memory.demotion`, then
     `rmdir` the cgroup.
  4. CAT arms: `rmdir /sys/fs/resctrl/tierce_fg /sys/fs/resctrl/tierce_bg`.
  5. Write back `kernel.numa_balancing` and
     `/sys/kernel/mm/numa/demotion_enabled` from the line `before the run:`
     in `run.log`.

## Examples

Set the data paths and prepare the host once after each boot:

```bash
export GAPBS_DIR=/path/to/gapbs        # bc, pr, benchmark/graphs/{kron,urand,twitter}.sg
export SPEC_DIR=/path/to/cpu2017       # only for the SPEC workloads
make -C tools                          # only for w_649_4t_long
sudo ../setup/env/prepare_environment.sh
```

**PACT, one workload, 1:1 split.**

```bash
sudo -E ./run.sh bc_kron_8t
```

**PACT, two workloads as one global instance.**

```bash
sudo -E ./run.sh bc_urand_4t pr_twitter_4t_long
```

**Tierce, one workload (wMLP).**

```bash
sudo -E ./run.sh --mode tierce --arm wmlp bc_kron_8t
```

**Tierce, FG with a BG workload.**

```bash
sudo -E ./run.sh --mode tierce --arm wmlp     bc_urand_4t pr_twitter_4t_long
sudo -E ./run.sh --mode tierce --arm wmlp-cat bc_urand_4t pr_twitter_4t_long
sudo -E ./run.sh --mode tierce --arm tierce bc_kron_4t w_649_4t_long
```

Repeat each command for the number of runs that you want to average. Compare
the arms on the same machine.

## Output

A run writes to `results/<wl>[-<wl>...]/<arm>-<timestamp>/`:

| File | What it holds |
|------|---------------|
| `summary.txt` | FG time, caps, peak node-0 and node-1 use, sample counts, `kernel.perf_event_max_sample_rate` after the run (PMI throttling can lower it), and the result of each check |
| `workload_0.time` | FG time in seconds (`fg_runtime_sec`) |
| `workload_<i>_real.time` | time of BG `i` from its start to the end of the FG |
| `workload_<i>.output` | stdout and stderr of workload `i` (0 = FG) |
| `workload_<i>_numastat.log` | node-0 and node-1 memory of workload `i` in MB, once per second |
| `workload_<i>_cgroup.csv` | cgroup counters once per second: `promotions`, `pgsteal_*`, `pgdemote_*`. `promotions` is the cgroup's `memory.promotion_counter`, which counts only the kernel's NUMA-balancing promotions. The runner turns NUMA balancing off, so this column stays 0. The runtime's promotions are in `migration_trace.csv` |
| `workload_<i>_memory.stat` | the cgroup's `memory.stat` at the end of the run |
| `workload_<i>_restart_loop.csv` | start and end times of each restart of a workload under the restart library |
| `vmstat.csv` | migration and demotion counters of `/proc/vmstat`, once per second |
| `before_vmstat.log`, `after_vmstat.log` | `/proc/vmstat` before the workloads start and after the run |
| `pact_debug.log` | runtime log, with the runtime statistics at its end |
| `pact_cmd.txt` | the runtime command line |
| `migration_trace.csv` | promotions and demotions per second, total and per workload. Runtime demotions (`*pact_demotion*`): 0, the runtime only promotes and the kernel demotes. The kernel demotions are in the `*kernel_demotion*` columns |
| `pac_table_dump.csv` | PAC-table snapshots (with `--dump-pac-table` and a runtime built with `make -C src DIAG=1`) |
| `manifest.txt` | mode, arm, workloads, CPUs, caps, kernel release and `/proc/version`, boot arguments, `kernel.perf_event_max_sample_rate`, `vm.min_free_kbytes`, `vm.watermark_scale_factor`, runtime binary checksum |
| `run.log` | the runner's own output |

`fg_runtime_sec` is the FG metric. Its timer starts when the start phase is
over: all workloads run, and the runtime has run for 3 s. It stops when the
FG exits. `fg_wall_sec` in `summary.txt` counts from the FG start instead.

## Check that it worked

The runner writes one `check_<name>=PASS|FAIL|SKIP` line per check into
`summary.txt`, and exits with status 1 if a check fails. No check may read
`FAIL`.

You do not need a reference number to see that tiering works. Look for
movement during the run:

```bash
tail -f results/bc_kron_8t/pact-*/vmstat.csv               # pgmigrate_success and pgdemote_kswapd grow
tail -f results/bc_kron_8t/pact-*/workload_0_numastat.log  # node 0 near the cap, node 1 holds the rest
```

`vmstat.csv` counts for the whole system. The runtime's promotions for
each workload are in `migration_trace.csv`.

`pgmigrate_success` counts the promotions of the runtime and the
demotions. `pgdemote_kswapd` counts the demotions, including those of
`kdemotiond`. `pgpromote_success` stays zero: it counts only the kernel's
own NUMA-balancing promotions, and the runner turns NUMA balancing off. If
`pgmigrate_success` stays at zero, check the modules
(`lsmod | grep -E 'tierinit|kswapdrst'`) and the topology
(`numactl --hardware`: node 1 has no CPUs).

## Workload data paths

Data locations come from environment variables. No machine-specific path is
built in:

```bash
export GAPBS_DIR=/path/to/gapbs        # bc, pr
export GAPBS_GRAPH_DIR=/path/to/graphs # default: ${GAPBS_DIR}/benchmark/graphs
export SPEC_DIR=/path/to/cpu2017       # licensed SPEC CPU 2017 run directories
export SILO_DIR=/path/to/silo
export UBENCH_DIR=/path/to/microbenchmarks
export RESTART_LOOP_SO=/path/to/restart_loop.so  # default: tools/restart_loop.so
```

| Workload | Use | Command | `_rss` (MB, sets the cap) |
|----------|-----|---------|----------|
| `bc_kron_8t` | solo | `bc -f kron.sg -i4 -n4`, 8 threads | 20000 |
| `bc_kron_4t` | FG | `bc -f kron.sg -i4 -n4`, 4 threads | 20000 |
| `bc_urand_4t` | FG | `bc -f urand.sg -i4 -n1`, 4 threads | 20484 |
| `pr_twitter_4t_long` | BG | `pr -f twitter.sg -i1000 -t1e-4 -n100`, 4 threads | 12616 |
| `w_649_8t` | solo | `649.fotonik3d_s` `cmd.sh`, 8 threads | 9643 |
| `w_649_4t_long` | BG | `649.fotonik3d_s` `cmd.sh` under the restart library, 4 threads | 9643 |
| `silo_5t` | solo | Silo `dbtest`, YCSB, 5 threads | 42112 |
| `bwaves_8t` | solo | `603.bwaves_s`, 8 threads | not set |
| `ptr_chase` | solo | pointer-chasing microbenchmark, 1 thread | not set |
| `seq_array` | solo | sequential-array microbenchmark, 1 thread | not set |

The runner needs `<name>_rss` to set the cap. `bwaves_8t`, `ptr_chase` and
`seq_array` have no measured RSS in `workloads.sh`. Measure it (see
[Measuring `_rss`](#measuring-_rss)) and set it before you run them.

### Getting the datasets

The artifact does not bundle workload data (some is licensed, and the graphs
are large). Build or get each from its upstream source:

- **GAPBS** ([github.com/sbeamer/gapbs](https://github.com/sbeamer/gapbs)).
  `make` builds `bc`, `pr` and `converter`. The graph targets of its
  `benchmark/bench.mk` build the inputs:

  ```bash
  cd gapbs && make
  make benchmark/graphs/kron.sg benchmark/graphs/urand.sg   # scale 27, average degree 16
  make benchmark/graphs/twitter.sg                          # downloads the twitter graph
  export GAPBS_DIR=$PWD
  ```

  You can also generate the Kronecker graph directly:
  `./converter -g27 -k16 -b benchmark/graphs/kron.sg` (2^27 vertices).
  The `_rss` values in `workloads.sh` are for these graphs. For another
  graph, measure the RSS and update `_rss`. The cap follows from it. The
  input must be larger than its cap, so that part of it lands on the slow
  tier.

- **SPEC CPU 2017** (`603.bwaves_s`, `649.fotonik3d_s`). A licensed
  benchmark. Install it yourself and point `SPEC_DIR` at the run tree. The
  workloads start the run command or `cmd.sh` in each benchmark directory.
  `${SPEC_DIR}/649.fotonik3d_s/cmd.sh` must start
  `fotonik3d_s_base.mytest-m64` on the reference input. The runner sets
  `OMP_NUM_THREADS` to the thread count of the workload.

- **Silo** ([github.com/stephentu/silo](https://github.com/stephentu/silo)).
  Build the `out-perf.masstree` target. `SILO_DIR` points at the build root
  (the runner expects `out-perf.masstree/benchmarks/dbtest`).

- **Microbenchmarks** (`ptr_chase`, `seq_array`). Point `UBENCH_DIR` at the
  directory that holds the two binaries.

## Adding your own workload

A workload is a set of shell variables in [`workloads.sh`](workloads.sh),
named `<name>_<field>`. `<name>` can use letters, digits and `_`, and must
not start with a digit. The runner reads these fields:

| Field | Required | Meaning |
|-------|----------|---------|
| `<name>_pname` | yes | Name of the process that allocates the workload's memory. The runner looks for it among the descendants of the shell that starts the workload. It compares the name with `/proc/<pid>/comm` (the first 15 characters of the binary's file name). The runtime manages this process and all its threads. |
| `<name>_rss` | yes | Peak resident set size in MB. The fast-tier cap of the workload is `_rss` x `--ratio`. |
| `<name>_omp_threads` | yes | Number of threads. The runner gives the workload this many CPUs, one per thread, and sets `OMP_NUM_THREADS` to it. |
| `<name>_workload_cmd` | yes | The command line. It must contain the literal text `$numactl_args` in front of the program (see below). |
| `<name>_vmtouch_file` | no | Input file that the runner loads into the page cache on node 1 before the run, or `""`. |
| `<name>_requires` | no | Bash array of files that must exist. The runner stops before the run if one is missing. |

### Rules for the command

- **Start the program through `$numactl_args`.** The runner replaces it with
  `exec numactl -C <cpus> --`. The `exec` keeps the PID of the shell that starts
  the workload. The runner has already put that shell into the workload's
  cgroup, so the workload joins the cgroup before it allocates memory. Write
  the command with `printf -v` and `%q`, as the existing entries do, so that
  paths with spaces stay intact.
- **Keep the program in the foreground.** The runner cannot find a program
  that daemonizes. The run then stops with
  `no process named <pname> under PID ...`.
- **Set the thread count of programs that do not read `OMP_NUM_THREADS`** in
  the command (for example a `--threads` option) to the same value as
  `_omp_threads`.
- **You can use a wrapper script** if it `exec`s the program or starts it as
  a child. `_pname` is the program, not the script.

### Measuring `_rss`

Run the workload alone, without caps, on the same input:

```bash
/usr/bin/time -v <program> <arguments> 2>&1 | grep "Maximum resident set size"
```

The value is in kB. `_rss` is that value / 1024.

### Foreground and background

A BG must run longer than the FG. Use one of these two methods:

- Give the BG more iterations. The GAPBS background runs 100 trials.
- Restart the BG in place each time it exits with
  [`tools/restart_loop.so`](tools/restart_loop.c), loaded with `LD_PRELOAD`.
  The PID stays the same (see the `w_649_4t_long` entry). The runner logs
  the restarts to `workload_<i>_restart_loop.csv`.

The threads of all workloads together must fit the CPU list (`--cpus`,
default `2-9`, eight CPUs).

## Quick-start checklist

- [ ] Booted into the patched kernel (`uname -r` shows `6.3.0-tierce`) with the three boot arguments - [`../setup/kernel/`](../setup/kernel/)
- [ ] Modules built (`tierinit.ko`, `kswapdrst.ko`) - [`../setup/kernel/`](../setup/kernel/)
- [ ] Host prepared after this boot (`sudo ../setup/env/prepare_environment.sh`) - [`../setup/env/`](../setup/env/)
- [ ] `../src/pact` built - [`../src/`](../src/)
- [ ] Data paths set (see above), and `make -C tools` for `w_649_4t_long`
- [ ] Run `sudo -E ./run.sh [--mode tierce --arm <arm>] <workload> [<workload> ...]`
- [ ] `summary.txt` shows no `FAIL` line
