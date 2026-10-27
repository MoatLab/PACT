#!/bin/bash
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/cxl-global.sh" || exit 1

# Validate args BEFORE any side effect: bring_all_cpus_online below re-populates
# node 1, so aborting after it would leave the CXL topology destroyed.
if [[ $# != 4 ]]; then
	echo "Usage: sudo ./modify-uncore-freq.sh [node0-min] [node0-max] [node1-min] [node1-max]" >&2
	exit 1
fi
# The driver keeps a ratio in steps of 100000 kHz. Accept only exact steps
# from 100000 to 12700000 kHz. The length limit of the pattern keeps the
# arithmetic tests below from overflow.
for freq in "$@"; do
	[[ "$freq" =~ ^[1-9][0-9]{5,7}$ ]] || {
		echo "ERROR: uncore frequency '$freq' is not an integer from 100000 to 12700000 (kHz)" >&2
		exit 1
	}
	if ((freq < 100000 || freq > 12700000)); then
		echo "ERROR: uncore frequency '$freq' is not from 100000 to 12700000 (kHz)" >&2
		exit 1
	fi
	if ((freq % 100000 != 0)); then
		echo "ERROR: uncore frequency '$freq' is not a multiple of 100000 (kHz)" >&2
		exit 1
	fi
done
# The min of each package must not be above its max. Test it here, before
# any change to the host.
if (($1 > $2)); then
	echo "ERROR: node 0 min uncore frequency $1 is above its max $2 (kHz)" >&2
	exit 1
fi
if (($3 > $4)); then
	echo "ERROR: node 1 min uncore frequency $3 is above its max $4 (kHz)" >&2
	exit 1
fi
TIERCE_UNCORE_DIR=${TIERCE_UNCORE_DIR:-/sys/devices/system/cpu/intel_uncore_frequency}
[[ -d "$TIERCE_UNCORE_DIR" ]] || {
	echo "ERROR: $TIERCE_UNCORE_DIR not found (needs CONFIG_INTEL_UNCORE_FREQ_CONTROL)" >&2
	exit 1
}

echo "setting to base conf ..."
check_base_conf
echo "setting all cores online ..."
bring_all_cpus_online

# The driver removes the directory of a package while no CPU of that
# package is online, and adds it again when one comes online. On a prepared
# host node 1 has no online CPU, so test the package directories only now.
for pkg in package_00_die_00 package_01_die_00; do
	[[ -d "$TIERCE_UNCORE_DIR/$pkg" ]] || {
		echo "ERROR: $TIERCE_UNCORE_DIR/$pkg not found after all CPUs came online" >&2
		exit 1
	}
done

ZERO_MIN_UNCORE_FREQ=$1
ZERO_MAX_UNCORE_FREQ=$2
ONE_MIN_UNCORE_FREQ=$3
ONE_MAX_UNCORE_FREQ=$4

# set_uncore FILE FREQ LABEL: write FREQ to FILE and read it back. The
# driver keeps a ratio in steps of 100000 kHz and rounds a value down to
# such a step. The argument test above accepts exact steps only. The test
# here is a second guard and writes nothing for a value that is not a step.
# The driver does not return an error when it refuses a value:
# store_min_max_freq_khz() ignores the result of uncore_write() and returns
# the byte count. Only the read-back shows a refused value. Return 1 when
# FREQ is not a step, the write fails, or FILE reads another value.
set_uncore()
{
	local file=$1 freq=$2 label=$3 curfreq want
	want=$((freq / 100000 * 100000))
	if [[ "$want" != "$freq" ]]; then
		echo "Fail! $freq is not a multiple of 100000 kHz ($label)" >&2
		return 1
	fi
	if ! echo "$freq" | sudo tee "$file" >/dev/null; then
		echo "Fail! Writing $freq to $file failed ($label)" >&2
		return 1
	fi
	curfreq=$(<"$file")
	if [[ "$want" == "$curfreq" ]]; then
		echo "Success! $label has been set to $curfreq"
	else
		echo "Fail! Current $label: $curfreq (wanted $want)" >&2
		return 1
	fi
}

main()
{
	local failed=0
	set_uncore "$TIERCE_UNCORE_DIR/package_00_die_00/min_freq_khz" "$ZERO_MIN_UNCORE_FREQ" \
		"node 0 min uncore frequency" || failed=1
	set_uncore "$TIERCE_UNCORE_DIR/package_00_die_00/max_freq_khz" "$ZERO_MAX_UNCORE_FREQ" \
		"node 0 max uncore frequency" || failed=1
	set_uncore "$TIERCE_UNCORE_DIR/package_01_die_00/min_freq_khz" "$ONE_MIN_UNCORE_FREQ" \
		"node 1 min uncore frequency" || failed=1
	set_uncore "$TIERCE_UNCORE_DIR/package_01_die_00/max_freq_khz" "$ONE_MAX_UNCORE_FREQ" \
		"node 1 max uncore frequency" || failed=1
	return "$failed"
}

if ! main; then
	echo "./modify-uncore-freq.sh FAILED: an uncore frequency is not set" >&2
	exit 1
fi
echo "./modify-uncore-freq.sh DONE"
exit 0
