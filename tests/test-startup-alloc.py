# SPDX-License-Identifier: MIT
"""Run actual workload startup with each calloc failure position."""
import subprocess
import sys

for fail_at in range(9):
    result = subprocess.run([sys.argv[1], str(fail_at), "0"], capture_output=True, text=True)
    fatal = fail_at in (5, 6, 7)
    assert result.returncode == (1 if fatal else 0), (fail_at, result.stdout, result.stderr)
    if fatal:
        assert "Memory allocation failed" in result.stderr
    else:
        assert "remaining=0" in result.stdout
print("PASS: successful startup and all eight calloc failure positions")
