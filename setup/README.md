# Host setup

Bring a fresh machine to a state where PACT can run, in PACT mode or in Tierce
mode. Do this **before** building `../src/` or using `../run/`.

| Subdir | Purpose |
|--------|---------|
| [`kernel/`](kernel/) | Build and install the patched **Linux 6.3** kernel (`6.3.0-tierce`) that both modes use: a per-cgroup fast-tier cap and per-cgroup demotion (`kdemotiond`). Build and load the two modules that the tiering subsystem needs: `tierinit` (registers the slow tier and the demotion targets) and `kswapdrst` (keeps `kswapd` and demotion from stalling). |
| [`env/`](env/) | Prepare the machine for controlled runs: uncore-frequency pinning, node-1 CPUs offline, performance governor, `perf_event_paranoid` -1, page cache dropped, and NMI watchdog / ASLR / KSM / NUMA balancing / THP / SMT / turbo / swap off. |
| [`tools/`](tools/) | Offline scripts that measure the CHA TID filter value of each core (Tierce mode, only for a CPU that the runtime does not know). |

## Order

1. **Kernel** - [`kernel/`](kernel/): `./setup_kernel.sh` builds the
   `linux-image` and `linux-headers` packages of `6.3.0-tierce`. Install
   both with `dpkg -i`.
2. **GRUB** - make `6.3.0-tierce` the default kernel and add three boot
   arguments to `GRUB_CMDLINE_LINUX` in `/etc/default/grub`, then run
   `sudo update-grub` and reboot (details in
   [`kernel/README.md`](kernel/README.md#grub)):

   ```
   rdt=cmt,mbmtotal,mbmlocal,l3cat systemd.unified_cgroup_hierarchy=yes mitigations=off
   ```

   The reasons for each argument are in
   [`kernel/README.md`](kernel/README.md#grub). No other boot argument is
   necessary.
3. **Modules** - [`kernel/`](kernel/): after the reboot, `./build_modules.sh`
   builds `tierinit.ko` and `kswapdrst.ko`. Load `tierinit` first, then
   `kswapdrst` (or let `run.sh` load them). Leave `tierinit` loaded until the
   next reboot: unloading it may lock up the CPUs.
4. **Environment** - [`env/`](env/): `sudo ./prepare_environment.sh` (or let
   the runner do it with `run.sh --prepare-env`).
5. **Build the runtime** - `make -C src` from the repository root (see
   [Build the runtime](../README.md#2-build-the-runtime---src)).
6. **Run** - [`../run/`](../run/): `run.sh` puts every workload in its own
   cgroup with a cap on its fast-tier (node 0) memory, and `kdemotiond`
   enforces each cap. See [Fast-tier size](#fast-tier-size) below.

> **Re-run `prepare_environment.sh` after EVERY reboot.** Its effects are
> runtime-only and do NOT persist across a reboot. The modules also need
> loading after a reboot. `run.sh` loads them if they are not loaded.

The env scripts source each other by their own location, so you can start
them from any directory (the runner calls
`../setup/env/prepare_environment.sh`).

## Check the CXL emulation (important)

The slow tier is emulated by a remote NUMA node with its CPUs taken offline
and its uncore frequency pinned low. `check_cxl_conf` and `run.sh`
**abort** if Node 1 still has online CPUs (an unchecked layout silently
produces invalid results). To see the layout yourself:

```bash
numactl --hardware        # 'node 1 cpus:' should be empty
```

The scripts do *not* check the latency. Check it yourself:

**Latency** - the emulated tiers should land near the PACT paper's targets
(~90 ns local DRAM, ~140 ns remote NUMA, ~190 ns CXL-emulated, see
[`../modeling/README.md`](../modeling/README.md)). Measure with a
pointer-chase latency tool (e.g. Intel MLC `--latency_matrix`, or the
`ptr_chase` microbenchmark) and adjust the uncore-frequency targets in
[`env/modify-uncore-freq.sh`](env/modify-uncore-freq.sh) /
`UNCORE_ARGS` until the slow tier matches. Pinning the frequency without
checking the resulting latency is the most common source of invalid
tiering results.

## Fast-tier size

Each workload gets its own fast-tier size. `run.sh` creates one cgroup per
workload and writes the cap to the cgroup's `memory.max_at_node0`. The kernel
allocates the workload's pages on node 0 up to the cap and then on node 1.
When a promotion would exceed the cap, the kernel refuses it and wakes
`kdemotiond`, which demotes cold pages of the same cgroup to node 1. Later
promotions then find free space under the cap.
The cap is `<name>_rss x --ratio`, with `<name>_rss` (MB) from
[`../run/workloads.sh`](../run/workloads.sh).

| Ratio (fast:slow) | `--ratio` | Cap |
|-------------------|-----------|-----|
| 1:1 (default) | `0.5` | `RSS x 0.5` |
| about 1:2 | `0.33` | `RSS x 0.33` (a little below `RSS / 3`) |
| about 8:1 | `0.89` | `RSS x 0.89` (a little above `RSS x 8/9`) |

`run.sh` cuts the cap to whole MB (`RSS x ratio`, rounded down), so the
ratio column is approximate.

`memory.max_at_node0` limits only the pages of the cgroup on node 0. Do NOT
use `memory.max` or `memory.high` for this: they limit the total memory of
the cgroup, not its fast-tier memory. To measure `_rss`, see
[`../run/README.md`](../run/README.md#measuring-_rss).

## Notes

- The current scripts assume a **2-NUMA-node** server (one fast tier, one
  CXL-like slow tier).
