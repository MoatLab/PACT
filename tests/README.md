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
