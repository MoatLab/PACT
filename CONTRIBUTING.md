# Contributing to PACT

Thanks for your interest. PACT is a research artifact for an ASPLOS 2026 paper;
contributions that fix bugs, improve portability, or sharpen the documentation
are welcome.

## Build, style, and checks

The user-space runtime lives in [`src/`](src/) and builds with a C toolchain
plus `libnuma`:

```bash
make -C src                                    # build ./src/pact
make -C src check-style                        # coarse brace-style gate (grep)
make -C src check-format CLANG_FORMAT=clang-format-18   # full clang-format gate
```

CI (`.github/workflows/ci.yml`) runs the build (default and `make logging`
variants) and both style gates on every push and pull request; please make sure
they pass locally first. Match the conventions in
[`src/CODING_STYLE.md`](src/CODING_STYLE.md).

## What can be tested without special hardware

Most of PACT's algorithms build and run anywhere, but end-to-end operation
needs a supported Intel server NUMA host (Skylake-X / Cascade Lake, Sapphire
Rapids, or Emerald Rapids) with working PEBS and CHA/uncore counters, root, and
reboot access (see the [README](README.md) requirements). If
a change affects behavior that only manifests on that hardware, describe the
machine you tested on (`./src/pact --version`, CPU model, kernel, `numactl -H`)
and attach the relevant logs from `run/results/<workload>/pact/` in your pull
request.

## Third-party and mixed-license code

Some directories vendor or adapt upstream code (kernel patches, `khashl.h`,
`minicoro.h`, and the baseline systems) under their own
licenses — see [`NOTICE`](NOTICE) and [`LICENSE`](LICENSE). Do not restyle
vendored files into PACT conventions, and do not add MIT/PACT headers to them.
New first-party files should carry the MIT SPDX header used across `src/`.

## Pull requests

Keep each PR focused on one logical change, with a clear description of the
problem and the fix. Update the relevant README/docs alongside code changes, and
note whether and how you tested on real tiering hardware.
