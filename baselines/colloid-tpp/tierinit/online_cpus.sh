#!/bin/bash
# Bring the slow-tier CPUs back online (reverse of offline_cpus.sh). The CPU
# list below is an example. Edit it to match your machine. Requires root.
set -euo pipefail
[[ $EUID -eq 0 ]] || { echo "run as root (sudo)"; exit 1; }

for i in 2 6 10 14 18 22 26 30 34 38 42 46 50 54 58 62; do
    echo 1 > "/sys/devices/system/cpu/cpu$i/online"
done
