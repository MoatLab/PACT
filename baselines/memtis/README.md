# Memtis - Linux v5.15.19 kernel

This folder builds the Memtis kernel used as a baseline in PACT evaluation. Memtis is a hardware-performance-counter-driven tiered memory management system that profiles per-page access frequency using PEBS and migrates pages between tiers accordingly.

---

## Folder contents

| File | Purpose |
|------|---------|
| `setup_memtis.sh` | One-shot build script - clone, patch, configure, compile |
| `memtis.patch` | Memtis patch applied to clean Linux v5.15.19 |
| `memtis-vmstat.patch`, `memtis-pte-allocation.patch` | Build patches applied after `memtis.patch` (see [Build patches](#build-patches)) |
| `config` | Base `.config` used as the starting configuration |

---

## Requirements

- `git`, `make`, `gcc` (gcc 9+ recommended)
- Kernel build dependencies:

```bash
# Ubuntu / Debian
sudo apt install git build-essential bc bison flex libelf-dev libssl-dev \
     libncurses-dev dwarves pahole

# RHEL / CentOS / Fedora
sudo dnf install git gcc make bc bison flex elfutils-libelf-devel openssl-devel \
     ncurses-devel dwarves
```

- At least **20 GB** free disk space
- At least **8 GB RAM** recommended for parallel build

---

## Usage

Memtis builds via `setup_memtis.sh` (not `compile.sh`). The script expects the
kernel tree at `./linux`; `v5.15.19` is a **stable** point release, so clone the
`linux-stable` tree (not `torvalds/linux`). The kernel install below needs root:

```bash
cd memtis
git clone https://git.kernel.org/pub/scm/linux/kernel/git/stable/linux.git linux
sudo ./setup_memtis.sh   # checks out tag v5.15.19
```

The script will:
1. Clone `linux-stable` at tag `v5.15.19` into `linux/`
2. Apply `memtis.patch` (sets `EXTRAVERSION = -htmm`)
3. Apply `memtis-vmstat.patch` and `memtis-pte-allocation.patch`
4. Apply the shared host-tool fix with `../common/apply-build-fixes.sh`
   (see [`../README.md`](../README.md))
5. Copy `config` as the base `.config`
6. Run `make olddefconfig` to resolve any new symbols
7. Force `CONFIG_INTEL_UNCORE_FREQ_CONTROL=y` (built-in `*`)
8. Build with `make -j$(nproc)`

If the repo is already present the clone step is skipped automatically.

---

## Installation (optional)

```bash
cd linux
sudo make modules_install
sudo make install
sudo reboot
```

Verify after reboot:
```bash
uname -r
# expected: 5.15.19-htmm
```

## Build patches

`memtis-vmstat.patch` makes the build work without memory-balloon support.
`memtis-pte-allocation.patch` checks the result of a page-table allocation
before Memtis uses it. To test the second patch, run from the repository
root after applying the patches:

```sh
python3 tests/test-memtis-pte-allocation.py baselines/memtis/linux/arch/x86/mm/pgtable.c
```
