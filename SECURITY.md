# Security policy

PACT is a **research prototype**, not production-hardened software. It runs as
root, programs hardware performance counters (PEBS/uncore), loads out-of-tree
kernel modules, installs kernels (the baseline build scripts run
`make install` and `update-grub`), and changes machine-wide
settings (NUMA demotion, THP, CPU online state, uncore frequency). Run it only
on a **dedicated or recoverable test machine** with console access, never on a
shared or production host.

## Supported versions

Security fixes are applied on the default branch and to the most
recent tagged release. There is no long-term-support branch and no response-time
guarantee.

## Reporting a vulnerability

Please report security issues **privately** to the maintainers rather than in a
public issue:

- Huaicheng Li — huaicheng@vt.edu

Include what you observed, how to reproduce it, and the affected revision
(`./src/pact --version`). We will acknowledge the report and coordinate a fix.

## Scope

In scope: memory-safety and privilege-handling defects in the user-space
runtime (`src/`) and the setup/run scripts. Out of scope: the risks inherent in
the documented operation (root access, kernel-module loading, boot/BIOS-level
changes) and issues in vendored or upstream third-party code (see
[`NOTICE`](NOTICE)), which should be reported to their upstream projects.
