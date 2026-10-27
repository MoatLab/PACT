#!/bin/bash
#
# Run one or more workloads under the PACT runtime, in PACT mode or in Tierce
# mode. The first workload is the foreground (FG). Every later workload is a
# background (BG) workload. Every workload runs in its own cgroup with a cap
# on its fast-tier (node 0) memory, and the kernel's per-cgroup demotion
# thread (kdemotiond) enforces each cap. Run as root. See README.md.
#
# Steps:
#   1. Check the kernel, load the tiering modules, check the tier layout.
#   2. Prepare the host (optional), check it, set the global kernel switches.
#   3. Preload the workload inputs into the slow-tier page cache (vmtouch).
#   4. Create one cgroup per workload with its fast-tier cap.
#   5. Start the BG workloads, then the FG. Each joins its cgroup before exec.
#   6. Start the runtime with one --workload PID CPUS NAME CGROUP per workload.
#   7. Wait for the FG, stop the BGs, let the runtime exit by itself.
#   8. Remove the cgroups, write the run summary, check the outputs.
#
# Safety: this script never unloads tierinit (unloading it may lock up
# the CPUs) and never sends SIGKILL to the runtime. It writes only finite
# caps to memory.max_at_node0, never "max": the runtime stops at start if a
# workload's cap is not finite.
# It enables each cgroup's memory.demotion at creation. At removal it writes
# "disabled" to stop the cgroup's demotion thread.

set -euo pipefail

RUN_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${RUN_DIR}/.." && pwd)"

# shellcheck source-path=SCRIPTDIR source=workloads.sh
source "${RUN_DIR}/workloads.sh"

# --- Paths (override via env) ---
PACT="${PACT:-${REPO_ROOT}/src/pact}"
VMTOUCH="${VMTOUCH:-vmtouch}"
TIERINIT_KO="${TIERINIT_KO:-${REPO_ROOT}/setup/kernel/tierinit/tierinit.ko}"
KSWAPDRST_KO="${KSWAPDRST_KO:-${REPO_ROOT}/setup/kernel/kswapdrst/kswapdrst.ko}"
PREPARE_ENV_SH="${REPO_ROOT}/setup/env/prepare_environment.sh"

CGROUP_ROOT=/sys/fs/cgroup
RESCTRL_ROOT=/sys/fs/resctrl
# tierinit puts node 0 in memory tier 4 and node 1 in memory tier 45.
TIER_FAST_NODELIST=/sys/devices/virtual/memory_tiering/memory_tier4/nodelist
TIER_SLOW_NODELIST=/sys/devices/virtual/memory_tiering/memory_tier45/nodelist
THP_ENABLED=/sys/kernel/mm/transparent_hugepage/enabled
THP_DEFRAG=/sys/kernel/mm/transparent_hugepage/defrag

# The runtime manages at most this many workloads.
MAX_WORKLOADS=16

# Seconds the runner waits for the runtime to exit by itself once the
# workloads are gone (it checks its workloads about once per second). After
# that it sends one SIGINT. It never sends SIGKILL.
RUNTIME_EXIT_TIMEOUT=60

# Columns of vmstat.csv, in this order.
VMSTAT_COLS="pgmigrate_success pgmigrate_fail pgmigrate_scan pgdemote_kswapd pgdemote_direct pgpromote_success numa_pte_updates numa_huge_pte_updates numa_hint_faults numa_hint_faults_local numa_pages_migrated thp_file_alloc thp_file_mapped"

