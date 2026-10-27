#!/bin/bash

# Workload definitions for run.sh.
# Source this file: do not execute directly.
#
# run.sh reads these variables per workload "<name>" by indirection:
#   <name>_pname         process name of the workload binary. The runtime
#                        manages this process.
#   <name>_rss           peak resident set size in MB. The fast-tier cap of
#                        the workload is <name>_rss x the ratio (run.sh
#                        --ratio, default 0.5, a 1:1 split). run.sh stops if
#                        a workload has no _rss.
#   <name>_vmtouch_file  input file to preload into the slow-tier page cache
#                        before the run (or "")
#   <name>_omp_threads   number of threads, one CPU each. Also
#                        OMP_NUM_THREADS for the run.
#   <name>_workload_cmd  the command line. run.sh replaces "$numactl_args"
#                        with "exec numactl -C <cpus> --": CPU pinning and
#                        default first-touch memory allocation. The exec
#                        keeps the PID of the shell that joined the
#                        workload's cgroup.
#   <name>_requires      (optional) files that must exist before the run
#
# Dataset/benchmark locations come from environment variables so no
# machine-specific path is baked in. Set these to match your machine:
#   GAPBS_DIR        built GAP Benchmark Suite (provides ./bc, ./pr and graphs)
#   GAPBS_GRAPH_DIR  serialized graphs (default: ${GAPBS_DIR}/benchmark/graphs)
#   SPEC_DIR         licensed SPEC CPU 2017 install
#   SILO_DIR         built Silo (out-perf.masstree/benchmarks/dbtest)
#   UBENCH_DIR       ptr_chase / seq_array microbenchmark binaries
#   RESTART_LOOP_SO  restart library for long background workloads
#                    (default: tools/restart_loop.so next to this file)
#
# Background ("_long") workloads must outlast the foreground. run.sh stops
# every background workload when the foreground (the first workload)
# finishes.
#   GAPBS: the same kernel with 100 trials (-n100).
#   SPEC:  the binary restarts in place when it exits (tools/restart_loop.c,
#          loaded with LD_PRELOAD). The PID stays the same, so the cgroup
#          membership and the runtime's view of the process stay valid.

# run.sh reads the variables below through indirection, and the literal
# text $numactl_args stays unexpanded until run.sh starts the workload.
# shellcheck disable=SC2034,SC2016

WORKLOADS_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

GAPBS_DIR="${GAPBS_DIR:-/path/to/gapbs}"
GAPBS_GRAPH_DIR="${GAPBS_GRAPH_DIR:-${GAPBS_DIR}/benchmark/graphs}"
SPEC_DIR="${SPEC_DIR:-/path/to/cpu2017}"
SILO_DIR="${SILO_DIR:-/path/to/silo}"
UBENCH_DIR="${UBENCH_DIR:-/path/to/microbenchmarks}"
RESTART_LOOP_SO="${RESTART_LOOP_SO:-${WORKLOADS_DIR}/tools/restart_loop.so}"

# <name>_rss sets the fast-tier cap, so it must be the workload's real peak
# RSS on your input. It is INPUT-DEPENDENT: for another input, measure
# "Maximum resident set size" with /usr/bin/time -v and update _rss
# (kbytes/1024 -> MB). Workloads without a measured _rss below
# (bwaves_8t, ptr_chase, seq_array) need one before run.sh accepts them.
#
# GRAPH GENERATION: the paper's bc-kron uses a Kronecker graph of scale 27,
# degree 16 (134.2M vertices, 2.11B edges, ~18 GB .sg file, ~19.5 GB RSS).
# Generate it once with the GAPBS converter:
#     ./converter -g27 -k16 -b benchmark/graphs/kron.sg
# The _rss values below are for THAT graph. If you use a different scale
# (e.g. -g25 for a smaller ~4.5 GB / ~5 GB-RSS graph), re-measure with:
#     /usr/bin/time -v ./bc -f kron.sg -i1 -n1   # "Maximum resident set size"
# The urand graph is scale 27, degree 16 as well. The twitter graph is the
# downloaded graph (see run/README.md).

# --- bc_kron_8t : GAP betweenness-centrality on a Kronecker graph, 8 threads
bc_kron_8t_pname="bc"
bc_kron_8t_rss=20000
bc_kron_8t_vmtouch_file="${GAPBS_GRAPH_DIR}/kron.sg"
bc_kron_8t_omp_threads=8
printf -v bc_kron_8t_workload_cmd '%s %q -f %q -i4 -n4' '$numactl_args' \
    "${GAPBS_DIR}/bc" "${GAPBS_GRAPH_DIR}/kron.sg"
bc_kron_8t_requires=("${GAPBS_DIR}/bc" "${GAPBS_GRAPH_DIR}/kron.sg")

# --- bc_kron_4t : same workload, 4 threads (colocation foreground)
bc_kron_4t_pname="bc"
bc_kron_4t_rss=20000
bc_kron_4t_vmtouch_file="${GAPBS_GRAPH_DIR}/kron.sg"
bc_kron_4t_omp_threads=4
bc_kron_4t_workload_cmd="$bc_kron_8t_workload_cmd"
bc_kron_4t_requires=("${bc_kron_8t_requires[@]}")

