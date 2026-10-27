#!/bin/bash

# run-models.sh: Collect perf counters across three memory tiers and
# produce the CSV file read by corr.plot for Figure 2.


SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

RAW_DIR="${SCRIPT_DIR}/raw"              # raw perf stat output files
OUTPUT_CSV=""                            # collected results CSV (default: RAW_DIR/models.csv)

# NUMA topology: adjust for your server (check with: numactl --hardware)
CPU_NODE=0      # CPU execution node
LOCAL_NODE=0    # Tier 1: local DRAM   (~90ns)
NUMA_NODE=1     # Tier 2: remote NUMA  (~140ns)
CXL_NODE=2      # Tier 3: CXL          (~190ns)

# Stock perf is enough: the four counters below are read from perf stat text
# output. Override with: PERF=/path/to/perf ./run-models.sh (default: perf in PATH)
PERF="${PERF:-perf}"

# Four perf counters; all collected in a single perf stat pass
PERF_EVENTS="CYCLE_ACTIVITY.STALLS_L3_MISS"
PERF_EVENTS+=",MEM_LOAD_RETIRED.L3_MISS"
PERF_EVENTS+=",OFFCORE_REQUESTS_OUTSTANDING.DEMAND_DATA_RD"
PERF_EVENTS+=",OFFCORE_REQUESTS_OUTSTANDING.CYCLES_WITH_DEMAND_DATA_RD"

export OMP_NUM_THREADS=8


# ── workloads ──────────────────────────────────────────────────────────────────

# SPEC CPU 2017: point SPEC_DIR at your (licensed) SPEC CPU 2017 install.
# Override with: SPEC_DIR=/path/to/cpu2017 ./run-models.sh
SPEC_DIR="${SPEC_DIR:-/path/to/cpu2017}"
SPEC_MCF_DIR="${SPEC_DIR}/505.mcf_r"
# Each command runs under bash -c. printf %q quotes the path for that shell.
printf -v SPEC_MCF_CMD 'cd %q && ./mcf_r_base.mytest-m64 inp.in' "${SPEC_MCF_DIR}"

SPEC_WORKLOADS=(
    "505.mcf_r|${SPEC_MCF_CMD}"
)

# GAP benchmark suite: point GAPBS_DIR at your gapbs build (see README).
# Override with: GAPBS_DIR=/path/to/gapbs ./run-models.sh
GAPBS_DIR="${GAPBS_DIR:-/path/to/gapbs}"
GAPBS_GRAPH_DIR="${GAPBS_DIR}/benchmark/graphs"
# vmtouch preloads the graph into the page cache. Override with:
# VMTOUCH=/path/to/vmtouch ./run-models.sh   (default: vmtouch in PATH)
VMTOUCH="${VMTOUCH:-vmtouch}"

GAPBS_WORKLOADS=(
    "bc-kron"
)


# ── argument parsing ───────────────────────────────────────────────────────────

RUN_SPEC=false
RUN_GAPBS=false

# func_need_value OPTION ARGC VALUE: exit 1 if OPTION has no value. A value
# that starts with "--" is the next option, not a value.
func_need_value() {
    if [[ "$2" -lt 2 || -z "$3" || "$3" == --* ]]; then
        echo "ERROR: option $1 needs a value" >&2
        exit 1
    fi
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --spec)       RUN_SPEC=true;           shift   ;;
        --gapbs)      RUN_GAPBS=true;          shift   ;;
        --output|--raw-dir|--local-node|--numa-node|--cxl-node|--cpu-node)
            func_need_value "$1" "$#" "${2:-}"
            case "$1" in
                --output)     OUTPUT_CSV="$2" ;;
                --raw-dir)    RAW_DIR="$2"    ;;
                --local-node) LOCAL_NODE="$2" ;;
                --numa-node)  NUMA_NODE="$2"  ;;
                --cxl-node)   CXL_NODE="$2"   ;;
                --cpu-node)   CPU_NODE="$2"   ;;
            esac
            shift 2 ;;
        *) echo "unknown option: $1" >&2; exit 1 ;;
    esac
done

if [[ "${RUN_SPEC}" == false && "${RUN_GAPBS}" == false ]]; then
    echo "usage: $0 --spec | --gapbs | --spec --gapbs" >&2
    exit 1
fi

for node in "${CPU_NODE}" "${LOCAL_NODE}" "${NUMA_NODE}" "${CXL_NODE}"; do
    if [[ ! "${node}" =~ ^[0-9]+$ ]]; then
        echo "ERROR: NUMA node '${node}' is not a non-negative integer" >&2
        exit 1
    fi
done

# The default output follows --raw-dir. An explicit --output stays as given.
OUTPUT_CSV="${OUTPUT_CSV:-${RAW_DIR}/models.csv}"

