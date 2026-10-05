#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 MoatLab, Virginia Tech.
"""Execute workload definitions against recording stand-ins without PMU hardware."""
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class WorkloadPathTests(unittest.TestCase):
    def test_gapbs_commands_preserve_paths_and_parameters(self):
        cases = {
            'bc_kron_8t': ('bc', 'kron.sg', ['-i4', '-n4']),
            'bc_kron_4t': ('bc', 'kron.sg', ['-i4', '-n4']),
        }
        with tempfile.TemporaryDirectory(prefix='pact-workloads-') as temp:
            # Literal shell metacharacters must remain part of a path.
            directory = Path(temp) / 'gap bs $literal;data'
            directory.mkdir()
            graphs = directory / 'graph inputs'
            graphs.mkdir()
            for binary in ('bc', 'pr', 'sssp', 'tc'):
                program = directory / binary
                program.write_text('#!/usr/bin/env python3\nimport sys,json\nprint(json.dumps({"binary": sys.argv[0].rsplit("/", 1)[-1], "args": sys.argv[1:]}))\n')
                program.chmod(0o755)
            env = dict(os.environ, GAPBS_DIR=str(directory), GAPBS_GRAPH_DIR=str(graphs), numactl_args='')
            for name, (binary, graph, options) in cases.items():
                with self.subTest(workload=name):
                    command = subprocess.check_output(
                        ['bash', '-eu', '-c', 'source "$1"; printf "%s" "${!2}"',
                         '--', str(ROOT / 'run/workloads.sh'), name + '_workload_cmd'],
                        env=env, text=True)
                    for script, extra in ((command, []), ('eval \"$1\"', ['--', command])):
                        result = subprocess.check_output(['bash', '-eu', '-c', script, *extra], env=env, text=True)
                        self.assertEqual(json.loads(result), {'binary': binary, 'args': ['-f', str(graphs / graph), *options]})

    def test_legacy_commands_preserve_paths(self):
        cases = {
            'bwaves_8t': ('SPEC_DIR', '603.bwaves_s/speed_bwaves_base.mytest-m64', ['bwaves_1']),
            'w_649_8t': ('SPEC_DIR', '649.fotonik3d_s/cmd.sh', []),
            'silo_5t': ('SILO_DIR', 'out-perf.masstree/benchmarks/dbtest',
                        ['--verbose', '--bench', 'ycsb', '--num-threads', '5',
                         '--scale-factor', '240000', '--parallel-loading', '--runtime', '300']),
            'ptr_chase': ('UBENCH_DIR', 'ptr_chase', ['5120', '10']),
            'seq_array': ('UBENCH_DIR', 'seq_array', ['5120', '10']),
        }
        with tempfile.TemporaryDirectory(prefix='pact-legacy-paths-') as temp:
            base = Path(temp) / 'inputs space $literal;data'
            for name, (variable, relative, arguments) in cases.items():
                with self.subTest(workload=name):
                    binary = base / relative
                    binary.parent.mkdir(parents=True, exist_ok=True)
                    binary.write_text('#!/usr/bin/env python3\nimport json,os,sys\nfrom pathlib import Path\nPath(os.environ["ARG_RECORD"]).write_text(json.dumps(sys.argv[1:]))\n')
                    binary.chmod(0o755)
                    if name == 'bwaves_8t':
                        (binary.parent / 'bwaves_1.in').touch()
                    record = Path(temp) / (name + '.json')
                    env = dict(os.environ, numactl_args='', ARG_RECORD=str(record))
                    env[variable] = str(base)
                    command = subprocess.check_output([
                        'bash', '-eu', '-c', 'source "$1"; printf "%s" "${!2}"',
                        '--', str(ROOT / 'run/workloads.sh'), name + '_workload_cmd'],
                        env=env, text=True)
                    for script, extra in ((command, []), ('eval \"$1\"', ['--', command])):
                        if record.exists():
                            record.unlink()
                        subprocess.run(['bash', '-eu', '-c', script, *extra], env=env, check=True,
                                       capture_output=True, text=True, timeout=5)
                        self.assertEqual(json.loads(record.read_text()), arguments)


if __name__ == '__main__':
    unittest.main()
