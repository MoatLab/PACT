# tierinit (Colloid-tpp)

Tier-initialization helper for the Colloid-tpp baseline: registers the far NUMA
node as the slow tier and provides `offline_cpus.sh` / `online_cpus.sh` to take
the slow-tier node's CPUs offline (CXL emulation) and back.
Build it after the kernel that it serves has booted. The Makefile uses
`/lib/modules/$(uname -r)/build`.
`offline_cpus.sh` and `online_cpus.sh` hold a fixed CPU list. See
[`../../README.md`](../../README.md#cpu-use) (CPU use).

```bash
make                 # -> tierinit.ko
sudo insmod tierinit.ko
sudo ./offline_cpus.sh   # take the slow-tier node's CPUs offline
```
