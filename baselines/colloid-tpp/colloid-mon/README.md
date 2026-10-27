# colloid-mon

Colloid monitoring kernel module for the Colloid-tpp baseline.
Build it after the kernel that it serves has booted. The Makefile uses
`/lib/modules/$(uname -r)/build`.

```bash
make        # -> colloid-mon.ko
```