# skip a tier if its NUMA node does not exist on this machine
NUMA_AVAILABLE=true
CXL_AVAILABLE=true
[[ ! -d "/sys/devices/system/node/node${NUMA_NODE}" ]] && NUMA_AVAILABLE=false
[[ ! -d "/sys/devices/system/node/node${CXL_NODE}"  ]] && CXL_AVAILABLE=false


# ── helpers ────────────────────────────────────────────────────────────────────

# Result of each workload and tier, keyed "name|tier". TIER_STATUS is ok,
# failed, or no_node (the NUMA node of the tier does not exist). TIER_VALUES
# holds the parsed counters of an ok tier.
declare -A TIER_STATUS=()
declare -A TIER_VALUES=()
FAILED_COUNT=0

# func_record_tier NAME TIER RC OUTFILE: record the result of one tier run.
# The tier is ok only if the run exited 0 and OUTFILE holds all four counters.
func_record_tier() {
    local name="$1" tier="$2" rc="$3" outfile="$4" values

    if [[ "${rc}" -ne 0 ]]; then
        echo "    ERROR: ${name}/${tier} failed (exit status ${rc})" >&2
    elif ! values=$(func_parse_tier_file "${outfile}"); then
        echo "    ERROR: ${name}/${tier}: a counter is missing or not a number in ${outfile}" >&2
        rc=1
    fi
    if [[ "${rc}" -ne 0 ]]; then
        TIER_STATUS["${name}|${tier}"]=failed
        FAILED_COUNT=$((FAILED_COUNT + 1))
        return
    fi
    TIER_STATUS["${name}|${tier}"]=ok
    TIER_VALUES["${name}|${tier}"]="${values}"
}

func_run_spec_tier() {
    local name="$1" cmd="$2" node="$3" tier="$4" rc=0
    local outfile="${RAW_DIR}/${name}_${tier}.txt"

    echo "    [${tier}]  node=${node}  →  $(basename "${outfile}")"

    # Remove the file of an earlier run, so that a failed run cannot use it.
    rm -f "${outfile}"
    numactl --cpunodebind="${CPU_NODE}" --membind="${node}" -- \
        "${PERF}" stat -e "${PERF_EVENTS}" \
                  -o "${outfile}" \
                  -- bash -c "${cmd}" 2>/dev/null || rc=$?
    func_record_tier "${name}" "${tier}" "${rc}" "${outfile}"
}

func_run_gapbs_tier() {
    local name="$1" node="$2" tier="$3" rc=0
    local outfile="${RAW_DIR}/${name}_${tier}.txt"

    echo "    [${tier}]  node=${node}  →  $(basename "${outfile}")"

    # Remove the file of an earlier run, so that a failed run cannot use it.
    rm -f "${outfile}"
    local GRAPH_DATASET="kron.sg"
    echo "    => Loading graph into page cache first"
    numactl --cpunodebind="${CPU_NODE}" --membind="${node}" \
        "${VMTOUCH}" -f -t "${GAPBS_GRAPH_DIR}/${GRAPH_DATASET}" -m 64G || rc=$?
    if [[ "${rc}" -ne 0 ]]; then
        echo "    ERROR: vmtouch failed for ${name}/${tier}" >&2
        func_record_tier "${name}" "${tier}" "${rc}" "${outfile}"
        return
    fi
    sleep 20

    local bench_cmd=("${GAPBS_DIR}/bc" -f "${GAPBS_GRAPH_DIR}/${GRAPH_DATASET}" -i4 -n4)
    echo "    => $(printf '%q ' "${bench_cmd[@]}")"
    export OMP_NUM_THREADS

    numactl --cpunodebind="${CPU_NODE}" --membind="${node}" -- \
        "${PERF}" stat -e "${PERF_EVENTS}" \
                  -o "${outfile}" \
                  -- "${bench_cmd[@]}" 2>/dev/null || rc=$?
    func_record_tier "${name}" "${tier}" "${rc}" "${outfile}"
}

# func_extract_int FILE PATTERN [EXCLUDE]: print the count of the first line
# of a perf stat -o file that matches PATTERN and not EXCLUDE. Return 1 if
# no line matches or the count is not an integer (e.g. "<not counted>").
func_extract_int() {
    local file="$1" pattern="$2" exclude="${3:-}" line count
    line=$(grep -i -- "${pattern}" "${file}" 2>/dev/null \
               | { if [[ -n "${exclude}" ]]; then grep -iv -- "${exclude}"; else cat; fi; } \
               | head -1)
    read -r count _ <<< "${line}"
    count="${count//,/}"
    [[ "${count}" =~ ^[0-9]+$ ]] || return 1
    echo "${count}"
}

