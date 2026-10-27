---
name: Bug / reproduction report
about: Report a problem building or running PACT, or reproducing a result
title: ""
labels: bug
---

**What happened**
A clear description of the problem and what you expected instead.

**How to reproduce**
The exact command(s) you ran (workload, mode and arm, runtime flags, or the figure/experiment).

**Environment**
- PACT version: (`./src/pact --version`)
- Mode: (`--mode pact` or `--mode tierce`, and the `run.sh` arm)
- CPU model: (`lscpu | grep 'Model name'`)
- Kernel and boot arguments: (`uname -r` and `cat /proc/cmdline`)
- NUMA topology: (`numactl -H`: node sizes and which nodes have CPUs)
- Fast-tier caps: (`--ratio`, and the caps in the run's `manifest.txt` or each workload cgroup's `memory.max_at_node0`)

**Logs**
Attach the relevant output, e.g. `pact_debug.log`, `summary.txt` and
`workload_<i>.output` from `run/results/<workloads>/<arm>-<timestamp>/`, or
the build log.
