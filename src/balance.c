/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 MoatLab, Virginia Tech. */

/* balance.c: per-workload demotion counters, both modes.
 *
 * The migration thread calls check_migration_balance() at ~1 Hz. It reads
 * each live workload cgroup's pgdemote count (memory.stat), relative to its
 * value at start, for the statistics and the migration trace.
 *
 * On the per-cgroup cap kernel, the cgroup's kdemotiond makes room under
 * the cap, and it runs when a promotion needs room. Demotions therefore
 * follow promotions.
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cgroup.h"
#include "error.h"
#include "pact.h"
#include "balance.h"

int read_cgroup_demotions(const char *cgroup_path, uint64_t *value)
{
    static const char *const keys[] = {"pgdemote"};
    return pact_cgroup_stat_sum(cgroup_path, keys, 1, value);
}

/* Update the workload's demotion counters from its cgroup's pgdemote.
 * Returns 0, or -1 when the count is unreadable. */
static int update_demotion_counts(pact_workload_t *wl)
{
    uint64_t demoted;
    if (read_cgroup_demotions(wl->cgroup_path, &demoted) < 0) {
        log_warning("check_migration_balance", "Cannot read pgdemote of %s", wl->cgroup_path);
        return -1;
    }
    /* pgdemote counts from the creation of the cgroup; pact_init snapshots
     * the startup value into demotion_baseline so this run's demotions count
     * from time zero (the first balance check runs ~1s in, and demotions
     * during that first second must not be lost). */
    uint64_t n_demoted = (demoted >= wl->demotion_baseline) ? demoted - wl->demotion_baseline : 0;
    atomic_store_explicit(&wl->stats.demotion_successes, n_demoted, memory_order_relaxed);
    return 0;
}

void check_migration_balance(pact_context_t *pact)
{
    for (int i = 0; i < pact->nr_workloads; i++) {
        if (!pact->workloads[i].exited) {
            (void)update_demotion_counts(&pact->workloads[i]);
        }
    }
}