# --- bc_urand_4t : GAP betweenness-centrality on a uniform random graph,
#     4 threads (colocation foreground)
bc_urand_4t_pname="bc"
bc_urand_4t_rss=20484
bc_urand_4t_vmtouch_file="${GAPBS_GRAPH_DIR}/urand.sg"
bc_urand_4t_omp_threads=4
printf -v bc_urand_4t_workload_cmd '%s %q -f %q -i4 -n1' '$numactl_args' \
    "${GAPBS_DIR}/bc" "${GAPBS_GRAPH_DIR}/urand.sg"
bc_urand_4t_requires=("${GAPBS_DIR}/bc" "${GAPBS_GRAPH_DIR}/urand.sg")

# --- pr_twitter_4t_long : GAP PageRank on the twitter graph, 4 threads,
#     100 trials (colocation background)
pr_twitter_4t_long_pname="pr"
pr_twitter_4t_long_rss=12616
pr_twitter_4t_long_vmtouch_file="${GAPBS_GRAPH_DIR}/twitter.sg"
pr_twitter_4t_long_omp_threads=4
printf -v pr_twitter_4t_long_workload_cmd '%s %q -f %q -i1000 -t1e-4 -n100' \
    '$numactl_args' "${GAPBS_DIR}/pr" "${GAPBS_GRAPH_DIR}/twitter.sg"
pr_twitter_4t_long_requires=("${GAPBS_DIR}/pr" "${GAPBS_GRAPH_DIR}/twitter.sg")

# --- bwaves_8t : SPEC CPU 2017 603.bwaves_s, 8 threads (no measured _rss)
bwaves_8t_pname="speed_bwaves_base.mytest-m64"
bwaves_8t_vmtouch_file=""
bwaves_8t_omp_threads=8
printf -v bwaves_8t_workload_cmd 'cd %q && %s' "${SPEC_DIR}/603.bwaves_s" \
    '$numactl_args ./speed_bwaves_base.mytest-m64 bwaves_1 < bwaves_1.in > bwaves_1.out 2>> bwaves_1.err'
bwaves_8t_requires=("${SPEC_DIR}/603.bwaves_s/speed_bwaves_base.mytest-m64"
    "${SPEC_DIR}/603.bwaves_s/bwaves_1.in")

# --- w_649_8t : SPEC CPU 2017 649.fotonik3d_s, 8 threads
w_649_8t_pname="fotonik3d_s_base.mytest-m64"
w_649_8t_rss=9643
w_649_8t_vmtouch_file=""
w_649_8t_omp_threads=8
printf -v w_649_8t_workload_cmd 'cd %q && %s' "${SPEC_DIR}/649.fotonik3d_s" \
    '$numactl_args ./cmd.sh'
w_649_8t_requires=("${SPEC_DIR}/649.fotonik3d_s/cmd.sh")

# --- w_649_4t_long : SPEC CPU 2017 649.fotonik3d_s, 4 threads, restarted in
#     place (colocation background). ${SPEC_DIR}/649.fotonik3d_s must hold a
#     run directory with a cmd.sh that starts fotonik3d_s_base.mytest-m64 on
#     the reference input. Build the restart library first: make -C tools
w_649_4t_long_pname="fotonik3d_s_base.mytest-m64"
w_649_4t_long_rss=9643
w_649_4t_long_vmtouch_file=""
w_649_4t_long_omp_threads=4
printf -v w_649_4t_long_workload_cmd 'cd %q && %s env LD_PRELOAD=%q ./cmd.sh' \
    "${SPEC_DIR}/649.fotonik3d_s" '$numactl_args' "${RESTART_LOOP_SO}"
w_649_4t_long_requires=("${SPEC_DIR}/649.fotonik3d_s/cmd.sh" "${RESTART_LOOP_SO}")

# --- silo_5t : Silo in-memory DB, YCSB bench, 5 threads
silo_5t_pname="dbtest"
silo_5t_rss=42112
silo_5t_vmtouch_file=""
silo_5t_omp_threads=5
printf -v silo_5t_workload_cmd '%s %q %s' '$numactl_args' \
    "${SILO_DIR}/out-perf.masstree/benchmarks/dbtest" \
    '--verbose --bench ycsb --num-threads 5 --scale-factor 240000 --parallel-loading --runtime 300'
silo_5t_requires=("${SILO_DIR}/out-perf.masstree/benchmarks/dbtest")

# --- ptr_chase : pointer-chasing microbenchmark (latency-bound, no measured _rss)
ptr_chase_pname="ptr_chase"
ptr_chase_vmtouch_file=""
ptr_chase_omp_threads=1
printf -v ptr_chase_workload_cmd 'cd %q && %s' "$UBENCH_DIR" \
    '$numactl_args timeout 200 ./ptr_chase 5120 10'
ptr_chase_requires=("${UBENCH_DIR}/ptr_chase")

# --- seq_array : sequential-array microbenchmark (bandwidth-bound, no measured _rss)
seq_array_pname="seq_array"
seq_array_vmtouch_file=""
seq_array_omp_threads=1
printf -v seq_array_workload_cmd 'cd %q && %s' "$UBENCH_DIR" \
    '$numactl_args timeout 200 ./seq_array 5120 10'
seq_array_requires=("${UBENCH_DIR}/seq_array")
