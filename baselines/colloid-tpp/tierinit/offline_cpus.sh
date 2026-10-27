#!/bin/bash
# Take the slow-tier (second-socket) CPUs offline to emulate a CPU-less CXL
# tier. The CPU list below is an example for one dual-socket Skylake-X host.
# Edit it to match the slow-tier node of your machine (`lscpu`, `numactl -H`).
# Requires root.
set -euo pipefail
[[ $EUID -eq 0 ]] || { echo "run as root (sudo)"; exit 1; }

for i in 2 6 10 14 18 22 26 30 34 38 42 46 50 54 58 62; do
    echo 0 > "/sys/devices/system/cpu/cpu$i/online"
done
