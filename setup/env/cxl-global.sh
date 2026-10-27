#!/bin/bash
#
# Machine-prep helpers for PACT experiments (sourced by prepare_environment.sh
# and modify-uncore-freq.sh).  Self-contained: only uses standard tools
# (numactl, lscpu) and sysfs/proc knobs, with no external globals or profilers.
#
# Each helper prints an error and returns 1 when a write fails. The scripts
# that source this file run with "set -euo pipefail", so a failed step stops
# the preparation. A knob that does not exist on this host (for example KSM)
# is skipped where the helper says so.

#-------------------------------------------------------------------------------

# Sysfs and proc paths of the helpers below. A test can point them at a fake
# tree.
TIERCE_SYS_CPU_DIR=${TIERCE_SYS_CPU_DIR:-/sys/devices/system/cpu}
TIERCE_SYS_NODE1_DIR=${TIERCE_SYS_NODE1_DIR:-/sys/devices/system/node/node1}
TIERCE_SYS_MM_DIR=${TIERCE_SYS_MM_DIR:-/sys/kernel/mm}
TIERCE_PROC_CPUINFO=${TIERCE_PROC_CPUINFO:-/proc/cpuinfo}

# cpu_online DIR: true when the CPU of sysfs directory DIR (.../cpuN) is
# online. A CPU without an online file (often CPU 0) cannot go offline.
# Per-CPU knobs of an offline CPU (for example cpufreq) fail with EBUSY, so
# the helpers below write per-CPU knobs only for online CPUs.
cpu_online() {
    [[ ! -e "$1/online" || "$(<"$1/online")" == 1 ]]
}

# write_knob VALUE FILE...: write VALUE to each FILE as root. Print an error
# and return 1 if any write fails.
write_knob() {
    local value=$1 f ok=0
    shift
    for f in "$@"; do
        if ! printf '%s\n' "$value" | sudo tee "$f" >/dev/null; then
            echo "ERROR: writing '$value' to $f failed" >&2
            ok=1
        fi
    done
    return "$ok"
}

# write_verify VALUE FILE: write VALUE to FILE as root, then read FILE back.
# Print an error and return 1 if the write fails or FILE reads another value.
write_verify() {
    local value=$1 f=$2 got
    write_knob "$value" "$f" || return 1
    got=$(<"$f")
    if [[ "$got" != "$value" ]]; then
        echo "ERROR: $f reads '$got' after writing '$value'" >&2
        return 1
    fi
}

# node1_online_cpus: the online CPUs of node 1, as numactl lists them
# (empty when node 1 has no online CPU).
node1_online_cpus() {
    local nc
    nc=$(sudo numactl --hardware | awk -F: '/^node 1 cpus/ {print $2}')
    echo "${nc//[[:space:]]/}"
}

flush_fs_caches() {
    write_knob 3 /proc/sys/vm/drop_caches
    sleep 10
}

disable_nmi_watchdog() {
    write_knob 0 /proc/sys/kernel/nmi_watchdog
}

# intel_pstate has no_turbo. Without intel_pstate, acpi-cpufreq has boost.
# A knob that already reads "turbo off" is not written: intel_pstate refuses
# any no_turbo write with EPERM when the BIOS turned turbo off, and then
# no_turbo reads 1. acpi-cpufreq has no boost file on a CPU without turbo.
# With neither file, a CPU that reports turbo (the ida or cpb flag) is an
# error: nothing can turn its turbo off.
disable_turbo() {
    local f rc=0
    if [[ -e "$TIERCE_SYS_CPU_DIR/intel_pstate/no_turbo" ]]; then
        f=$TIERCE_SYS_CPU_DIR/intel_pstate/no_turbo
        if [[ "$(<"$f")" == 1 ]]; then
            echo "  turbo already off (no_turbo reads 1)"
        else
            write_verify 1 "$f"
        fi
    elif [[ -e "$TIERCE_SYS_CPU_DIR/cpufreq/boost" ]]; then
        f=$TIERCE_SYS_CPU_DIR/cpufreq/boost
        if [[ "$(<"$f")" == 0 ]]; then
            echo "  turbo already off (cpufreq boost reads 0)"
        else
            write_verify 0 "$f"
        fi
    else
        grep -qwE 'ida|cpb' "$TIERCE_PROC_CPUINFO" || rc=$?
        if [[ $rc -eq 0 ]]; then
            echo "ERROR: the CPU reports turbo (ida/cpb flag), but there is no turbo control (no intel_pstate/no_turbo, no cpufreq/boost)" >&2
            return 1
        elif [[ $rc -ne 1 ]]; then
            echo "ERROR: cannot read $TIERCE_PROC_CPUINFO" >&2
            return 1
        fi
        echo "  NOTE: the CPU reports no turbo (no ida/cpb flag) and there is no turbo control: nothing to turn off"
    fi
}

