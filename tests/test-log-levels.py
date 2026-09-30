#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 MoatLab, Virginia Tech.
"""Check emitted records, default filtering, and the real CLI level map."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = r'''
#include <stdlib.h>
#include "error.h"
int main(int argc, char **argv)
{
    if (argc > 1) {
        set_log_level(atoi(argv[1]));
    }
    log_error("fixture", "level0");
    log_warning("fixture", "level1");
    log_info("fixture", "level2");
    log_debug("fixture", "level3");
    log_trace("fixture", "level4");
    return 0;
}
'''
with tempfile.TemporaryDirectory() as directory:
    fixture = Path(directory) / 'fixture.c'
    binary = Path(directory) / 'fixture'
    fixture.write_text(source)
    subprocess.run([*shlex.split(os.environ.get('CC', 'cc')), '-Wall', '-Wextra', '-Werror',
                    '-I', str(root / 'src'), str(fixture), str(root / 'src/error.c'),
                    '-o', str(binary)], check=True)
    for argument in (None, 0, 1, 2, 3, 4):
        result = subprocess.run([str(binary), *([] if argument is None else [str(argument)])],
                                capture_output=True, text=True, check=True)
        output = result.stdout + result.stderr
        level = 2 if argument is None else argument
        for record in range(5):
            assert (f'level{record}' in output) == (record <= level), (argument, output)

for level in range(5):
    result = subprocess.run([str(root / 'src/pact'), '--log-level', str(level), '--help'],
                            capture_output=True, text=True)
    assert result.returncode == 0, (level, result.stderr)
    assert '3=debug 4=trace' in result.stdout
for level in (-1, 5):
    result = subprocess.run([str(root / 'src/pact'), '--log-level', str(level), '--help'],
                            capture_output=True, text=True)
    assert result.returncode != 0, level
print('Default and explicit log-level filtering, CLI range, and help map: PASS')
