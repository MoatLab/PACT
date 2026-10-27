# CHA TID discovery

In Tierce mode, the runtime measures the memory-level parallelism (MLP) of
each workload from the TOR counters of the CHAs (the LLC slices) with the
thread-ID (TID) filter on. Each core has one TID filter value. The runtime
does not measure these values when it starts. It uses a table that is
compiled into [`../../src/pmu-platform.c`](../../src/pmu-platform.c)
(`skx_core_to_tid` and `emr_core_to_tid`). The two scripts here measure that
table on a machine and print it as a C array.

PACT mode does not use the table: its global MLP counts with the TID filter
off.

| Script | CPU | Cores | Counter used |
|--------|-----|-------|--------------|
| [`discover_tid_mapping_skx.sh`](discover_tid_mapping_skx.sh) | Skylake-SP | 10 | `UNC_CHA_TOR_OCCUPANCY.IA_MISS` (event 0x36, umask 0x21) with the DRD filter in `config1` |
| [`discover_tid_mapping.sh`](discover_tid_mapping.sh) | Emerald Rapids | 32 | `UNC_CHA_TOR_INSERTS.IA_MISS_DRD` (event 0x35, raw config `0x00c817fe00010135`) |

## When you need them

The compiled tables hold the core numbering of a two-socket Xeon Silver
4114 (Skylake-SP, 10 cores per socket) and a Xeon Gold 6530 (Emerald
Rapids). On these CPUs, with the same core count and core numbering, the
compiled tables apply.

Run the scripts when your CPU model, core count, or core numbering differs.
A wrong table gives wrong per-workload MLP values without an error message.
Tierce mode stops at start if a CPU of a workload has no entry in the
table.

## What they do

For each core, the script starts Intel MLC on that core and counts the TOR
traffic of each candidate TID (`0x00`, `0x08`, `0x10`, ...), each on a
different CHA. The candidate with the highest count is the TID of that
core. At the end the script prints the C array and checks that every core
got a unique TID. If a check fails, it prints `FAIL` and exits with status
1.

## Requirements

- Root (the scripts call `sudo`).
- Intel Memory Latency Checker (MLC). It is not included. Download it from
  Intel and point `MLC` at the binary.
- `perf` with the `uncore_cha_*` PMUs. Point `PERF` at the binary.
- At least as many CHAs as TID candidates (`NUM_CANDIDATES`).
- Edit `NUM_CORES` and `NUM_CANDIDATES` at the top of the script to match
  your CPU.

## Usage

```bash
sudo env MLC=/path/to/mlc PERF=/path/to/perf ./discover_tid_mapping_skx.sh
```

Copy the printed array only when the unique-TID check prints `PASS`. On a
Skylake-SP CPU with another core numbering, the comparison with the compiled
table fails, and the script exits with status 1 for that reason alone. Copy
the array into `src/pmu-platform.c` in place of the table of your platform,
and rebuild the runtime (`make -C ../../src`).

`src/pmu-platform.c` also holds a CHA-to-core table (`skx_cha_to_core_map`,
`emr_cha_to_core_map`). It pairs each core with one CHA. These scripts do not
measure it. For a CPU with another layout, you must supply that table
yourself.