# Parse one raw perf file → echo "l3_stall  l3_miss  mlp  llc_miss_mlp"
# MLP          = DEMAND_DATA_RD / CYCLES_WITH_DEMAND_DATA_RD
# llc_miss_mlp = l3_miss / MLP  (columns 15/16/17 in CSV, plotted by corr.plot)
# Return 1 if the file or one of the four counters is missing.
func_parse_tier_file() {
    local file="$1"

    [[ -f "${file}" ]] || return 1

    local stall miss outstanding outstanding_cyc

    stall=$(func_extract_int "${file}" "cycle_activity\.stalls_l3_miss") || return 1
    miss=$(func_extract_int  "${file}" "mem_load_retired\.l3_miss") || return 1

    # The exclude pattern tells .demand_data_rd from .cycles_with_demand_data_rd
    outstanding=$(func_extract_int "${file}" \
                      "offcore_requests_outstanding\.demand_data_rd" "cycles_with") || return 1

    outstanding_cyc=$(func_extract_int "${file}" \
                          "offcore_requests_outstanding\.cycles_with_demand_data_rd") || return 1

    awk -v stall="${stall}" \
        -v miss="${miss}" \
        -v out="${outstanding}" \
        -v cyc="${outstanding_cyc}" \
        'BEGIN {
            mlp          = (cyc > 0) ? out / cyc : 1.0
            llc_miss_mlp = (mlp > 0) ? miss / mlp : miss
            printf "%.0f %.0f %.6f %.6f\n", stall, miss, mlp, llc_miss_mlp
        }'
}


# ── collect ────────────────────────────────────────────────────────────────────

func_collect() {
    echo "==> Collecting perf counters"
    echo ""

    local count=0

    if [[ "${RUN_SPEC}" == true ]]; then
        for entry in "${SPEC_WORKLOADS[@]}"; do
            local name="${entry%%|*}"
            local cmd="${entry#*|}"

            echo "  [${name}]"
            func_run_spec_tier "${name}" "${cmd}" "${LOCAL_NODE}" "local"
            if [[ "${NUMA_AVAILABLE}" == true ]]; then
                func_run_spec_tier "${name}" "${cmd}" "${NUMA_NODE}" "remote_numa"
            else
                TIER_STATUS["${name}|remote_numa"]=no_node
            fi
            if [[ "${CXL_AVAILABLE}" == true ]]; then
                func_run_spec_tier "${name}" "${cmd}" "${CXL_NODE}"  "cxl"
            else
                TIER_STATUS["${name}|cxl"]=no_node
            fi
            echo ""
            count=$((count + 1))
        done
    fi

    if [[ "${RUN_GAPBS}" == true ]]; then
        for name in "${GAPBS_WORKLOADS[@]}"; do
            echo "  [${name}]"
            func_run_gapbs_tier "${name}" "${LOCAL_NODE}" "local"
            if [[ "${NUMA_AVAILABLE}" == true ]]; then
                func_run_gapbs_tier "${name}" "${NUMA_NODE}" "remote_numa"
            else
                TIER_STATUS["${name}|remote_numa"]=no_node
            fi
            if [[ "${CXL_AVAILABLE}" == true ]]; then
                func_run_gapbs_tier "${name}" "${CXL_NODE}"  "cxl"
            else
                TIER_STATUS["${name}|cxl"]=no_node
            fi
            echo ""
            count=$((count + 1))
        done
    fi

    echo "==> Ran ${count} workload(s), ${FAILED_COUNT} tier run(s) failed"
    echo ""
}




