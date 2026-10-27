# PAC modeling

Scripts for collecting the perf counters used in the correlation plots of
Figure 2 of the PACT paper (ASPLOS'26), which motivate the LLC-miss/MLP
metric across three memory tiers.

---

## Directory structure

```
modeling/
├── run-models.sh        # collect perf counters → CSV
├── raw/                 # created at runtime - perf stat output + models.csv
├── skx/                 # Skylake-X reference data + gnuplot
│   ├── dat/
│   │   └── r-l3_stall_skx.csv   # gnuplot input (complete SKX reference)
│   ├── eps/             # created at runtime - gnuplot writes corr.pdf here
│   └── plot/
│       └── corr.plot    # gnuplot script for Figure 2
└── README.md
```

---

## What is being measured

Each workload runs three times - once per memory tier - under `numactl --membind`:

| Tier | Latency | Node variable |
|------|---------|--------------|
| Tier 1 | ~90ns  | `LOCAL_NODE` - local DRAM |
| Tier 2 | ~140ns | `NUMA_NODE`  - remote NUMA |
| Tier 3 | ~190ns | `CXL_NODE`   - CXL-emulated |

Four hardware counters collected per run (single `perf stat` pass):

| Counter | Role |
|---------|------|
| `CYCLE_ACTIVITY.STALLS_L3_MISS` | `l3_stall` |
| `MEM_LOAD_RETIRED.L3_MISS` | `l3_miss` |
| `OFFCORE_REQUESTS_OUTSTANDING.DEMAND_DATA_RD` | MLP numerator |
| `OFFCORE_REQUESTS_OUTSTANDING.CYCLES_WITH_DEMAND_DATA_RD` | MLP denominator |

MLP here is `OFFCORE_REQUESTS_OUTSTANDING.DEMAND_DATA_RD / ...CYCLES...`, an
offcore-outstanding proxy used only to fit the offline model. The PACT
*runtime* derives the slow-tier MLP from CHA/TOR occupancy counters instead
(see `src/pmu.c`). Both estimate memory-level parallelism.

Metrics written to CSV (per tier): `l3_stall`, `demand_l2_mlp` (the MLP
above), `l3_miss`, and `l3_miss/mlp`. `run-models.sh` writes 0 to the
`offcore_load_latency` and `lat*l3_miss/mlp` columns. These columns keep the
format of the reference dataset in `skx/dat/`, which holds values for them.

> **Note:** The counter names are those of Intel Skylake-X (SKX). `corr.plot`
> fits the model slopes from the data file (gnuplot `fit`). Its axis ranges
> and tier labels match the SKX reference dataset in `skx/`.

---

## Previously gathered data - Intel Skylake-X

`skx/dat/r-l3_stall_skx.csv` contains the complete reference dataset
collected on an Intel Skylake-X server (95 workloads × 3 tiers).  
To regenerate Figure 2 from this data:

```bash
cd skx
gnuplot plot/corr.plot
# output → eps/corr.pdf
```

Newly collected data is written to `raw/models.csv` by `run-models.sh`. Copy it to `skx/dat/r-l3_stall_skx.csv` before plotting.

---

## Running workloads

### 1. Verify NUMA topology

```bash
numactl --hardware
```

Edit `CPU_NODE`, `LOCAL_NODE`, `NUMA_NODE`, `CXL_NODE` at the top of `run-models.sh`
to match your server.

> **Topology note.** The model spans three latency tiers (`LOCAL_NODE`,
> `NUMA_NODE`, `CXL_NODE`), so it needs three NUMA nodes. The two-node
> layout of [`../setup/`](../setup/) has no third tier. On a two-node
> machine, point `NUMA_NODE` and `CXL_NODE` at the same slow node, or plot
> the reference data in `skx/dat/` (see "Previously gathered data"). A tier
> whose node does not exist gets zeros in its columns.

### 2. Run SPEC CPU 2017 workloads

```bash
sudo ./run-models.sh --spec
```

Runs every workload in `SPEC_WORKLOADS` × 3 tiers.

### 3. Run GAPBS workloads

```bash
sudo ./run-models.sh --gapbs
```

Runs every workload in `GAPBS_WORKLOADS` × 3 tiers.  
Graph is preloaded into the target NUMA node's page cache via `vmtouch` before each tier run.

To set `PERF`, `SPEC_DIR`, `GAPBS_DIR` or `VMTOUCH`, pass them through
`sudo env`, for example:

```bash
sudo env PERF=/path/to/perf GAPBS_DIR=/path/to/gapbs ./run-models.sh --gapbs
```

### Options and failed runs

`--raw-dir DIR`, `--output FILE` and the `--*-node` options set the output
paths and the NUMA nodes. A workload with a failed tier run gets no row in
the CSV, and the script then prints `FAILED` and exits with status 1.

### 4. Plot

Copy the collected CSV to the gnuplot data folder, then run the plot:

```bash
cp raw/models.csv skx/dat/r-l3_stall_skx.csv
cd skx && gnuplot plot/corr.plot
# output → skx/eps/corr.pdf
```

---


## Adding or changing workloads

Workloads are defined at the top of `run-models.sh` in two separate arrays.

**SPEC CPU 2017** - format `"NAME|COMMAND"`:

The command runs under `bash -c`. Quote a path in it with `printf %q`, so
that a path with spaces or shell characters stays one word:

```bash
SPEC_DIR="/path/to/cpu2017/run"
printf -v SPEC_MCF_CMD 'cd %q && ./mcf_r_base.mytest-m64 inp.in' "${SPEC_DIR}/505.mcf_r"
printf -v SPEC_GCC_CMD 'cd %q && ./cpugcc_r_base.mytest gcc-pp.c ...' "${SPEC_DIR}/502.gcc_r"

SPEC_WORKLOADS=(
    "505.mcf_r|${SPEC_MCF_CMD}"
    "502.gcc_r|${SPEC_GCC_CMD}"
    # add more SPEC workloads here
)
```

**GAPBS** - just the workload name; the run command is inside `func_run_gapbs_tier`:

```bash
GAPBS_WORKLOADS=(
    "bc-kron"
    # add more GAPBS workloads here (requires matching func_run_gapbs_tier logic)
)
```

Each workload runs under `numactl --cpunodebind --membind` automatically -
no need to add `numactl` inside the command.


---

## Prerequisites

- `numactl` - `apt install numactl`
- `vmtouch` - `apt install vmtouch` (GAPBS graph preloading)
- `perf` - stock `perf` from `linux-tools`; the four counters are read from `perf stat` text output. Point at a specific build with `PERF=/path/to/perf ./run-models.sh` (defaults to `perf` in `$PATH`)
- `gnuplot` - required only for plotting, not for data collection
- Workload binaries installed and paths configured in `run-models.sh`. The
  GAPBS runs use the same `kron.sg` graph as `run/` - generate the paper's
  scale-27 graph as described in
  [`../run/README.md`](../run/README.md#getting-the-datasets)
  ("Getting the datasets").