usage() {
    cat <<EOF
Usage: sudo -E $0 [options] <workload> [<workload> ...] [-- <runtime flags>]

Runs the workloads (names from workloads.sh) under the PACT runtime. The
first workload is the foreground (FG). The runner times it, and in the CAT
arms it gets the protected LLC ways. Every later workload is a background
(BG) workload. The runner stops it when the FG finishes.

Mode and arm:
  --mode pact|tierce   runtime mode (default: pact)
  --arm NAME           arm of the mode (default: pact in PACT mode, wmlp in
                       Tierce mode)

  PACT mode arm:
    pact        one global instance over all workloads: one PAC table,
                threshold and promotion queue, and one global MLP with the
                TID filter off. PEBS period 400
  Tierce mode arms (one domain per workload, per-workload MLP with the TID
  filter on):
    wmlp        per-workload MLP (wMLP), PEBS period 100
    wmlp-ldlat  wMLP with load-latency (LDLAT) sampling, period 5, gate 350
                (no software gate above the hardware floor)
    wmlp-cat    wMLP with a static LLC way split (needs a BG workload)
    tierce      wMLP + LDLAT with gate 700 + static LLC way split (needs a
                BG workload)
    wmlp-dcat   wMLP with dynamic LLC way control, MPKI signal (needs a BG
                workload)

Runtime options (passed to the runtime, default: the runtime's):
  --pebs-period N      PEBS sample period of the slow-tier LLC-miss event,
                       arms without LDLAT only: pact, wmlp, wmlp-cat,
                       wmlp-dcat (runtime default: 400 in PACT mode, 100 in
                       Tierce mode)
  --ldlat-period N     PEBS sample period of the LDLAT event, LDLAT arms
                       only: wmlp-ldlat, tierce (runtime default: 5)
  --ldlat-gate N       LDLAT latency gate in cycles, 350 or 700, wmlp-ldlat
                       and tierce only (default: the arm's gate, 350 for
                       wmlp-ldlat and 700 for tierce)
  --cat-signal NAME    LLC way control signal of the CAT arms: static, mpki,
                       probe, bw or l2lat (default: static for wmlp-cat and
                       tierce, mpki for wmlp-dcat)
  --cat-wmax N         most LLC ways of the FG (with static: its way count),
                       CAT arms only (runtime default: 3*ways/4, 8 of 11 ways
                       on Skylake-SP, 15 of 20 on Emerald Rapids)
  --dump-pac-table     write PAC-table snapshots to <outdir>/pac_table_dump.csv
                       (needs a diagnostic runtime: make -C src DIAG=1)
  --dump-pac-interval SEC
                       snapshot period in seconds (runtime default: 30)
  -- FLAGS...          pass further runtime flags as they are (see
                       src/pact --help), for example -- --cat-wmin 2

Run options:
  --ratio R            fast-tier cap as a share of each workload's RSS
                       (<name>_rss in workloads.sh) (default: 0.5, a 1:1 split)
  --cpus LIST          node-0 CPUs for the workloads, one per thread
                       (default: 2-9). The FG gets the first ones, each BG
                       the next ones. CPU 0 is not allowed.
  --runtime-cpu N      CPU of the runtime's event loop and migration thread
                       (default: 1)
  --outdir DIR         output directory
                       (default: run/results/<wl>[-<wl>...]/<arm>-<timestamp>)
  --prepare-env        run ../setup/env/prepare_environment.sh before
                       the host check
  --skip-setup         skip the page-cache flush and the vmtouch preload
  --thp                set transparent huge pages to "always" for the run
                       (enabled and defrag) and restore both afterwards
  -h, --help           show this help

Environment: PACT (runtime binary, default ../src/pact), VMTOUCH,
TIERINIT_KO, KSWAPDRST_KO, and the data paths of workloads.sh (GAPBS_DIR,
GAPBS_GRAPH_DIR, SPEC_DIR, SILO_DIR, UBENCH_DIR, RESTART_LOOP_SO).
EOF
}

die() {
    echo "ERROR: $*" >&2
    exit 1
}

now() {
    date +%s.%N
}

# elapsed T0 T1 prints T1 - T0 in seconds with 3 decimals.
elapsed() {
    awk -v a="$1" -v b="$2" 'BEGIN { printf "%.3f", b - a }'
}

# is_alive PID: the process exists and is not a zombie.
is_alive() {
    local st
    st=$(ps -o stat= -p "$1" 2>/dev/null) || return 1
    [[ "$st" != Z* ]]
}

join_by_comma() {
    local IFS=,
    echo "$*"
}

# write_check PATH VALUE [REGEX]: write VALUE to PATH, read PATH back, and
# compare the result with REGEX (default: VALUE). Print an error and return
# 1 on any failure.
write_check() {
    local path=$1 value=$2 want=${3:-$2} got
    if [[ ! -e "$path" ]]; then
        echo "ERROR: $path does not exist" >&2
        return 1
    fi
    if ! printf '%s\n' "$value" >"$path"; then
        echo "ERROR: writing '$value' to $path failed" >&2
        return 1
    fi
    if ! got=$(<"$path"); then
        echo "ERROR: reading $path failed" >&2
        return 1
    fi
    if [[ ! "$got" =~ ^(${want})$ ]]; then
        echo "ERROR: $path reads '$got' after writing '$value' (expected: $want)" >&2
        return 1
    fi
    echo "  $path = $got"
}

# write_verify PATH VALUE [REGEX]: write_check, and exit on failure.
write_verify() {
    write_check "$@" || exit 1
}

# expand_cpu_list "2-5,8" prints "2 3 4 5 8".
expand_cpu_list() {
    local spec=$1 part lo hi c
    local -a parts out=()
    IFS=, read -ra parts <<<"$spec"
    for part in "${parts[@]}"; do
        if [[ "$part" =~ ^([0-9]+)-([0-9]+)$ ]]; then
            lo=${BASH_REMATCH[1]}
            hi=${BASH_REMATCH[2]}
            ((lo <= hi)) || die "invalid CPU range '$part'"
            for ((c = lo; c <= hi; c++)); do
                out+=("$c")
            done
        elif [[ "$part" =~ ^[0-9]+$ ]]; then
            out+=("$part")
        else
            die "invalid CPU list '$spec'"
        fi
    done
    echo "${out[*]}"
}

cpu_is_online() {
    local f=/sys/devices/system/cpu/cpu$1/online
    [[ -d /sys/devices/system/cpu/cpu$1 ]] || return 1
    # CPU 0 often has no online file: it cannot go offline.
    [[ -e "$f" ]] || return 0
    [[ "$(<"$f")" == 1 ]]
}

cpu_on_node() {
    [[ -e /sys/devices/system/node/node$2/cpu$1 ]]
}

cpu_family() {
    awk -F': ' '$1 ~ /^cpu family/ {print $2; exit}' /proc/cpuinfo
}

cpu_model() {
    awk -F': ' '$1 ~ /^model[[:space:]]*$/ {print $2; exit}' /proc/cpuinfo
}

# Skylake-SP and Cascade Lake: family 6, model 85.
is_skx() {
    [[ "$(cpu_family)" == 6 && "$(cpu_model)" == 85 ]]
}

module_loaded() {
    grep -q "^${1//-/_} " /proc/modules
}

# load_module NAME KO: insmod KO unless NAME is loaded.
load_module() {
    local name=$1 ko=$2
    if module_loaded "$name"; then
        echo "  module $name: already loaded"
        return 0
    fi
    [[ -f "$ko" ]] || die "kernel module $name is not loaded and $ko is missing. Build it with setup/kernel/build_modules.sh (see setup/kernel/README.md)"
    insmod "$ko" || die "insmod $ko failed"
    module_loaded "$name" || die "$name is not in /proc/modules after insmod"
    echo "  module $name: loaded from $ko"
}

check_tier_layout() {
    local fast slow
    if [[ ! -r "$TIER_FAST_NODELIST" || ! -r "$TIER_SLOW_NODELIST" ]]; then
        die "memory tier files missing ($TIER_FAST_NODELIST, $TIER_SLOW_NODELIST): tierinit did not set up the tiers"
    fi
    fast=$(<"$TIER_FAST_NODELIST")
    slow=$(<"$TIER_SLOW_NODELIST")
    [[ "$fast" == 0 ]] || die "memory_tier4 holds nodes '$fast', expected '0'"
    [[ "$slow" == 1 ]] || die "memory_tier45 holds nodes '$slow', expected '1'"
    echo "  memory_tier4 (fast) = node $fast, memory_tier45 (slow) = node $slow"
}

# check_host_env: read-only check of the state that
# setup/env/prepare_environment.sh sets. Print one line per problem and
# return 1 if there is any.
check_host_env() {
    local bad=0 c g v rc ngov=0
    local -a slow_gov=()

    if (($(wc -l </proc/swaps) > 1)); then
        echo "  NOT PREPARED: swap is on" >&2
        bad=1
    fi
    if [[ "$(</proc/sys/kernel/nmi_watchdog)" != 0 ]]; then
        echo "  NOT PREPARED: nmi_watchdog is on (it takes a PMU counter)" >&2
        bad=1
    fi
    if [[ "$(</proc/sys/kernel/randomize_va_space)" != 0 ]]; then
        echo "  NOT PREPARED: ASLR is on" >&2
        bad=1
    fi
    if [[ -e /sys/kernel/mm/ksm/run && "$(</sys/kernel/mm/ksm/run)" != 0 ]]; then
        echo "  NOT PREPARED: KSM is on" >&2
        bad=1
    fi
    if ((!USE_THP)) && [[ "$(<"$THP_ENABLED")" != *"[never]"* ]]; then
        echo "  NOT PREPARED: transparent huge pages are not 'never' (use --thp to run with THP)" >&2
        bad=1
    fi
    if [[ -e /sys/devices/system/cpu/smt/active && "$(</sys/devices/system/cpu/smt/active)" != 0 ]]; then
        echo "  NOT PREPARED: SMT is on" >&2
        bad=1
    fi
    # intel_pstate has no_turbo. Without intel_pstate, acpi-cpufreq has boost.
    if [[ -e /sys/devices/system/cpu/intel_pstate/no_turbo ]]; then
        if [[ "$(</sys/devices/system/cpu/intel_pstate/no_turbo)" != 1 ]]; then
            echo "  NOT PREPARED: turbo is on" >&2
            bad=1
        fi
    elif [[ -e /sys/devices/system/cpu/cpufreq/boost ]]; then
        if [[ "$(</sys/devices/system/cpu/cpufreq/boost)" != 0 ]]; then
            echo "  NOT PREPARED: turbo (cpufreq boost) is on" >&2
            bad=1
        fi
    else
        # acpi-cpufreq has no boost file on a CPU without turbo. A CPU that
        # reports turbo (the ida or cpb flag) needs a turbo control.
        rc=0
        grep -qwE 'ida|cpb' /proc/cpuinfo || rc=$?
        if [[ $rc -eq 0 ]]; then
            echo "  ERROR: the CPU reports turbo (ida/cpb flag), but there is no turbo control (no intel_pstate/no_turbo, no cpufreq/boost)" >&2
            bad=1
        elif [[ $rc -eq 1 ]]; then
            echo "  NOTE: the CPU reports no turbo (no ida/cpb flag) and there is no turbo control: turbo not checked"
        else
            echo "  ERROR: cannot read /proc/cpuinfo to check for turbo" >&2
            bad=1
        fi
    fi
    v=$(</sys/devices/system/node/node1/cpulist)
    if [[ -n "$v" ]]; then
        echo "  NOT PREPARED: node 1 (slow tier) has online CPUs: $v" >&2
        bad=1
    fi
    for c in $(expand_cpu_list "$(</sys/devices/system/cpu/online)"); do
        g=/sys/devices/system/cpu/cpu$c/cpufreq/scaling_governor
        [[ -e "$g" ]] || continue
        ngov=$((ngov + 1))
        [[ "$(<"$g")" == performance ]] || slow_gov+=("$c")
    done
    if [[ $ngov -eq 0 ]]; then
        echo "  NOT PREPARED: no online CPU has a cpufreq scaling_governor file: the governor cannot be checked" >&2
        bad=1
    fi
    if ((${#slow_gov[@]} > 0)); then
        echo "  NOT PREPARED: governor is not 'performance' on CPUs ${slow_gov[*]}" >&2
        bad=1
    fi
    v=$(</proc/sys/kernel/perf_event_paranoid)
    if ((v > 0)); then
        echo "  NOT PREPARED: kernel.perf_event_paranoid is $v (must be <= 0 for PEBS sampling)" >&2
        bad=1
    fi
    return "$bad"
}

# --- Argument parsing ---
MODE=pact
ARM=""
PEBS_PERIOD=""
LDLAT_PERIOD=""
LDLAT_GATE=""
CAT_SIGNAL=""
CAT_WMAX=""
RATIO=0.5
WORKLOAD_CPUS="2-9"
RUNTIME_CPU=1
OUTDIR=""
PREPARE_ENV=0
SKIP_SETUP=0
USE_THP=0
DUMP_PAC=0
DUMP_PAC_INTERVAL=""
WL_NAME=()
RUNTIME_EXTRA=()

need_value() {
    [[ $# -ge 2 && -n "$2" ]] || die "$1 needs a value"
}

while [[ $# -gt 0 ]]; do
    case "$1" in
    --mode)
        need_value "$@"
        MODE=$2
        shift 2
        ;;
    --arm)
        need_value "$@"
        ARM=$2
        shift 2
        ;;
    --pebs-period)
        need_value "$@"
        PEBS_PERIOD=$2
        shift 2
        ;;
    --ldlat-period)
        need_value "$@"
        LDLAT_PERIOD=$2
        shift 2
        ;;
    --ldlat-gate)
        need_value "$@"
        LDLAT_GATE=$2
        shift 2
        ;;
    --cat-signal)
        need_value "$@"
        CAT_SIGNAL=$2
        shift 2
        ;;
    --cat-wmax)
        need_value "$@"
        CAT_WMAX=$2
        shift 2
        ;;
    --dump-pac-table)
        DUMP_PAC=1
        shift
        ;;
    --dump-pac-interval)
        need_value "$@"
        DUMP_PAC_INTERVAL=$2
        shift 2
        ;;
    --ratio)
        need_value "$@"
        RATIO=$2
        shift 2
        ;;
    --cpus)
        need_value "$@"
        WORKLOAD_CPUS=$2
        shift 2
        ;;
    --runtime-cpu)
        need_value "$@"
        RUNTIME_CPU=$2
        shift 2
        ;;
    --outdir)
        need_value "$@"
        OUTDIR=$2
        shift 2
        ;;
    --prepare-env)
        PREPARE_ENV=1
        shift
        ;;
    --skip-setup)
        SKIP_SETUP=1
        shift
        ;;
    --thp)
        USE_THP=1
        shift
        ;;
    -h | --help)
        usage
        exit 0
        ;;
    --)
        shift
        RUNTIME_EXTRA=("$@")
        break
        ;;
    -*)
        usage >&2
        die "unknown option: $1"
        ;;
    *)
        WL_NAME+=("$1")
        shift
        ;;
    esac
