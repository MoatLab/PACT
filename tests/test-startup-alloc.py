# SPDX-License-Identifier: MIT
"""Run actual workload startup with each calloc failure position."""
import subprocess
import sys

for fail_at in range(10):
    result = subprocess.run([sys.argv[1], str(fail_at), "0"], capture_output=True, text=True)
    fatal = fail_at in (6, 7, 8)
    assert result.returncode == (1 if fatal else 0), (fail_at, result.stdout, result.stderr)
    if fatal:
        assert "Memory allocation failed" in result.stderr
    else:
        assert "remaining=0" in result.stdout
for fail_at in (0, 1):
    result = subprocess.run([sys.argv[1], str(fail_at), "1"], capture_output=True, text=True)
    assert result.returncode == 0, (fail_at, result.stdout, result.stderr)
    assert ("rc=-1" in result.stdout) == (fail_at == 1), (fail_at, result.stdout)
print("PASS: successful startup, all nine calloc failure positions, and the per-CPU state")