# 0: no randomization, everything is static
# 1: conservative randomization, shared libraries, stack, mmap(), VDSO and heap
# are randomized
# 2: full randomization, the above points in 1 plus brk()
disable_va_aslr() {
    write_knob 0 /proc/sys/kernel/randomize_va_space
}

disable_swap() {
    sudo swapoff -a
}

# A kernel without KSM has no ksm/run file: nothing to turn off.
disable_ksm() {
    if [[ -e "$TIERCE_SYS_MM_DIR/ksm/run" ]]; then
        write_knob 0 "$TIERCE_SYS_MM_DIR/ksm/run"
    fi
}

disable_numa_balancing() {
    write_knob 0 /proc/sys/kernel/numa_balancing
}

# disable transparent hugepages
disable_thp() {
    write_knob never "$TIERCE_SYS_MM_DIR/transparent_hugepage/enabled"
}

# SMT off. A host without SMT reports notsupported or notimplemented, and a
# host booted with nosmt reports forceoff: nothing to write then.
disable_ht() {
    local f=$TIERCE_SYS_CPU_DIR/smt/control state
    [[ -e "$f" ]] || return 0
    state=$(<"$f")
    case "$state" in
    off | forceoff | notsupported | notimplemented) return 0 ;;
    esac
    write_verify off "$f"
}

# online_cpu FILE: write 1 to the online file of a CPU. While smt/control is
# not "on", the kernel refuses a CPU that is not the primary thread of its
# core with EPERM (kernel/cpu.c, cpu_smt_allowed()). Such a CPU stays offline
# on purpose, so it is skipped. Sysfs cannot tell that thread apart before the
# write: an offline CPU has no topology directory (the kernel creates it at
# CPUHP_TOPOLOGY_PREPARE and removes it at offline). Any other failure prints
# an error and returns 1.
online_cpu() {
    local f=$1 msg smt=on
    if [[ -e "$TIERCE_SYS_CPU_DIR/smt/control" ]]; then
        smt=$(<"$TIERCE_SYS_CPU_DIR/smt/control")
    fi
    if msg=$(printf '1\n' | sudo env LC_ALL=C tee "$f" 2>&1 >/dev/null); then
        return 0
    fi
    if [[ "$smt" != on && "$msg" == *"Operation not permitted"* ]]; then
        echo "  skipping $f: an SMT thread that the SMT setting '$smt' keeps offline"
        return 0
    fi
    echo "ERROR: writing '1' to $f failed: $msg" >&2
    return 1
}

# Bring every offline CPU online that the SMT setting allows. CPU 0 often
# has no online file.
bring_all_cpus_online() {
    local f
    for f in "$TIERCE_SYS_CPU_DIR"/cpu[0-9]*/online; do
        [[ -e "$f" ]] || continue
        if [[ "$(<"$f")" != 1 ]]; then
            online_cpu "$f" || return 1
        fi
    done
}

