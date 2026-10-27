#!/bin/bash
#
# Run this once after every boot, before ../../run/run.sh, or let the runner
# call it:  sudo -E ./run.sh --prepare-env <workload> ...
#
# Any failed step stops the script with a non-zero status.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR" || exit 1
source ./cxl-global.sh || { echo "Error: cannot source ./cxl-global.sh" >&2; exit 1; }

# Uncore frequency args (kHz): node0-min node0-max node1-min node1-max
# Default: fast tier (node 0) pinned high, slow tier (node 1) pinned low.
UNCORE_ARGS="${UNCORE_ARGS:-2000000 2000000 500000 500000}"
read -ra uncore_args <<<"$UNCORE_ARGS"

echo "=== [1/4] Setting uncore frequency: $UNCORE_ARGS ==="
./modify-uncore-freq.sh "${uncore_args[@]}"

echo "=== [2/4] Configuring CXL experiment environment (check_cxl_conf) ==="
check_cxl_conf

echo "=== [3/4] Flushing filesystem caches ==="
flush_fs_caches

# PACT opens PEBS + CHA/uncore PMU counters and PEBS-samples the workload's
# (another uid's) accesses. perf_event_paranoid must be permissive or PACT
# collects zero PEBS samples (no per-page criticality, no migration) even when
# running as root. -1 allows full access; use 0 if your policy forbids -1.
echo "=== [4/4] Lowering perf_event_paranoid for PEBS/PMU access ==="
write_knob -1 /proc/sys/kernel/perf_event_paranoid
echo "    perf_event_paranoid = $(cat /proc/sys/kernel/perf_event_paranoid)"

echo "=== prepare_environment.sh DONE ==="
