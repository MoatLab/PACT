#!/bin/bash
# SPDX-License-Identifier: MIT
#
# Measure the CHA TID filter value of each core on an Emerald Rapids CPU
# (32 cores) and print it as the emr_core_to_tid table of
# src/pmu-platform.c. See README.md. Run as root, on an idle machine.
set -euo pipefail

MLC="${MLC:-mlc}"
PERF="${PERF:-perf}"
MEASURE_TIME=3      # Seconds per perf stat invocation
RAMP_TIME=2         # Seconds to let MLC generate steady traffic
NUM_CORES=32
NUM_CANDIDATES=32   # TID candidates: 0x00 to 0xF8, stride 0x08

# UNC_CHA_TOR_INSERTS.IA_MISS_DRD (local + remote) on EMR needs the
# extended umask encoding:
#   config = (ext_umask << 32) | (umask_low << 8) | event_code | (1 << 16)
# JSON UMask 0xc817fe01: umask_low = 0x01, ext_umask = 0xc817fe, event 0x35.
# Bit 16 enables the TID filter; the TID value goes into config1.
RAW_CONFIG=0x00c817fe00010135

declare -a CORE_TO_TID
mlc_pid=""

# stop_mlc: stop the MLC run that this script started, and reap it. MLC
# runs in its own process group (setsid below), so the signal goes to that
# group only and never to MLC runs of other users. After 10 s, SIGKILL.
stop_mlc() {
    local n
    [[ -n "$mlc_pid" ]] || return 0
    kill -TERM -- "-$mlc_pid" 2>/dev/null || true
    for ((n = 0; n < 20; n++)); do
        pgrep -g "$mlc_pid" >/dev/null || break
        sleep 0.5
    done
    if pgrep -g "$mlc_pid" >/dev/null; then
        echo "MLC (process group $mlc_pid) still runs after 10 s: SIGKILL" >&2
        kill -KILL -- "-$mlc_pid" 2>/dev/null || true
    fi
    wait "$mlc_pid" 2>/dev/null || true
    mlc_pid=""
}
# On SIGINT or SIGTERM the script exits. The EXIT trap then stops MLC.
trap stop_mlc EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

for core in $(seq 0 $((NUM_CORES - 1))); do
    echo "=== Core $core ==="

    # Start MLC pinned to this core, node 0 memory, no latency thread.
    # Job control is off in a script, so setsid does not fork: $! is the
    # leader of the new process group.
    setsid sudo "$MLC" --loaded_latency -k"$core" -j0 -T -d0 &>/dev/null &
    mlc_pid=$!
    sleep "$RAMP_TIME"

    # Build event list: 32 TID candidates spread across 32 different CHAs.
    # Each event on a different CHA device -> no multiplexing.
    # Traffic from core X distributes across all CHAs via address hashing,
    # so the correct TID will show nonzero counts regardless of which CHA tests it.
    events=""
    tid_candidates=()
    for i in $(seq 0 $((NUM_CANDIDATES - 1))); do
        tid=$((i * 8))
        tid_hex=$(printf "0x%02x" "$tid")
        tid_candidates+=("$tid_hex")
        if [[ -n "$events" ]]; then events+=","; fi
        # Each candidate on a different CHA (cha_0 tests 0x00, cha_1 tests 0x08, etc.)
        events+="uncore_cha_${i}/config=${RAW_CONFIG},config1=${tid_hex}/"
    done

    # Run perf stat with CSV output -- single invocation, no multiplexing
    output=$(sudo "$PERF" stat -x, -e "$events" -- sleep "$MEASURE_TIME" 2>&1)

    # Parse CSV output: pick TID with highest count
    best_count=0
    matched_tid=""
    idx=0
    while IFS= read -r line; do
        # Skip empty lines and comments
        [[ -z "$line" || "$line" == \#* ]] && continue
        # First field is the count
        count=$(echo "$line" | cut -d, -f1)
        # Skip non-numeric (e.g., "<not supported>")
        if [[ "$count" =~ ^[0-9]+$ ]] && [[ $count -gt $best_count ]]; then
            best_count=$count
            matched_tid="${tid_candidates[$idx]}"
        fi
        idx=$((idx + 1))
    done <<< "$output"

    # Stop MLC for this core
    stop_mlc

    if [[ -z "$matched_tid" ]]; then
        echo "WARNING: No TID found for core $core (best_count=$best_count)"
        CORE_TO_TID[core]="???"
    else
        CORE_TO_TID[core]="$matched_tid"
        echo "core $core -> tid $matched_tid (count=$best_count)"
    fi
done

# Validate before printing: every core needs a TID, and no TID may repeat.
# Count in bash: a grep that finds no line exits 1 and stops the script.
failed=0
missing=0
declare -A tid_seen=()
for i in $(seq 0 $((NUM_CORES - 1))); do
    if [[ "${CORE_TO_TID[$i]}" == "???" ]]; then
        missing=$((missing + 1))
    else
        tid_seen["${CORE_TO_TID[$i]}"]=1
    fi
done
total_valid=$((NUM_CORES - missing))
unique_count=${#tid_seen[@]}

# Print C array
echo ""
if [[ $missing -gt 0 || $unique_count -ne $total_valid ]]; then
    echo "/* INVALID: $missing core(s) have no TID (???) and $((total_valid - unique_count)) TID(s) repeat. Do not copy this table. */"
fi
echo "/* EMR core-to-TID table measured on this machine */"
printf "static const int emr_core_to_tid[%d] = {" "$NUM_CORES"
for i in $(seq 0 $((NUM_CORES - 1))); do
    if [[ $i -gt 0 ]]; then printf ", "; fi
    printf "%s" "${CORE_TO_TID[$i]}"
done
printf "};\n"

echo ""
echo "=== Validation ==="
echo "Unique TIDs: $unique_count / $total_valid valid entries"
if [[ $unique_count -eq $total_valid && $total_valid -eq $NUM_CORES ]]; then
    echo "PASS: All $NUM_CORES cores have unique TID mappings"
else
    echo "FAIL: Expected $NUM_CORES unique TIDs, got $unique_count"
    failed=1
fi

if [[ $failed -ne 0 ]]; then
    echo "ERROR: a validation check failed (see the FAIL lines above)" >&2
    exit 1
fi
