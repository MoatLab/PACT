# Runtime regression tests

Run `make -C src test-pebs-startup` to verify that failed PEBS setup returns
an error without enabling counters, and successful setup still enables them.
The test exercises the actual setup function with mocked PMU calls and uses
AddressSanitizer and UndefinedBehaviorSanitizer; hardware access is not required.

`make -C src test-log-levels` checks actual emitted records for the default
INFO setting and levels 0–4, then checks the real CLI range and help text.
The test requires Python 3 and a C compiler, but no PMU hardware.

Run `make -C src test-sampling-window` without root or PMU hardware.
The test executes the actual PEBS coroutine with an expired scheduler deadline
and stubbed counter operations. It requires the next deadline to leave a full
configured sampling interval after counters restart. It does not establish
hardware sampling coverage or application performance.

## Thread counter coverage

`make -C src test-thread-coverage` runs the actual counting setup with real
thread creation and `/proc` discovery, mocking perf event creation and control.
It checks attachment to existing and later workers, stable descriptor reuse,
exited-thread cleanup, non-inheriting groups, and failed-member cleanup under
ASan/UBSan. Hardware counter accuracy and performance require native tests.

The sampling-window test also requires the old PEBS ring to be drained before
counter restart, so newly covered threads cannot enter the previous window.

`make -C src test-workload-exit` creates a real child and checks exit before
reaping it. A pidfd must report termination while `kill(pid, 0)` still succeeds.
The sampling-window test covers exit becoming visible during counter refresh.

`make -C src test-pebs-quality` checks published perf-ring boundaries, wrapping
records, loss/throttle accounting, target-thread coverage and staging/update
queue drops through the actual reader and aggregation code under ASan/UBSan.
It does not require PMU hardware.

`make -C src test-cha-freshness` injects failed and malformed CHA group reads,
checks recovery boundaries and scheduling-time rollback, and rejects MLP from
incomplete CHA coverage. It exercises the runtime reader under ASan/UBSan.

`make -C src test-pmu-control` injects reset, enable, and disable failures
into core, CHA, and task groups. Every failure must stop sampling and attempt
to disable all configured groups; offline core slots are skipped.

## Bin-index bounds

`make -C src test-bin-index` runs the actual PAC update and promotion-queue
path with AddressSanitizer, UndefinedBehaviorSanitizer and float-cast overflow
checks. It verifies very small finite bin widths, scores at the uint64 limit,
zero scores, and ordinary admission boundaries. Oversized bin indices saturate
without changing the score or normal threshold behavior.

## Adaptive binning quartiles

`make -C src test-binning` compares the actual quartile calculation with an
independent full-sort oracle for repeated, ordered, reverse-ordered and random
scores, including the production reservoir size of 100. It also checks input
preservation under ASan/UBSan. Optional `--bench` output from the test binary
measures local CPU time only; application performance needs native experiments.

`make -C src test-pac-stats` exercises the actual distribution logger and
parses emitted total/per-tier averages for scores near UINT64_MAX. The
accumulators must not wrap, and an empty table must not emit a distribution.
This checks telemetry arithmetic, not workload performance.

`make -C src test-pac-threshold` compares the actual histogram count with
promotion-queue admission for fractional, ordinary, and very large accepted
bin widths. It checks float-cast overflow under ASan/UBSan. The `PAC_DIST`
`threshold` field is a floating-point value and may use scientific notation or
`inf`; consumers should parse it as a floating-point number. These diagnostic
checks do not establish an application performance improvement.

`make -C src test-quiet-stats` runs the actual statistics coroutine across
all five logging levels. Error/warning levels skip collection; INFO and higher
still report the PAC distribution. Every level must yield and shut down cleanly.
This verifies skipped diagnostic work, not application performance improvement.

`make -C src test-startup-alloc` exercises workload initialization with each
`calloc` call failed in turn. It checks that a missing PAC table is rejected
and recoverable failures free all earlier allocations. A fixed affinity fixture
keeps the test independent of host CPU numbering. Fatal `safe_calloc` failures
must retain their existing nonzero exit behavior. UBSan rejects invalid
accesses inside allocation helpers before the caller can handle failure.

