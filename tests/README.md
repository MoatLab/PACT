# Runtime regression tests

The tests need no PMU hardware and no root. Each `make -C src <target>`
builds and runs one test, most of them with AddressSanitizer and
UndefinedBehaviorSanitizer. CI runs all of them.

| Target | What it checks |
|--------|----------------|
| `test-pebs-startup` | PEBS setup failures return an error and enable no counter |
| `test-log-levels` | the log levels and the `--log-level` range (needs Python 3) |
| `test-sampling-window` | sampling-window deadlines in both modes |
| `test-thread-coverage` | counting groups follow the workload threads |
| `test-task-scan-error` | a failed scan of a workload's task directory |
| `test-thread-exit` | a thread that exits during the counting refresh |
| `test-workload-exit` | workload exit detection with pidfds |
| `test-workload-gone` | sampling while workloads exit |
| `test-pebs-quality` | the PEBS ring reader and sample accounting |
| `test-cha-freshness` | failed CHA reads and the MLP of stale CHAs |
| `test-pmu-control` | reset, enable and disable failures of the counter groups |
| `test-bin-index` | PAC bin index bounds |
| `test-binning` | adaptive bin quartiles against a full sort |
| `test-pac-stats` | the PAC distribution log |
| `test-pac-threshold` | the PACT top-bin admission |
| `test-quiet-stats` | the statistics coroutine at each log level |
| `test-startup-alloc` | allocation failures at workload start |
| `test-pool-limit` | `--pac-pool-max` |
| `test-pool-fallback` | PAC tables without the object pool |
| `test-migration-recovery` | migration failures and the node query before each call |
| `test-event-loop-startup` | event loop start and shutdown, with each startup failure |
| `test-migration-stop` | migration thread stop and result handling |
| `test-migration-wakeup` | migration worker wakeup |
| `test-migration-wait-stress` | a burst of queued pages, then single pages |
| `test-cli-modes` | mode options, `--workload` rules and LDLAT gate combinations |
| `test-tierce-admission` | the Tierce 0.90 quantile bar |
| `test-workload-mlp` | per-workload MLP |
| `test-domain-round-robin` | migration batching in both modes |
| `test-tierce-sampling` | Tierce sample attribution, `S_w` and the LDLAT `S_p` |
| `test-ldlat-dyn` | the LDLAT-Dyn gate rules |
| `test-cat-control` | the LLC way controller |
| `test-cat-start` | LLC way controller start and start failures |
| `test-migration-trace` | the `--trace-migration` CSV |
| `test-diag-build` | the `make DIAG=1` build and its options |
| `test-cgroup` | the cgroup files and the demotion counters |
| `test-pact-global` | PACT-mode sampling over two workloads |

Two tests run with Python 3 instead of `make`:

- `python3 tests/test-workload-paths.py` checks the workload definitions of
  `run/workloads.sh` with paths that contain spaces and shell characters.
- `tests/test-memtis-pte-allocation.py` needs a patched Memtis kernel tree
  (see [`../baselines/memtis/`](../baselines/memtis/)). CI does not run it.