# The performance governor on every online CPU. An offline CPU keeps its
# cpufreq files, but they fail with EBUSY. The prepared host keeps such CPUs
# offline (node 1, SMT siblings), and run.sh checks the governor of online
# CPUs only. Return 1 when no online CPU has a governor file: then nothing
# sets the CPU frequency.
set_performance_mode() {
    local cpu governor ngov=0
    local -a offline=() already=()
    for cpu in "$TIERCE_SYS_CPU_DIR"/cpu[0-9]*; do
        [[ -d "$cpu" ]] || continue
        if ! cpu_online "$cpu"; then
            offline+=("${cpu##*/cpu}")
            continue
        fi
        governor=$cpu/cpufreq/scaling_governor
        [[ -e "$governor" ]] || continue
        ngov=$((ngov + 1))
        if [[ "$(<"$governor")" == performance ]]; then
            already+=("${cpu##*/cpu}")
        else
            write_verify performance "$governor" || return 1
        fi
    done
    if [[ $ngov -eq 0 ]]; then
        echo "ERROR: no online CPU has $TIERCE_SYS_CPU_DIR/cpuN/cpufreq/scaling_governor: cannot set the performance governor" >&2
        return 1
    fi
    if ((${#offline[@]} > 0)); then
        echo "  governor: skipped offline CPUs ${offline[*]}"
    fi
    if ((${#already[@]} > 0)); then
        echo "  governor: ${#already[@]} online CPUs already at performance"
    fi
}

# Place CPUs in the performance governor.
set_cpu_governor() {
    set_performance_mode
}

# offline_node1_cpus: take every online CPU of node 1 offline.
offline_node1_cpus() {
    local f
    for f in "$TIERCE_SYS_NODE1_DIR"/cpu[0-9]*/online; do
        [[ -e "$f" ]] || continue
        if [[ "$(<"$f")" != 0 ]]; then
            write_knob 0 "$f"
        fi
    done
}

# Keep all cores on Node 0 online while keeping all cores on Node 1 offline
configure_cxl_exp_cores() {
    # To be safe, let's bring all the cores online first
    bring_all_cpus_online

    # Disable all cores on Node 1
    offline_node1_cpus
}

configure_base_exp_cores() {
    # To be safe, let's bring all the cores online first
    bring_all_cpus_online
    # Leave half of the cores online for both Node 0 and Node 1. This assumes
    # that CPUs 0 to N-1 are node 0 and CPUs N to 2N-1 are node 1, where N is
    # the number of cores per socket (SMT off).
    local cores_per_socket half_cores_per_socket total_cores i f
    cores_per_socket=$(lscpu | awk -F: '/^Core\(s\) per socket/ {gsub(/ /, "", $2); print $2}')
    [[ "$cores_per_socket" =~ ^[1-9][0-9]*$ ]] || {
        echo "ERROR: cannot read the cores per socket from lscpu" >&2
        return 1
    }
    half_cores_per_socket=$((cores_per_socket / 2))
    total_cores=$((cores_per_socket * 2))
    for ((i = half_cores_per_socket; i < cores_per_socket; i++)); do
        f=$TIERCE_SYS_CPU_DIR/cpu$i/online
        [[ ! -e "$f" ]] || write_knob 0 "$f"
    done

    for ((i = cores_per_socket + half_cores_per_socket; i < total_cores; i++)); do
        f=$TIERCE_SYS_CPU_DIR/cpu$i/online
        [[ ! -e "$f" ]] || write_knob 0 "$f"
    done
}

check_base_conf() {
    disable_nmi_watchdog
    disable_va_aslr
    disable_ksm
    disable_numa_balancing
    disable_thp
    disable_ht
    disable_turbo
    configure_base_exp_cores
    set_cpu_governor
    disable_swap

    # Everything looks correct
    [[ -n "$(node1_online_cpus)" ]] && return 0

    # Node 1 has no online CPU: bring its CPUs online.
    echo "===> Warning: Base experiment environment NOT properly setup"
    echo "     All cores on Node 1 are offline..."

    echo "===> Enabling all the cores on Node 1 now ..."
    local f
    for f in "$TIERCE_SYS_NODE1_DIR"/cpu[0-9]*/online; do
        [[ ! -e "$f" ]] || online_cpu "$f" || return 1
    done

    if [[ -z "$(node1_online_cpus)" ]]; then
        echo "===> Failed to bring up all the cores on Node 1 for Base experiments ..." >&2
        return 1
    fi

    sleep 60
}

check_cxl_conf() {
    disable_nmi_watchdog
    disable_va_aslr
    disable_ksm
    disable_numa_balancing
    disable_thp
    disable_ht
    disable_turbo
    configure_cxl_exp_cores
    set_cpu_governor
    disable_swap

    local nc
    nc=$(node1_online_cpus)

    # Everything looks correct
    [[ -z "$nc" ]] && return 0

    # Node 1 still has online cores: CXL emulation is NOT set up. Retry the
    # offlining, and abort if it still fails. With the slow tier's cores
    # online the results are invalid, so fail loudly rather than warn.
    echo "===> Warning: CXL experiment environment NOT properly setup"
    echo "     I see online cores [$nc] on Node 1; retrying offline..."

    echo "===> Disabling all the cores on Node 1 now ..."
    offline_node1_cpus

    if [[ -n "$(node1_online_cpus)" ]]; then
        echo "===> ERROR: could not offline Node 1 cores for CXL emulation." >&2
        echo "     The slow (CXL-like) tier must be CPU-less; aborting to avoid" >&2
        echo "     invalid measurements." >&2
        return 1
    fi

    sleep 60
}
