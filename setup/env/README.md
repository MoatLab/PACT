# Environment preparation

Machine-wide configuration for controlled PACT measurements. Run on the host
after booting into the patched kernel (see [`../kernel/`](../kernel/)).

| Script | Purpose |
|--------|---------|
| `prepare_environment.sh` | One-shot prep. Calls the other two. It pins the uncore frequency, turns off the NMI watchdog, address-space randomization, KSM, NUMA balancing, THP, SMT, turbo and swap, sets the performance governor on the online CPUs, takes the node-1 CPUs offline, drops the page cache, and sets `perf_event_paranoid` to -1. |
| `modify-uncore-freq.sh` | Pin per-node min/max uncore frequency (fast tier high, slow tier low). |
| `cxl-global.sh` | Helper functions (NMI watchdog, ASLR, governor, turbo, THP, KSM, NUMA balancing, SMT, swap, CPU online state, cache flush) sourced by the above. |

## Usage

```bash
# Full one-shot prep (requires root):
sudo ./prepare_environment.sh
```

Override the default uncore targets via `UNCORE_ARGS` (use `sudo env` so the
variable survives into the root shell):

```bash
sudo env UNCORE_ARGS="2000000 2000000 500000 500000" ./prepare_environment.sh
```

Each `UNCORE_ARGS` value must be a multiple of 100000 kHz, from 100000 to
12700000 kHz. The min of each node must not be above its max.
`modify-uncore-freq.sh` rejects any other value before it changes the host.

> **Do not run `modify-uncore-freq.sh` standalone after CXL setup.** It brings
> all CPUs back online (to enter a known base state before re-pinning), which
> re-populates node 1 and destroys the CPU-less slow tier. Only
> `prepare_environment.sh` runs it in the correct order (frequency pin, then
> offline node 1). If you must re-pin frequencies, re-run the full
> `prepare_environment.sh`.

The scripts locate each other by their own directory, so they work regardless
of the current working directory (the runner invokes
`../setup/env/prepare_environment.sh`).

## Next step

Build the runtime (`make -C src` from the repository root), then run a
workload with [`../../run/`](../../run/).