done

NWL=${#WL_NAME[@]}
if ((NWL < 1)); then
    usage >&2
    die "give at least one workload"
fi
((NWL <= MAX_WORKLOADS)) || die "at most $MAX_WORKLOADS workloads (got $NWL)"

# --- Mode and arm definitions ---
# Each arm maps to runtime flags. USE_LDLAT/USE_CAT turn on --use-ldlat and
# --use-cat. CAT_DEFAULT is the arm's signal. GATE_DEFAULT is the arm's
# LDLAT gate. The runner always passes the gate of an LDLAT arm as
# --ldlat-gate: 350 for wmlp-ldlat (no software gate) and 700 for tierce.
USE_LDLAT=0
USE_CAT=0
CAT_DEFAULT=static
GATE_DEFAULT=""

case "$MODE" in
pact) ARM=${ARM:-pact} ;;
tierce) ARM=${ARM:-wmlp} ;;
*) die "--mode must be pact or tierce" ;;
esac

case "$ARM" in
pact) ARM_MODE=pact ;;
wmlp) ARM_MODE=tierce ;;
wmlp-ldlat)
    ARM_MODE=tierce
    USE_LDLAT=1
    GATE_DEFAULT=350
    ;;
wmlp-cat)
    ARM_MODE=tierce
    USE_CAT=1
    ;;
tierce)
    ARM_MODE=tierce
    USE_LDLAT=1
    GATE_DEFAULT=700
    USE_CAT=1
    ;;
wmlp-dcat)
    ARM_MODE=tierce
    USE_CAT=1
    CAT_DEFAULT=mpki
    ;;
*)
    usage >&2
    die "unknown arm: $ARM"
    ;;
esac
[[ "$ARM_MODE" == "$MODE" ]] ||
    die "arm $ARM belongs to $ARM_MODE mode. Run it with --mode $ARM_MODE"

# --- Option checks (each option belongs to the arms that use its flag) ---
if ((USE_LDLAT)); then
    [[ -z "$PEBS_PERIOD" ]] ||
        die "--pebs-period applies to the arms without LDLAT. Arm $ARM samples the LDLAT event: use --ldlat-period"
elif [[ -n "$LDLAT_PERIOD" ]]; then
    die "--ldlat-period applies only to the LDLAT arms (wmlp-ldlat, tierce). Arm $ARM uses --pebs-period"
fi
[[ -z "$PEBS_PERIOD" || "$PEBS_PERIOD" =~ ^[1-9][0-9]*$ ]] || die "--pebs-period must be a positive integer"
[[ -z "$LDLAT_PERIOD" || "$LDLAT_PERIOD" =~ ^[1-9][0-9]*$ ]] || die "--ldlat-period must be a positive integer"
if ((USE_LDLAT)); then
    LDLAT_GATE=${LDLAT_GATE:-$GATE_DEFAULT}
    [[ "$LDLAT_GATE" == "350" || "$LDLAT_GATE" == "700" ]] || die "--ldlat-gate must be 350 or 700"
elif [[ -n "$LDLAT_GATE" ]]; then
    die "--ldlat-gate applies only to the wmlp-ldlat and tierce arms"
fi
if ((USE_CAT)); then
    CAT_SIGNAL=${CAT_SIGNAL:-$CAT_DEFAULT}
    case "$CAT_SIGNAL" in
    static | mpki | probe | bw | l2lat) ;;
    *) die "--cat-signal must be static, mpki, probe, bw or l2lat" ;;
    esac
    ((NWL >= 2)) || die "arm $ARM splits the LLC between the FG and the BG workloads and needs a BG workload"
else
    [[ -z "$CAT_SIGNAL" ]] || die "--cat-signal applies only to the CAT arms (wmlp-cat, tierce, wmlp-dcat)"
    [[ -z "$CAT_WMAX" ]] || die "--cat-wmax applies only to the CAT arms (wmlp-cat, tierce, wmlp-dcat)"
fi
[[ -z "$CAT_WMAX" || "$CAT_WMAX" =~ ^[1-9][0-9]*$ ]] || die "--cat-wmax must be a positive integer"
[[ "$RUNTIME_CPU" =~ ^[0-9]+$ ]] || die "--runtime-cpu must be a CPU number"
((RUNTIME_CPU != 0)) || die "--runtime-cpu must not be CPU 0 (CPU 0 stays free for login shells and the kswapdrst work)"
if [[ -n "$DUMP_PAC_INTERVAL" ]] && ((!DUMP_PAC)); then
    die "--dump-pac-interval needs --dump-pac-table"
fi
[[ -z "$DUMP_PAC_INTERVAL" || "$DUMP_PAC_INTERVAL" =~ ^[1-9][0-9]*$ ]] || die "--dump-pac-interval must be a positive integer"
awk -v r="$RATIO" 'BEGIN { exit !(r ~ /^[0-9]*\.?[0-9]+$/ && r > 0 && r <= 1) }' ||
    die "--ratio must be in (0, 1]"
# Flags that the runner sets itself cannot come through "--".
for f in "${RUNTIME_EXTRA[@]}"; do
    case "$f" in
    --mode | --workload | --use-ldlat | --use-cat | --cat-signal | --cat-wmax | \
        --pebs-period | --ldlat-period | --ldlat-gate | --monitor-cpu | --migration-cpu | \
        --trace-migration | --dump-pac-table | --dump-pac-interval)
        die "runtime flag $f comes from the arm or a runner option. Do not pass it after --"
        ;;
    esac
done

# --- Workload definitions ---
declare -a WL_PNAME WL_RSS WL_VMTOUCH WL_THREADS WL_CMD WL_CPUS WL_CGROUP WL_CAP_MB
for ((i = 0; i < NWL; i++)); do
    name=${WL_NAME[i]}
    [[ "$name" =~ ^[A-Za-z_][A-Za-z0-9_]*$ ]] || die "invalid workload name '$name'"
    v="${name}_pname"
    WL_PNAME[i]=${!v:-}
    v="${name}_rss"
    WL_RSS[i]=${!v:-}
    v="${name}_vmtouch_file"
    WL_VMTOUCH[i]=${!v:-}
    v="${name}_omp_threads"
    WL_THREADS[i]=${!v:-}
    v="${name}_workload_cmd"
    WL_CMD[i]=${!v:-}
    if [[ -z "${WL_PNAME[i]}" || -z "${WL_THREADS[i]}" || -z "${WL_CMD[i]}" ]]; then
        die "workload '$name' is not defined completely in workloads.sh"
    fi
    [[ -n "${WL_RSS[i]}" ]] ||
        die "workload '$name' has no ${name}_rss in workloads.sh. Measure its peak RSS (see run/README.md) and set it"
    [[ "${WL_RSS[i]}" =~ ^[1-9][0-9]*$ ]] || die "${name}_rss must be a positive integer (MB)"
    [[ "${WL_THREADS[i]}" =~ ^[1-9][0-9]*$ ]] || die "${name}_omp_threads must be a positive integer"
    WL_CGROUP[i]="${name}_${i}"
    WL_CAP_MB[i]=$(awk -v r="${WL_RSS[i]}" -v f="$RATIO" 'BEGIN { printf "%d", r * f }')
    ((WL_CAP_MB[i] > 0)) || die "the cap of workload '$name' is 0 MB (${name}_rss x --ratio)"
