# Runtime regression tests

Run `make -C src test-pebs-startup` to verify that failed PEBS setup returns
an error without enabling counters, and successful setup still enables them.
The test exercises the actual setup function with mocked PMU calls and uses
AddressSanitizer and UndefinedBehaviorSanitizer; hardware access is not required.

`make -C src test-log-levels` checks actual emitted records for the default
INFO setting and levels 0–4, then checks the real CLI range and help text.
The test requires Python 3 and a C compiler, but no PMU hardware.
