# Runtime regression tests

Run `make -C src test-pebs-startup` to verify that failed PEBS setup returns
an error without enabling counters, and successful setup still enables them.
The test exercises the actual setup function with mocked PMU calls and uses
AddressSanitizer and UndefinedBehaviorSanitizer; hardware access is not required.