func_write_csv() {
    echo "==> Writing CSV → ${OUTPUT_CSV}"

    if [[ ! -f "${OUTPUT_CSV}" ]]; then
        if ! {
            printf ",workload,"
            printf "l3_stall_1,l3_stall_2,l3_stall_3,"
            printf "demand_l2_mlp_1,demand_l2_mlp_2,demand_l2_mlp_3,"
            printf "l3_miss_1,l3_miss_2,l3_miss_3,"
            printf "offcore_load_latency_1,offcore_load_latency_2,offcore_load_latency_3,"
            printf "(l3_miss/mlp)_1,(l3_miss/mlp)_2,(l3_miss/mlp)_3,"
            printf "(lat*l3_miss/mlp)_1,(lat*l3_miss/mlp)_2,(lat*l3_miss/mlp)_3\n"
        } > "${OUTPUT_CSV}"; then
            echo "ERROR: cannot write ${OUTPUT_CSV}" >&2
            exit 1
        fi
    fi

    local idx written=0 skipped=0
    # Row index = count of data rows already in the CSV. grep -c prints "0" AND
    # exits 1 on a header-only file, so `|| echo 0` would make idx="0\n0" and
    # break the printf/arithmetic below; capture then default instead.
    idx=$(grep -c "^[0-9]" "${OUTPUT_CSV}" 2>/dev/null) || true
    idx=${idx:-0}

    local all_names=()
    [[ "${RUN_SPEC}"  == true ]] && for entry in "${SPEC_WORKLOADS[@]}";  do all_names+=("${entry%%|*}"); done
    [[ "${RUN_GAPBS}" == true ]] && for name  in "${GAPBS_WORKLOADS[@]}"; do all_names+=("${name}"); done

    # A workload with a failed tier gets no row: a failed run is never
    # written as measured zeros. A tier whose NUMA node does not exist is
    # written as zeros (see README.md).
    local tier failed_tiers t1 t2 t3
    for name in "${all_names[@]}"; do
        failed_tiers=""
        for tier in local remote_numa cxl; do
            if [[ "${TIER_STATUS["${name}|${tier}"]:-failed}" == failed ]]; then
                failed_tiers+=" ${tier}"
            fi
        done
        if [[ -n "${failed_tiers}" ]]; then
            echo "  skip ${name}: failed tier(s):${failed_tiers}" >&2
            skipped=$((skipped + 1))
            continue
        fi

        t1="${TIER_VALUES["${name}|local"]:-0 0 0 0}"
        t2="${TIER_VALUES["${name}|remote_numa"]:-0 0 0 0}"
        t3="${TIER_VALUES["${name}|cxl"]:-0 0 0 0}"
        read -r stall1 miss1 mlp1 llc_miss_mlp1 <<< "${t1}"
        read -r stall2 miss2 mlp2 llc_miss_mlp2 <<< "${t2}"
        read -r stall3 miss3 mlp3 llc_miss_mlp3 <<< "${t3}"

        if ! printf "%d,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,0,0,0,%s,%s,%s,0,0,0\n" \
            "${idx}" "${name}" \
            "${stall1}" "${stall2}" "${stall3}" \
            "${mlp1}"   "${mlp2}"   "${mlp3}"   \
            "${miss1}"  "${miss2}"  "${miss3}"  \
            "${llc_miss_mlp1}" "${llc_miss_mlp2}" "${llc_miss_mlp3}" \
            >> "${OUTPUT_CSV}"; then
            echo "ERROR: cannot append to ${OUTPUT_CSV}" >&2
            exit 1
        fi

        echo "  [${idx}] ${name}"
        idx=$((idx + 1))
        written=$((written + 1))
    done

    echo ""
    echo "==> ${written} workload(s) written to ${OUTPUT_CSV}"
    if [[ "${skipped}" -gt 0 ]]; then
        echo "    (${skipped} skipped: a tier failed)" >&2
    fi
}


# ── main ───────────────────────────────────────────────────────────────────────

func_main() {
    echo "run-models.sh"
    echo "  CPU node:      ${CPU_NODE}"
    echo "  Tier 1 local:  node ${LOCAL_NODE}  (~90ns)"
    echo "  Tier 2 NUMA:   node ${NUMA_NODE}  (~140ns)  $( [[ "${NUMA_AVAILABLE}" == false ]] && printf "[SKIPPED: node not found]" )"
    echo "  Tier 3 CXL:    node ${CXL_NODE}  (~190ns)  $( [[ "${CXL_AVAILABLE}" == false ]] && printf "[SKIPPED: node not found]" )"
    echo "  Suites:        $( [[ "${RUN_SPEC}" == true ]] && printf "SPEC "; [[ "${RUN_GAPBS}" == true ]] && printf "GAPBS" )"
    echo "  Raw dir:       ${RAW_DIR}/"
    echo "  Output CSV:    ${OUTPUT_CSV}"
    echo ""

    if ! mkdir -p "${RAW_DIR}" "$(dirname "${OUTPUT_CSV}")"; then
        echo "ERROR: cannot create ${RAW_DIR} or the directory of ${OUTPUT_CSV}" >&2
        exit 1
    fi

    func_collect

    func_write_csv

    echo ""
    echo "  Output CSV:    ${OUTPUT_CSV}"
    echo "  To plot:       cp $(printf '%q %q' "${OUTPUT_CSV}" "${SCRIPT_DIR}/skx/dat/r-l3_stall_skx.csv")"
    echo "                 cd $(printf '%q' "${SCRIPT_DIR}/skx") && gnuplot plot/corr.plot"

    if [[ "${FAILED_COUNT}" -gt 0 ]]; then
        echo "" >&2
        echo "FAILED: ${FAILED_COUNT} tier run(s) failed. Their workloads are not in the CSV." >&2
        exit 1
    fi
}

echo "run ..."
func_main
echo "Done"