done

# --- CPU allocation: FG first, then each BG, from the workload CPU list ---
# expand_cpu_list runs in a subshell here, so its die() ends only that
# subshell: test its status.
cpu_slots=$(expand_cpu_list "$WORKLOAD_CPUS") || exit 1
read -ra CPU_SLOTS <<<"$cpu_slots"
# Compare CPU numbers as integers, so "2,02" repeats CPU 2.
declare -A cpu_seen=()
for c in "${CPU_SLOTS[@]}"; do
    n=$((10#$c))
    [[ -z "${cpu_seen[$n]:-}" ]] || die "CPU $n appears twice in --cpus '$WORKLOAD_CPUS'"
    cpu_seen[$n]=1
done
next=0
for ((i = 0; i < NWL; i++)); do
    cpus=()
    for ((t = 0; t < WL_THREADS[i]; t++)); do
        ((next < ${#CPU_SLOTS[@]})) || die "not enough CPUs in '$WORKLOAD_CPUS' for $NWL workload(s)"
        cpus+=("${CPU_SLOTS[next]}")
        next=$((next + 1))
    done
    WL_CPUS[i]=$(join_by_comma "${cpus[@]}")
done
USED_CPUS=("${CPU_SLOTS[@]:0:next}")

[[ $EUID -eq 0 ]] || die "run as root: sudo -E $0 ..."

[[ -x "$PACT" ]] || die "runtime binary $PACT not found. Build it with 'make -C src' (or set PACT)"
if ((DUMP_PAC)); then
    # --help lists --dump-pac-table only in a diagnostic build.
    rt_help=$("$PACT" --help 2>&1) || die "$PACT --help failed"
    [[ "$rt_help" == *--dump-pac-table* ]] ||
        die "$PACT is not a diagnostic build. Build it with 'make -C src DIAG=1' (or set PACT)"
fi

# --- Output directory and run log ---
if [[ -z "$OUTDIR" ]]; then
    OUTDIR="${RUN_DIR}/results/$(
        IFS=-
        echo "${WL_NAME[*]}"
    )/${ARM}-$(date +%Y%m%d-%H%M%S)"
fi
[[ ! -e "$OUTDIR" ]] || die "output directory $OUTDIR exists"
mkdir -p "$OUTDIR"
OUTDIR=$(realpath "$OUTDIR")
# The log copy ignores SIGINT, SIGTERM and SIGHUP so that cleanup output is
# not lost on Ctrl+C or when the terminal goes away.
exec > >(
    trap '' INT TERM HUP
    exec tee -a "${OUTDIR}/run.log"
) 2>&1

# --- State used by cleanup ---
declare -a LAUNCHER_PID TARGET_PID LAUNCHER_REAPED LAUNCH_TIME NUMASTAT_PID
SYS_MON_PID=""
RUNTIME_PID=""
RUNTIME_REAPED=0
RUNTIME_STATUS=""
RUNTIME_STUCK=0
CGROUPS_CREATED=0
THP_CHANGED=0
PREV_THP=""
PREV_THP_DEFRAG=""
NUMA_BAL_CHANGED=0
PREV_NUMA_BAL=""
DEMOTION_CHANGED=0
PREV_DEMOTION=""
CLEANED=0
CLEANUP_FAILED=0

# ---------------------------------------------------------------------------
# Helpers that use the run state
# ---------------------------------------------------------------------------

# stop_loop PID: stop a monitor loop and its current child. A loop that is
# already gone is not an error, so errexit is off inside this function.
stop_loop() {
    local -
    set +e
    local pid=$1
    [[ -n "$pid" ]] || return 0
    if kill -STOP "$pid" 2>/dev/null; then
        # A stopped loop cannot start a new child: stop the child, then the loop.
        pkill -TERM -P "$pid" 2>/dev/null
        kill -TERM "$pid" 2>/dev/null
        kill -CONT "$pid" 2>/dev/null
    fi
    wait "$pid" 2>/dev/null
    return 0
}

# find_target_pid LAUNCHER PNAME: the PID of the workload binary, searched in
# the process tree of the launching shell by its 15-character comm name.
find_target_pid() {
    local want p kids
    local -a queue kids_arr
    want=$(basename -- "$2")
    want=${want:0:15}
    queue=("$1")
    while ((${#queue[@]} > 0)); do
        p=${queue[0]}
        queue=("${queue[@]:1}")
        if [[ "$(cat "/proc/$p/comm" 2>/dev/null)" == "$want" ]]; then
            echo "$p"
            return 0
        fi
        if kids=$(pgrep -P "$p"); then
            read -ra kids_arr <<<"${kids//$'\n'/ }"
            queue+=("${kids_arr[@]}")
        fi
    done
    return 1
}

# check_own_session PID WHAT: setsid must not fork, so that PID is the
# process itself (job control is off in this script, see "set +m" below).
check_own_session() {
    local sid
    sid=$(ps -o sid= -p "$1" | tr -d ' ')
    [[ "$sid" == "$1" ]] || die "$2 (PID $1) is not in its own session (session $sid)"
}

# create_cgroup NAME CAP_BYTES: a cgroup with per-node limits on, demotion
# on, and the fast-tier cap in memory.max_at_node0.
create_cgroup() {
    local cg=$1 bytes=$2 d=${CGROUP_ROOT}/$1 c
    if [[ -d "$d" ]]; then
        [[ -z "$(<"$d/cgroup.procs")" ]] || die "cgroup $cg exists and has processes"
        echo "  removing the stale cgroup $cg"
        remove_cgroup "$cg" || die "could not remove the stale cgroup $cg"
    fi
    for c in memory cpuset; do
        if ! grep -qw "$c" "${CGROUP_ROOT}/cgroup.subtree_control"; then
            printf '+%s\n' "$c" >"${CGROUP_ROOT}/cgroup.subtree_control" ||
                die "enabling the $c controller failed"
            grep -qw "$c" "${CGROUP_ROOT}/cgroup.subtree_control" ||
                die "the $c controller is not enabled in ${CGROUP_ROOT}/cgroup.subtree_control"
        fi
    done
    mkdir "$d"
    CGROUPS_CREATED=1
    [[ -e "$d/memory.max_at_node0" ]] ||
        die "$d/memory.max_at_node0 is missing: the running kernel has no per-cgroup fast-tier cap (see setup/kernel/README.md)"
    write_verify "$d/memory.mmctl_enabled" enabled '\[enabled\] disabled'
    write_verify "$d/memory.demotion" enabled '\[enabled\] disabled'
    write_verify "$d/memory.max_at_node0" "$bytes"
}

# remove_cgroup NAME: stop its demotion thread and remove it. The cap goes
# with the cgroup, so it does not write memory.max_at_node0. Returns 1 on
# failure (used during cleanup).
remove_cgroup() {
    local cg=$1 d=${CGROUP_ROOT}/$1
    [[ -d "$d" ]] || return 0
    if [[ -n "$(<"$d/cgroup.procs")" ]]; then
        echo "ERROR: cgroup $cg still has processes: $(tr '\n' ' ' <"$d/cgroup.procs")" >&2
        return 1
    fi
    # A kernel without the core patch has no memory.demotion, and so no
    # demotion thread to stop.
    if [[ -e "$d/memory.demotion" ]]; then
        write_check "$d/memory.demotion" disabled 'enabled \[disabled\]' || return 1
    fi
    if ! rmdir "$d"; then
        echo "ERROR: rmdir $d failed" >&2
        return 1
    fi
    echo "  removed cgroup $cg"
}

# start_workload I: start workload I in its own session. The launching shell
# writes its PID into the cgroup before it execs the workload, so every page
# the workload allocates is charged to the cgroup.
start_workload() {
    local i=$1 launch numactl_args
    local -a envs
    launch="echo \$\$ > ${CGROUP_ROOT}/${WL_CGROUP[i]}/cgroup.procs && ${WL_CMD[i]}"
    numactl_args="exec numactl -C ${WL_CPUS[i]} --"
    envs=("numactl_args=${numactl_args}" "OMP_NUM_THREADS=${WL_THREADS[i]}")
    if [[ "${WL_CMD[i]}" == *restart_loop.so* ]]; then
        envs+=("RESTART_LOOP_LOG=${OUTDIR}/workload_${i}_restart_loop.csv")
    fi
    echo "--- Starting workload $i: ${WL_NAME[i]} (CPUs ${WL_CPUS[i]}) ---"
    echo "  command: ${WL_CMD[i]}"
    echo "  numactl_args: ${numactl_args}"
    env "${envs[@]}" setsid bash -c "$launch" >"${OUTDIR}/workload_${i}.output" 2>&1 &
    LAUNCHER_PID[i]=$!
    LAUNCH_TIME[i]=$(now)
    LAUNCHER_REAPED[i]=0

    sleep 2
    if ! is_alive "${LAUNCHER_PID[i]}"; then
        tail -n 20 "${OUTDIR}/workload_${i}.output" >&2
        die "workload $i (${WL_NAME[i]}) exited right after start"
    fi
    check_own_session "${LAUNCHER_PID[i]}" "workload $i"
    local n target=""
    for ((n = 0; n < 60; n++)); do
        if target=$(find_target_pid "${LAUNCHER_PID[i]}" "${WL_PNAME[i]}"); then
            break
        fi
        target=""
        sleep 0.5
    done
    [[ -n "$target" ]] || die "no process named ${WL_PNAME[i]} under PID ${LAUNCHER_PID[i]}"
    TARGET_PID[i]=$target
    echo "  launcher PID ${LAUNCHER_PID[i]}, workload PID $target ($(cat "/proc/$target/comm"))"
    grep -qx "0::/${WL_CGROUP[i]}" "/proc/$target/cgroup" ||
        die "workload PID $target is not in cgroup ${WL_CGROUP[i]}: $(cat "/proc/$target/cgroup")"
    echo "  workload PID $target is in cgroup ${WL_CGROUP[i]}"
    sleep 1
    start_numastat_monitor "$i" "$target"
}

start_system_monitor() {
    (
        set +e
        t0=$(now)
        {
            echo "# start_epoch: $t0"
            echo "timestamp,${VMSTAT_COLS// /,}"
        } >"${OUTDIR}/vmstat.csv"
        for ((j = 0; j < NWL; j++)); do
            {
                echo "# start_epoch: $t0"
                echo "timestamp,promotions,pgsteal_kswapd,pgsteal_direct,pgsteal_khugepaged,pgdemote_kswapd,pgdemote_direct"
            } >"${OUTDIR}/workload_${j}_cgroup.csv"
        done
        while true; do
            t=$(elapsed "$t0" "$(now)")
            awk -v t="$t" -v cols="$VMSTAT_COLS" '
                { v[$1] = $2 }
                END {
                    n = split(cols, c, " ")
                    printf "%s", t
                    for (k = 1; k <= n; k++) printf ",%s", ((c[k] in v) ? v[c[k]] : 0)
                    printf "\n"
                }' /proc/vmstat >>"${OUTDIR}/vmstat.csv"
            for ((j = 0; j < NWL; j++)); do
                d=${CGROUP_ROOT}/${WL_CGROUP[j]}
                [[ -r "$d/memory.stat" ]] || continue
                promo=0
                if [[ -r "$d/memory.promotion_counter" ]]; then
                    promo=$(<"$d/memory.promotion_counter")
                    promo=${promo#*:}
                fi
                awk -v t="$t" -v p="$promo" '
                    $1 == "pgsteal_kswapd" { a = $2 }
                    $1 == "pgsteal_direct" { b = $2 }
                    $1 == "pgsteal_khugepaged" { c = $2 }
                    $1 == "pgdemote_kswapd" { d = $2 }
                    $1 == "pgdemote_direct" { e = $2 }
                    END { printf "%s,%s,%d,%d,%d,%d,%d\n", t, p, a, b, c, d, e }
                ' "$d/memory.stat" >>"${OUTDIR}/workload_${j}_cgroup.csv"
            done
            sleep 1
        done
    ) &
    SYS_MON_PID=$!
}

# start_numastat_monitor I PID: node-0 and node-1 memory of PID, once per second.
start_numastat_monitor() {
    local i=$1 pid=$2 f=${OUTDIR}/workload_$1_numastat.log
    (
        set +e
        t0=$(now)
        {
            echo "# start_epoch: $t0"
            echo "timestamp,node0_mb,node1_mb"
        } >"$f"
        while true; do
            t=$(elapsed "$t0" "$(now)")
            numastat -p "$pid" 2>/dev/null |
                awk -v t="$t" '$1 == "Total" { printf "%s,%.1f,%.1f\n", t, $2, $3 }' >>"$f"
            sleep 1
        done
    ) &
    NUMASTAT_PID[i]=$!
}

# stop_workload I: SIGTERM to the workload's process group (its session).
# After 15 s, SIGKILL. The whole group gets the signal so that a wrapper
# script under the restart library does not start the binary again. A
# launcher that is already reaped (the FG after its wait) still gets the
# check for processes left in its group.
# The runner can kill workloads. It never kills the runtime.
stop_workload() {
    local i=$1 l=${LAUNCHER_PID[$1]:-} n st=0
    [[ -n "$l" ]] || return 0
    if [[ "${LAUNCHER_REAPED[i]:-0}" != 0 ]]; then
        kill_group_leftovers "$l"
        return 0
    fi
    if is_alive "$l"; then
        echo "  stopping workload $i (${WL_NAME[i]}): SIGTERM to process group $l"
        if ! kill -TERM -- "-$l" 2>/dev/null; then
            echo "  process group $l already gone"
        fi
        for ((n = 0; n < 30; n++)); do
            is_alive "$l" || break
            sleep 0.5
        done
        if is_alive "$l"; then
            echo "  workload $i still running after 15 s: SIGKILL to process group $l"
            if ! kill -KILL -- "-$l" 2>/dev/null; then
                echo "  process group $l already gone"
            fi
        fi
    fi
    wait "$l" 2>/dev/null || st=$?
    echo "  workload $i (${WL_NAME[i]}) ended with status $st"
    kill_group_leftovers "$l"
    LAUNCHER_REAPED[i]=1
}

# kill_group_leftovers PGID: SIGKILL to processes left in a workload's
# process group, then wait up to 10 s until the group is empty.
kill_group_leftovers() {
    local l=$1 n
    pgrep -g "$l" >/dev/null || return 0
    echo "  processes left in group $l: SIGKILL"
    if ! kill -KILL -- "-$l" 2>/dev/null; then
        echo "  process group $l already gone"
    fi
    for ((n = 0; n < 20; n++)); do
        pgrep -g "$l" >/dev/null || return 0
        sleep 0.5
    done
    echo "ERROR: processes are still in group $l after SIGKILL: $(pgrep -g "$l" | tr '\n' ' ')" >&2
}

# wait_runtime_exit: the runtime exits by itself about one second after all
# of its workloads are gone. It never gets SIGKILL from this script.
wait_runtime_exit() {
    local n
    [[ -n "$RUNTIME_PID" ]] || return 0
    ((RUNTIME_REAPED == 0)) || return 0
    echo "  waiting for the runtime (PID $RUNTIME_PID) to exit by itself"
    for ((n = 0; n < RUNTIME_EXIT_TIMEOUT * 2; n++)); do
        is_alive "$RUNTIME_PID" || break
        sleep 0.5
    done
    if is_alive "$RUNTIME_PID"; then
        echo "  runtime still running after ${RUNTIME_EXIT_TIMEOUT} s: sending SIGINT (graceful stop)"
        if ! kill -INT "$RUNTIME_PID"; then
            echo "  runtime exited before the SIGINT"
        fi
        for ((n = 0; n < 120; n++)); do
            is_alive "$RUNTIME_PID" || break
            sleep 0.5
        done
    fi
    if is_alive "$RUNTIME_PID"; then
        echo "ERROR: the runtime (PID $RUNTIME_PID) did not exit. It is left running on purpose." >&2
        echo "       See ${OUTDIR}/pact_debug.log. Stop it with SIGINT, not SIGKILL." >&2
        RUNTIME_STUCK=1
        return 1
    fi
    RUNTIME_STATUS=0
    wait "$RUNTIME_PID" || RUNTIME_STATUS=$?
    RUNTIME_REAPED=1
    echo "  runtime exited with status $RUNTIME_STATUS"
}

snapshot_cgroups() {
    local i d
    for ((i = 0; i < NWL; i++)); do
        d=${CGROUP_ROOT}/${WL_CGROUP[i]}
        [[ -d "$d" ]] || continue
        {
            echo "# memory.max_at_node0: $(<"$d/memory.max_at_node0")"
            echo "# memory.demotion: $(<"$d/memory.demotion")"
            echo "# memory.promotion_counter: $(cat "$d/memory.promotion_counter" 2>/dev/null)"
            cat "$d/memory.stat"
        } >"${OUTDIR}/workload_${i}_memory.stat"
    done
}

# restore_kernel_state: each global switch and the THP policy back to its
# value before the run. It writes only the settings that this run changed.
# A run that stops before step 2 changed none of them. The modules stay
# loaded. The runner never unloads tierinit.
restore_kernel_state() {
    local ok=0
    if ((NUMA_BAL_CHANGED)); then
        write_check /proc/sys/kernel/numa_balancing "$PREV_NUMA_BAL" || ok=1
        NUMA_BAL_CHANGED=0
    fi
    if ((DEMOTION_CHANGED)); then
        if ((PREV_DEMOTION)); then
            write_check /sys/kernel/mm/numa/demotion_enabled 1 '1|true' || ok=1
        else
            write_check /sys/kernel/mm/numa/demotion_enabled 0 '0|false' || ok=1
        fi
        DEMOTION_CHANGED=0
    fi
    if ((THP_CHANGED)); then
        # The defrag policy "defer+madvise" holds a "+": escape it for the regex.
        write_check "$THP_ENABLED" "$PREV_THP" ".*\[${PREV_THP}\].*" || ok=1
        write_check "$THP_DEFRAG" "$PREV_THP_DEFRAG" ".*\[${PREV_THP_DEFRAG//+/\\+}\].*" || ok=1
        THP_CHANGED=0
    fi
    return "$ok"
}

# remove_cat_groups: remove the runtime's LLC way groups if they are still
# there. The runtime removes them at exit. A runtime that crashed leaves
# them, and their CPUs keep the FG and BG way masks until the groups go.
# rmdir gives their CPUs and tasks back to the default group.
remove_cat_groups() {
    local g ok=0
    for g in tierce_fg tierce_bg; do
        [[ -d "${RESCTRL_ROOT}/$g" ]] || continue
        echo "  removing the LLC way group $g that the runtime left"
        if ! rmdir "${RESCTRL_ROOT}/$g"; then
            echo "ERROR: could not remove the resctrl group $g" >&2
            ok=1
        fi
    done
    return "$ok"
}

# cleanup: idempotent. It runs on normal completion and on any exit.
cleanup() {
    local had_e=0 i
    ((CLEANED == 0)) || return 0
    CLEANED=1
    [[ $- == *e* ]] && had_e=1
    set +e
    trap '' INT TERM HUP
    echo "=== Cleanup ==="
    for ((i = NWL - 1; i >= 0; i--)); do
        stop_workload "$i"
    done
    wait_runtime_exit
    if ((USE_CAT && RUNTIME_STUCK == 0)); then
        remove_cat_groups || CLEANUP_FAILED=1
    fi
    stop_loop "$SYS_MON_PID"
    for ((i = 0; i < NWL; i++)); do
        stop_loop "${NUMASTAT_PID[i]:-}"
    done
    if ((CGROUPS_CREATED)); then
        snapshot_cgroups
        for ((i = 0; i < NWL; i++)); do
            remove_cgroup "${WL_CGROUP[i]}" || CLEANUP_FAILED=1
        done
    fi
    restore_kernel_state || CLEANUP_FAILED=1
    cat /proc/vmstat >"${OUTDIR}/after_vmstat.log"
    trap on_interrupt INT TERM HUP
    if ((had_e)); then
        set -e
    fi
    return 0
}

# shellcheck disable=SC2329  # called by the EXIT trap
on_exit() {
    local rc=$?
    cleanup
    if ((CLEANUP_FAILED && rc == 0)); then
        rc=1
    fi
    exit "$rc"
}

# shellcheck disable=SC2329  # called by the INT, TERM and HUP traps
on_interrupt() {
    echo "Interrupted"
    exit 130
}

# Background jobs stay in this script's process group, so setsid below
# never forks and $! is the started process itself.
set +m
trap on_exit EXIT
# HUP: a dropped ssh session runs the same cleanup as Ctrl+C.
trap on_interrupt INT TERM HUP

# ---------------------------------------------------------------------------
# Run
# ---------------------------------------------------------------------------

echo "=== Run: mode=$MODE arm=$ARM workloads=${WL_NAME[*]} ==="
for ((i = 0; i < NWL; i++)); do
    role=FG
    ((i == 0)) || role=BG
    echo "  $role ${WL_NAME[i]}: CPUs ${WL_CPUS[i]}, RSS ${WL_RSS[i]} MB, cap ${WL_CAP_MB[i]} MB on node 0 (ratio $RATIO)"
done
echo "  runtime CPU: $RUNTIME_CPU"
echo "  output: $OUTDIR"

# --- Step 1: kernel, modules, tier layout ---
echo "=== [1/8] Kernel, modules, tier layout ==="
echo "  kernel $(uname -r), CPU family $(cpu_family) model $(cpu_model)"
[[ "$(stat -f -c %T "$CGROUP_ROOT")" == cgroup2fs ]] ||
    die "$CGROUP_ROOT is not cgroup v2 (boot with systemd.unified_cgroup_hierarchy=yes, see setup/kernel/README.md)"
[[ -e /sys/devices/system/node/node1 ]] || die "node 1 (slow tier) not found"
cmdline=" $(</proc/cmdline) "
if ((USE_CAT)) && is_skx && [[ ! "$cmdline" =~ \ rdt=[^\ ]*l3cat ]]; then
    die "LLC way control needs the boot argument rdt=cmt,mbmtotal,mbmlocal,l3cat on Skylake-SP (see setup/kernel/README.md)"
fi
if [[ "$cmdline" != *" mitigations=off "* ]]; then
    echo "  NOTE: the boot arguments lack mitigations=off (see setup/kernel/README.md)"
fi
for tool in numactl numastat setsid pgrep awk insmod; do
    command -v "$tool" >/dev/null || die "$tool not found"
done
for ((i = 0; i < NWL; i++)); do
    if [[ -n "${WL_VMTOUCH[i]}" ]] && ((!SKIP_SETUP)); then
        command -v "$VMTOUCH" >/dev/null || die "vmtouch not found (set VMTOUCH)"
    fi
    if declare -p "${WL_NAME[i]}_requires" >/dev/null 2>&1; then
        v="${WL_NAME[i]}_requires[@]"
        for p in "${!v}"; do
            [[ -e "$p" ]] || die "${WL_NAME[i]}: $p not found (check the data paths in workloads.sh)"
        done
    fi
done
if pgrep -x "$(basename -- "$PACT")" >/dev/null; then
    die "a runtime process ($(basename -- "$PACT")) is already running"
fi
for c in "${USED_CPUS[@]}" "$RUNTIME_CPU"; do
    ((c != 0)) || die "CPU 0 is in --cpus. CPU 0 stays free for login shells and the kswapdrst work"
    cpu_on_node "$c" 0 || die "CPU $c is not on node 0"
done
for c in "${USED_CPUS[@]}"; do
    [[ "$c" != "$RUNTIME_CPU" ]] || die "runtime CPU $RUNTIME_CPU is also a workload CPU"
done
# Load order: tierinit, then kswapdrst. tierinit stays loaded until reboot.
load_module tierinit "$TIERINIT_KO"
load_module kswapdrst "$KSWAPDRST_KO"
check_tier_layout

# --- Step 2: host preparation and global kernel switches ---
echo "=== [2/8] Host ==="
if ((PREPARE_ENV)); then
    "$PREPARE_ENV_SH"
fi
if ! check_host_env; then
    die "the host is not prepared: run setup/env/prepare_environment.sh or pass --prepare-env"
fi
for c in "${USED_CPUS[@]}" "$RUNTIME_CPU"; do
    cpu_is_online "$c" || die "CPU $c is offline"
done
echo "  host settings: ok"
for g in tierce_fg tierce_bg; do
    if [[ -d "${RESCTRL_ROOT}/$g" ]]; then
        rmdir "${RESCTRL_ROOT}/$g" || die "could not remove the stale resctrl group $g"
        [[ ! -d "${RESCTRL_ROOT}/$g" ]] || die "resctrl group $g still exists"
        echo "  removed the stale resctrl group $g"
    fi
done
# The global demotion switch stays off: kdemotiond enforces each cgroup's
# cap. Each cgroup's memory.demotion is enabled from its creation to its
# removal. Cleanup writes the values from before the run back to both
# switches.
PREV_DEMOTION=$(</sys/kernel/mm/numa/demotion_enabled) || die "cannot read /sys/kernel/mm/numa/demotion_enabled"
case "$PREV_DEMOTION" in
    0 | false) PREV_DEMOTION=0 ;;
    1 | true) PREV_DEMOTION=1 ;;
    *) die "/sys/kernel/mm/numa/demotion_enabled reads '$PREV_DEMOTION' (expected 0, 1, false or true)" ;;
esac
PREV_NUMA_BAL=$(</proc/sys/kernel/numa_balancing) || die "cannot read /proc/sys/kernel/numa_balancing"
[[ "$PREV_NUMA_BAL" =~ ^[0-9]+$ ]] ||
    die "/proc/sys/kernel/numa_balancing reads '$PREV_NUMA_BAL' (expected an integer)"
echo "  before the run: demotion_enabled=$PREV_DEMOTION numa_balancing=$PREV_NUMA_BAL"
DEMOTION_CHANGED=1
write_verify /sys/kernel/mm/numa/demotion_enabled 0 '0|false'
NUMA_BAL_CHANGED=1
write_verify /proc/sys/kernel/numa_balancing 0
if ((USE_THP)); then
    PREV_THP=$(grep -oE '\[[a-z]+\]' "$THP_ENABLED" | tr -d '[]')
    PREV_THP_DEFRAG=$(grep -oE '\[[a-z+]+\]' "$THP_DEFRAG" | tr -d '[]')
    [[ -n "$PREV_THP" && -n "$PREV_THP_DEFRAG" ]] || die "cannot read the THP policy"
    THP_CHANGED=1
    write_verify "$THP_ENABLED" always '.*\[always\].*'
    write_verify "$THP_DEFRAG" always '.*\[always\].*'
fi
if ((!SKIP_SETUP)); then
    printf '3\n' >/proc/sys/vm/drop_caches || die "writing /proc/sys/vm/drop_caches failed"
    sleep 10
    echo "  page cache dropped"
fi

# --- Runtime command line ---
rt_cmd=("$PACT" --mode "$MODE")
if ((USE_LDLAT)); then
    rt_cmd+=(--use-ldlat --ldlat-gate "$LDLAT_GATE")
fi
if ((USE_CAT)); then
    rt_cmd+=(--use-cat --cat-signal "$CAT_SIGNAL")
fi
if [[ -n "$CAT_WMAX" ]]; then
    rt_cmd+=(--cat-wmax "$CAT_WMAX")
fi
if [[ -n "$PEBS_PERIOD" ]]; then
    rt_cmd+=(--pebs-period "$PEBS_PERIOD")
fi
if [[ -n "$LDLAT_PERIOD" ]]; then
    rt_cmd+=(--ldlat-period "$LDLAT_PERIOD")
fi
if ((DUMP_PAC)); then
    rt_cmd+=(--dump-pac-table "${OUTDIR}/pac_table_dump.csv")
    if [[ -n "$DUMP_PAC_INTERVAL" ]]; then
        rt_cmd+=(--dump-pac-interval "$DUMP_PAC_INTERVAL")
    fi
fi
rt_cmd+=("${RUNTIME_EXTRA[@]}")
rt_cmd+=(--monitor-cpu "$RUNTIME_CPU" --migration-cpu "$RUNTIME_CPU"
    --trace-migration "${OUTDIR}/migration_trace.csv")

# --- Manifest ---
{
    echo "mode=$MODE"
    echo "arm=$ARM"
    echo "workloads=${WL_NAME[*]}"
    for ((i = 0; i < NWL; i++)); do
        echo "workload_${i}=${WL_NAME[i]} cpus=${WL_CPUS[i]} threads=${WL_THREADS[i]} rss_mb=${WL_RSS[i]} cap_mb=${WL_CAP_MB[i]} cgroup=${WL_CGROUP[i]}"
        echo "workload_${i}_cmd=${WL_CMD[i]}"
    done
    echo "ratio=$RATIO"
    echo "runtime_cpu=$RUNTIME_CPU"
    echo "pebs_period=${PEBS_PERIOD:-default} ldlat_period=${LDLAT_PERIOD:-default}"
    echo "ldlat_gate=${LDLAT_GATE:-none}"
    echo "cat_signal=${CAT_SIGNAL:-none} cat_wmax=${CAT_WMAX:-default}"
    echo "runtime_extra=${RUNTIME_EXTRA[*]}"
    echo "dump_pac_table=$((DUMP_PAC)) dump_pac_interval=${DUMP_PAC_INTERVAL:-default}"
    echo "skip_setup=$((SKIP_SETUP)) thp=$((USE_THP))"
    echo "kernel=$(uname -r)"
    echo "kernel_version=$(</proc/version)"
    echo "cmdline=$(</proc/cmdline)"
    echo "perf_event_max_sample_rate=$(</proc/sys/kernel/perf_event_max_sample_rate)"
    echo "min_free_kbytes=$(</proc/sys/vm/min_free_kbytes)"
    echo "watermark_scale_factor=$(</proc/sys/vm/watermark_scale_factor)"
    echo "cpu_model=$(cpu_family)/$(cpu_model)"
    echo "date=$(date -Is)"
    echo "runtime_bin=$PACT"
    echo "runtime_sha256=$(sha256sum "$PACT" | awk '{print $1}')"
    if commit=$(git -C "$REPO_ROOT" rev-parse HEAD 2>/dev/null); then
        echo "tree_commit=$commit"
    fi
} >"${OUTDIR}/manifest.txt"

# --- Step 3: preload inputs into the slow-tier page cache ---
echo "=== [3/8] Preload inputs (vmtouch, node 1) ==="
if ((SKIP_SETUP)); then
    echo "  skipped (--skip-setup)"
else
    for ((i = 0; i < NWL; i++)); do
        if [[ -n "${WL_VMTOUCH[i]}" ]]; then
            echo "  ${WL_VMTOUCH[i]}"
            numactl --membind 1 "$VMTOUCH" -f -t "${WL_VMTOUCH[i]}" -m 64G
            sleep 3
        fi
    done
fi

# --- Step 4: cgroups with fast-tier caps ---
echo "=== [4/8] Cgroups ==="
for ((i = 0; i < NWL; i++)); do
    echo "  ${WL_CGROUP[i]}: cap ${WL_CAP_MB[i]} MB"
    create_cgroup "${WL_CGROUP[i]}" "$((WL_CAP_MB[i] * 1024 * 1024))"
done

# --- Step 5: start the workloads, BGs first ---
echo "=== [5/8] Workloads ==="
cat /proc/vmstat >"${OUTDIR}/before_vmstat.log"
start_system_monitor
for ((i = NWL - 1; i >= 0; i--)); do
    start_workload "$i"
done

# --- Step 6: runtime ---
echo "=== [6/8] Runtime ==="
for ((i = 0; i < NWL; i++)); do
    rt_cmd+=(--workload "${TARGET_PID[i]}" "${WL_CPUS[i]}" "${WL_NAME[i]}" "${WL_CGROUP[i]}")
done
printf '%q ' "${rt_cmd[@]}" >"${OUTDIR}/pact_cmd.txt"
echo >>"${OUTDIR}/pact_cmd.txt"
echo "  $(<"${OUTDIR}/pact_cmd.txt")"
# Own session: a Ctrl+C on the terminal does not reach the runtime. The
# runner stops the workloads, and the runtime then exits by itself.
setsid "${rt_cmd[@]}" >"${OUTDIR}/pact_debug.log" 2>&1 &
RUNTIME_PID=$!
sleep 3
if ! is_alive "$RUNTIME_PID"; then
    tail -n 20 "${OUTDIR}/pact_debug.log" >&2
    die "the runtime exited during start"
fi
check_own_session "$RUNTIME_PID" "the runtime"
echo "  runtime started (PID $RUNTIME_PID)"
T_START=$(now)

# --- Step 7: wait for the FG, stop the BGs, let the runtime exit ---
echo "=== [7/8] Waiting for the FG (${WL_NAME[0]}) ==="
fg_status=0
wait "${LAUNCHER_PID[0]}" || fg_status=$?
T_END=$(now)
LAUNCHER_REAPED[0]=1
FG_RUNTIME=$(elapsed "$T_START" "$T_END")
FG_WALL=$(elapsed "${LAUNCH_TIME[0]}" "$T_END")
echo "  FG finished (status $fg_status): ${FG_RUNTIME} s after the start phase, ${FG_WALL} s after launch"
echo "$FG_RUNTIME" >"${OUTDIR}/workload_0.time"
declare -a BG_ALIVE BG_WALL
for ((i = 1; i < NWL; i++)); do
    BG_ALIVE[i]=1
    is_alive "${LAUNCHER_PID[i]}" || BG_ALIVE[i]=0
    BG_WALL[i]=$(elapsed "${LAUNCH_TIME[i]}" "$T_END")
    echo "${BG_WALL[i]}" >"${OUTDIR}/workload_${i}_real.time"
    stop_workload "$i"
done
if ! wait_runtime_exit; then
    echo "  the runtime did not exit. The summary marks the run as failed"
fi

# --- Step 8: outputs and checks ---
echo "=== [8/8] Outputs ==="
cleanup
set -e

checks_failed=0
summary="${OUTDIR}/summary.txt"
check() {
    local name=$1 ok=$2 detail=$3
    if ((ok)); then
        echo "check_${name}=PASS ${detail}" >>"$summary"
    else
        echo "check_${name}=FAIL ${detail}" >>"$summary"
        checks_failed=1
    fi
}
# skip_check NAME DETAIL: a check that this run cannot make. It does not fail.
skip_check() {
    echo "check_$1=SKIP $2" >>"$summary"
}
# The runtime's log level: the last "--log-level N" after --, else the
# default 2 (info). The runtime takes N only as a whole decimal integer in
# 0-4, so "00" is 0, and it stops at start on any other value, such as "1x"
# or "x". The CAT check reads an info line of pact_debug.log.
log_level_value() {
    local digits=$1
    if [[ ! "$digits" =~ ^[0-9]+$ ]]; then
        echo -1 # not a whole integer: the runtime stops at start
        return
    fi
    while [[ "$digits" == 0?* ]]; do
        digits=${digits#0}
    done
    if ((${#digits} > 9)); then
        echo -1 # out of the runtime's range 0-4: it stops at start
    else
        echo "$((10#$digits))"
    fi
}
rt_log_level=2
for ((k = 0; k + 1 < ${#RUNTIME_EXTRA[@]}; k++)); do
    if [[ "${RUNTIME_EXTRA[k]}" == --log-level ]]; then
        rt_log_level=$(log_level_value "${RUNTIME_EXTRA[k + 1]}")
    fi
done
{
    echo "mode=$MODE"
    echo "arm=$ARM"
    echo "workloads=${WL_NAME[*]}"
    echo "fg_runtime_sec=$FG_RUNTIME"
    echo "fg_wall_sec=$FG_WALL"
    # PMI throttling lowers this limit during a run; the manifest has the
    # value from before the run.
    echo "perf_event_max_sample_rate_after=$(</proc/sys/kernel/perf_event_max_sample_rate)"
    for ((i = 1; i < NWL; i++)); do
        echo "bg${i}_wall_sec=${BG_WALL[i]}"
    done
} >"$summary"

# numastat_max I COLUMN: largest value of a column of workload I's numastat log
# (2 = node 0, 3 = node 1, MB).
numastat_max() {
    awk -F, -v c="$2" '$1 ~ /^[0-9.]+$/ && $c + 0 > m { m = $c + 0 } END { printf "%.1f", m }' \
        "${OUTDIR}/workload_$1_numastat.log"
}

check fg_exit "$((fg_status == 0))" "status=$fg_status"
for ((i = 1; i < NWL; i++)); do
    check "bg${i}_outlived_fg" "${BG_ALIVE[i]}" "BG ${WL_NAME[i]} must still run when the FG finishes"
done
for ((i = 0; i < NWL; i++)); do
    {
        echo "wl${i}_node0_max_mb=$(numastat_max "$i" 2)"
        echo "wl${i}_node1_max_mb=$(numastat_max "$i" 3)"
        echo "wl${i}_cap_mb=${WL_CAP_MB[i]}"
    } >>"$summary"
done
node0_max=$(numastat_max 0 2)
node1_max=$(numastat_max 0 3)
# The FG fills its cap and spills to node 1; node-0 use must not exceed the
# cap by more than 5%.
ok=$(awk -v n="$node0_max" -v c="${WL_CAP_MB[0]}" 'BEGIN { print (n <= c * 1.05) ? 1 : 0 }')
check fg_node0_within_cap "$ok" "node0_max=${node0_max}MB cap=${WL_CAP_MB[0]}MB"
ok=$(awk -v n="$node1_max" 'BEGIN { print (n > 0) ? 1 : 0 }')
check fg_uses_slow_tier "$ok" "node1_max=${node1_max}MB"
check runtime_exit "$((RUNTIME_STUCK == 0 && ${RUNTIME_STATUS:-1} == 0))" \
    "status=${RUNTIME_STATUS:-none} stuck=$RUNTIME_STUCK"
# PEBS samples per workload: the last "PEBS samples: N" line under each
# "=== Workload <i> (" heading of the runtime's statistics.
declare -A samples=()
while read -r idx cnt; do
    samples[$idx]=$cnt
done < <(awk '
    /^=== Workload [0-9]+ \(/ { w = $3 }
    /^PEBS samples: [0-9]+/ && w != "" { print w, $3 }
' "${OUTDIR}/pact_debug.log")
for ((i = 0; i < NWL; i++)); do
    cnt=${samples[$i]:-0}
    echo "wl${i}_pebs_samples=$cnt" >>"$summary"
    check "wl${i}_samples" "$((cnt > 0))" "pebs_samples=$cnt"
done
rows=0
promos=0
if [[ -f "${OUTDIR}/migration_trace.csv" ]]; then
    rows=$(($(wc -l <"${OUTDIR}/migration_trace.csv") - 1))
    promos=$(awk -F, '
        NR == 1 { for (k = 1; k <= NF; k++) if ($k == "total_pact_promotions") col = k; next }
        col { p = $col }
        END { print p + 0 }' "${OUTDIR}/migration_trace.csv")
fi
echo "migration_trace_rows=$rows" >>"$summary"
echo "total_promotions=$promos" >>"$summary"
check migration_trace "$((rows > 0))" "rows=$rows"
if ((rows > 0 && promos == 0)); then
    # Not a failure: a workload that fits its cap needs no promotion. In a
    # run with the default ratio it means that every promotion failed.
    echo "warning_no_promotions=the runtime promoted no page (expected only when every workload fits its cap)" >>"$summary"
fi
if ((USE_CAT)); then
    if [[ "$rt_log_level" =~ ^[01]$ ]]; then
        skip_check cat_active "--log-level $rt_log_level hides the 'CAT enabled:' line"
    else
        cat_ok=0
        grep -q "CAT enabled: .*signal=${CAT_SIGNAL}," "${OUTDIR}/pact_debug.log" && cat_ok=1
        check cat_active "$cat_ok" "'CAT enabled: ... signal=${CAT_SIGNAL}' in pact_debug.log"
    fi
fi
if ((DUMP_PAC)); then
    dump_ok=0
    [[ -s "${OUTDIR}/pac_table_dump.csv" ]] && dump_ok=1
    check pac_table_dump "$dump_ok" "pac_table_dump.csv exists and is not empty"
fi
check cleanup "$((CLEANUP_FAILED == 0))" "cgroups removed, kernel settings restored"

echo ""
cat "$summary"
echo ""
echo "=== Run complete: $OUTDIR ==="
if ((checks_failed)); then
    echo "Some checks failed. See summary.txt and README.md (Check that it worked)."
    exit 1
fi
exit 0
