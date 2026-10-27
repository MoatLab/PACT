/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 MoatLab, Virginia Tech. */
/* cgroup.h: the per-workload cgroup files the runtime reads.
 *
 * Every workload runs in its own cgroup v2 directory with a fast-tier
 * (node 0) cap in memory.max_at_node0; the kernel's per-cgroup demotion
 * thread enforces the cap. The runtime checks the cap and the two switches
 * that make the kernel enforce it at start, and reads the cgroup's demotion
 * and reclaim counters from memory.stat.
 */

#ifndef PACT_CGROUP_H
#define PACT_CGROUP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Directory a relative --workload CGROUP word is resolved against. */
#define PACT_CGROUP_ROOT "/sys/fs/cgroup"

/* Resolve a --workload CGROUP word: an absolute path stays as given, a
 * relative one is taken under PACT_CGROUP_ROOT. Returns 0, or -1 when the
 * path does not fit in len bytes. */
int pact_cgroup_path(const char *word, char *out, size_t len);

/* Read the cgroup's fast-tier cap (memory.max_at_node0) in bytes. Returns 0
 * for a finite cap, -1 when the file is unreadable, or -2 when it holds no
 * finite cap: "max" (no limit), a value that is not a number, or a value at
 * or above the kernel's page-counter maximum in bytes. */
int pact_cgroup_node0_cap(const char *path, uint64_t *bytes);

/* Read an enabled/disabled switch of the cgroup (memory.mmctl_enabled or
 * memory.demotion). The kernel shows the current value in brackets:
 * "[enabled] disabled" or "enabled [disabled]". Returns 1 when enabled, 0
 * when disabled, or -1 when the file is unreadable or holds another text. */
int pact_cgroup_switch(const char *path, const char *name);

/* Sum the values of the given memory.stat keys. Returns 0, or -1 when the
 * file is unreadable or holds none of the keys. */
int pact_cgroup_stat_sum(const char *path, const char *const *keys, int nr_keys, uint64_t *sum);

#endif /* PACT_CGROUP_H */
