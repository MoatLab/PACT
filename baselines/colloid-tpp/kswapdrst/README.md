# kswapdrst (Colloid-tpp)

kswapd-reset helper for the Colloid-tpp baseline: keeps `kswapd` from
permanently backing off so demotion does not stall.
Build it after the kernel that it serves has booted. The Makefile uses
`/lib/modules/$(uname -r)/build`.

```bash
make                  # -> kswapdrst.ko
sudo insmod kswapdrst.ko
```
